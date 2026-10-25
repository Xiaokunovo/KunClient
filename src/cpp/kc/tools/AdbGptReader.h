#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AdbGptReader —— 从 Android 侧（adb + root）读真实 GPT 分区表
//
//  背景：fastboot 协议没有「分区 → 主机」的通用命令，`fastboot getvar all` 只给
//  `partition-size:<name>: 0x...`（引导器上报的名字 + 大小），没有起始 LBA、
//  类型 GUID、属性位 —— 那不是 GPT，只是一份名字表。要拿到真正的 GPT 结构，
//  只能从磁盘头取原始扇区，喂给已有的 GptParser：
//      dd if=/dev/block/sda bs=512 count=48  →  GptParser::parse
//
//  磁盘节点（/dev/block/sda、/dev/block/mmcblk0…）不能硬编码：
//    · 先 `ls -l /dev/block/by-name/` 反查 by-name 符号链接的目标，按「整盘」投票；
//    · 再退回 `/proc/partitions` 取块数最大的整盘。
//
//  读取量固定 48×512 = 24576 字节 = 6×4096：
//    · 512 字节扇区：GPT 头在 512，分区项在 1024..17408 → 够；
//    · 4096 字节扇区（UFS 4Kn）：头在 4096，分区项在 8192..24576 → 刚好够。
//    两种对齐都满足，所以不用先知道扇区大小。
//
//  ⚠️ 全部方法同步阻塞（QProcess waitForFinished），只能在 worker 线程调用。
// ─────────────────────────────────────────────────────────────────────────────

#include "core/Types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>

namespace kc {

class PlatformToolsManager;

class AdbGptReader
{
public:
    struct Result
    {
        bool ok = false;
        QString disk;                              // /dev/block/sda
        quint32 sectorSize = 512;                  // 由 EFI PART 的位置嗅探
        QByteArray head;                           // 磁盘头原始字节（24576）
        QVector<swt::edl::PartitionInfo> partitions;
        QString error;
    };

    using LogFn = std::function<void(const QString&)>;

    // 读磁盘头 + 解析 GPT（adbd 需 root，会先尝试 `adb root`）。
    static Result read(PlatformToolsManager& tools, const LogFn& log = nullptr);

    // 只取磁盘头原始数据（备份 GPT 用）。disk 为空 → 自动探测。
    static bool readHead(PlatformToolsManager& tools, const QString& disk, QByteArray* data,
                         QString* error, const LogFn& log = nullptr);

    // 把原始 GPT 数据写回磁盘头（恢复 GPT）。data 必须是扇区大小的整数倍。
    static bool writeHead(PlatformToolsManager& tools, const QString& disk, const QByteArray& data,
                          quint32 sectorSize, QString* error, const LogFn& log = nullptr);

    // 把一个分区整段导出到主机文件（root + dd + adb pull）。
    static bool pullPartition(PlatformToolsManager& tools, const QString& partition,
                              const QString& localPath, QString* error, const LogFn& log = nullptr);

    // 探测磁盘节点（返回 /dev/block/sda 这种全路径）；失败返回空串并写 error。
    static QString detectDisk(PlatformToolsManager& tools, const LogFn& log, QString* error);

    // 0 = 不像 GPT；否则 512 / 4096。
    static quint32 sniffSectorSize(const QByteArray& data);

    // 当前 adbd 是否 root（必要时执行 `adb root`）。
    static bool ensureRoot(PlatformToolsManager& tools, const LogFn& log, QString* error);

    // 磁盘头读取字节数（导出给 UI 提示用）。
    static constexpr int kHeadBytes = 24576;
};

} // namespace kc
