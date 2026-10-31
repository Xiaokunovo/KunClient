#include "kc/tools/PreflightChecks.h"

namespace kc::tools {

namespace {

QString humanBytes(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("-");
    const double kb = 1024.0;
    if (bytes < kb) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < kb * kb) return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 1);
    if (bytes < kb * kb * kb) return QStringLiteral("%1 MB").arg(bytes / (kb * kb), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (kb * kb * kb), 0, 'f', 2);
}

} // namespace

QString checkLevelName(CheckLevel level)
{
    switch (level) {
    case CheckLevel::Info:    return QStringLiteral("正常");
    case CheckLevel::Warning: return QStringLiteral("提醒");
    case CheckLevel::Error:   return QStringLiteral("必须处理");
    default:                  return QStringLiteral("未知");
    }
}

CheckItem checkBattery(int percent, bool charging)
{
    CheckItem item;
    item.title = QStringLiteral("电量");

    // 读不到电量不是错误 —— 有些设备 / 链路就是不报电量，
    // 拿它拦刷机会让用户在没法解决的地方卡住。
    if (percent < 0) {
        item.level = CheckLevel::Info;
        item.detail = QStringLiteral("读不到电量（不影响刷机，但请确认设备电量充足）");
        return item;
    }

    if (percent < 20 && !charging) {
        item.level = CheckLevel::Error;
        item.detail = QStringLiteral("电量 %1% 且未充电 —— 刷机中途掉电可能导致变砖，"
                                     "请先充电到 20% 以上").arg(percent);
        return item;
    }
    if (percent < 40 && !charging) {
        item.level = CheckLevel::Warning;
        item.detail = QStringLiteral("电量 %1% 且未充电，建议先充一会儿再刷").arg(percent);
        return item;
    }

    item.level = CheckLevel::Info;
    item.detail = charging ? QStringLiteral("电量 %1%（充电中）").arg(percent)
                           : QStringLiteral("电量 %1%").arg(percent);
    return item;
}

CheckItem checkDiskSpace(qint64 availableBytes, qint64 requiredBytes)
{
    CheckItem item;
    item.title = QStringLiteral("磁盘空间");

    if (requiredBytes <= 0) {
        item.level = CheckLevel::Info;
        item.detail = QStringLiteral("本次操作不需要额外磁盘空间");
        return item;
    }
    if (availableBytes < 0) {
        item.level = CheckLevel::Info;
        item.detail = QStringLiteral("读不到磁盘剩余空间");
        return item;
    }

    // 1.5 倍是「够用」的底线：备份 + 临时文件 + 系统缓存都会额外占空间。
    const qint64 needError = static_cast<qint64>(requiredBytes * 1.5);
    const qint64 needWarn = requiredBytes * 2;

    if (availableBytes < needError) {
        item.level = CheckLevel::Error;
        item.detail = QStringLiteral("剩余 %1，需要约 %2（含余量）。空间不足会让写入中途失败")
                          .arg(humanBytes(availableBytes), humanBytes(needError));
        return item;
    }
    if (availableBytes < needWarn) {
        item.level = CheckLevel::Warning;
        item.detail = QStringLiteral("剩余 %1，建议留到 %2 以上")
                          .arg(humanBytes(availableBytes), humanBytes(needWarn));
        return item;
    }

    item.level = CheckLevel::Info;
    item.detail = QStringLiteral("剩余 %1，足够（需要约 %2）")
                      .arg(humanBytes(availableBytes), humanBytes(requiredBytes));
    return item;
}

CheckItem checkImageSize(qint64 imageBytes, qint64 partitionBytes,
                         const QString& partitionName)
{
    CheckItem item;
    item.title = QStringLiteral("镜像大小");

    if (imageBytes <= 0 || partitionBytes <= 0) {
        item.level = CheckLevel::Info;
        item.detail = QStringLiteral("大小信息不完整，跳过检查");
        return item;
    }

    if (imageBytes > partitionBytes) {
        item.level = CheckLevel::Error;
        item.detail = QStringLiteral("镜像 %1 比分区 %2（%3）大，写不进去")
                          .arg(humanBytes(imageBytes), partitionName,
                               humanBytes(partitionBytes));
        return item;
    }

    item.level = CheckLevel::Info;
    item.detail = QStringLiteral("镜像 %1 / 分区 %2（%3）")
                      .arg(humanBytes(imageBytes), partitionName,
                           humanBytes(partitionBytes));
    return item;
}

CheckLevel worstLevel(const QVector<CheckItem>& items)
{
    CheckLevel worst = CheckLevel::Info;
    for (const CheckItem& item : items) {
        if (item.level == CheckLevel::Error) return CheckLevel::Error;
        if (item.level == CheckLevel::Warning) worst = CheckLevel::Warning;
    }
    return worst;
}

QString summaryLine(const QVector<CheckItem>& items)
{
    int errors = 0, warnings = 0;
    for (const CheckItem& item : items) {
        if (item.level == CheckLevel::Error) ++errors;
        else if (item.level == CheckLevel::Warning) ++warnings;
    }

    if (errors > 0) {
        return QStringLiteral("%1 项必须处理，%2 项提醒").arg(errors).arg(warnings);
    }
    if (warnings > 0) {
        return QStringLiteral("全部可继续，但有 %1 项提醒").arg(warnings);
    }
    return QStringLiteral("全部正常");
}

} // namespace kc::tools
