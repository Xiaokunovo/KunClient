using System;
using System.Windows;
using System.Windows.Interop;

namespace KunFlashTool.App.Services
{
    /// <summary>
    /// 应用级 USB 设备插拔通知。
    ///
    /// 为什么要有它：设备切模式（ADB ↔ Fastboot ↔ EDL）时，Windows 会重新枚举 USB 接口，
    /// 触发 <c>WM_DEVICECHANGE</c>。主页（DeviceHomePage）据此**主动重探**设备模式，
    /// 不必再手动点「立即刷新」。
    ///
    /// 实现：在 MainWindow 的 HWND 上挂一个 Win32 消息钩子，纯消息驱动，
    /// 不依赖 WMI / SerialPort（DeviceWatcher 那套），也不常驻轮询吃 CPU。
    /// 任何 USB 设备（鼠标 / 键盘 / U 盘）插拔都会触发，但主页只在自己可见时才去重探，
    /// 而且去抖到一次物理变化只触发一轮探测。
    /// </summary>
    public static class UsbDeviceNotifier
    {
        /// <summary>USB 设备插入或移除时触发（不区分具体设备）。</summary>
        public static event EventHandler? DeviceChanged;

        private static HwndSource? _hook;
        private static bool _initialized;

        private const int WM_DEVICECHANGE = 0x0219;
        private const int DBT_DEVICEARRIVAL = 0x8000;
        private const int DBT_DEVICEREMOVECOMPLETE = 0x8004;

        /// <summary>在 MainWindow 的句柄上挂钩子。幂等，可重复调用。</summary>
        public static void Initialize(Window window)
        {
            if (_initialized) return;
            _initialized = true;
            var handle = new WindowInteropHelper(window).EnsureHandle();
            _hook = HwndSource.FromHwnd(handle);
            _hook.AddHook(WndProc);
        }

        private static IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msg == WM_DEVICECHANGE)
            {
                int ev = wParam.ToInt32();
                if (ev == DBT_DEVICEARRIVAL || ev == DBT_DEVICEREMOVECOMPLETE)
                    DeviceChanged?.Invoke(null, EventArgs.Empty);
            }
            return IntPtr.Zero;
        }
    }
}
