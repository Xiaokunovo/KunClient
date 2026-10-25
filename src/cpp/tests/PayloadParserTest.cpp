// Payload（CrAU + protobuf manifest）解析回归：合成一份带 REPLACE / REPLACE_BZ /
// REPLACE_XZ / REPLACE_ZSTD / ZERO / SOURCE_COPY 的 payload，逐分区提取后比对字节。

#include "storage/PayloadParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>

#include <cstring>
#include <iostream>

namespace {

using swt::edl::PayloadInfo;
using swt::edl::PayloadParser;

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// ── 手写 protobuf 编码 ────────────────────────────────────────────────────
void putVarint(QByteArray& out, quint64 value)
{
    while (value >= 0x80) {
        out.append(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.append(static_cast<char>(value));
}

void putTag(QByteArray& out, quint32 field, quint32 wire)
{
    putVarint(out, (static_cast<quint64>(field) << 3) | wire);
}

void putBytesField(QByteArray& out, quint32 field, const QByteArray& value)
{
    putTag(out, field, 2);
    putVarint(out, static_cast<quint64>(value.size()));
    out.append(value);
}

void putVarintField(QByteArray& out, quint32 field, quint64 value)
{
    putTag(out, field, 0);
    putVarint(out, value);
}

void appendBe64(QByteArray& out, quint64 value)
{
    char buffer[8];
    qToBigEndian<quint64>(value, buffer);
    out.append(buffer, 8);
}

void appendBe32(QByteArray& out, quint32 value)
{
    char buffer[4];
    qToBigEndian<quint32>(value, buffer);
    out.append(buffer, 4);
}

// ── 压缩样本 ──────────────────────────────────────────────────────────────
QByteArray bz2Compress(const QByteArray& input)
{
    unsigned int destinationLength =
        static_cast<unsigned int>(input.size() + input.size() / 100 + 600);
    QByteArray output(static_cast<qsizetype>(destinationLength), '\0');
    const int rc = BZ2_bzBuffToBuffCompress(output.data(), &destinationLength,
                                            const_cast<char*>(input.constData()),
                                            static_cast<unsigned int>(input.size()), 9, 0, 30);
    if (rc != BZ_OK) return {};
    output.resize(static_cast<qsizetype>(destinationLength));
    return output;
}

QByteArray xzCompress(const QByteArray& input)
{
    const size_t bound = lzma_stream_buffer_bound(static_cast<size_t>(input.size()));
    QByteArray output(static_cast<qsizetype>(bound), '\0');
    size_t outputPosition = 0;
    const lzma_ret rc = lzma_easy_buffer_encode(
        6, LZMA_CHECK_CRC32, nullptr,
        reinterpret_cast<const uint8_t*>(input.constData()),
        static_cast<size_t>(input.size()),
        reinterpret_cast<uint8_t*>(output.data()), &outputPosition, bound);
    if (rc != LZMA_OK) return {};
    output.resize(static_cast<qsizetype>(outputPosition));
    return output;
}

QByteArray zstdCompress(const QByteArray& input)
{
    const size_t bound = ZSTD_compressBound(static_cast<size_t>(input.size()));
    QByteArray output(static_cast<qsizetype>(bound), '\0');
    const size_t written = ZSTD_compress(output.data(), bound, input.constData(),
                                         static_cast<size_t>(input.size()), 3);
    if (ZSTD_isError(written)) return {};
    output.resize(static_cast<qsizetype>(written));
    return output;
}

// ── payload 构造 ──────────────────────────────────────────────────────────
struct OpSpec {
    int type = 0;
    QByteArray data;
    QVector<QPair<quint64, quint64>> extents;   // (start_block, num_blocks)
};

struct PartSpec {
    QString name;
    quint64 size = 0;
    QVector<OpSpec> ops;
};

QByteArray buildPayload(const QVector<PartSpec>& partitions)
{
    QByteArray manifest;
    QByteArray dataArea;

    for (const PartSpec& partition : partitions) {
        QByteArray partitionBytes;
        putBytesField(partitionBytes, 1, partition.name.toUtf8());

        QByteArray info;
        putVarintField(info, 1, partition.size);
        putBytesField(partitionBytes, 7, info);

        for (const OpSpec& op : partition.ops) {
            QByteArray opBytes;
            putVarintField(opBytes, 1, static_cast<quint64>(op.type));
            putVarintField(opBytes, 2, static_cast<quint64>(dataArea.size()));
            putVarintField(opBytes, 3, static_cast<quint64>(op.data.size()));
            for (const auto& extent : op.extents) {
                QByteArray extentBytes;
                putVarintField(extentBytes, 1, extent.first);
                putVarintField(extentBytes, 2, extent.second);
                putBytesField(opBytes, 6, extentBytes);
            }
            putBytesField(partitionBytes, 8, opBytes);
            dataArea.append(op.data);
        }

        putBytesField(manifest, 13, partitionBytes);
    }

    QByteArray file("CrAU");
    appendBe64(file, 2);   // version
    appendBe64(file, static_cast<quint64>(manifest.size()));
    appendBe32(file, 0);   // metadata signature size
    file.append(manifest);
    file.append(dataArea);
    return file;
}

QByteArray pattern(char value, int size)
{
    return QByteArray(size, value);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

int main()
{
    constexpr quint64 kBlock = PayloadParser::BlockSize;

    const QByteArray bootData = pattern('B', 4096);
    QByteArray vendorData = pattern('V', 4096) + pattern('W', 4096);
    const QByteArray systemData = pattern('S', 4096);
    const QByteArray productData = pattern('P', 8192);

    QVector<PartSpec> partitions;

    {
        PartSpec boot;
        boot.name = QStringLiteral("boot");
        boot.size = 4096;
        OpSpec op;
        op.type = 0;   // REPLACE
        op.data = bootData;
        op.extents = { { 0, 1 } };
        boot.ops = { op };
        partitions.append(boot);
    }
    {
        PartSpec vendor;
        vendor.name = QStringLiteral("vendor");
        vendor.size = 3 * kBlock;
        OpSpec write;
        write.type = 1;   // REPLACE_BZ
        write.data = bz2Compress(vendorData);
        // 故意拆成两段不连续的 extent，验证跨 extent 顺序写。
        write.extents = { { 0, 1 }, { 2, 1 } };
        OpSpec zero;
        zero.type = 6;    // ZERO（中间那块）
        zero.extents = { { 1, 1 } };
        vendor.ops = { write, zero };
        partitions.append(vendor);
    }
    {
        PartSpec system;
        system.name = QStringLiteral("system");
        system.size = 4096;
        OpSpec op;
        op.type = 14;     // REPLACE_ZSTD
        op.data = zstdCompress(systemData);
        op.extents = { { 0, 1 } };
        system.ops = { op };
        partitions.append(system);
    }
    {
        PartSpec product;
        product.name = QStringLiteral("product");
        product.size = 8192;
        OpSpec op;
        op.type = 8;      // REPLACE_XZ
        op.data = xzCompress(productData);
        op.extents = { { 0, 2 } };
        product.ops = { op };
        partitions.append(product);
    }
    {
        PartSpec odm;
        odm.name = QStringLiteral("odm");
        odm.size = 4096;
        OpSpec op;
        op.type = 4;      // SOURCE_COPY（增量包）
        op.extents = { { 0, 1 } };
        odm.ops = { op };
        partitions.append(odm);
    }

    const QByteArray payload = buildPayload(partitions);
    check(!payload.isEmpty(), "build synthetic payload");

    QTemporaryDir temp;
    check(temp.isValid(), "temp dir");
    const QString source = QDir(temp.path()).filePath(QStringLiteral("payload.bin"));
    {
        QFile file(source);
        check(file.open(QIODevice::WriteOnly), "write payload");
        file.write(payload);
        file.close();
    }

    check(PayloadParser::isCrAU(source), "isCrAU detects CrAU");
    check(!PayloadParser::isCrAU(QDir(temp.path()).filePath(QStringLiteral("nope.bin"))),
          "isCrAU rejects missing file");

    // ── inspect ──────────────────────────────────────────────────────────
    {
        PayloadInfo info;
        QString error;
        check(PayloadParser::inspect(source, &info, &error), "inspect succeeds");
        if (!error.isEmpty()) std::cerr << "  error: " << error.toStdString() << '\n';
        check(info.partitions.size() == 5, "inspect lists 5 partitions");
        QStringList names;
        for (const auto& partition : info.partitions) names << partition.name;
        check(names.contains(QStringLiteral("boot")), "inspect has boot");
        check(names.contains(QStringLiteral("vendor")), "inspect has vendor");
        check(names.contains(QStringLiteral("odm")), "inspect has odm");
        for (const auto& partition : info.partitions) {
            if (partition.name != QLatin1String("vendor")) continue;
            check(partition.size == 3 * kBlock, "inspect vendor size");
            check(partition.operationCount == 2, "inspect vendor op count");
        }
        check(info.dataOffset > 24, "inspect data offset");
    }

    // ── 提取：boot / vendor / system / product ───────────────────────────
    {
        const QString outDir = QDir(temp.path()).filePath(QStringLiteral("out"));
        QStringList extracted;
        QString error;
        const QStringList wanted = { QStringLiteral("boot"), QStringLiteral("vendor"),
                                     QStringLiteral("system"), QStringLiteral("product") };
        const bool ok = PayloadParser::extract(source, outDir, wanted, &extracted, &error);
        if (!ok) std::cerr << "  error: " << error.toStdString() << '\n';
        check(ok, "extract four partitions");
        check(extracted.size() == 4, "four files extracted");

        check(readFile(QDir(outDir).filePath(QStringLiteral("boot.img"))) == bootData,
              "REPLACE partition bytes match");

        const QByteArray vendorImage = readFile(QDir(outDir).filePath(QStringLiteral("vendor.img")));
        check(vendorImage.size() == static_cast<qsizetype>(3 * kBlock), "vendor size");
        check(vendorImage.left(4096) == pattern('V', 4096), "BZ extent 0");
        check(vendorImage.mid(4096, 4096) == QByteArray(4096, '\0'), "ZERO extent 1");
        check(vendorImage.mid(8192, 4096) == pattern('W', 4096), "BZ extent 2 (out of order)");

        check(readFile(QDir(outDir).filePath(QStringLiteral("system.img"))) == systemData,
              "REPLACE_ZSTD bytes match");
        check(readFile(QDir(outDir).filePath(QStringLiteral("product.img"))) == productData,
              "REPLACE_XZ bytes match");
    }

    // ── 增量包操作必须明确报错，不能静默写空 ─────────────────────────────
    {
        const QString outDir = QDir(temp.path()).filePath(QStringLiteral("out-odm"));
        QStringList extracted;
        QString error;
        const bool ok = PayloadParser::extract(source, outDir, { QStringLiteral("odm") },
                                               &extracted, &error);
        check(!ok, "incremental op is rejected");
        check(error.contains(QStringLiteral("增量")), "incremental error message");
    }

    // ── 不存在的分区 ─────────────────────────────────────────────────────
    {
        const QString outDir = QDir(temp.path()).filePath(QStringLiteral("out-missing"));
        QStringList extracted;
        QString error;
        check(!PayloadParser::extract(source, outDir, { QStringLiteral("no_such") }, &extracted,
                                      &error),
              "unknown partition rejected");
        check(error.contains(QStringLiteral("没有分区")), "unknown partition message");
    }

    // ── 非 CrAU 文件不能误判 ─────────────────────────────────────────────
    {
        const QString fake = QDir(temp.path()).filePath(QStringLiteral("fake.bin"));
        QFile file(fake);
        if (!file.open(QIODevice::WriteOnly)) {
            std::cerr << "cannot create fake.bin\n";
            return 1;
        }
        file.write(QByteArray(4096, 'X'));
        file.close();

        PayloadInfo info;
        QString error;
        check(!PayloadParser::inspect(fake, &info, &error), "non-CrAU rejected");
        check(!error.isEmpty(), "non-CrAU error message");
    }

    if (failures != 0) {
        std::cerr << failures << " payload check(s) failed\n";
        return 1;
    }
    std::cout << "Payload parser tests passed\n";
    return 0;
}
