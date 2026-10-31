#include "kc/tools/FlashReplay.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace kc::tools {
namespace {

// 设备摘要的分隔符：MainWindow 用 " · " 拼「端口 · 机型 · 序列号」。
constexpr char kDeviceSeparator[] = " · ";

/// 端口号形状（COM7 / COM12）：只有端口、没有序列号的摘要无法确认是不是同一台。
bool looksLikePort(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^COM\\d+$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(text.trimmed()).hasMatch();
}

/// 需要「来源包」是**输入文件**的操作：包不存在就没法重放。
bool needsInputPackage(const QString& operation)
{
    return operation == QStringLiteral("写入") || operation == QStringLiteral("XML 刷机");
}

/// 需要「来源包」是**输出目录**的操作：目录不存在是正常的（执行时创建）。
bool needsOutputDirectory(const QString& operation)
{
    return operation == QStringLiteral("读取") || operation == QStringLiteral("回读");
}

/// 记录里的 package 到底是路径还是「给人看的摘要」。
/// 判断依据：绝对路径、或含目录分隔符 → 当路径；否则（"boot.img 等 5 个镜像" /
/// "123 MB"）当摘要。摘要不能拿去当路径用，否则会拼出个不存在的文件去刷。
bool isPathLike(const QString& package)
{
    if (package.isEmpty()) return false;
    const QFileInfo info(package);
    if (info.isAbsolute()) return true;
    return package.contains(QLatin1Char('/')) || package.contains(QLatin1Char('\\'));
}

/// 分区摘要里的「共 N 个」数字。
constexpr int kUnknownTotal = -1;

} // namespace

bool isReplayableOperation(const QString& operation)
{
    // 只放行四类「刷完就完、参数简单」的操作。GPT 写入会改分区表、整盘擦除与
    // 恢复出厂会清整机、基带备份要指定目录 —— 这些误触代价太大，重放入口不给。
    return operation == QStringLiteral("写入") || operation == QStringLiteral("XML 刷机")
           || operation == QStringLiteral("擦除") || operation == QStringLiteral("读取")
           || operation == QStringLiteral("回读");
}

QString deviceIdentity(const QString& deviceSummary)
{
    QStringList parts = deviceSummary.split(QString::fromUtf8(kDeviceSeparator),
                                            Qt::SkipEmptyParts);
    for (QString& part : parts) part = part.trimmed();
    parts.removeAll(QString());

    if (parts.isEmpty()) return QString();
    // 两段以上：最后一段就是 identity（MainWindow 的拼法是「端口 · 机型 · identity」）。
    if (parts.size() >= 2) return parts.constLast();
    // 只有一段：是序列号还是端口说不准。端口形状的一律不认（端口会随插拔变），
    // 其余按 identity 用 —— 有些机型拿不到 model，摘要就只剩 "0x1234abcd"。
    if (looksLikePort(parts.constFirst())) return QString();
    return parts.constFirst();
}

