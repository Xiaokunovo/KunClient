#include "kc/tools/FirmwareChecksum.h"

#include <QStringList>

namespace kc::tools {
namespace {

/// 十六进制字符判定（QChar 没有现成的 isHexDigit）。
bool isHexDigit(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

} // namespace

QString formatChecksumLine(const QString& hash, const QString& name)
{
    QString cleanName = name;
    cleanName.remove(QLatin1Char('\r'));
    cleanName.remove(QLatin1Char('\n'));
    return hash.toLower() + QStringLiteral("  ") + cleanName;
}

QVector<ChecksumRecord> parseChecksumText(const QByteArray& text)
{
    QVector<ChecksumRecord> records;
    const QString content = QString::fromUtf8(text);
    const QStringList lines = content.split(QLatin1Char('\n'));
    records.reserve(lines.size());

    for (const QString& rawLine : lines) {
        // Windows 换行：先剥 \r，再剥首尾空白（有的工具会在行尾留空格）。
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

        // 头部：一串十六进制（大小写都收），后面紧跟两个空格或「空格 + *」。
        int i = 0;
        const int n = line.size();
        while (i < n && isHexDigit(line.at(i))) ++i;
        const int hashLen = i;
        // 只认 32（MD5）/ 40（SHA-1，顺带兼容）/ 64（SHA-256）三种长度。
        if (hashLen != 32 && hashLen != 40 && hashLen != 64) continue;
        if (i >= n || !line.at(i).isSpace()) continue;

        // 跳过分隔空白；coreutils 二进制模式在文件名前多一个 '*'。
        while (i < n && line.at(i).isSpace()) ++i;
        if (i < n && line.at(i) == QLatin1Char('*')) ++i;

        const QString name = line.mid(i).trimmed();
        if (name.isEmpty()) continue;

        records.push_back({ line.left(hashLen).toLower(), name });
    }
    return records;
}

QString checksumFileName(const QString& targetName, kc::crypto::HashAlgo algo)
{
    const QString suffix = QStringLiteral(".") + kc::crypto::hashAlgoName(algo);
    if (targetName.isEmpty()) {
        // 目录批量：沿用 coreutils 的约定名，一个算法一个名字。
        return algo == kc::crypto::HashAlgo::Md5 ? QStringLiteral("MD5SUMS")
                                                 : QStringLiteral("SHA256SUMS");
    }
    return targetName + suffix;
}

QString normalizeHash(const QString& raw, kc::crypto::HashAlgo algo)
{
    QString value = raw;
    value.remove(QLatin1Char(' '));
    value.remove(QLatin1Char('\t'));
    value.remove(QLatin1Char('\r'));
    value.remove(QLatin1Char('\n'));
    value = value.toLower();
    if (value.size() != kc::crypto::hashAlgoLength(algo) * 2) return {};
    for (const QChar ch : value) {
        if (!isHexDigit(ch)) return {};
    }
    return value;
}

} // namespace kc::tools
