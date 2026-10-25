#include "kc/tools/AdbGptReader.h"

#include "kc/tools/PlatformToolsManager.h"
#include "storage/GptParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>

namespace kc {
namespace {

// 远程临时文件都放这里：shell 用户可写，且不进 /sdcard（省一次 MediaStore 扫描）。
const char* kRemoteHead = "/data/local/tmp/kc_gpt_head.bin";
const char* kRemoteRestore = "/data/local/tmp/kc_gpt_restore.bin";

QString localTemp(const QString& tag)
{
    return QDir(QDir::tempPath())
        .filePath(QStringLiteral("kc_%1_%2.bin")
                      .arg(tag)
                      .arg(QCoreApplication::applicationPid()));
}

// /dev/block/sde12 → sde；/dev/block/mmcblk0p12 → mmcblk0；mmcblk0boot0 → mmcblk0。
// 只认整盘名，分区后缀一律剥掉；dm-/loop- 这类虚拟设备返回空（不能当 GPT 源）。
QString diskOf(const QString& node)
{
    static const QRegularExpression re(
        QStringLiteral("^(mmcblk\\d+|sd[a-z]+|nvme\\d+n\\d+|vd[a-z]+|xvd[a-z]+)"));
    const auto m = re.match(node.trimmed());
    return m.hasMatch() ? m.captured(1) : QString();
}

bool looksLikeRoot(const QString& text)
{
    return text.contains(QStringLiteral("uid=0"));
}

} // namespace

quint32 AdbGptReader::sniffSectorSize(const QByteArray& data)
{
    static const QByteArray signature("EFI PART");
    if (data.size() >= 512 + 8 && data.mid(512, 8) == signature) return 512;
    if (data.size() >= 4096 + 8 && data.mid(4096, 8) == signature) return 4096;
    return 0;
}

bool AdbGptReader::ensureRoot(PlatformToolsManager& tools, const LogFn& log, QString* error)
{
    auto id = tools.executeAdb(QStringLiteral("shell id"), 15000);
    if (id.success && looksLikeRoot(id.output)) return true;

    auto suId = tools.executeAdb(QStringLiteral("shell su -c id"), 15000);
    if (suId.success && looksLikeRoot(suId.output)) {
        if (log) log(QStringLiteral("[GPT] 已通过 su 获得 root"));
        return true;
    }

    // userdebug / eng 版本上 `adb root` 能把 adbd 提权（正式版会拒绝）。
    const auto root = tools.executeAdb(QStringLiteral("root"), 20000);
    const QString rootText = root.output + root.error;
    const bool rootAccepted = rootText.contains(QStringLiteral("restarting adbd as root"))
                              || rootText.contains(QStringLiteral("adbd is already running as root"));
    if (rootAccepted) {
        if (log) log(QStringLiteral("[GPT] adb root 已接受，等待 adbd 重启…"));
        tools.executeAdb(QStringLiteral("wait-for-device"), 20000);
        QThread::msleep(800);
        id = tools.executeAdb(QStringLiteral("shell id"), 15000);
        if (id.success && looksLikeRoot(id.output)) return true;
        suId = tools.executeAdb(QStringLiteral("shell su -c id"), 15000);
        if (suId.success && looksLikeRoot(suId.output)) return true;
    }

    if (error) {
        const QString reason = rootText.trimmed().isEmpty()
                                   ? QStringLiteral("adb root 无响应")
                                   : rootText.trimmed().left(160);
        *error = QStringLiteral("adbd 不是 root（%1）。读磁盘头 / 分区镜像需要 root："
                                "请用 userdebug 或 eng 版本 `adb root`，"
                                "或改用 EDL 页读取 GPT。")
                     .arg(reason);
    }
    return false;
}

QString AdbGptReader::detectDisk(PlatformToolsManager& tools, const LogFn& log, QString* error)
{
    // ① by-name 符号链接反查：目标就是真实块设备，名字也是用户认得的。
    const auto ls = tools.executeAdb(QStringLiteral("shell ls -l /dev/block/by-name/"), 20000);
    QHash<QString, int> votes;
    if (ls.success && !ls.output.trimmed().isEmpty()) {
        static const QRegularExpression link(
            QStringLiteral("->\\s*/dev/block/([A-Za-z0-9_.\\-]+)"));
        const QStringList lines =
            ls.output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const auto m = link.match(line);
            if (!m.hasMatch()) continue;
            const QString disk = diskOf(m.captured(1));
            if (!disk.isEmpty()) votes[disk] += 1;
        }
    }

