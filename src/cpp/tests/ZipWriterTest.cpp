#include "kc/tools/ZipWriter.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// ── 测试自带的极简 ZIP 解析器：按 APPNOTE 逐字节读回来，验证产物结构 ────────
//  刻意不复用 ZipWriter 的任何常量/代码，避免「自己写错自己验」的假通过。
quint16 readU16(const QByteArray& bytes, qsizetype offset)
{
    const quint16 low = static_cast<quint8>(bytes.at(offset));
    const quint16 high = static_cast<quint8>(bytes.at(offset + 1));
    return static_cast<quint16>(low | static_cast<quint16>(high << 8));
}

quint32 readU32(const QByteArray& bytes, qsizetype offset)
{
    return static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) |
           (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8) |
           (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16) |
           (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
}

/// 从尾部找 EOCD 签名（EOCD 固定 22 字节 + 最长 65535 字节注释）。
qsizetype findEndOfCentralDirectory(const QByteArray& bytes)
{
    if (bytes.size() < 22) return -1;   // 比 EOCD 还短，不可能是 zip
    const qsizetype last = bytes.size() - 22;
    const qsizetype first = qMax<qsizetype>(0, bytes.size() - 22 - 0xFFFF);
    for (qsizetype i = last; i >= first; --i) {
        if (readU32(bytes, i) == 0x06054b50u) return i;
    }
    return -1;
}

struct CentralEntry
{
    QByteArray name;
    quint16 versionMadeBy = 0;
    quint16 versionNeeded = 0;
    quint16 flags = 0;
    quint16 method = 0;
    quint16 dosTime = 0;
    quint16 dosDate = 0;
    quint32 crc = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint16 extraLength = 0;
    quint16 commentLength = 0;
    quint32 localOffset = 0;
};

/// 解析中央目录；结构不合规范时返回 false（越界也算）。
bool parseCentralDirectory(const QByteArray& bytes, qsizetype eocdOffset,
                           QVector<CentralEntry>* entries)
{
    entries->clear();
    if (eocdOffset < 0 || eocdOffset + 22 > bytes.size()) return false;

    const quint16 count = readU16(bytes, eocdOffset + 10);
    const quint32 size = readU32(bytes, eocdOffset + 12);
    const quint32 offset = readU32(bytes, eocdOffset + 16);
    if (static_cast<qint64>(offset) + static_cast<qint64>(size) != static_cast<qint64>(eocdOffset)) {
        return false;
    }

    qsizetype cursor = offset;
    for (quint16 index = 0; index < count; ++index) {
        if (cursor + 46 > bytes.size() || readU32(bytes, cursor) != 0x02014b50u) return false;
        CentralEntry entry;
        entry.versionMadeBy = readU16(bytes, cursor + 4);
        entry.versionNeeded = readU16(bytes, cursor + 6);
        entry.flags = readU16(bytes, cursor + 8);
        entry.method = readU16(bytes, cursor + 10);
        entry.dosTime = readU16(bytes, cursor + 12);
        entry.dosDate = readU16(bytes, cursor + 14);
        entry.crc = readU32(bytes, cursor + 16);
        entry.compressedSize = readU32(bytes, cursor + 20);
        entry.uncompressedSize = readU32(bytes, cursor + 24);
        const quint16 nameLength = readU16(bytes, cursor + 28);
        entry.extraLength = readU16(bytes, cursor + 30);
        entry.commentLength = readU16(bytes, cursor + 32);
        entry.localOffset = readU32(bytes, cursor + 42);

        const qsizetype nameStart = cursor + 46;
        if (nameStart + nameLength > bytes.size()) return false;
        entry.name = bytes.mid(nameStart, nameLength);
        entries->push_back(entry);

        cursor = nameStart + nameLength + entry.extraLength + entry.commentLength;
    }
    return true;
}

bool readWholeFile(const QString& path, QByteArray* out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    *out = file.readAll();
    return true;
}

} // namespace

