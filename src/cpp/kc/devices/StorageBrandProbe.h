#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  StorageBrandProbe — 「小米字库牌子」的纯逻辑层（不碰串口、不开线程）
//
//  来源：MiUfsProbe（C# 反编译源码，酷安@皓皓的小月，「FDL · 9008 UFS 查询」）。
//  它的核心发现是：**只要把 Firehose 引导（prog_*firehose*.elf / xbl_*devprg*.melf）
//  传进去，再发一条 `<nop>`，loader 自己会把存储芯片那行打印出来** ——
//
//      INFO: ufs: SAMSUNG          ← 字库厂商（唯一可靠的文本来源）
//      Chip serial num (0x…)       ← 芯片序列号
//
//  不需要 configure、不需要 getstorageinfo、更不碰任何分区：NOP 是纯读命令，
//  对设备零写入。有些 loader 不返回 storage_info JSON，只打这一行 —— 所以
//  「字库信息空着」很多时候不是没读，而是没解析这行（见 protocol/Firehose.cpp
//  的 observeMessages，那里已经是同一套正则）。
//
//  本文件只做**离线**那部分，方便单测：
//    · 从任意文本（Firehose 回包 / MiFlash 日志）里解析存储类型、厂商、芯片序列号；
//    · 厂商串 → 中英文品牌名（三星 / Samsung …）；
//    · 固件目录里按 MiUfsProbe 的优先级挑 Firehose 引导文件；
//    · 9008 端口指纹（VID 0x05C6 + PID 0x9008 优先，描述关键字兜底）。
//  真正的探测流程（Sahara 上传 + NOP）在 kc/devices/StorageBrandRunner.cpp，
//  入口是 EDL 页「高级功能 ▾ → 字库牌子」（2026-10-21 从独立页搬进去的）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QSerialPortInfo>
#include <QString>
#include <QStringList>

namespace kc::devices {

/// 一次「字库牌子」探测的结论。字段全空 = 什么都没解析到。
struct StorageBrandResult {
    QString storageType;   ///< "UFS" / "EMMC"（loader 打印的形态，大写）
    QString vendor;        ///< 厂商原始串，已规整为大写，如 "SAMSUNG"
    QString chipSerial;    ///< `Chip serial num (…)` 里的值，原样保留（含 0x 前缀）
    QString rawLine;       ///< 命中的原文，如 "INFO: ufs: SAMSUNG"（给人核对用）

    [[nodiscard]] bool isEmpty() const
    {
        return storageType.isEmpty() && vendor.isEmpty() && chipSerial.isEmpty();
    }
};

/// 从任意文本里抽字库信息。两种形态都认：
///   `INFO: ufs: SAMSUNG`（带厂商）与 `INFO: emmc`（只有类型）。
StorageBrandResult parseStorageBrand(const QString& text);

/// 厂商串 → 中文品牌名（"三星"）。未知厂商返回空串。
QString storageBrandChineseName(const QString& vendor);

/// 厂商串 → 英文品牌名（"Samsung"）。未知厂商返回空串。
QString storageBrandEnglishName(const QString& vendor);

/// 结果标题：中文名 + 英文名（"三星 Samsung"）；未知厂商回落原始串。
QString storageBrandTitle(const StorageBrandResult& result);

/// 在固件目录里找 Firehose 引导文件。
///
/// dir 可以是 ROM 根目录（会自动下钻到名字带 images 的子目录），也可以直接是
/// images 目录。文件名口径与 MiUfsProbe 的 ProgrammerPattern 一致，优先级：
///   prog_firehose_lite.elf（0）> 名字带 devprg（1）> 其余 firehose（2）。
/// 找不到返回空串。
QString findProgrammerInDirectory(const QString& dir);

/// 串口指纹：是不是 Qualcomm 9008（EDL）端口。
///
/// 🔴 只认描述关键字不够（小米/一加/OPPO 定制驱动写 "QUSB_BULK_CID:…"），
///    VID 0x05C6 + PID 0x9008 才是不会变的组合。口径与 ui/MainWindow.cpp 的
///    isEdlPort 一致 —— 两处都是同一套判据，改一边必须改另一边。
bool isQualcommEdlPort(const QSerialPortInfo& port);

/// 当前插着的 9008 端口名（按 COM 号自然序）。没有返回空表。
QStringList availableEdlPorts();

} // namespace kc::devices
