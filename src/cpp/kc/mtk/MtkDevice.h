#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkDevice — MTK 设备发现（纯查询，无阻塞 IO）
//
//  设备识别（口径与上游一致）：
//    · VID 固定 0x0E8D（MediaTek）；
//    · PID 区分阶段：BROM（0x0003 及变体）/ 0x2000=Preloader / 0x2001=DA；
//    · 经 usbser 驱动呈现为 COM 口（友好名常见 "MTK USB Port (COMxx)"）。
//    枚举走 SetupAPI 的 Ports 类 + cfgmgr32 查父设备硬件 ID（不依赖 devguid.h，
//    GUID 按公开值本地定义）。
//
//  BROM / XFlash 会话在 MtkBrom.h、MtkXflash.h；整机编排在 MtkDeviceSession.h。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"

#include <QString>
#include <QVector>

#include <cstdint>

namespace kc::mtk {

/// 一个在线 MTK 设备（按阶段归类）。
struct MtkUsbDevice {
    DeviceStage stage = DeviceStage::Disconnected;
    uint16_t vid = 0;
    uint16_t pid = 0;
    QString comPort;    // 关联 COM 口（无驱动 / 无端口时为空）
    QString devicePath; // 设备实例 ID（诊断用）
};

/// 枚举当前在线的 MTK 设备（纯查询，无阻塞 IO）。
QVector<MtkUsbDevice> scanMtkDevices();

/// 在扫描结果里挑一个可用设备。
///   atLeast = Disconnected → 不限阶段；否则要求设备阶段 >= atLeast
///   （枚举序：Brom < Preloader < Da，即 Da 最"高"）。
///   多候选时优先有 COM 口、且阶段更高的那个。找不到返回 false。
bool findMtkDevice(DeviceStage atLeast, MtkUsbDevice* out);

} // namespace kc::mtk