bool isSameDevice(const QString& summaryA, const QString& summaryB)
{
    const QString a = deviceIdentity(summaryA);
    const QString b = deviceIdentity(summaryB);
    // 任一方拿不到 identity 就无法确认 → 当不一致。这里**不能**退回比端口号：
    // 批量场景下 COM 口会重排，端口相等不代表是同一台机器。
    if (a.isEmpty() || b.isEmpty()) return false;
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

QStringList parsePartitionSummary(const QString& summary, int* totalCount)
{
    if (totalCount) *totalCount = kUnknownTotal;

    const QString text = summary.trimmed();
    if (text.isEmpty()) return {};

    // ① 「a, b, c 等 12 个」：只存了前几个名字，12 是总数。
    static const QRegularExpression withTotal(
        QStringLiteral("^(.*?)\\s*等\\s*(\\d+)\\s*个$"));
    QString head = text;
    const QRegularExpressionMatch totalMatch = withTotal.match(text);
    if (totalMatch.hasMatch()) {
        head = totalMatch.captured(1).trimmed();
        if (totalCount) *totalCount = totalMatch.captured(2).toInt();
    }

    // ② 「69 个分区」/「12 个」：只有数量，没有名字。
    static const QRegularExpression countOnly(QStringLiteral("^(\\d+)\\s*个(分区)?$"));
    const QRegularExpressionMatch countMatch = countOnly.match(head);
    if (countMatch.hasMatch()) {
        if (totalCount) *totalCount = countMatch.captured(1).toInt();
        return {};
    }

    // ③ XML 刷机的分区摘要写的是「N 个 program 条目」——那不是分区名，别硬拆。
    if (head.endsWith(QStringLiteral("条目"))) return {};

    // ④ 普通枚举：按 ", " 切（记录里就是这个格式）。
    QStringList names;
    const QStringList raw = head.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& item : raw) {
        const QString name = item.trimmed();
        if (!name.isEmpty()) names.push_back(name);
    }
    if (totalCount && *totalCount == kUnknownTotal) *totalCount = names.size();
    return names;
}

