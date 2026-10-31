#include "core/Types.h"
#include "protocol/Firehose.h"
#include "transport/SerialTransport.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QVector>
#include <QXmlStreamReader>

#include <atomic>
#include <iostream>
#include <limits>
#include <utility>

namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

// 协议假设备：基线非零，擦除后按测试场景返回零/未变化/NAK。
// 所有 I/O 均在内存完成；从不调用 open() 或真实串口实现。
class FakeFirehose final : public swt::edl::SerialTransport {
public:
    explicit FakeFirehose(std::atomic_bool& cancel) : cancellation(cancel) {}
    bool failTailBaseline = false;
    bool failHeadBaseline = false;
    bool unchangedTail = false;
    bool unchangedHead = false;
    bool failProbe = false;
    bool cancelProbe = false;
    bool delayedErase = false;
    bool probeText = false;
    bool bareStorageLine = false;
    int probeCount = 0;
    int writes = 0;
    int eraseCalls = 0;
    quint64 start = 0;
    bool isOpen() const override { return true; }
    bool writeAll(QByteArrayView bytes, int, QString*) override
    {
        ++writes;
        QXmlStreamReader xml(QByteArray(bytes.data(), bytes.size()));
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == QLatin1String("read")) {
                reading = true;
                rawResponse = true;
                start = xml.attributes().value(QLatin1String("start_sector")).toULongLong();
            } else if (xml.name() == QLatin1String("erase")) {
                reading = false;
                erased = true;
                ++eraseCalls;
            }
        }
        return true;
    }
    bool readUntil(QByteArrayView, QByteArray* output, qsizetype, int, QString* error) override
    {
        if (reading && rawResponse) {
            rawResponse = false;
            const bool reject = !erased && ((start == 10 && failHeadBaseline) ||
                                            (start == 11 && failTailBaseline));
            if (reject) {
                *output = QByteArrayLiteral("<data><response value=\"NAK\"/></data>");
                return true;
            }
            if (erased && failProbe) {
                if (error) *error = QStringLiteral("模拟回读失败");
                return false;
            }
            if (erased && cancelProbe) {
                cancellation.store(true);
                if (error) *error = QStringLiteral("操作已停止");
                return false;
            }
            *output = QByteArrayLiteral("<data><response value=\"ACK\" rawmode=\"true\"/></data>");
        } else if (probeText) {
            *output = QByteArrayLiteral(
                "<data>"
                "<log value=\"INFO: ufs: SAMSUNG\"/>"
                "<log value=\"Chip serial num (1234ABCD)\"/>"
                "<response value=\"ACK\"/></data>");
        } else if (bareStorageLine) {
            *output = QByteArrayLiteral(
                "<data><log value=\"INFO: emmc\"/><response value=\"ACK\"/></data>");
        } else {
            *output = QByteArrayLiteral("<data><response value=\"ACK\"/></data>");
        }
        return true;
    }
    bool readExactly(qsizetype size, QByteArray* output, int, QString*) override
    {
        const bool unchanged = (start == 10 && unchangedHead) || (start == 11 && unchangedTail)
            || (erased && delayedErase && probeCount++ == 0);
        *output = QByteArray(size, !erased || unchanged ? '\x5a' : '\0');
        return true;
    }
private:
    std::atomic_bool& cancellation;
    bool erased = false;
    bool reading = false;
    bool rawResponse = false;
};

void testErase()
{
    using Client = swt::edl::FirehoseClient;
    using Result = Client::EraseOutcome;
    for (int scenario = 0; scenario < 9; ++scenario) {
        std::atomic_bool cancelled{false};
        FakeFirehose transport(cancelled);
        transport.failTailBaseline = scenario == 1 || scenario == 7;
        transport.failHeadBaseline = scenario == 2;
        transport.unchangedTail = scenario == 3;
        transport.unchangedHead = scenario == 4 || scenario == 7;
        transport.failProbe = scenario == 5;
        transport.cancelProbe = scenario == 6;
        transport.delayedErase = scenario == 8;
        Client client(transport, cancelled, {}, {});
        QString error;
        const Result expected[] = {Result::Verified, Result::Unverified, Result::Unverified,
            Result::NotLanded, Result::NotLanded, Result::Unverified, Result::Failed,
            Result::NotLanded, Result::Verified};
        const auto result = client.erase(10, 2, 0, 512, &error);
        check(result == expected[scenario], "erase must distinguish missing baseline, unchanged sentinel and cancellation");
        check(transport.eraseCalls == 1, "erase ACK must not trigger duplicate destructive command");
        if (scenario == 6) check(error.contains(QStringLiteral("停止")), "cancelled verification must carry stopped terminal reason");
    }
}