    if (!votes.isEmpty()) {
        QString best;
        int bestVotes = -1;
        // 排序后再挑，避免 QHash 迭代顺序导致同一台设备两次结果不同。
        QStringList keys = votes.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& key : keys) {
            if (votes.value(key) > bestVotes) {
                bestVotes = votes.value(key);
                best = key;
            }
        }
        if (!best.isEmpty()) {
            if (log) {
                log(QStringLiteral("[GPT] 磁盘节点: /dev/block/%1（by-name 反查，%2 个分区指向它）")
                        .arg(best)
                        .arg(bestVotes));
            }
            return QStringLiteral("/dev/block/") + best;
        }
    }

    // ② /proc/partitions 兜底：取块数最大的整盘（分区名带后缀，整盘名不带）。
    const auto parts = tools.executeAdb(QStringLiteral("shell cat /proc/partitions"), 20000);
    if (parts.success) {
        static const QRegularExpression row(
            QStringLiteral("^\\s*\\d+\\s+\\d+\\s+(\\d+)\\s+([A-Za-z0-9_\\-]+)\\s*$"));
        qint64 bestBlocks = -1;
        QString best;
        const QStringList lines =
            parts.output.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const auto m = row.match(line);
            if (!m.hasMatch()) continue;
            const QString name = m.captured(2);
            if (diskOf(name) != name) continue; // 只认整盘
            const qint64 blocks = m.captured(1).toLongLong();
            if (blocks > bestBlocks) {
                bestBlocks = blocks;
                best = name;
            }
        }
        if (!best.isEmpty()) {
            if (log) {
                log(QStringLiteral("[GPT] 磁盘节点: /dev/block/%1（/proc/partitions，%2 KB）")
                        .arg(best)
                        .arg(bestBlocks));
            }
            return QStringLiteral("/dev/block/") + best;
        }
    }

    if (error) {
        *error = QStringLiteral("找不到磁盘节点（%1）。请确认设备已连上 adb 且 `ls /dev/block/by-name/` 可读。")
                     .arg(ls.success ? QStringLiteral("/dev/block/by-name 为空")
                                     : ls.error.trimmed().left(120));
    }
    return QString();
}

