#include "kc/tools/ZipWriter.h"

#include <QSaveFile>

#include <array>

namespace kc::tools {
namespace {

// ── ZIP 是小端格式，这里统一走两个追加函数，避免每处手写位运算出错 ──────────
void appendU16(QByteArray& out, quint16 value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
}

void appendU32(QByteArray& out, quint32 value)
{
    out.append(static_cast<char>(value & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 24) & 0xFF));
}

// ── 结构常量（PKWARE APPNOTE）────────────────────────────────────────────────
constexpr quint32 kLocalHeaderSignature = 0x04034b50;
constexpr quint32 kCentralDirSignature = 0x02014b50;
constexpr quint32 kEndOfCentralDirSignature = 0x06054b50;

constexpr quint16 kVersionNeeded = 20;          ///< 2.0：stored + UTF-8 名足够
constexpr quint16 kVersionMadeBy = 0x0014;      ///< 高字节 0 = MS-DOS，低字节 2.0
constexpr quint16 kFlagUtf8Name = 0x0800;       ///< 通用标志位 bit 11：文件名为 UTF-8
constexpr quint16 kMethodStored = 0;            ///< 只存储，不压缩
constexpr quint16 kDosTime = 0;                 ///< 00:00:00
constexpr quint16 kDosDate = 0x0021;            ///< 1980-01-01（DOS 纪元，固定值保证确定性）

constexpr qsizetype kLocalHeaderSize = 30;
constexpr qsizetype kCentralDirEntrySize = 46;
constexpr qsizetype kEndOfCentralDirSize = 22;

constexpr quint64 kMaxU32 = 0xFFFFFFFFull;
constexpr qsizetype kMaxNameBytes = 0xFFFF;

// 单个条目在本地头 + 数据区占的字节数（与写盘逻辑一一对应）。
qsizetype localEntryBytes(qsizetype nameBytes, qsizetype dataBytes)
{
    return kLocalHeaderSize + nameBytes + dataBytes;
}

// 单个条目在中央目录里占的字节数。
qsizetype centralEntryBytes(qsizetype nameBytes)
{
    return kCentralDirEntrySize + nameBytes;
}

} // namespace

void ZipWriter::addFile(const QString& name, const QByteArray& data)
{
    // 反斜杠统一成 `/`：Windows 调用方传 QDir::filePath() 时天然带 `\`，
    // 直接写进 zip 会被部分解压工具当成字面字符，解出来是一个怪文件名。
    QString normalized = name;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QByteArray utf8Name = normalized.toUtf8();
    const quint32 checksum = crc32(data);

    // 重名后加的覆盖前面的：诊断包里同名条目只该有一份，否则解压结果取决于
    // 工具实现（有的后者覆盖、有的前者优先），不可控。
    // 条目数很少（诊断包通常 < 10 个），线性查重足够，不为此引入额外索引。
    for (Entry& entry : m_entries) {
        if (entry.name == utf8Name) {
            entry.data = data;
            entry.crc = checksum;
            return;
        }
    }

    Entry entry;
    entry.name = utf8Name;
    entry.data = data;
    entry.crc = checksum;
    m_entries.push_back(entry);
}

