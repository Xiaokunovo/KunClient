#include "core/Types.h"
#include "protocol/Firehose.h"
#include "transport/SerialTransport.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QXmlStreamReader>

#include <atomic>
#include <iostream>
#include <limits>

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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testStorageProbeText();
    testRangePrechecks();
    testErase();
    if (failures) return 1;
    std::cout << "EDL protocol fake-device regressions passed\n";
    return 0;
}