bool AdbGptReader::readHead(PlatformToolsManager& tools, const QString& diskIn, QByteArray* data,
                            QString* error, const LogFn& log)
{
    if (!data) return false;
    data->clear();

    const QString disk = diskIn.isEmpty() ? detectDisk(tools, log, error) : diskIn;
    if (disk.isEmpty()) return false;

    if (!ensureRoot(tools, log, error)) return false;

    const QString remote = QString::fromLatin1(kRemoteHead);
    const QString local = localTemp(QStringLiteral("gpt_head"));
    QFile::remove(local);

    // 两种对齐都试：4Kn 设备对 512 粒度的读可能直接 EINVAL。
    const QVector<QPair<int, int>> plans = { { 512, 48 }, { 4096, 6 } };
    bool got = false;
    QString lastError;

    for (const auto& plan : plans) {
        if (got) break;
        const QString dd = QStringLiteral("dd if=%1 of=%2 bs=%3 count=%4")
                               .arg(disk, remote)
                               .arg(plan.first)
                               .arg(plan.second);
        // chmod 是给后面 `adb pull`（shell 用户）看的：root 建的 0600 文件拉不下来。
        const QStringList variants = {
            QStringLiteral("shell su -c \"%1 && chmod 644 %2\"").arg(dd, remote),
            QStringLiteral("shell %1").arg(dd),
        };
        for (const QString& args : variants) {
            if (got) break;
            const auto r = tools.executeAdb(args, 60000);
            if (!r.success) {
                lastError = (r.output + r.error).trimmed();
                if (log) {
                    log(QStringLiteral("[GPT] dd 失败（bs=%1）: %2")
                            .arg(plan.first)
                            .arg(lastError.left(160)));
                }
                continue;
            }
            QFile::remove(local);
            const auto pull =
                tools.executeAdb(QStringLiteral("pull %1 \"%2\"").arg(remote, local), 60000);
            const qint64 size = QFileInfo(local).size();
            if (pull.success && size >= kHeadBytes) {
                got = true;
                break;
            }
            lastError = pull.success
                            ? QStringLiteral("拉回来的文件只有 %1 字节").arg(size)
                            : pull.error.trimmed();
            if (log) log(QStringLiteral("[GPT] pull 失败: %1").arg(lastError.left(160)));
        }
    }

    tools.executeAdb(QStringLiteral("shell rm -f %1").arg(remote), 15000);

    if (!got) {
        if (error) {
            *error = QStringLiteral("读取 %1 磁盘头失败：%2")
                         .arg(disk, lastError.isEmpty() ? QStringLiteral("dd / pull 均未成功")
                                                        : lastError.left(200));
        }
        QFile::remove(local);
        return false;
    }

    QFile file(local);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开临时文件失败: %1").arg(file.errorString());
        QFile::remove(local);
        return false;
    }
    *data = file.readAll();
    file.close();
    QFile::remove(local);

    if (log) {
        log(QStringLiteral("[GPT] 已读取 %1 磁盘头 %2 字节").arg(disk).arg(data->size()));
    }
    return true;
}

AdbGptReader::Result AdbGptReader::read(PlatformToolsManager& tools, const LogFn& log)
{
    Result result;

    result.disk = detectDisk(tools, log, &result.error);
    if (result.disk.isEmpty()) return result;

    if (!readHead(tools, result.disk, &result.head, &result.error, log)) return result;

    result.sectorSize = sniffSectorSize(result.head);
    if (result.sectorSize == 0) {
        result.error = QStringLiteral("%1 头部没有 EFI PART 签名（空盘 / 分区表损坏 / 读到的不是整盘）")
                           .arg(result.disk);
        return result;
    }

    if (!swt::edl::GptParser::parse(result.head, result.sectorSize, 0, &result.partitions,
                                    &result.error)) {
        return result;
    }
    result.ok = true;
    if (log) {
        log(QStringLiteral("[GPT] 解析成功：%1 个分区（%2，%3 字节/扇区）")
                .arg(result.partitions.size())
                .arg(result.disk)
                .arg(result.sectorSize));
    }
    return result;
}

