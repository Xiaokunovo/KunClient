#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  DeviceWatcher —— USB 设备检测与监控（Qualcomm EDL / MTK / 展讯 / ADB / Fastboot）
//  移植自 KunFlashTool.Core.Modules.Common.DeviceWatcher.cs（911 行）。
//
//  C# 原实现基于 WMI（ManagementEventWatcher + Win32_PnPEntity 查询）。
//  此处改用 Win32 SetupAPI（SetupDiGetClassDevs / SetupDiEnumDeviceInfo /
//  SetupDiGetDeviceRegistryProperty）枚举 USB 设备 —— 不打开任何串口端口，
//  **绝不**触发 qcusbser 的 DTR 边沿，因此不会吃掉高通设备的一次性 HELLO_REQ。
//  刷新采用 2 秒轮询（与 C# 的 WMI WITHIN 2 轮询回落等价）。
//
//  识别优先级（与 C# 一致）：VID/PID 精确匹配 → 驱动服务名 → 描述文本模式。
//  VID/PID 表与描述模式必须原样保留。
// ─────────────────────────────────────────────────────────────────────────────

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

class QThread;

namespace kc {

// 设备类型（对应 C# 的 DeviceType）。
enum class DeviceType {
    Unknown,
    Edl9008,     // 高通 EDL 9008
    Diag,        // 高通 Diag
    MtkPreloader,// MTK Preloader
    MtkBrom,     // MTK Brom
    Spreadtrum,  // 展讯下载模式
    Adb,         // ADB 设备
    Fastboot     // Fastboot 设备
};

// 设备信息（对应 C# 的 DeviceInfo）。
struct DeviceInfo
{
    QString portName;
    QString deviceId;
    QString description;
    QString manufacturer;
    QString vid;
    QString pid;
    DeviceType type = DeviceType::Unknown;

    // 端口是否可用（存在，非「已打开」）。
    bool isPortAvailable = false;
    QString portStatus;

    // 设备类型显示名。
    QString typeName() const;

    bool isUnknown() const { return type == DeviceType::Unknown; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DeviceWatcher —— 设备插拔监控（QObject，信号驱动）。
// ─────────────────────────────────────────────────────────────────────────────
class DeviceWatcher : public QObject
{
    Q_OBJECT

public:
    explicit DeviceWatcher(QObject* parent = nullptr);
    ~DeviceWatcher() override;

    // 开始监视（幂等，重复调用先 Stop）。立即扫描一次已连接设备。
    void start();
    // 停止监视。
    void stop();

    // 获取当前所有设备（含非串口的 EDL / ADB / Fastboot 设备）。
    QList<DeviceInfo> allDevices() const;

    // 指定端口号的设备信息（未找到返回仅含 portName 的占位）。
    DeviceInfo deviceInfo(const QString& portName) const;

    // 按类型过滤。
    QList<DeviceInfo> findDevicesByType(DeviceType type) const;

    // 同步等待指定类型的设备出现。超时返回 false，device 保持原值。
    bool waitForDevice(DeviceType type, int timeoutMs, DeviceInfo* device,
                       const std::function<bool()>& isCancelled = nullptr);

    // 所有设备 + 端口可用性测试。
    QList<DeviceInfo> allDevicesWithTest() const;

    // 快速检测 9008 设备（带重试）。
    bool findAvailable9008Device(DeviceInfo* device, int maxRetries = 3) const;
    // 快速检测 MTK 设备。
    bool findAvailableMtkDevice(DeviceInfo* device) const;

    // ── 端口测试（静态）─────────────────────────────────────────────────────
    // 【非侵入式】只确认端口存在，绝不打开端口。
    static bool testPortAvailable(const QString& portName, QString* status);
    // 【侵入式】真正打开端口以确认可独占。会触发 DTR 边沿，仅限显式「测试端口」动作。
    static bool probePortByOpening(const QString& portName, QString* status);
    // 测试设备端口并更新状态（非侵入式）。
    static void testDevicePort(DeviceInfo& device);

    // ── 已知设备 VID/PID 表（原样保留 C# 表）────────────────────────────────
    static QHash<QString, DeviceType> knownDevices();

signals:
    // 设备接入（新增端口或有类型识别的设备）。
    void deviceConnected(const DeviceInfo& device);
    // 设备移除（仅保留 portName）。
    void deviceDisconnected(const DeviceInfo& device);
    // 设备列表变化。
    void devicesChanged(const QList<DeviceInfo>& devices);

private slots:
    void onPoll();

private:
    QList<DeviceInfo> enumerateDevices() const;

    QTimer m_pollTimer;
    QSet<QString> m_lastPorts;
    bool m_started = false;
};

} // namespace kc
