#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PreFlashBackup —— 刷机前自动备份（GPT + 关键分区）
//
//  为什么要有这个：这工具最怕的不是「功能少」，是**刷坏了救不回来**。
//  分区表（GPT）一旦写坏，设备会变成「连不上、也认不出分区」的砖 ——
//  而 GPT 本身只有几十 KB，备份它几乎零成本。
//
//  原来的备份是**手动步骤**（EDL 页「备份分区」），而手动步骤恰恰是最容易忘的
//  那一步。这里把它变成「开始写入前自动做一次」，忘不了。
//
//  ── 备份什么 ────────────────────────────────────────────────────────────
//    ① GPT 本身（主 + 备份两份，各 34 扇区）—— 最关键的几十 KB
//    ② 关键分区（按白名单）：boot / init_boot / vendor_boot / dtbo / recovery /
//       vbmeta / misc / persist / modem / efs 之类
//       · 只备「小分区」：超过 sizeCapBytes 的跳过（system / super 动辄几 GB，
//         备了也没地方放，而且那些丢了重刷即可，不像 persist / modem 有唯一数据）
//    ③ manifest.json：设备信息 + 每个文件的大小 / sha256 / 时间
//
//  ── 为什么是「失败即阻止」───────────────────────────────────────────────
//    备份失败还继续刷，等于「以为上了保险、其实没有」。宁可让用户先解决
//    备份失败的原因（磁盘满 / 权限 / 设备读不出），也不要让他带着虚假的安全感刷。
//    （与「快捷指令失败即中止」同一个哲学。）
//
//  🔴 这一层是**纯逻辑**：不碰 UI、不碰设备。真正的读分区由调用方注入回调。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace kc::diag {

/// 一个待备份项。
struct BackupTarget {
    QString name;      ///< 分区名（GPT 用固定名 "__gpt__" 之类，见 kGptTargetName）
    qint64 sizeBytes = 0;
    bool isGpt = false;   ///< true = GPT 本体（主+备），读法与其他分区不同
};

/// 备份计划。
struct BackupPlan {
    QVector<BackupTarget> targets;   ///< 按读取顺序
    QStringList skipped;             ///< 跳过的分区名 + 原因（给人看）
    qint64 totalBytes = 0;           ///< 预计总大小
};

/// 备份结果（单个文件）。
struct BackupFileResult {
    QString name;
    QString path;
    qint64 sizeBytes = 0;
    QString sha256;
    bool ok = false;
    QString error;
};

/// 整次备份的结果。
struct BackupResult {
    bool ok = false;
    QString directory;                    ///< 落盘目录（ok 时非空）
    QVector<BackupFileResult> files;
    QStringList skipped;
    QString error;                        ///< ok=false 时的原因
    qint64 elapsedMs = 0;
    int succeededCount = 0;
    int failedCount = 0;
};

/// GPT 在备份目录里的固定文件名（不带扩展名）。
extern const char* const kGptTargetName;

/// 默认的关键分区白名单。
/// 口径：**有唯一数据、且体积小**的分区。system / super / userdata 不在此列 ——
/// 那些丢了重刷就行，而且动辄几 GB。
QStringList defaultKeyPartitions();

/// 单个分区的大小上限（默认 256 MiB）。
/// 超过就不备：既装不下，也不是「丢了就救不回来」的那类。
constexpr qint64 kDefaultSizeCapBytes = 256LL * 1024 * 1024;

/// 制定备份计划。纯函数，不看磁盘也不看设备。
///   partitions : 当前设备的分区表（name + sizeBytes）
///   sizeCap    : 单分区上限；<=0 用默认值
///   includeGpt : 是否包含 GPT 本体（默认 true，几乎总该是 true）
BackupPlan buildBackupPlan(const QVector<QPair<QString, qint64>>& partitions,
                           qint64 sizeCap = kDefaultSizeCapBytes,
                           bool includeGpt = true);

/// 备份根目录：%LOCALAPPDATA%\KunClientState\preflash-backup
QString backupRootDir();

/// 为一次备份生成目录名并创建：<时间戳>_<设备标识>
/// 设备标识会被清洗（去掉路径分隔符 / 空格 / 非法字符）。
/// 创建失败返回空串。
QString createBackupDir(const QString& deviceTag, QString* error = nullptr);

/// 写 manifest.json。
///   result     : 已填好的文件清单
///   deviceInfo : 设备摘要（型号 / 序列号 / 链路），原样写进 manifest
bool writeManifest(const QString& dir, const BackupResult& result,
                   const QString& deviceInfo, QString* error = nullptr);

/// 只保留最近 N 次备份（按目录名的时间戳前缀排序），更早的删掉。
/// 返回删掉的目录数。磁盘不是无限的 —— 每次备份可能几十 MB。
int pruneOldBackups(int keepCount = 10);

} // namespace kc::diag