ReplayPlan buildReplayPlan(const kc::diag::FlashRecord& record, const QString& currentDevice)
{
    ReplayPlan plan;
    plan.operation = record.operation;
    plan.packagePath = record.package;

    // ── ① 操作类型：不在白名单里的直接拒绝，并说清楚去哪儿做 ──────────────
    if (record.operation.isEmpty()) {
        plan.reason = QStringLiteral("这条记录没有操作类型，无法判断要重放什么。");
        return plan;
    }
    if (!isReplayableOperation(record.operation)) {
        plan.reason = QStringLiteral(
                          "「%1」不支持一键重放：这类操作会改动分区表或清空整机，"
                          "误触代价太大。请到对应页面手动操作（那里有完整确认）。")
                          .arg(record.operation);
        return plan;
    }

    // ── ② 设备：必须与记录里的设备是同一台 ────────────────────────────────
    if (currentDevice.trimmed().isEmpty()) {
        plan.reason = QStringLiteral("当前没有连接设备。重放需要先连上记录里的那台设备。");
        return plan;
    }
    if (!isSameDevice(record.device, currentDevice)) {
        plan.reason = QStringLiteral(
                          "设备不一致，已取消重放。\n"
                          "记录里的设备：%1\n"
                          "当前连接的设备：%2\n"
                          "为避免把包刷进错误的机器，这里不做任何自动换机处理。")
                          .arg(record.device.isEmpty() ? QStringLiteral("（记录里没有设备信息）")
                                                       : record.device,
                               currentDevice);
        return plan;
    }

    // 设备对上了，但端口可能变了（COM7 → COM9），提醒一句免得用户以为搞错了。
    if (record.device != currentDevice) {
        plan.notes.push_back(QStringLiteral("设备序列号一致，端口/摘要与记录略有不同：%1 → %2")
                                 .arg(record.device, currentDevice));
    } else {
        plan.notes.push_back(QStringLiteral("设备与记录一致：%1").arg(currentDevice));
    }

    // ── ③ 来源包：输入包必须存在；输出目录不存在是正常的 ─────────────────
    if (needsInputPackage(record.operation)) {
        if (record.package.isEmpty()) {
            plan.reason = QStringLiteral(
                              "记录里没有来源包，无法重放。请在刷机页重新选择要写入的包。");
            return plan;
        }
        if (!isPathLike(record.package)) {
            // 历史里只存了「boot.img 等 5 个镜像」这种摘要，它不是可用路径。
            // 硬拿去当路径会拼出一个不存在的文件，所以这里如实拒绝。
            plan.reason = QStringLiteral(
                              "记录里的来源包只是摘要（%1），不是可用的文件路径，"
                              "无法自动重放。请重新选择来源包。")
                              .arg(record.package);
            return plan;
        }
        const QFileInfo info(record.package);
        if (!info.exists()) {
            plan.reason = QStringLiteral("来源包已不存在：%1\n它可能被移动、改名或删除了。")
                              .arg(record.package);
            return plan;
        }
        plan.notes.push_back(QStringLiteral("来源包已确认存在：%1").arg(record.package));
    } else if (needsOutputDirectory(record.operation)) {
        if (record.package.isEmpty()) {
            plan.reason = QStringLiteral(
                              "记录里没有回读输出目录，无法重放。请重新指定输出目录。");
            return plan;
        }
        if (!isPathLike(record.package)) {
            plan.reason = QStringLiteral("记录里的输出位置只是摘要（%1），不是可用路径，"
                                         "请重新指定输出目录。")
                              .arg(record.package);
            return plan;
        }
        if (!QFileInfo(record.package).exists()) {
            // 输出目录本来就可以不存在 —— 执行时创建，不拦。
            plan.notes.push_back(
                QStringLiteral("输出目录尚不存在，执行时会创建：%1").arg(record.package));
        } else {
            plan.notes.push_back(QStringLiteral("输出目录：%1").arg(record.package));
        }
    }

    // ── ④ 分区：能从摘要里解析出来就带上，解析不出就说清楚 ─────────────────
    int total = kUnknownTotal;
    plan.partitions = parsePartitionSummary(record.partitions, &total);
    if (!plan.partitions.isEmpty()) {
        if (total > plan.partitions.size()) {
            plan.notes.push_back(
                QStringLiteral("记录只保存了前 %1 个分区名（共 %2 个），执行前请核对分区列表。")
                    .arg(plan.partitions.size())
                    .arg(total));
        } else {
            plan.notes.push_back(
                QStringLiteral("将重放 %1 个分区：%2")
                    .arg(plan.partitions.size())
                    .arg(plan.partitions.join(QStringLiteral(", "))));
        }
    } else if (!record.partitions.isEmpty()) {
        plan.notes.push_back(QStringLiteral("记录里的分区信息是「%1」，没有具体分区名；"
                                            "执行时会按来源包的全部内容处理。")
                                 .arg(record.partitions));
    } else {
        plan.notes.push_back(QStringLiteral("记录里没有分区信息，执行时会按来源包的全部内容处理。"));
    }

    // ── ⑤ 上一次是失败的：允许重放，但提醒一下这不是「成功现场」 ───────────
    if (!record.success) {
        plan.notes.push_back(
            QStringLiteral("注意：这条记录上一次是**失败**的。重放会从头再试一遍。"));
    }

    plan.ok = true;
    return plan;
}

QString describeReplay(const ReplayPlan& plan)
{
    if (!plan.ok) {
        return QStringLiteral("无法重放这次操作。\n\n%1")
            .arg(plan.reason.isEmpty() ? QStringLiteral("（没有给出原因）") : plan.reason);
    }

    QStringList lines;
    lines.push_back(QStringLiteral("将重放这次操作："));
    lines.push_back(QStringLiteral("  操作：%1").arg(plan.operation));
    if (!plan.packagePath.isEmpty()) {
        lines.push_back(QStringLiteral("  来源：%1").arg(plan.packagePath));
    }
    if (!plan.partitions.isEmpty()) {
        lines.push_back(
            QStringLiteral("  分区：%1").arg(plan.partitions.join(QStringLiteral(", "))));
    }
    if (!plan.notes.isEmpty()) {
        lines.push_back(QString());
        lines.push_back(QStringLiteral("提醒："));
        for (const QString& note : plan.notes) {
            lines.push_back(QStringLiteral("  · %1").arg(note));
        }
    }
    lines.push_back(QString());
    lines.push_back(QStringLiteral("重放会覆盖设备上的数据，请确认后再继续。"));
    return lines.join(QLatin1Char('\n'));
}

} // namespace kc::tools