bool AdbGptReader::writeHead(PlatformToolsManager& tools, const QString& diskIn,
                             const QByteArray& data, quint32 sectorSize, QString* error,
                             const LogFn& log)
{
    if (sectorSize < 512 || data.isEmpty() || (data.size() % static_cast<qsizetype>(sectorSize)) != 0
        || data.size() < static_cast<qsizetype>(sectorSize) * 2) {
        if (error) {
            *error = QStringLiteral("GPT 数据长度非法：%1 字节 / %2 字节扇区").arg(data.size()).arg(sectorSize);
        }
        return false;
    }
    if (sniffSectorSize(data) == 0) {
        if (error) *error = QStringLiteral("不是有效的 GPT 文件：缺少 EFI PART 签名");
        return false;
    }

    const QString disk = diskIn.isEmpty() ? detectDisk(tools, log, error) : diskIn;
    if (disk.isEmpty()) return false;
    if (!ensureRoot(tools, log, error)) return false;

    const QString local = localTemp(QStringLiteral("gpt_restore"));
    QFile file(local);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("写临时文件失败: %1").arg(file.errorString());
        return false;
    }
    const bool written = file.write(data) == data.size();
    file.close();
    if (!written) {
        if (error) *error = QStringLiteral("写临时文件失败（磁盘空间不足？）");
        QFile::remove(local);
        return false;
    }

    const QString remote = QString::fromLatin1(kRemoteRestore);
    const auto push = tools.executeAdb(QStringLiteral("push \"%1\" %2").arg(local, remote), 120000);
    if (!push.success) {
        if (error) *error = QStringLiteral("adb push 失败: %1").arg(push.error.trimmed().left(160));
        QFile::remove(local);
        return false;
    }
    tools.executeAdb(QStringLiteral("shell chmod 644 %1").arg(remote), 15000);

    const int count = int(data.size() / static_cast<qsizetype>(sectorSize));
    const QString dd = QStringLiteral("dd if=%1 of=%2 bs=%3 count=%4 conv=fsync")
                           .arg(remote, disk)
                           .arg(sectorSize)
                           .arg(count);
    const QStringList variants = {
        QStringLiteral("shell su -c \"%1\"").arg(dd),
        QStringLiteral("shell %1").arg(dd),
    };

    bool ok = false;
    QString lastError;
    for (const QString& args : variants) {
        if (log) log(QStringLiteral("[GPT] 写回: %1").arg(args));
        const auto r = tools.executeAdb(args, 120000);
        lastError = (r.output + r.error).trimmed();
        if (r.success) {
            ok = true;
            break;
        }
    }

    tools.executeAdb(QStringLiteral("shell rm -f %1").arg(remote), 15000);
    QFile::remove(local);

    if (!ok) {
        if (error) {
            *error = QStringLiteral("写回 %1 失败：%2")
                         .arg(disk, lastError.isEmpty() ? QStringLiteral("dd 未成功") : lastError.left(200));
        }
        return false;
    }
    if (log) {
        log(QStringLiteral("[GPT] 已把 %1 个扇区（%2 字节）写回 %3 头部")
                .arg(count)
                .arg(data.size())
                .arg(disk));
    }
    return true;
}

bool AdbGptReader::pullPartition(PlatformToolsManager& tools, const QString& partition,
                                 const QString& localPath, QString* error, const LogFn& log)
{
    if (partition.trimmed().isEmpty() || localPath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("分区名或保存路径为空");
        return false;
    }
    if (!ensureRoot(tools, log, error)) return false;

    const QString remote =
        QStringLiteral("/data/local/tmp/kc_part_%1.img")
            .arg(QString(partition).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                                            QStringLiteral("_")));

    // 物理分区在 by-name 下；动态分区（super 里的）只在 /dev/block/mapper 下。
    const QStringList sources = {
        QStringLiteral("/dev/block/by-name/%1").arg(partition),
        QStringLiteral("/dev/block/mapper/%1").arg(partition),
    };

    bool ok = false;
    QString lastError;
    for (const QString& source : sources) {
        const QString dd =
            QStringLiteral("dd if=%1 of=%2 bs=1M").arg(source, remote)
            + QStringLiteral(" && chmod 644 ") + remote;
        const QStringList variants = {
            QStringLiteral("shell su -c \"%1\"").arg(dd),
            QStringLiteral("shell %1").arg(dd),
        };
        for (const QString& args : variants) {
            const auto r = tools.executeAdb(args, 0); // 分区可能有几个 GB，不限时
            if (!r.success) {
                lastError = (r.output + r.error).trimmed();
                continue;
            }
            const auto pull =
                tools.executeAdb(QStringLiteral("pull %1 \"%2\"").arg(remote, localPath), 0);
            if (pull.success) {
                ok = true;
                break;
            }
            lastError = pull.error.trimmed();
        }
        if (ok) break;
        if (log) log(QStringLiteral("[GPT] %1 不可读: %2").arg(source, lastError.left(120)));
    }

    tools.executeAdb(QStringLiteral("shell rm -f %1").arg(remote), 15000);

    if (!ok && error) {
        *error = QStringLiteral("导出 %1 失败：%2").arg(
            partition, lastError.isEmpty() ? QStringLiteral("dd / pull 未成功") : lastError.left(200));
    }
    return ok;
}

} // namespace kc
