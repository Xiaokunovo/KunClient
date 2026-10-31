#include "kc/diag/PreFlashBackup.h"

#include "kc/config/AppStatePaths.h"
#include "kc/security/Sha256.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace kc::diag {

const char* const kGptTargetName = "__gpt__";

namespace {

/// 写一个文件并算 sha256（分块读，别把几十 MB 整块塞进内存）。
bool writeAndHash(const QString& path, const QByteArray& data, QString* sha256, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法写入 %1").arg(QFileInfo(path).fileName());
        return false;
    }
    if (f.write(data) != data.size()) {
        if (error) *error = QStringLiteral("写入不完整：%1").arg(QFileInfo(path).fileName());
        f.close();
        return false;
    }
    f.close();
    if (sha256) *sha256 = QString::fromLatin1(kc::crypto::sha256(data).toHex());
    return true;
}

} // namespace

QStringList defaultKeyPartitions()
{
    // 口径：有唯一数据（丢了无法重建）且体积小。
    //   · persist / modem / efs / nvram：射频校准 / IMEI 之类，刷机不会重写它们，
    //     但一旦被误擦就永久丢失，必须能还原。
    //   · boot / init_boot / vendor_boot / dtbo / recovery / vbmeta：刷坏了开不了机，
    //     但通常能重刷回来 —— 备一份是为了「快速回到刷之前那个状态」。
    //   · system / super / userdata / vendor：**故意不备**。动辄几 GB，
    //     而且丢了重刷即可，属于「有包就能修」的一类。
    return {
        QStringLiteral("persist"),
        QStringLiteral("modem"),
        QStringLiteral("modemst1"),
        QStringLiteral("modemst2"),
        QStringLiteral("efs"),
        QStringLiteral("nvram"),
        QStringLiteral("nvdata"),
        QStringLiteral("boot"),
        QStringLiteral("init_boot"),
        QStringLiteral("vendor_boot"),
        QStringLiteral("dtbo"),
        QStringLiteral("recovery"),
        QStringLiteral("vbmeta"),
        QStringLiteral("vbmeta_system"),
        QStringLiteral("misc"),
    };
}

BackupPlan buildBackupPlan(const QVector<QPair<QString, qint64>>& partitions,
                           qint64 sizeCap, bool includeGpt)
{
    BackupPlan plan;
    if (sizeCap <= 0) sizeCap = kDefaultSizeCapBytes;

    // GPT 永远排第一：它是「其余一切的解释器」，必须先落地。
    // 大小按规范固定值算（主 34 扇区 + 备 34 扇区，扇区 512 → 34816 字节），
    // 实际读取由调用方按这个量要。
    if (includeGpt) {
        BackupTarget gpt;
        gpt.name = QString::fromLatin1(kGptTargetName);
        gpt.sizeBytes = 34 * 512 * 2;
        gpt.isGpt = true;
        plan.targets.append(gpt);
        plan.totalBytes += gpt.sizeBytes;
    }

    const QStringList keys = defaultKeyPartitions();
    for (const auto& p : partitions) {
        const QString name = p.first.trimmed();
        if (name.isEmpty()) continue;

        // 只备白名单里的。
        // 🔴 白名单外的**大分区**要记一条跳过原因：用户看到「system 没被备份」
        //    会怀疑是漏了，而不是「有意不备」。小分区（几十个几百 KB 的杂项）
        //    不记 —— 那会把 skipped 刷成一屏噪音，反而盖掉真正重要的信息。
        if (!keys.contains(name, Qt::CaseInsensitive)) {
            constexpr qint64 kExplainThreshold = 512LL * 1024 * 1024;   // 512 MiB
            if (p.second >= kExplainThreshold) {
                plan.skipped.append(
                    QStringLiteral("%1（%2 GB，非关键分区，丢了重刷即可）")
                        .arg(name)
                        .arg(p.second / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1));
            }
            continue;
        }

        const qint64 size = p.second;
        if (size <= 0) {
            plan.skipped.append(QStringLiteral("%1（大小未知）").arg(name));
            continue;
        }
        if (size > sizeCap) {
            plan.skipped.append(
                QStringLiteral("%1（%2 MB，超过 %3 MB 上限）")
                    .arg(name)
                    .arg(size / (1024.0 * 1024.0), 0, 'f', 1)
                    .arg(sizeCap / (1024.0 * 1024.0), 0, 'f', 0));
            continue;
        }

        BackupTarget t;
        t.name = name;
        t.sizeBytes = size;
        plan.targets.append(t);
        plan.totalBytes += size;
    }
    return plan;
}

QString backupRootDir()
{
    return kc::AppStatePaths::Combine(QStringLiteral("preflash-backup"));
}

QString createBackupDir(const QString& deviceTag, QString* error)
{
    const QString root = backupRootDir();
    if (!QDir().mkpath(root)) {
        if (error) *error = QStringLiteral("无法创建备份根目录：%1").arg(root);
        return QString();
    }

    // 设备标识清洗：路径分隔符 / 空格 / 冒号之类会让目录名不可用。
    QString tag = deviceTag.trimmed();
    tag.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\s]+")), QStringLiteral("_"));
    if (tag.isEmpty()) tag = QStringLiteral("unknown");
    if (tag.size() > 48) tag = tag.left(48);

    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString dir = QDir(root).filePath(QStringLiteral("%1_%2").arg(stamp, tag));
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("无法创建备份目录：%1").arg(dir);
        return QString();
    }
    return dir;
}

bool writeManifest(const QString& dir, const BackupResult& result,
                   const QString& deviceInfo, QString* error)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("createdAt"),
                QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("device"), deviceInfo);
    root.insert(QStringLiteral("ok"), result.ok);
    root.insert(QStringLiteral("elapsedMs"), static_cast<double>(result.elapsedMs));
    root.insert(QStringLiteral("succeeded"), result.succeededCount);
    root.insert(QStringLiteral("failed"), result.failedCount);

    QJsonArray files;
    for (const BackupFileResult& f : result.files) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), f.name);
        o.insert(QStringLiteral("file"), QFileInfo(f.path).fileName());
        o.insert(QStringLiteral("sizeBytes"), static_cast<double>(f.sizeBytes));
        o.insert(QStringLiteral("sha256"), f.sha256);
        o.insert(QStringLiteral("ok"), f.ok);
        if (!f.error.isEmpty()) o.insert(QStringLiteral("error"), f.error);
        files.append(o);
    }
    root.insert(QStringLiteral("files"), files);

    QJsonArray skipped;
    for (const QString& s : result.skipped) skipped.append(s);
    root.insert(QStringLiteral("skipped"), skipped);

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QString ignored;
    return writeAndHash(QDir(dir).filePath(QStringLiteral("manifest.json")), json,
                        &ignored, error);
}

int pruneOldBackups(int keepCount)
{
    if (keepCount < 0) keepCount = 0;
    const QString root = backupRootDir();
    QDir dir(root);
    if (!dir.exists()) return 0;

    // 目录名以 yyyyMMdd-HHmmss 开头 —— 字典序即时间序，不用解析时间。
    const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const int removeCount = names.size() - keepCount;
    if (removeCount <= 0) return 0;

    int removed = 0;
    for (int i = 0; i < removeCount; ++i) {
        QDir sub(dir.filePath(names.at(i)));
        if (sub.removeRecursively()) ++removed;
    }
    return removed;
}

} // namespace kc::diag
