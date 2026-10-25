#include "oplus/OpsExtractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QVector>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace swt::edl {
namespace {

using ProgressFn = std::function<void(int, const QString&)>;
using LogFn = std::function<void(const QString&)>;
using CancelFn = std::function<bool()>;

constexpr qint64 kCopyBuffer = 2 * 1024 * 1024;
constexpr qint64 kMaxSettingsLength = 64LL * 1024 * 1024;

/// 滚动密钥初值（128 位常量，三套 MBox 共用）。
constexpr quint32 kInitialKey[4] = { 0x9EE3B5D1, 0x9D04EA5E, 0xABD51D67, 0xAFCBAFD2 };

// 邮箱常量表：前 16 字节是常量，0x3C 处是轮数（0x0A → 8 轮），其余为 0。
constexpr quint8 kMbox5[62] = {
    0x60, 0x8A, 0x3F, 0x2D, 0x68, 0x6B, 0xD4, 0x23, 0x51, 0x0C, 0xD0, 0x95, 0xBB, 0x40, 0xE9, 0x76,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0A, 0,
};

constexpr quint8 kMbox6[62] = {
    0xAA, 0x69, 0x82, 0x9E, 0x5D, 0xDE, 0xB1, 0x3D, 0x30, 0xBB, 0x81, 0xA3, 0x46, 0x65, 0xA3, 0xE1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0A, 0,
};

constexpr quint8 kMbox4[62] = {
    0xC4, 0x5D, 0x05, 0x71, 0x99, 0xDD, 0xBB, 0xEE, 0x29, 0xA1, 0x6D, 0xC7, 0xAD, 0xBF, 0xA4, 0x3F,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0A, 0,
};

// AES S 盒（与 oplus/AesCfb.cpp 同一张表；这里只为展开下面的 T 表）。
constexpr quint8 kAesSBox[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

quint8 xtime(quint8 value)
{
    return static_cast<quint8>((value << 1) ^ ((value & 0x80) ? 0x1B : 0x00));
}

/// 每个 S 盒值展开成 8 字节：2s, s, s, 3s, 2s, s, s, 3s。
const quint8* sboxTable()
{
    static const QByteArray table = [] {
        QByteArray data(256 * 8, '\0');
        for (int index = 0; index < 256; ++index) {
            const quint8 value = kAesSBox[index];
            const quint8 doubled = xtime(value);
            const quint8 tripled = static_cast<quint8>(doubled ^ value);
            char* entry = data.data() + index * 8;
            entry[0] = static_cast<char>(doubled);
            entry[1] = static_cast<char>(value);
            entry[2] = static_cast<char>(value);
            entry[3] = static_cast<char>(tripled);
            entry[4] = static_cast<char>(doubled);
            entry[5] = static_cast<char>(value);
            entry[6] = static_cast<char>(value);
            entry[7] = static_cast<char>(tripled);
        }
        return data;
    }();
    return reinterpret_cast<const quint8*>(table.constData());
}

quint32 getSBox(int offset)
{
    return qFromLittleEndian<quint32>(sboxTable() + offset);
}

/// 密钥扩展：结构上等同「AES S 盒 + 列混合常量」的一轮类 AES 变换。
void keyUpdate(const quint32 iv[4], const quint8* constants, quint32 out[4])
{
    quint32 d = iv[0] ^ constants[0];
    quint32 a = iv[1] ^ constants[1];
    quint32 b = iv[2] ^ constants[2];
    quint32 c = iv[3] ^ constants[3];

    quint32 e = getSBox(static_cast<int>(((b >> 16) & 0xFF) * 8 + 2))
                ^ getSBox(static_cast<int>(((a >> 8) & 0xFF) * 8 + 3))
                ^ getSBox(static_cast<int>((c >> 24) * 8 + 1))
                ^ getSBox(static_cast<int>((d & 0xFF) * 8))
                ^ constants[4];
    quint32 h = getSBox(static_cast<int>(((c >> 16) & 0xFF) * 8 + 2))
                ^ getSBox(static_cast<int>(((b >> 8) & 0xFF) * 8 + 3))
                ^ getSBox(static_cast<int>((d >> 24) * 8 + 1))
                ^ getSBox(static_cast<int>((a & 0xFF) * 8))
                ^ constants[5];
    quint32 i = getSBox(static_cast<int>(((d >> 16) & 0xFF) * 8 + 2))
                ^ getSBox(static_cast<int>(((c >> 8) & 0xFF) * 8 + 3))
                ^ getSBox(static_cast<int>((a >> 24) * 8 + 1))
                ^ getSBox(static_cast<int>((b & 0xFF) * 8))
                ^ constants[6];
    a = getSBox(static_cast<int>(((d >> 8) & 0xFF) * 8 + 3))
        ^ getSBox(static_cast<int>(((a >> 16) & 0xFF) * 8 + 2))
        ^ getSBox(static_cast<int>((b >> 24) * 8 + 1))
        ^ getSBox(static_cast<int>((c & 0xFF) * 8))
        ^ constants[7];

    int g = 8;
    const int rounds = constants[0x3C] - 2;
    for (int round = 0; round < rounds; ++round) {
        const quint32 oldE = e;
        const quint32 oldH = h;
        const quint32 oldI = i;
        const quint32 oldA = a;
        const quint32 highE = oldE >> 24;
        const quint32 middleH = oldH >> 16;
        const quint32 highH = oldH >> 24;
        const quint32 middleE = oldE >> 16;
        const quint32 highI = oldI >> 24;
        const quint32 byteE = oldE >> 8;

        e = getSBox(static_cast<int>(((oldI >> 16) & 0xFF) * 8 + 2))
            ^ getSBox(static_cast<int>(((oldH >> 8) & 0xFF) * 8 + 3))
            ^ getSBox(static_cast<int>((oldA >> 24) * 8 + 1))
            ^ getSBox(static_cast<int>((oldE & 0xFF) * 8))
            ^ constants[g];
        h = getSBox(static_cast<int>(((oldA >> 16) & 0xFF) * 8 + 2))
            ^ getSBox(static_cast<int>(((oldI >> 8) & 0xFF) * 8 + 3))
            ^ getSBox(static_cast<int>(highE * 8 + 1))
            ^ getSBox(static_cast<int>((oldH & 0xFF) * 8))
            ^ constants[g + 1];
        i = getSBox(static_cast<int>((middleE & 0xFF) * 8 + 2))
            ^ getSBox(static_cast<int>(((oldA >> 8) & 0xFF) * 8 + 3))
            ^ getSBox(static_cast<int>(highH * 8 + 1))
            ^ getSBox(static_cast<int>((oldI & 0xFF) * 8))
            ^ constants[g + 2];
        a = getSBox(static_cast<int>((byteE & 0xFF) * 8 + 3))
            ^ getSBox(static_cast<int>((middleH & 0xFF) * 8 + 2))
            ^ getSBox(static_cast<int>(highI * 8 + 1))
            ^ getSBox(static_cast<int>((oldA & 0xFF) * 8))
            ^ constants[g + 3];
        g += 4;
    }

    out[0] = (getSBox(static_cast<int>(((i >> 16) & 0xFF) * 8)) & 0x00FF0000u)
             ^ (getSBox(static_cast<int>(((h >> 8) & 0xFF) * 8 + 1)) & 0x0000FF00u)
             ^ (getSBox(static_cast<int>((a >> 24) * 8 + 3)) & 0xFF000000u)
             ^ (getSBox(static_cast<int>((e & 0xFF) * 8 + 2)) & 0x000000FFu)
             ^ constants[g];
    out[1] = (getSBox(static_cast<int>(((a >> 16) & 0xFF) * 8)) & 0x00FF0000u)
             ^ (getSBox(static_cast<int>(((i >> 8) & 0xFF) * 8 + 1)) & 0x0000FF00u)
             ^ (getSBox(static_cast<int>((e >> 24) * 8 + 3)) & 0xFF000000u)
             ^ (getSBox(static_cast<int>((h & 0xFF) * 8 + 2)) & 0x000000FFu)
             ^ constants[g + 3];
    out[2] = (getSBox(static_cast<int>(((e >> 16) & 0xFF) * 8)) & 0x00FF0000u)
             ^ (getSBox(static_cast<int>(((a >> 8) & 0xFF) * 8 + 1)) & 0x0000FF00u)
             ^ (getSBox(static_cast<int>((h >> 24) * 8 + 3)) & 0xFF000000u)
             ^ (getSBox(static_cast<int>((i & 0xFF) * 8 + 2)) & 0x000000FFu)
             ^ constants[g + 2];
    out[3] = (getSBox(static_cast<int>(((h >> 16) & 0xFF) * 8)) & 0x00FF0000u)
             ^ (getSBox(static_cast<int>(((e >> 8) & 0xFF) * 8 + 1)) & 0x0000FF00u)
             ^ (getSBox(static_cast<int>((i >> 24) * 8 + 3)) & 0xFF000000u)
             ^ (getSBox(static_cast<int>((a & 0xFF) * 8 + 2)) & 0x000000FFu)
             ^ constants[g + 1];
}

// ── 文件 / 路径 ────────────────────────────────────────────────────────────

bool readAt(QFile& file, qint64 offset, qint64 length, QByteArray* out, QString* error)
{
    if (!out) return false;
    if (offset < 0 || length < 0 || offset + length > file.size()) {
        if (error) *error = QStringLiteral("固件表里的镜像范围超出了文件大小。");
        return false;
    }
    if (!file.seek(offset)) {
        if (error) *error = QStringLiteral("固件定位失败: %1").arg(file.errorString());
        return false;
    }
    *out = file.read(length);
    if (out->size() != length) {
        if (error) *error = QStringLiteral("读取固件时文件意外结束。");
        return false;
    }
    return true;
}

bool resolveOutputPath(const QString& root, const QString& relative, QString* out, QString* error)
{
    QString normalized = relative;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (normalized.startsWith(QLatin1Char('/'))) normalized.remove(0, 1);
    if (normalized.isEmpty()) {
        if (error) *error = QStringLiteral("固件表里出现了空的输出文件名。");
        return false;
    }

    const QString base = QDir::cleanPath(QDir(root).absolutePath());
    const QString destination = QDir::cleanPath(base + QLatin1Char('/') + normalized);
    if (!destination.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("固件表里有不安全的输出路径: %1").arg(relative);
        return false;
    }
    if (out) *out = destination;
    return true;
}

bool ensureParentDirectory(const QString& filePath, QString* error)
{
    const QDir parent = QFileInfo(filePath).absoluteDir();
    if (parent.exists()) return true;
    if (QDir().mkpath(parent.absolutePath())) return true;
    if (error) *error = QStringLiteral("无法创建输出目录: %1").arg(parent.absolutePath());
    return false;
}

bool copyRange(QFile& input, QFile& output, qint64 length, const QString& what, QString* error,
               const CancelFn& cancel)
{
    QByteArray buffer(static_cast<qsizetype>(std::min<qint64>(kCopyBuffer, std::max<qint64>(length, 1))),
                      '\0');
    while (length > 0) {
        if (cancel && cancel()) {
            if (error) *error = QStringLiteral("已取消: %1").arg(what);
            return false;
        }
        const qint64 requested = std::min<qint64>(buffer.size(), length);
        const qint64 read = input.read(buffer.data(), requested);
        if (read <= 0) {
            if (error) *error = QStringLiteral("读取镜像数据时文件意外结束: %1").arg(what);
            return false;
        }
        if (output.write(buffer.constData(), read) != read) {
            if (error) *error = QStringLiteral("写入失败: %1").arg(output.errorString());
            return false;
        }
        length -= read;
    }
    return true;
}

// ── settings.xml 解析 ──────────────────────────────────────────────────────

struct OpsEntry {
    QString fileName;
    qint64 start = 0;
    qint64 length = 0;
    bool encrypted = false;
};

QString attributeOf(const QXmlStreamAttributes& attributes, const QString& name)
{
    for (const QXmlStreamAttribute& attribute : attributes) {
        if (attribute.name().toString().compare(name, Qt::CaseInsensitive) == 0) {
            return attribute.value().toString();
        }
    }
    return {};
}

QString fileNameOf(const QXmlStreamAttributes& attributes)
{
    QString name = attributeOf(attributes, QStringLiteral("Path"));
    if (name.trimmed().isEmpty()) name = attributeOf(attributes, QStringLiteral("filename"));
    return name.trimmed().isEmpty() ? QString() : name;
}

void addEntry(QVector<OpsEntry>* entries, const QXmlStreamAttributes& attributes, bool encrypted)
{
    const QString fileName = fileNameOf(attributes);
    if (fileName.isEmpty()) return;

    qint64 sectors = 0;
    qint64 length = 0;
    const QString offsetText = attributeOf(attributes, QStringLiteral("FileOffsetInSrc"));
    const QString lengthText = attributeOf(attributes, QStringLiteral("SizeInByteInSrc"));
    bool ok = false;
    sectors = offsetText.trimmed().toLongLong(&ok);
    if (!ok) return;
    length = lengthText.trimmed().toLongLong(&ok);
    if (!ok || length <= 0) return;

    OpsEntry entry;
    entry.fileName = fileName;
    entry.start = sectors * OpsExtractor::SectorSize;
    entry.length = length;
    entry.encrypted = encrypted;
    entries->append(entry);
}

QVector<OpsEntry> parseEntries(const QString& xml, QString* error)
{
    QVector<OpsEntry> raw;
    QXmlStreamReader reader(xml);
    QStringList stack;

    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = reader.name().toString();
            const int depth = stack.size();   // 祖先个数
            if (depth == 1) {
                // 当前元素是根的直接子节点 = 段名。段名含 "Program" 的走
                // descendantsAndSelf 口径：段自身带 Path/filename 也算一条。
                if (name.contains(QStringLiteral("Program"), Qt::CaseInsensitive)
                    && !fileNameOf(reader.attributes()).isEmpty()) {
                    addEntry(&raw, reader.attributes(), false);
                }
            } else if (depth >= 2) {
                const QString section = stack.at(1);   // 段名（根的直接子节点）
                const bool isSahara =
                    section.compare(QStringLiteral("SAHARA"), Qt::CaseInsensitive) == 0;
                const bool isProvision =
                    section.compare(QStringLiteral("UFS_PROVISION"), Qt::CaseInsensitive) == 0;
                const bool isProgram =
                    section.contains(QStringLiteral("Program"), Qt::CaseInsensitive);
                if (isSahara) {
                    if (name.compare(QStringLiteral("File"), Qt::CaseInsensitive) == 0) {
                        addEntry(&raw, reader.attributes(), true);
                    }
                } else if (isProvision) {
                    if (name.compare(QStringLiteral("File"), Qt::CaseInsensitive) == 0) {
                        addEntry(&raw, reader.attributes(), false);
                    }
                } else if (isProgram) {
                    if (!fileNameOf(reader.attributes()).isEmpty()) {
                        addEntry(&raw, reader.attributes(), false);
                    }
                }
            }
            stack.append(name);
        } else if (token == QXmlStreamReader::EndElement) {
            if (!stack.isEmpty()) stack.removeLast();
        } else if (token == QXmlStreamReader::Invalid) {
            if (error) *error = QStringLiteral("settings.xml 解析失败: %1").arg(reader.errorString());
            return {};
        }
    }
    if (reader.hasError()) {
        if (error) *error = QStringLiteral("settings.xml 解析失败: %1").arg(reader.errorString());
        return {};
    }

