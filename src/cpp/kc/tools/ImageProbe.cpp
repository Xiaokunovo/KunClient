#include "kc/tools/ImageProbe.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace kc::tools {

namespace {

/// ext4 的超级块魔数在偏移 0x438，占 2 字节，小端 0x53EF。
constexpr qint64 kExt4MagicOffset = 0x438;
/// 读文件头多少字节够用：ext4 的魔数在 0x438，往后留一点余量。
constexpr qint64 kProbeBytes = 0x600;   // 1536

/// 全 0 判定：只检查读到的这段（不是整个文件）——
/// 大文件全 0 通常是预分配的空镜像，头 1.5KB 全 0 就足够说明问题。
bool allZero(const QByteArray& data)
{
    for (char c : data) {
        if (c != 0) return false;
    }
    return !data.isEmpty();
}

QString hexHead(const QByteArray& data, int n)
{
    QStringList parts;
    for (int i = 0; i < qMin(n, data.size()); ++i) {
        parts << QStringLiteral("%1").arg(static_cast<quint8>(data.at(i)), 2, 16, QLatin1Char('0')).toUpper();
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace

QString imageKindName(ImageKind kind)
{
    switch (kind) {
    case ImageKind::AndroidSparse: return QStringLiteral("Android sparse 镜像");
    case ImageKind::AndroidBoot:   return QStringLiteral("Android boot 镜像");
    case ImageKind::Ext4:          return QStringLiteral("ext4 文件系统");
    case ImageKind::AllZero:       return QStringLiteral("全 0（擦除标记）");
    case ImageKind::Plain:         return QStringLiteral("普通数据");
    case ImageKind::Unknown:
    default:                       return QStringLiteral("未识别");
    }
}

ImageProbeResult probeImage(const QString& path)
{
    ImageProbeResult r;

    if (path.trimmed().isEmpty()) {
        r.error = QStringLiteral("路径为空");
        return r;
    }

    QFileInfo info(path);
    if (!info.exists()) {
        r.error = QStringLiteral("文件不存在");
        return r;
    }
    if (!info.isFile()) {
        r.error = QStringLiteral("不是文件（可能是目录）");
        return r;
    }

    r.sizeBytes = info.size();
    if (r.sizeBytes == 0) {
        // 0 字节不是「读失败」，是一个**有效结论**：空镜像。
        r.kind = ImageKind::AllZero;
        r.kindName = imageKindName(r.kind);
        r.evidence = QStringLiteral("文件大小为 0");
        r.ok = true;
        return r;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("打不开文件：%1").arg(f.errorString());
        return r;
    }
    // 🔴 只读头部 —— 系统镜像动辄几 GB，不能为了判断格式整块读。
    const QByteArray head = f.read(kProbeBytes);
    f.close();

    if (head.isEmpty()) {
        r.error = QStringLiteral("读不到文件内容");
        return r;
    }
    r.ok = true;

    // ── ① Android sparse：前 4 字节 0x3AFF26ED（小端存为 ED 26 FF 3A）──
    if (head.size() >= 4) {
        const auto b0 = static_cast<quint8>(head.at(0));
        const auto b1 = static_cast<quint8>(head.at(1));
        const auto b2 = static_cast<quint8>(head.at(2));
        const auto b3 = static_cast<quint8>(head.at(3));
        if (b0 == 0x3A && b1 == 0xFF && b2 == 0x26 && b3 == 0xED) {
            r.kind = ImageKind::AndroidSparse;
            r.kindName = imageKindName(r.kind);
            r.evidence = QStringLiteral("文件头 %1").arg(hexHead(head, 4));
            return r;
        }
        // 另一种写法（大端序的同一魔数），有些工具会产出
        if (b0 == 0xED && b1 == 0x26 && b2 == 0xFF && b3 == 0x3A) {
            r.kind = ImageKind::AndroidSparse;
            r.kindName = imageKindName(r.kind);
            r.evidence = QStringLiteral("文件头 %1（大端序）").arg(hexHead(head, 4));
            return r;
        }
    }

    // ── ② Android boot：前 8 字节 "ANDROID!" ─────────────────────────────
    if (head.size() >= 8 && head.left(8) == QByteArray("ANDROID!")) {
        r.kind = ImageKind::AndroidBoot;
        r.kindName = imageKindName(r.kind);
        r.evidence = QStringLiteral("文件头是 ANDROID! 标识");
        return r;
    }

    // ── ③ ext4：偏移 0x438 处 2 字节小端 0xEF53 ──────────────────────────
    if (head.size() >= kExt4MagicOffset + 2) {
        const auto lo = static_cast<quint8>(head.at(kExt4MagicOffset));
        const auto hi = static_cast<quint8>(head.at(kExt4MagicOffset + 1));
        if (lo == 0x53 && hi == 0xEF) {
            r.kind = ImageKind::Ext4;
            r.kindName = imageKindName(r.kind);
            r.evidence = QStringLiteral("偏移 0x438 处是 ext4 魔数 53 EF");
            return r;
        }
    }

    // ── ④ 全 0 ────────────────────────────────────────────────────────────
    if (allZero(head)) {
        r.kind = ImageKind::AllZero;
        r.kindName = imageKindName(r.kind);
        r.evidence = QStringLiteral("头部 %1 字节全为 0").arg(head.size());
        return r;
    }

    // ── ⑤ 普通数据 ────────────────────────────────────────────────────────
    r.kind = ImageKind::Plain;
    r.kindName = imageKindName(r.kind);
    r.evidence = QStringLiteral("无可识别魔数，按原始数据对待");
    return r;
}

QString suggestPartitionName(ImageKind kind, const QString& fileName)
{
    const QString base = QFileInfo(fileName).completeBaseName().toLower();

    // 文件名里的关键词优先（用户自己命名通常最准）。
    // 顺序有讲究：先匹配更具体的（init_boot 要先于 boot），否则 boot 会把
    // init_boot / vendor_boot 全吃掉。
    static const struct { const char* key; const char* part; } kMap[] = {
        {"init_boot", "init_boot"},
        {"vendor_boot", "vendor_boot"},
        {"vbmeta_system", "vbmeta_system"},
        {"vbmeta", "vbmeta"},
        {"recovery", "recovery"},
        {"boot", "boot"},
        {"dtbo", "dtbo"},
        {"super", "super"},
        {"system", "system"},
        {"vendor", "vendor"},
        {"product", "product"},
        {"odm", "odm"},
        {"persist", "persist"},
        {"modem", "modem"},
        {"misc", "misc"},
        {"userdata", "userdata"},
    };
    for (const auto& m : kMap) {
        if (base.contains(QString::fromLatin1(m.key))) {
            return QString::fromLatin1(m.part);
        }
    }

    // 文件名猜不出时，按类型给一个**很保守**的提示：
    // boot image 大概率是 boot 分区，ext4 大概率是 system 类分区 —— 但都不确定，
    // 所以宁可返回空（界面就不显示建议），也不乱猜。
    Q_UNUSED(kind);
    return QString();
}

} // namespace kc::tools