// 诊断包 zip 打包器的结构回归。
// 断言不靠 Qt 的 zip 模块（项目里没有），而是按 PKWARE APPNOTE 逐字节解析产物：
// 签名、条目数、CRC、大小、local header 偏移、数据区都必须对得上，否则用户拿到的
// 包在 7-Zip / 资源管理器里就是「压缩包损坏」。
int main()
{
    using kc::tools::ZipWriter;

    // ── CRC-32 公开测试向量 ────────────────────────────────────────────────
    //   空串 = 0x00000000，check 值 "123456789" = 0xCBF43926，
    //   "The quick brown fox jumps over the lazy dog" = 0x414FA339。
    check(ZipWriter::crc32(QByteArray()) == 0x00000000u,
          "CRC-32 of empty input is 0x00000000");
    check(ZipWriter::crc32(QByteArray("123456789")) == 0xCBF43926u,
          "CRC-32 of \"123456789\" matches the published check value");
    check(ZipWriter::crc32(QByteArray("The quick brown fox jumps over the lazy dog")) == 0x414FA339u,
          "CRC-32 of the pangram matches the published test vector");
    // 单字节也要对：空表/错表最容易在「长度 1」上暴露（"a" = 0xE8B7BE43）
    check(ZipWriter::crc32(QByteArray("a")) == 0xE8B7BE43u,
          "CRC-32 of a single byte matches the published test vector");
    // 二进制数据（含 0x00 / 0xFF）同样要算对，日志里出现非 ASCII 字节不能崩
    check(ZipWriter::crc32(QByteArray(1, '\0')) == 0xD202EF8Du,
          "CRC-32 of a single zero byte matches the published test vector");
    const QByteArray binary = QByteArray::fromHex("00017F80FF");
    const QByteArray binaryTweaked = QByteArray::fromHex("00017F80FE");
    check(binary.size() == 5, "binary test payload is five bytes");
    check(ZipWriter::crc32(binary) != ZipWriter::crc32(binaryTweaked),
          "CRC-32 distinguishes payloads differing in the last byte");

    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory created");
    if (!dir.isValid()) return 1;
    const QDir workDir(dir.path());

    // ── 含 2 个文件的包：写出来再按字节解析回去 ───────────────────────────
    const QByteArray logData =
        "2026-10-29 18:31:53 [INFO] session start\n"
        "2026-10-29 18:31:54 [WARN] fastboot device not found\n";
    const QByteArray infoData = "OS: Windows 11 23H2\nQt: 6.8.1\n";
    const QByteArray logName("logs/session-20261029.log");
    const QByteArray infoName("system-info.txt");

    const QString zipPath = workDir.filePath(QStringLiteral("diagnostics.zip"));
    {
        ZipWriter writer;
        writer.addFile(QStringLiteral("logs/session-20261029.log"), logData);
        writer.addFile(QStringLiteral("system-info.txt"), infoData);
        check(writer.fileCount() == 2, "two files registered");

        QString error;
        const bool written = writer.writeTo(zipPath, &error);
        check(written, "zip with two files written");
        if (!written) std::cerr << "  writeTo error: " << error.toStdString() << '\n';

        // totalBytes() 必须与实际落盘字节数一致（两者由同一套长度公式推出来）
        QFile writtenFile(zipPath);
        check(writtenFile.open(QIODevice::ReadOnly), "written zip opened for size check");
        const qint64 actualSize = writtenFile.size();
        writtenFile.close();
        check(writer.totalBytes() == actualSize,
              "totalBytes() equals the size of the file on disk");
    }

    QByteArray zipBytes;
    check(readWholeFile(zipPath, &zipBytes), "zip read back from disk");

    const qsizetype eocd = findEndOfCentralDirectory(zipBytes);
    check(eocd >= 0, "EOCD signature 0x06054b50 found");
    if (eocd >= 0) {
        check(readU16(zipBytes, eocd + 4) == 0, "EOCD disk number is 0");
        check(readU16(zipBytes, eocd + 6) == 0, "EOCD central directory start disk is 0");
        check(readU16(zipBytes, eocd + 8) == 2, "EOCD entries on this disk is 2");
        check(readU16(zipBytes, eocd + 10) == 2, "EOCD total entry count is 2");
        check(readU16(zipBytes, eocd + 20) == 0, "EOCD archive comment length is 0");
        check(static_cast<qint64>(eocd) + 22 == zipBytes.size(),
              "EOCD is the last record in the file");
    }

    QVector<CentralEntry> entries;
    check(parseCentralDirectory(zipBytes, eocd, &entries),
          "central directory parses as a well-formed ZIP record chain");
    check(entries.size() == 2, "central directory holds two entries");
    if (entries.size() == 2) {
        // 顺序应与 addFile 一致（诊断包里的条目顺序对用户是稳定的）
        const CentralEntry& first = entries.at(0);
        const CentralEntry& second = entries.at(1);
        check(first.name == logName, "first central entry name matches");
        check(second.name == infoName, "second central entry name matches");
        check(first.crc == ZipWriter::crc32(logData), "first entry CRC matches its payload");
        check(second.crc == ZipWriter::crc32(infoData), "second entry CRC matches its payload");
        check(first.compressedSize == static_cast<quint32>(logData.size()),
              "first entry compressed size equals payload size (stored)");
        check(first.uncompressedSize == static_cast<quint32>(logData.size()),
              "first entry uncompressed size equals payload size");
        check(second.compressedSize == static_cast<quint32>(infoData.size()),
              "second entry compressed size equals payload size");
        check(first.method == 0 && second.method == 0, "both entries use method 0 (stored)");
        check((first.flags & 0x0800) != 0 && (second.flags & 0x0800) != 0,
              "UTF-8 name flag (bit 11) set on both central entries");
        check(first.versionMadeBy == 0x0014, "version made by is 0x0014 (MS-DOS 2.0)");
        check(first.versionNeeded == 20, "version needed is 20");
        check(first.dosTime == 0 && first.dosDate == 0x0021,
              "fixed DOS timestamp 1980-01-01 00:00 for reproducible output");
        check(first.extraLength == 0 && first.commentLength == 0,
              "no extra field and no comment in central entry");

        // local header 偏移必须真的指向 0x04034b50，且数据区与输入一致
        const auto verifyLocal = [&zipBytes](const CentralEntry& entry, const QByteArray& expected,
                                             const char* label) {
            const qsizetype local = entry.localOffset;
            check(local + 30 <= zipBytes.size(), label);
            if (local + 30 > zipBytes.size()) return;
            check(readU32(zipBytes, local) == 0x04034b50u, "local header signature at offset");
            check(readU16(zipBytes, local + 4) == 20, "local header version needed is 20");
            check((readU16(zipBytes, local + 6) & 0x0800) != 0,
                  "local header UTF-8 name flag set");
            check(readU16(zipBytes, local + 8) == 0, "local header method is 0 (stored)");
            check(readU16(zipBytes, local + 10) == 0 && readU16(zipBytes, local + 12) == 0x0021,
                  "local header fixed DOS timestamp");
            check(readU32(zipBytes, local + 14) == entry.crc, "local header CRC matches");
            check(readU32(zipBytes, local + 18) == entry.compressedSize,
                  "local header compressed size matches");
            check(readU32(zipBytes, local + 22) == entry.uncompressedSize,
                  "local header uncompressed size matches");
            const quint16 nameLength = readU16(zipBytes, local + 26);
            const quint16 extraLength = readU16(zipBytes, local + 28);
            check(static_cast<qsizetype>(nameLength) == entry.name.size(),
                  "local header name length matches");
            check(extraLength == 0, "local header extra length is 0");
            const qsizetype nameStart = local + 30;
            check(zipBytes.mid(nameStart, nameLength) == entry.name,
                  "local header name bytes match central directory");
            const qsizetype dataStart = nameStart + nameLength + extraLength;
            check(zipBytes.mid(dataStart, entry.compressedSize) == expected,
                  "file data follows the local header byte for byte");
        };
        verifyLocal(first, logData, "first local header fits in the file");
        verifyLocal(second, infoData, "second local header fits in the file");

        // 两个本地头不能互相重叠：第一个条目的数据结束处必须小于第二个条目的起点
        const qsizetype firstEnd = entries.at(0).localOffset + 30 + entries.at(0).name.size() +
                                   entries.at(0).compressedSize;
        check(firstEnd <= static_cast<qsizetype>(entries.at(1).localOffset),
              "local entries do not overlap");
    }

    // ── 空包（0 个文件）也要是合法 zip ─────────────────────────────────────
    {
        const QString emptyPath = workDir.filePath(QStringLiteral("empty.zip"));
        ZipWriter writer;
        check(writer.fileCount() == 0, "empty writer reports zero files");
        check(writer.totalBytes() == 22, "empty archive is exactly the 22-byte EOCD");
        QString error;
        check(writer.writeTo(emptyPath, &error), "empty archive written");
        QByteArray emptyBytes;
        check(readWholeFile(emptyPath, &emptyBytes), "empty archive read back");
        check(emptyBytes.size() == 22, "empty archive is 22 bytes on disk");
        const qsizetype emptyEocd = findEndOfCentralDirectory(emptyBytes);
        check(emptyEocd == 0, "empty archive starts with the EOCD");
        if (emptyEocd == 0) {
            check(readU16(emptyBytes, 8) == 0 && readU16(emptyBytes, 10) == 0,
                  "empty archive EOCD reports zero entries");
            check(readU32(emptyBytes, 12) == 0 && readU32(emptyBytes, 16) == 0,
                  "empty archive EOCD has zero central directory size and offset");
        }
    }

    // ── 确定性：同样输入写两次，字节必须完全一致 ───────────────────────────
    {
        const QString pathA = workDir.filePath(QStringLiteral("det-a.zip"));
        const QString pathB = workDir.filePath(QStringLiteral("det-b.zip"));
        const auto buildArchive = [](const QString& path) {
            ZipWriter writer;
            writer.addFile(QStringLiteral("logs/a.log"), QByteArray("alpha\n"));
            writer.addFile(QStringLiteral("logs/b.log"), QByteArray("beta\n"));
            writer.addFile(QStringLiteral("system-info.txt"), QByteArray("info\n"));
            return writer.writeTo(path);
        };
        check(buildArchive(pathA), "determinism archive A written");
        check(buildArchive(pathB), "determinism archive B written");
        QByteArray bytesA;
        QByteArray bytesB;
        check(readWholeFile(pathA, &bytesA) && readWholeFile(pathB, &bytesB),
              "both determinism archives read back");
        check(bytesA == bytesB, "same input produces identical zip bytes");
    }

    // ── 文件名规范化：反斜杠 → `/`，中文名 UTF-8 往返 ─────────────────────
    {
        const QString path = workDir.filePath(QStringLiteral("names.zip"));
        const QByteArray payload("x");
        ZipWriter writer;
        writer.addFile(QStringLiteral("logs\\nested\\session.log"), payload);
        writer.addFile(QStringLiteral("日志/会话-20261029.log"), payload);
        writer.addFile(QStringLiteral("system-info.txt"), QByteArray());

        QString error;
        check(writer.writeTo(path, &error), "archive with odd names written");

        QByteArray bytes;
        check(readWholeFile(path, &bytes), "archive with odd names read back");
        check(writer.totalBytes() == bytes.size(),
              "totalBytes() matches the file on disk for UTF-8 names too");
        QVector<CentralEntry> named;
        check(parseCentralDirectory(bytes, findEndOfCentralDirectory(bytes), &named),
              "archive with odd names parses");
        check(named.size() == 3, "archive with odd names holds three entries");
        if (named.size() == 3) {
            check(named.at(0).name == QByteArray("logs/nested/session.log"),
                  "backslashes normalized to forward slashes");
            check(!named.at(0).name.contains('\\'),
                  "normalized name has no remaining backslash");
            check(named.at(1).name == QStringLiteral("日志/会话-20261029.log").toUtf8(),
                  "Chinese file name survives as UTF-8 bytes");
            check((named.at(1).flags & 0x0800) != 0,
                  "UTF-8 flag set for Chinese name");
            check(QString::fromUtf8(named.at(1).name).toUtf8() == named.at(1).name,
                  "Chinese name bytes are valid UTF-8 (round-trips unchanged)");
            // 0 字节文件合法：size 0、CRC 0
            check(named.at(2).compressedSize == 0 && named.at(2).uncompressedSize == 0,
                  "zero-byte file stores size 0");
            check(named.at(2).crc == 0, "zero-byte file has CRC 0");
            // 第三个（最后一个）条目紧贴中央目录：本地头 + 名字之后就是中央目录起点
            const qsizetype lastLocal = named.at(2).localOffset;
            const qsizetype lastDataStart = lastLocal + 30 + readU16(bytes, lastLocal + 26);
            const qsizetype eocdOffset = findEndOfCentralDirectory(bytes);
            const quint32 centralOffset = readU32(bytes, eocdOffset + 16);
            check(lastDataStart == static_cast<qsizetype>(centralOffset),
                  "zero-byte entry ends exactly where the central directory begins");
        }
    }

    // ── 重名：后加的覆盖前面的（诊断包里同名条目只该有一份）───────────────
    {
        const QString path = workDir.filePath(QStringLiteral("duplicate.zip"));
        ZipWriter writer;
        writer.addFile(QStringLiteral("logs/a.log"), QByteArray("first\n"));
        writer.addFile(QStringLiteral("logs/a.log"), QByteArray("second\n"));
        check(writer.fileCount() == 1, "duplicate name keeps a single entry");

        QString error;
        check(writer.writeTo(path, &error), "duplicate-name archive written");
        QByteArray bytes;
        check(readWholeFile(path, &bytes), "duplicate-name archive read back");
        QVector<CentralEntry> dup;
        check(parseCentralDirectory(bytes, findEndOfCentralDirectory(bytes), &dup),
              "duplicate-name archive parses");
        check(dup.size() == 1 && dup.at(0).crc == ZipWriter::crc32(QByteArray("second\n")),
              "later duplicate replaces the earlier payload");
    }

    // ── 失败路径：写不进去必须返回 false 并给中文错误，不能静默成功 ────────
    {
        ZipWriter writer;
        writer.addFile(QStringLiteral("logs/a.log"), QByteArray("x\n"));
        QString error;
        const QString badPath = workDir.filePath(QStringLiteral("no-such-dir/nested/out.zip"));
        check(!writer.writeTo(badPath, &error), "write into a missing directory fails");
        check(!error.isEmpty(), "failure fills the error message");
        // 成功时不应残留上一次的错误
        QString cleanError = QStringLiteral("stale");
        check(writer.writeTo(workDir.filePath(QStringLiteral("ok.zip")), &cleanError),
              "write to a valid path succeeds");
        check(cleanError.isEmpty(), "success clears the error string");
    }

    if (failures == 0) {
        std::cout << "zipwriter-tests: all checks passed\n";
        return 0;
    }
    std::cerr << "zipwriter-tests: " << failures << " check(s) failed\n";
    return 1;
}