    // 去重：同一 (文件名, 起点, 长度) 只保留第一条。
    QVector<OpsEntry> unique;
    QSet<QString> seen;
    for (const OpsEntry& entry : raw) {
        const QString key = QStringLiteral("%1|%2|%3").arg(entry.fileName).arg(entry.start).arg(
            entry.length);
        if (seen.contains(key)) continue;
        seen.insert(key);
        unique.append(entry);
    }
    return unique;
}

bool tryReadSettings(QFile& input, const quint8* mbox, QString* xml)
{
    if (input.size() < OpsExtractor::SectorSize * 2) return false;

    QByteArray footer;
    QString ignored;
    if (!readAt(input, input.size() - OpsExtractor::SectorSize, OpsExtractor::SectorSize, &footer,
                &ignored)) {
        return false;
    }

    const qint64 xmlLength = qFromLittleEndian<quint32>(footer.constData() + 0x18);
    if (xmlLength < 32 || xmlLength > kMaxSettingsLength) return false;

    const QByteArray mboxBytes(reinterpret_cast<const char*>(mbox), 62);

    auto tryAt = [&](qint64 padding) {
        const qint64 encryptedLength = xmlLength + padding;
        const qint64 offset = input.size() - OpsExtractor::SectorSize - encryptedLength;
        if (offset < 0) return false;

        QByteArray encrypted;
        if (!readAt(input, offset, encryptedLength, &encrypted, &ignored)) return false;
        const QByteArray decrypted = OpsExtractor::decryptCustom(encrypted, mboxBytes);
        if (decrypted.size() < xmlLength) return false;

        const QByteArray xmlBytes = decrypted.left(xmlLength);
        if (!xmlBytes.contains(QByteArray("xml "))) return false;

        QString candidate = QString::fromUtf8(xmlBytes);
        if (candidate.startsWith(QChar(0xFEFF))) candidate.remove(0, 1);
        while (candidate.endsWith(QChar('\0'))) candidate.chop(1);

        QXmlStreamReader validator(candidate);
        while (!validator.atEnd()) validator.readNext();
        if (validator.hasError()) return false;

        *xml = candidate;
        return true;
    };

    // 主流口径：明文补齐到 512 的整数倍，恰好整扇区时再多补一整个扇区。
    const qint64 aligned = OpsExtractor::SectorSize - (xmlLength % OpsExtractor::SectorSize);
    if (tryAt(aligned)) return true;

    // 兼容口径：恰好整扇区时不再多补。只在前一种失败后重试。
    const qint64 tight = aligned % OpsExtractor::SectorSize;
    return tight != aligned && tryAt(tight);
}

} // namespace