bool ZipWriter::writeTo(const QString& path, QString* error) const
{
    if (error) error->clear();

    // ── 先做规模检查：超限就明确失败，绝不写一个截断的坏包 ────────────────
    if (m_entries.size() > 0xFFFF) {
        if (error) {
            *error = QStringLiteral("文件条目数 %1 超过 ZIP 上限 65535，无法打包")
                         .arg(m_entries.size());
        }
        return false;
    }

    quint64 total = static_cast<quint64>(kEndOfCentralDirSize);
    for (const Entry& entry : m_entries) {
        if (entry.name.size() > kMaxNameBytes) {
            if (error) {
                *error = QStringLiteral("文件名过长（超过 65535 字节），无法打包：%1")
                             .arg(QString::fromUtf8(entry.name));
            }
            return false;
        }
        if (static_cast<quint64>(entry.data.size()) > kMaxU32) {
            if (error) {
                *error = QStringLiteral("单个文件超过 4 GiB，ZIP（非 ZIP64）无法打包：%1")
                             .arg(QString::fromUtf8(entry.name));
            }
            return false;
        }
        total += static_cast<quint64>(localEntryBytes(entry.name.size(), entry.data.size()));
        total += static_cast<quint64>(centralEntryBytes(entry.name.size()));
    }
    if (total > kMaxU32) {
        if (error) {
            *error = QStringLiteral("打包后总大小 %1 字节超过 4 GiB，ZIP（非 ZIP64）无法打包")
                         .arg(total);
        }
        return false;
    }

    // ── 落盘：QSaveFile 先写临时文件，commit() 才原子替换目标 ──────────────
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("无法创建 zip 文件：%1").arg(output.errorString());
        }
        return false;
    }

    QVector<quint32> offsets;
    offsets.reserve(m_entries.size());

    quint64 offset = 0;
    for (const Entry& entry : m_entries) {
        // 前面已按总大小校验过，这里再兜一次：中央目录里 offset 是 quint32。
        if (offset > kMaxU32) {
            output.cancelWriting();
            if (error) *error = QStringLiteral("本地文件头偏移超过 4 GiB，无法打包");
            return false;
        }
        offsets.push_back(static_cast<quint32>(offset));

        const quint32 size = static_cast<quint32>(entry.data.size());
        const quint16 nameBytes = static_cast<quint16>(entry.name.size());

        QByteArray header;
        header.reserve(kLocalHeaderSize + entry.name.size());
        appendU32(header, kLocalHeaderSignature);
        appendU16(header, kVersionNeeded);
        appendU16(header, kFlagUtf8Name);
        appendU16(header, kMethodStored);
        appendU16(header, kDosTime);
        appendU16(header, kDosDate);
        appendU32(header, entry.crc);
        appendU32(header, size);   // compressed size = uncompressed size（stored）
        appendU32(header, size);
        appendU16(header, nameBytes);
        appendU16(header, 0);      // extra field 长度
        header.append(entry.name);

        if (output.write(header) != header.size() || output.write(entry.data) != entry.data.size()) {
            output.cancelWriting();
            if (error) {
                *error = QStringLiteral("写入 zip 数据失败：%1").arg(output.errorString());
            }
            return false;
        }
        offset += static_cast<quint64>(header.size()) + static_cast<quint64>(entry.data.size());
    }

    // ── 中央目录：每个条目一条记录，最后是 EOCD ───────────────────────────
    const quint64 centralDirOffset = offset;
    for (qsizetype index = 0; index < m_entries.size(); ++index) {
        const Entry& entry = m_entries.at(index);
        const quint32 size = static_cast<quint32>(entry.data.size());
        const quint16 nameBytes = static_cast<quint16>(entry.name.size());

        QByteArray record;
        record.reserve(kCentralDirEntrySize + entry.name.size());
        appendU32(record, kCentralDirSignature);
        appendU16(record, kVersionMadeBy);
        appendU16(record, kVersionNeeded);
        appendU16(record, kFlagUtf8Name);
        appendU16(record, kMethodStored);
        appendU16(record, kDosTime);
        appendU16(record, kDosDate);
        appendU32(record, entry.crc);
        appendU32(record, size);
        appendU32(record, size);
        appendU16(record, nameBytes);
        appendU16(record, 0);      // extra field 长度
        appendU16(record, 0);      // comment 长度
        appendU16(record, 0);      // 起始磁盘号
        appendU16(record, 0);      // internal attributes
        appendU32(record, 0);      // external attributes
        appendU32(record, offsets.at(index));
        record.append(entry.name);

        if (output.write(record) != record.size()) {
            output.cancelWriting();
            if (error) {
                *error = QStringLiteral("写入 zip 中央目录失败：%1").arg(output.errorString());
            }
            return false;
        }
        offset += static_cast<quint64>(record.size());
    }

    const quint64 centralDirSize = offset - centralDirOffset;
    const quint16 entryCount = static_cast<quint16>(m_entries.size());

    QByteArray endRecord;
    endRecord.reserve(kEndOfCentralDirSize);
    appendU32(endRecord, kEndOfCentralDirSignature);
    appendU16(endRecord, 0);                                       // 本磁盘号
    appendU16(endRecord, 0);                                       // 中央目录起始磁盘号
    appendU16(endRecord, entryCount);                              // 本磁盘条目数
    appendU16(endRecord, entryCount);                              // 总条目数
    appendU32(endRecord, static_cast<quint32>(centralDirSize));
    appendU32(endRecord, static_cast<quint32>(centralDirOffset));
    appendU16(endRecord, 0);                                       // comment 长度

    if (output.write(endRecord) != endRecord.size()) {
        output.cancelWriting();
        if (error) {
            *error = QStringLiteral("写入 zip 结束记录失败：%1").arg(output.errorString());
        }
        return false;
    }

    if (!output.commit()) {
        if (error) {
            *error = QStringLiteral("保存 zip 文件失败：%1").arg(output.errorString());
        }
        return false;
    }
    return true;
}

int ZipWriter::fileCount() const
{
    return static_cast<int>(m_entries.size());
}

qint64 ZipWriter::totalBytes() const
{
    qint64 total = kEndOfCentralDirSize;
    for (const Entry& entry : m_entries) {
        total += localEntryBytes(entry.name.size(), entry.data.size());
        total += centralEntryBytes(entry.name.size());
    }
    return total;
}

quint32 ZipWriter::crc32(const QByteArray& data)
{
    // 标准 IEEE 802.3 反射多项式表，首次调用时生成（C++11 起局部 static 初始化线程安全）。
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> values{};
        for (quint32 index = 0; index < 256; ++index) {
            quint32 value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            values[index] = value;
        }
        return values;
    }();

    quint32 value = 0xFFFFFFFFu;
    for (const char ch : data) {
        const quint8 byte = static_cast<quint8>(ch);
        value = table[(value ^ byte) & 0xFFu] ^ (value >> 8);
    }
    return value ^ 0xFFFFFFFFu;
}

} // namespace kc::tools
