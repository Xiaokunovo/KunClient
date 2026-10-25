#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  StorageBrandRunner — 「字库牌子」的**执行层**（开串口 / 传引导 / 发 NOP）
//
//  纯逻辑（解析厂商串、挑引导文件、9008 端口指纹）在 StorageBrandProbe.h；
//  这里只负责跟设备打交道，所以两个入口都能用：
//    · EDL 页「高级功能 → 字库牌子」（2026-10-21 起的主入口，日志进 EDL 日志）
//    · （独立页 StorageBrandPage 已于 2026-10-21 下线，只留这条主入口）
//
//  流程与 MiUfsProbe 对齐：peek 判状态 → 必要时 Sahara 上传引导 → 发 <nop>
//  让 loader 自己打印 `INFO: ufs|emmc: <vendor>` → 解析不到再 getstorageinfo 兜底。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/devices/StorageBrandProbe.h"

#include <QString>

#include <atomic>
#include <functional>

namespace kc::devices {

struct StorageBrandProbeOutcome {
    bool ok = false;
    QString error;              ///< ok=false 时的原因（可直接进日志）
    StorageBrandResult result;  ///< 解析到的存储类型 / 厂商 / 芯片序列号
};

/// 同步执行一次探测。**必须在后台线程调用**（会阻塞读串口）。
/// @param portName       9008 串口名（COMx）
/// @param programmerPath Firehose 引导文件（EDL 页当前选中的那个）
/// @param cancelled      取消标志（调用方持有，生命周期要长于本次调用）
/// @param log            逐行日志回调（可空）
StorageBrandProbeOutcome runStorageBrandProbe(const QString& portName,
                                              const QString& programmerPath,
                                              std::atomic_bool& cancelled,
                                              const std::function<void(const QString&)>& log);

} // namespace kc::devices