QByteArray OpsExtractor::mboxConstants(int version)
{
    const quint8* data = nullptr;
    switch (version) {
    case 4: data = kMbox4; break;
    case 5: data = kMbox5; break;
    case 6: data = kMbox6; break;
    default: return {};
    }
    return QByteArray(reinterpret_cast<const char*>(data), 62);
}

QByteArray OpsExtractor::decryptCustom(const QByteArray& cipher, const QByteArray& mbox)
{
    if (cipher.isEmpty()) return {};
    if (mbox.size() < 0x3D) return {};

    QByteArray out(cipher.size(), '\0');
    quint32 rolling[4] = { kInitialKey[0], kInitialKey[1], kInitialKey[2], kInitialKey[3] };
    const quint8* constants = reinterpret_cast<const quint8*>(mbox.constData());
    const bool useMbox = cipher.size() > 15;

    qsizetype offset = 0;
    while (offset < cipher.size()) {
        const int blockLength = static_cast<int>(std::min<qsizetype>(16, cipher.size() - offset));
        quint8 block[16] = { 0 };
        std::memcpy(block, cipher.constData() + offset, static_cast<size_t>(blockLength));

        quint32 expanded[4];
        keyUpdate(rolling, useMbox ? constants : sboxTable(), expanded);

        quint8 plain[16] = { 0 };
        for (int word = 0; word < 4; ++word) {
            const quint32 encryptedWord = qFromLittleEndian<quint32>(block + word * 4);
            const quint32 decryptedWord = expanded[word] ^ encryptedWord;
            qToLittleEndian<quint32>(decryptedWord, plain + word * 4);
            rolling[word] = encryptedWord;   // 密文反馈
        }
        std::memcpy(out.data() + offset, plain, static_cast<size_t>(blockLength));
        offset += blockLength;
    }
    return out;
}

