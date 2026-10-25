#include "kc/auth/SignCodec.h"

#include <QRegularExpression>

namespace kc::auth {

QString cleanHex(QString text)
{
    text.remove(QLatin1Char(' '));
    text.remove(QLatin1Char(':'));
    text.remove(QLatin1Char('-'));
    text.remove(QLatin1Char('\t'));
    text.remove(QLatin1Char('\r'));
    text.remove(QLatin1Char('\n'));
    while (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    return text;
}

QByteArray tryParseBytes(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return {};

    // 先试 hex：偶数长度且全为十六进制字符才算。
    const QString hex = cleanHex(trimmed);
    if (hex.size() % 2 == 0 && hex.size() > 0) {
        static const QRegularExpression hexRe(QStringLiteral("^[0-9a-fA-F]+$"));
        if (QRegularExpressionMatch m = hexRe.match(hex); m.hasMatch()) {
            QByteArray bytes;
            bytes.reserve(hex.size() / 2);
            for (int i = 0; i + 1 < hex.size(); i += 2) {
                bool ok = false;
                const int v = hex.mid(i, 2).toInt(&ok, 16);
                if (!ok) { bytes.clear(); break; }
                bytes.append(static_cast<char>(v));
            }
            if (!bytes.isEmpty()) return bytes;
        }
    }

    // 再试 Base64。
    const QByteArray b64 = QByteArray::fromBase64(trimmed.toLatin1());
    if (!b64.isEmpty() && b64.size() >= 8) return b64;
    return {};
}

} // namespace kc::auth
