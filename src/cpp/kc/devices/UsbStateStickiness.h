#pragma once

#include <QtGlobal>

// ─────────────────────────────────────────────────────────────────────────────
//  UsbStateStickiness — USB 连接状态「粘滞」（纯逻辑，可单测）
//
//  移植自 YukiClient `Core/UsbStateStickiness.cs`（86 行）。
//
//  要解决的现象：设备主页每 5 秒探测一轮（adb devices / fastboot devices /
//  DeviceWatcher 枚举），三者交替给出不同结果时，状态标签会来回闪
//  （用户口径「一会儿连一会儿断」）。根因不是设备真的断了，而是：
//    · adb 服务刚重启 / 第一次 spawn 时 `adb devices` 偶发空表；
//    · 设备正在切模式（adb → fastboot），中间必然有一小段两边都探不到；
//    · fastboot 的 `devices` 在 USB 重新枚举期间返回空。
//
//  规则：
//    ① 状态一旦成立（非 0）就**至少保持 MinHoldSeconds 秒**，期间探测翻回 0 也不清除；
//    ② 保持期满后，还要**连续 ConfirmTicks 次**探测都不是该状态才真正退出；
//    ③ 探测到**另一个非 0 状态**（模式切换）时立即采纳 —— 这是确定性事件，
//       粘滞只针对「探测抖动」，不能把模式切换也拖 10 秒。
//
//  用法（每轮探测一次）：
//      const int effective = m_stickiness.apply(rawState, nowMs);
//      // effective 就是本轮对外应显示的状态
// ─────────────────────────────────────────────────────────────────────────────

namespace kc {

class UsbStateStickiness final {
public:
    /// 状态成立后至少保持的秒数（期间探测翻脸不清除）。
    static constexpr double MinHoldSeconds = 10.0;
    /// 保持期满后，需要连续多少次探测「不再成立」才清除。
    static constexpr int ConfirmTicks = 4;

    /// 喂入本轮探测到的状态（0 = 什么都没探到），返回本轮对外生效的状态。
    /// 返回值与上一轮相同 = 显示不需要更新。
    int apply(int rawState, qint64 nowMs)
    {
        if (rawState != 0) {
            if (rawState != m_state) {
                // 新状态 / 模式切换：立即采纳（确定性事件不粘滞）。
                m_state = rawState;
                m_sinceMs = nowMs;
                m_missTicks = 0;
            } else {
                m_missTicks = 0;   // 仍然成立，清零退出计数
            }
            return m_state;
        }

        // 本轮什么都没探到：只有「保持期已过」且「连续 ConfirmTicks 次都没探到」才清除。
        if (m_state == 0) return 0;
        ++m_missTicks;
        const double heldSeconds = static_cast<double>(nowMs - m_sinceMs) / 1000.0;
        if (heldSeconds >= MinHoldSeconds && m_missTicks >= ConfirmTicks) {
            reset();
        }
        return m_state;
    }

    /// 确定性清零：用户手动刷新、切页重进、显式「断开」时调用。
    /// 粘滞是为探测抖动设计的，不能卡住这种明确事件。
    void reset()
    {
        m_state = 0;
        m_sinceMs = 0;
        m_missTicks = 0;
    }

    int state() const { return m_state; }
    bool hasState() const { return m_state != 0; }
    int missTicks() const { return m_missTicks; }

private:
    int m_state = 0;
    qint64 m_sinceMs = 0;
    int m_missTicks = 0;
};

} // namespace kc