QByteArray OpsExtractor::encryptCustom(const QByteArray& plain, const QByteArray& mbox)
{
    if (plain.isEmpty()) return {};
    if (mbox.size() < 0x3D) return {};

    QByteArray out(plain.size(), '\0');
    quint32 rolling[4] = { kInitialKey[0], kInitialKey[1], kInitialKey[2], kInitialKey[3] };
    const quint8* constants = reinterpret_cast<const quint8*>(mbox.constData());
    const bool useMbox = plain.size() > 15;

    qsizetype offset = 0;
    while (offset < plain.size()) {
        const int blockLength = static_cast<int>(std::min<qsizetype>(16, plain.size() - offset));
        quint8 block[16] = { 0 };
        std::memcpy(block, plain.constData() + offset, static_cast<size_t>(blockLength));

        quint32 expanded[4];
        keyUpdate(rolling, useMbox ? constants : sboxTable(), expanded);

        quint8 cipherBlock[16] = { 0 };
        for (int word = 0; word < 4; ++word) {
            const quint32 plainWord = qFromLittleEndian<quint32>(block + word * 4);
            const quint32 encryptedWord = expanded[word] ^ plainWord;
            qToLittleEndian<quint32>(encryptedWord, cipherBlock + word * 4);
            rolling[word] = encryptedWord;   // 反馈的仍是密文
        }
        std::memcpy(out.data() + offset, cipherBlock, static_cast<size_t>(blockLength));
        offset += blockLength;
    }
    return out;
}

