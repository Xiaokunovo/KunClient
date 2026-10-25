// USB 连接状态粘滞回归：保持期、连续确认、模式切换立即采纳、手动清零。

#include "kc/devices/UsbStateStickiness.h"

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// 状态编号与 DeviceHomePage::Mode 对齐：0=None 1=Adb 2=Recovery 3=Fastboot 4=Edl。
constexpr int kNone = 0;
constexpr int kAdb = 1;
constexpr int kFastboot = 3;

} // namespace

int main()
{
    {
        kc::UsbStateStickiness sticky;
        check(sticky.apply(kNone, 0) == kNone, "empty probe stays empty");
        check(sticky.apply(kAdb, 1000) == kAdb, "first detection is adopted");
        check(sticky.hasState(), "state recorded");
    }

    {
        // 保持期内翻回 None 不清除
        kc::UsbStateStickiness sticky;
        sticky.apply(kAdb, 0);
        check(sticky.apply(kNone, 2000) == kAdb, "hold: 2s miss keeps ADB");
        check(sticky.apply(kNone, 5000) == kAdb, "hold: 5s miss keeps ADB");
        check(sticky.apply(kNone, 9000) == kAdb, "hold: 9s miss keeps ADB");
        check(sticky.missTicks() == 3, "miss ticks accumulate during hold");
    }

    {
        // 保持期满 + 连续 4 次确认才清除
        kc::UsbStateStickiness sticky;
        sticky.apply(kAdb, 0);
        sticky.apply(kNone, 2000);
        sticky.apply(kNone, 4000);
        sticky.apply(kNone, 6000);
        check(sticky.apply(kNone, 8000) == kAdb,
              "8s: hold not over yet even with 4 misses");
        check(sticky.apply(kNone, 10000) == kNone,
              "10s + 5 misses clears state");
        check(!sticky.hasState(), "state cleared");
    }

    {
        // 保持期过后但确认次数不足，仍不清除
        kc::UsbStateStickiness sticky;
        sticky.apply(kAdb, 0);
        sticky.apply(kAdb, 1000);
        sticky.apply(kNone, 11000);
        check(sticky.apply(kNone, 13000) == kAdb, "only 2 misses after hold → keep");
        check(sticky.apply(kNone, 15000) == kAdb, "3 misses → keep");
        check(sticky.apply(kNone, 17000) == kNone, "4 misses → clear");
    }

    {
        // 模式切换立即采纳（adb → fastboot）
        kc::UsbStateStickiness sticky;
        sticky.apply(kAdb, 0);
        check(sticky.apply(kFastboot, 1000) == kFastboot, "mode switch adopted immediately");
        check(sticky.missTicks() == 0, "mode switch resets miss ticks");
        // 切回来也立即
        check(sticky.apply(kAdb, 2000) == kAdb, "switch back adopted immediately");
    }

    {
        // 探测恢复时清零退出计数（抖动不该累积成"断线"）
        kc::UsbStateStickiness sticky;
        sticky.apply(kAdb, 0);
        sticky.apply(kNone, 2000);
        sticky.apply(kNone, 4000);
        check(sticky.missTicks() == 2, "two misses");
        sticky.apply(kAdb, 6000);
        check(sticky.missTicks() == 0, "reappearing probe resets miss ticks");
        check(sticky.apply(kNone, 8000) == kAdb, "still ADB after reset");
    }

    {
        // 手动清零：无论保持期是否结束都立即生效
        kc::UsbStateStickiness sticky;
        sticky.apply(kAdb, 0);
        sticky.reset();
        check(sticky.state() == kNone, "reset clears state");
        check(sticky.apply(kNone, 1000) == kNone, "reset then empty stays empty");
        check(sticky.apply(kAdb, 2000) == kAdb, "detection after reset adopted");
    }

    if (failures != 0) {
        std::cerr << failures << " USB stickiness check(s) failed\n";
        return 1;
    }
    std::cout << "USB state stickiness tests passed\n";
    return 0;
}
