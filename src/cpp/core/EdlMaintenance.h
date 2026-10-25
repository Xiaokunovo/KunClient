#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace swt::edl {

/// BCB（bootloader control block，bootloader 控制块）载荷大小：misc 分区起始
/// 2048 字节。命令串放在偏移 0，其余清零。
constexpr quint32 BcbBlockBytes = 2048;

/// devinfo 分区里 OEM 解锁开关字节的偏移（0x1F4）：0x00 = 锁定，0x01 = 已解锁。
/// 写 1 即「强开 OEM 解锁」；2024+ 新机型可能因 AR fuse 已烧写而失效。
constexpr quint32 DevinfoUnlockByteOffset = 0x1F4;

/// 构造写入 misc 分区的 BCB 载荷：command 放偏移 0（NUL 结尾），其余
/// BcbBlockBytes-1 字节清零。命令为空或超过 31 字符（bootloader_message
/// 的 command[32]）时返回空 QByteArray —— 调用方必须按失败处理，
/// 绝不能退化成「写一块全零」把用户设备锁死在错误重启目标上。
QByteArray buildBcbPayload(const QString& command);

enum class DevinfoUnlockResult {
    Changed,         ///< 解锁字节由 0x00 改为 0x01，调用方需要写回设备
    AlreadyUnlocked, ///< 解锁字节已是 0x01，无需改写
    Rejected,        ///< 缓冲区太小或字节值不是 0/1 —— fail-closed，error 有说明
};

/// 在**已读回的 devinfo 首扇区**上应用 OEM 解锁开关。纯内存操作，
/// 不碰设备；真正的写回由调用方决定（先改后写，写失败不留半成品状态）。
DevinfoUnlockResult applyDevinfoUnlock(QByteArray* sector, QString* error);

/// 通用恢复出厂要擦除的分区（按归一化名匹配 GPT，A/B 槽位自动覆盖）：
/// userdata 是用户数据本体，metadata 是动态分区/文件系统元数据，
/// 只擦这两个才等价于系统设置里的「恢复出厂设置」。
QStringList factoryResetPartitionNames();

/// 基带备份预设分区：modem + NV（modemst1/st2）+ SIM 相关（fsg/fsc）+
/// 蓝牙固件。这些正是刷机后最需要原样备份回来的「指纹类」分区。
QStringList basebandBackupPartitionNames();

/// GPT 导出文件名：LUN 0 主表 → gpt_main0.bin；备份表 → gpt_backup0.bin。
QString gptFileName(quint32 lun, bool backup);

} // namespace swt::edl
