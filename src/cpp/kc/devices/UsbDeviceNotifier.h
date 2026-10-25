#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  UsbDeviceNotifier —— 应用级 USB 设备插拔通知
//  移植自 KunFlashTool.App.Services.UsbDeviceNotifier.cs（52 行）。
//
//  为什么要有它：设备切模式（ADB ↔ Fastboot ↔ EDL）时 Windows 会重新枚举 USB
//  接口，触发 WM_DEVICECHANGE。主页据此主动重探设备模式，不必手动刷新。
//
//  C# 在 MainWindow 的 HWND 上挂 HwndSource 钩子。移植到 Qt 后改为在
//  QCoreApplication 上安装 QAbstractNativeEventFilter —— 纯消息驱动，覆盖所有
//  顶层窗口，不依赖 WMI / SerialPort，也不常驻轮询吃 CPU。
//
//  任何 USB 设备（鼠标 / 键盘 / 闪存盘）插拔都会触发；调用方自行去抖并在可见时才重探。
// ─────────────────────────────────────────────────────────────────────────────

#include <QAbstractNativeEventFilter>
#include <QObject>

namespace kc {

class UsbDeviceNotifier : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    // 进程级单例（首次调用时创建并安装事件过滤器）。
    static UsbDeviceNotifier* instance();

    // 在给定顶层窗口 / 全应用上挂钩子。幂等，可重复调用。
    // 返回是否成功（非 Windows 平台返回 false）。
    bool initialize();

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    // USB 设备插入或移除时触发（不区分具体设备）。
    void deviceChanged();

private:
    explicit UsbDeviceNotifier(QObject* parent = nullptr);

    bool m_initialized = false;
};

} // namespace kc