// MiUfsProbe 的存储探测形态：loader 只打文本行，没有 storage_info JSON。
void testStorageProbeText()
{
    using Client = swt::edl::FirehoseClient;
    std::atomic_bool cancelled{false};
    FakeFirehose transport(cancelled);
    transport.probeText = true;
    Client client(transport, cancelled, {}, {});
    QString error;
    check(client.ping(&error, 500), "nop ping succeeds while parsing storage log lines");
    swt::edl::DeviceInfo info;
    client.enrichDeviceInfo(&info);
    check(info.storage == QStringLiteral("UFS"), "INFO ufs line sets storage type");
    check(info.storageVendor == QStringLiteral("SAMSUNG"), "INFO ufs line sets storage vendor");
    check(info.serial == QStringLiteral("1234abcd"), "standalone chip serial line is parsed without Chip ID");

    // 无 vendor 的降级形态只补类型，不臆造厂商。
    std::atomic_bool cancelled2{false};
    FakeFirehose bare(cancelled2);
    bare.probeText = false;
    bare.bareStorageLine = true;
    Client bareClient(bare, cancelled2, {}, {});
    check(bareClient.ping(&error, 500), "nop ping succeeds for bare storage line");
    swt::edl::DeviceInfo bareInfo;
    bareClient.enrichDeviceInfo(&bareInfo);
    check(bareInfo.storage == QStringLiteral("EMMC"), "bare INFO emmc line sets storage type");
    check(bareInfo.storageVendor.isEmpty(), "bare storage line must not invent a vendor");
}

void testRangePrechecks()
{
    using Client = swt::edl::FirehoseClient;
    std::atomic_bool cancelled{false};
    FakeFirehose transport(cancelled);
    Client client(transport, cancelled, {}, {});
    QByteArray image(512, 'A');
    QBuffer input(&image);
    input.open(QIODevice::ReadOnly);
    QByteArray bytes;
    QBuffer output(&bytes);
    output.open(QIODevice::WriteOnly);
    QString error;
    constexpr quint64 maximum = std::numeric_limits<quint64>::max();
    check(!client.program(input, 512, 0, 0, 0, 512, &error), "zero target capacity cannot mean unlimited write");
    check(!client.program(input, 513, 0, 2, 0, 512, &error), "short random-access source rejected before RAW");
    check(input.seek(1), "test source positioned at offset 1");
    check(!client.program(input, 512, 0, 1, 0, 512, &error), "remaining size rather than whole source size is validated");
    input.seek(0);
    check(!client.program(input, maximum, 0, 1, 0, 512, &error), "sector rounding overflow rejected");
    check(!client.program(input, 512, maximum, 1, 0, 512, &error), "program target address overflow rejected");
    check(!client.readToDevice(output, maximum, 1, 0, 512, &error), "read address overflow rejected");
    check(client.erase(maximum, 1, 0, 512, &error) == Client::EraseOutcome::Failed, "erase address overflow rejected");
    cancelled.store(true);
    check(!client.program(input, 512, 0, 1, 0, 512, &error), "cancelled write must not begin RAW");
    check(client.erase(10, 2, 0, 512, &error) == Client::EraseOutcome::Failed, "pre-cancelled erase rejected");
    check(transport.writes == 0, "all invalid/cancelled calls must produce zero transport writes");
}
} // namespace

// ── 传输层缓冲：真实 SerialTransport 逻辑，只把「从端口取数」换成内存供给 ──
// 覆盖读游标、交付后压缩、帧切分保留尾巴、以及单次填充的预算上界。
class ScriptedTransport final : public swt::edl::SerialTransport {
public:
    explicit ScriptedTransport(QByteArray feed) : m_feed(std::move(feed)) {}
    /// 每次 fillBuffer 供多少字节（模拟一次 USB 唤醒能拿到的量）。
    qsizetype feedStep = 4096;
    int fillCalls = 0;
    /// 见过的最大单次填充量，用来证明预算生效。
    qsizetype maxFill = 0;

protected:
    bool fillBuffer(int, qsizetype budget, QString*) override
    {
        ++fillCalls;
        if (m_offset >= m_feed.size()) return true;
        const qsizetype want = qMin(qMin(feedStep, budget), m_feed.size() - m_offset);
        if (want <= 0) return true;
        m_buffer += QByteArray(m_feed.constData() + m_offset, want);
        m_offset += want;
        maxFill = qMax(maxFill, want);
        return true;
    }

private:
    QByteArray m_feed;
    qsizetype m_offset = 0;
};