bool OpsExtractor::extract(const QString& sourcePath, const QString& outputDirectory,
                           OpsExtractResult* result, QString* error, const ProgressFn& progress,
                           const LogFn& log, const CancelFn& cancel)
{
    if (result) *result = OpsExtractResult{};
    if (error) error->clear();

    if (sourcePath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未指定 OPS 固件路径。");
        return false;
    }
    if (outputDirectory.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未指定输出目录。");
        return false;
    }
    if (!QFileInfo::exists(sourcePath)) {
        if (error) *error = QStringLiteral("OPS 固件不存在: %1").arg(sourcePath);
        return false;
    }
    if (!QDir().mkpath(outputDirectory)) {
        if (error) *error = QStringLiteral("无法创建输出目录: %1").arg(outputDirectory);
        return false;
    }

    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 OPS: %1").arg(input.errorString());
        return false;
    }
    if (input.size() < SectorSize * 2) {
        if (error) *error = QStringLiteral("文件太小, 不是有效的 OPS 固件。");
        return false;
    }

    struct MboxCandidate {
        const quint8* data;
        int version;
    };
    const MboxCandidate candidates[] = {
        { kMbox5, 5 },
        { kMbox6, 6 },
        { kMbox4, 4 },
    };

    QString settingsXml;
    int mboxVersion = 0;
    for (const MboxCandidate& candidate : candidates) {
        if (cancel && cancel()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        if (tryReadSettings(input, candidate.data, &settingsXml)) {
            mboxVersion = candidate.version;
            break;
        }
    }
    if (mboxVersion == 0) {
        if (error) {
            *error = QStringLiteral(
                "无法识别 OPS 的邮箱常量: 只支持旧版 MBox 4 / 5 / 6 格式的固件。");
        }
        return false;
    }

    if (progress) {
        progress(0, QStringLiteral("已识别 MBox %1 格式并解密 settings.xml").arg(mboxVersion));
    }

    QString settingsPath;
    if (!resolveOutputPath(outputDirectory, QStringLiteral("settings.xml"), &settingsPath, error)) {
        return false;
    }
    if (!ensureParentDirectory(settingsPath, error)) return false;
    {
        QFile settings(settingsPath);
        if (!settings.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法写入 settings.xml: %1").arg(settings.errorString());
            return false;
        }
        settings.write(settingsXml.toUtf8());
        settings.close();
    }
    if (result) result->settingsPath = settingsPath;
    if (log) log(QStringLiteral("[OPS] settings.xml 已输出: %1").arg(settingsPath));

    const QVector<OpsEntry> entries = parseEntries(settingsXml, error);
    if (entries.isEmpty()) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("settings.xml 中没有可提取的镜像条目。");
        }
        return false;
    }

    // 先整体校验范围，免得写到一半才失败。
    qint64 totalBytes = 0;
    for (const OpsEntry& entry : entries) {
        if (entry.start < 0 || entry.length < 0 || entry.start > input.size()
            || entry.length > input.size() - entry.start) {
            if (error) {
                *error = QStringLiteral("固件表里的镜像范围超出了文件大小: %1").arg(entry.fileName);
            }
            return false;
        }
        totalBytes += entry.length;
    }
    if (log) {
        log(QStringLiteral("[OPS] 镜像表解析完成: %1 个镜像, 合计 %2 字节")
                .arg(entries.size())
                .arg(totalBytes));
    }

    const QByteArray mboxBytes(reinterpret_cast<const char*>(
                                   mboxVersion == 5 ? kMbox5 : (mboxVersion == 6 ? kMbox6 : kMbox4)),
                               62);

    qint64 doneBytes = 0;
    for (int index = 0; index < entries.size(); ++index) {
        if (cancel && cancel()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        const OpsEntry& entry = entries.at(index);

        QString destination;
        if (!resolveOutputPath(outputDirectory, entry.fileName, &destination, error)) return false;
        if (!ensureParentDirectory(destination, error)) return false;

        if (progress) {
            progress(totalBytes > 0 ? static_cast<int>(doneBytes * 100 / totalBytes) : 0,
                     QStringLiteral("正在解包 %1 (%2 字节%3)")
                         .arg(entry.fileName)
                         .arg(entry.length)
                         .arg(entry.encrypted ? QStringLiteral(", 已加密") : QString()));
        }

        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法创建输出文件 %1: %2").arg(destination,
                                                                              output.errorString());
            return false;
        }
        if (!input.seek(entry.start)) {
            if (error) *error = QStringLiteral("固件定位失败。");
            return false;
        }

        if (entry.encrypted) {
            QByteArray encrypted;
            if (!readAt(input, entry.start, entry.length, &encrypted, error)) return false;
            const QByteArray decrypted = decryptCustom(encrypted, mboxBytes);
            if (output.write(decrypted) != decrypted.size()) {
                if (error) *error = QStringLiteral("写入失败: %1").arg(output.errorString());
                return false;
            }
        } else {
            if (!copyRange(input, output, entry.length, entry.fileName, error, cancel)) return false;
        }
        output.close();

        doneBytes += entry.length;
        if (result) {
            result->fileCount += 1;
            result->files << entry.fileName;
        }
        if (progress) {
            progress(totalBytes > 0 ? static_cast<int>(doneBytes * 100 / totalBytes) : 100,
                     QStringLiteral("已解包 %1").arg(entry.fileName));
        }
    }

    if (result) {
        result->mboxVersion = mboxVersion;
        result->totalBytes = totalBytes;
    }
    if (progress) progress(100, QStringLiteral("全部镜像已解包"));
    return true;
}

} // namespace swt::edl