void testTransportBuffering()
{
    using swt::edl::SerialTransport;

    // 1) readExactly 跨多次填充拼接，且字节序正确。
    {
        QByteArray feed;
        for (int i = 0; i < 5000; ++i) feed.append(static_cast<char>(i & 0xFF));
        ScriptedTransport transport(feed);
        QByteArray out;
        QString error;
        check(transport.readExactly(5000, &out, 5000, &error), "readExactly must stitch multiple fills");
        check(out.size() == 5000, "readExactly must deliver the requested size");
        check(out == feed, "readExactly must preserve byte order across fills");
        check(transport.fillCalls > 1, "5000 bytes in 4096-byte fills must need more than one fill");
    }

    // 2) 读游标：连续两次小读之后，缓冲不应按每次交付都重排（交付量远大于阈值时才压缩）。
    {
        QByteArray feed(200 * 1024, 'q');
        ScriptedTransport transport(feed);
        transport.feedStep = 200 * 1024;
        QString error;
        QByteArray first;
        check(transport.readExactly(100 * 1024, &first, 5000, &error), "large first read must succeed");
        QByteArray second;
        check(transport.readExactly(100 * 1024, &second, 5000, &error), "large second read must succeed");
        check(first == QByteArray(100 * 1024, 'q') && second == first,
              "both halves must match the fed data");
        // 每次填充按「当前待收量」封顶，所以两次 100 KiB 读各触发一次填充；
        // 关键是已经取到的数据不会被重复索取或丢失。
        check(transport.fillCalls == 2,
              "each read must fetch only what it still needs, without refetching delivered data");
    }

    // 3) readUntil 必须把终结符之后的字节留在缓冲里（RAW 数据紧跟在 ACK 后面）。
    {
        const QByteArray raw(64, '\0');
        QByteArray feed = QByteArrayLiteral("<data><response value=\"ACK\" rawmode=\"true\"/></data>") + raw;
        ScriptedTransport transport(feed);
        transport.feedStep = 1;   // 逐字节供给：逼出「帧还没收全」的循环
        QByteArray frame;
        QString error;
        check(transport.readUntil(QByteArrayView("</data>"), &frame, 4096, 5000, &error),
              "readUntil must find the terminator across byte-at-a-time fills");
        check(frame.endsWith("</data>"), "frame must end at the terminator");
        QByteArray tail;
        check(transport.readExactly(raw.size(), &tail, 5000, &error),
              "bytes after the terminator must stay buffered");
        check(tail == raw, "raw payload after </data> must not be dropped");
    }

    // 4) 单次填充不超过预算：否则进度回调与取消检查会被饿死。
    {
        QByteArray feed(64 * 1024, 'z');
        ScriptedTransport transport(feed);
        transport.feedStep = 64 * 1024;
        QByteArray out;
        QString error;
        check(transport.readExactly(1024, &out, 5000, &error), "small read must succeed");
        check(transport.maxFill <= 1024,
              "fillBuffer must not pull more than the pending request needs");
    }

    // 5) 终结符跨填充边界：增量扫描必须回退 marker.size()-1 才能拼出 "</data>"。
    //    这是 readUntil 改成增量查找后最容易回归的地方。
    {
        // 每次只给 3 字节，迫使 "</data>" 被拆到两次填充里。
        QByteArray feed = QByteArrayLiteral("<data><response value=\"ACK\"/></data>");
        ScriptedTransport transport(feed);
        transport.feedStep = 3;
        QByteArray frame;
        QString error;
        check(transport.readUntil(QByteArrayView("</data>"), &frame, 4096, 5000, &error),
              "incremental scan must still match a terminator split across fills");
        check(frame.endsWith("</data>"),
              "split terminator must not be missed by the rescan window");
        check(frame == feed, "frame must cover the whole buffered response");
    }

    // 6) 连续多帧：一条命令的响应常被拆成多条 <data>，逐帧取不能丢内容。
    {
        QByteArray feed;
        for (int i = 0; i < 5; ++i) {
            feed += QByteArrayLiteral("<data><log value=\"frame\"/></data>");
        }
        ScriptedTransport transport(feed);
        transport.feedStep = 7;   // 奇数步长，故意错开帧边界
        QString error;
        int frames = 0;
        for (; frames < 5; ++frames) {
            QByteArray frame;
            if (!transport.readUntil(QByteArrayView("</data>"), &frame, 4096, 5000, &error)) break;
            if (!frame.endsWith("</data>")) break;
        }
        check(frames == 5, "five back-to-back frames must all be extracted");
    }
}

// ── payload 协商：先按大值申请，被拒后必须能降档，且不能卡在失败上 ──
// 这台工具靠「往大了要」拿大分片（设备取 min(申请值, 支持值)），但老 loader
// 可能直接 NAK。降档链一旦走不通，原本能刷的设备就变成刷不了 —— 必须锁住。
class FakeConfigure final : public swt::edl::SerialTransport {
public:
    explicit FakeConfigure(std::atomic_bool& cancel) : cancellation(cancel) {}
    /// 每次 configure 收到的 MaxPayloadSizeToTargetInBytes，按顺序记录。
    QVector<quint64> requested;
    /// true = 设备 ACK 并把值压到 supported；false = 设备直接 NAK。
    bool accept = true;
    quint64 supported = 1024 * 1024;
    bool nakWithHint = true;
    bool isOpen() const override { return true; }
    bool writeAll(QByteArrayView bytes, int, QString*) override
    {
        QXmlStreamReader xml(QByteArray(bytes.data(), bytes.size()));
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() != QLatin1String("configure")) continue;
            requested.push_back(xml.attributes()
                                    .value(QLatin1String("MaxPayloadSizeToTargetInBytes"))
                                    .toULongLong());
        }
        return true;
    }
    bool readUntil(QByteArrayView, QByteArray* output, qsizetype, int, QString*) override
    {
        if (accept) {
            *output = QStringLiteral("<data><response value=\"ACK\" "
                                     "MaxPayloadSizeToTargetInBytes=\"%1\"/></data>")
                          .arg(supported).toUtf8();
        } else if (nakWithHint) {
            *output = QStringLiteral("<data><response value=\"NAK\"/>"
                                     "<log value=\"MaxPayloadSizeToTargetInBytesSupported = %1\"/>"
                                     "</data>")
                          .arg(supported).toUtf8();
        } else {
            *output = QByteArrayLiteral("<data><response value=\"NAK\"/></data>");
        }
        return true;
    }
    bool readExactly(qsizetype size, QByteArray* output, int, QString*) override
    {
        *output = QByteArray(size, '\0');
        return true;
    }
private:
    std::atomic_bool& cancellation;
};

void testPayloadNegotiation()
{
    using Client = swt::edl::FirehoseClient;

    // 1) 主机首次申请值必须远大于 1 MB：申请小了设备只会给申请值。
    {
        std::atomic_bool cancelled{false};
        FakeConfigure transport(cancelled);
        transport.accept = true;
        transport.supported = 8 * 1024 * 1024;
        Client client(transport, cancelled, {}, {});
        QString error;
        check(client.configure(swt::edl::StorageType::Ufs, &error),
              "configure must succeed when the target accepts the requested payload");
        check(transport.requested.size() == 1, "an accepted configure must be sent exactly once");
        check(transport.requested.value(0) >= 8ULL * 1024ULL * 1024ULL,
              "host must ask for a large payload up front, not the 1 MiB default");
        check(client.maximumPayload() == 8ULL * 1024ULL * 1024ULL,
              "negotiated payload must follow the target answer");
    }

    // 2) 设备 NAK 并报上限：必须降档重试，且重试值就是设备报的值。
    {
        std::atomic_bool cancelled{false};
        FakeConfigure transport(cancelled);
        transport.accept = false;
        transport.nakWithHint = true;
        transport.supported = 1024 * 1024;
        Client client(transport, cancelled, {}, {});
        QString error;
        client.configure(swt::edl::StorageType::Ufs, &error);
        check(transport.requested.size() == 2,
              "a rejected configure must be retried once with a downgraded payload");
        check(transport.requested.value(1) == 1024ULL * 1024ULL,
              "retry must use the MaxPayloadSizeToTargetInBytesSupported value");
    }

    // 3) 设备 NAK 但没报上限：必须退回 1 MB 再试，不能直接失败。
    {
        std::atomic_bool cancelled{false};
        FakeConfigure transport(cancelled);
        transport.accept = false;
        transport.nakWithHint = false;
        Client client(transport, cancelled, {}, {});
        QString error;
        client.configure(swt::edl::StorageType::Ufs, &error);
        check(transport.requested.size() == 2,
              "a NAK without a supported hint must still be retried");
        check(transport.requested.value(1) == 1024ULL * 1024ULL,
              "retry without a hint must fall back to the long-verified 1 MiB");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testStorageProbeText();
    testRangePrechecks();
    testErase();
    testTransportBuffering();
    testPayloadNegotiation();
    if (failures) return 1;
    std::cout << "EDL protocol fake-device regressions passed\n";
    return 0;
}
