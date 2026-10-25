#include "kc/devices/DeviceWatcher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QThread>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <setupapi.h>
#  include <cfgmgr32.h>
#  include <initguid.h>
#endif

namespace kc {
namespace {

// 从文本里提取 "(COM12)" 形态的端口名；找不到返回空串。
QString extractPortName(const QString& text)
{
    if (text.isEmpty()) return QString();
    const QRegularExpression re(QStringLiteral("\\(COM(\\d+)\\)"));
    const QRegularExpressionMatch m = re.match(text);
    if (m.hasMatch()) return QStringLiteral("COM") + m.captured(1);
    return QString();
}

// 从 DeviceID / HardwareID 里提取 4 位十六进制 VID / PID（大写）。
void extractVidPid(const QString& idSource, QString* vid, QString* pid)
{
    vid->clear();
    pid->clear();
    if (idSource.contains(QStringLiteral("VID_"))) {
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("VID_([0-9A-Fa-f]{4})")).match(idSource);
        if (m.hasMatch()) *vid = m.captured(1).toUpper();
    }
    if (idSource.contains(QStringLiteral("PID_"))) {
        const QRegularExpressionMatch m =
            QRegularExpression(QStringLiteral("PID_([0-9A-Fa-f]{4})")).match(idSource);
        if (m.hasMatch()) *pid = m.captured(1).toUpper();
    }
}

// 根据设备描述文本识别类型（对应 C# IdentifyByDescription）。
DeviceType identifyByDescription(const QString& textUpper)
{
    const QString& t = textUpper;

    if (t.contains(QStringLiteral("QUALCOMM HS-USB QDLOADER 9008"))
        || t.contains(QStringLiteral("QDLOADER 9008"))
        || t.contains(QStringLiteral("QDL DEVICE"))
        || (t.contains(QStringLiteral("9008")) && t.contains(QStringLiteral("QUALCOMM")))
        || (t.contains(QStringLiteral("EDL")) && t.contains(QStringLiteral("QUALCOMM")))
        || t.contains(QStringLiteral("QUALCOMM HS-USB DIAGNOSTICS 9008"))) {
        return DeviceType::Edl9008;
    }

    if ((t.contains(QStringLiteral("QUALCOMM")) && t.contains(QStringLiteral("DIAG")))
        || t.contains(QStringLiteral("QUALCOMM HS-USB DIAGNOSTICS 900E"))
        || t.contains(QStringLiteral("NMEA DEVICE"))) {
        return DeviceType::Diag;
    }

    if (t.contains(QStringLiteral("MEDIATEK PRELOADER"))
        || t.contains(QStringLiteral("MTK PRELOADER"))
        || t.contains(QStringLiteral("MEDIATEK USB PORT"))
        || t.contains(QStringLiteral("MTK USB PORT"))
        || t.contains(QStringLiteral("DA USB VCOM"))
        || (t.contains(QStringLiteral("MEDIATEK")) && t.contains(QStringLiteral("USB VCOM")))) {
        return DeviceType::MtkPreloader;
    }

    if (t.contains(QStringLiteral("MEDIATEK BROM")) || t.contains(QStringLiteral("MTK BROM"))
        || (t.contains(QStringLiteral("MEDIATEK")) && t.contains(QStringLiteral("BOOTROM")))) {
        return DeviceType::MtkBrom;
    }

    if (t.contains(QStringLiteral("SPREADTRUM")) || t.contains(QStringLiteral("UNISOC"))
        || t.contains(QStringLiteral("SPRD"))) {
        return DeviceType::Spreadtrum;
    }

    if ((t.contains(QStringLiteral("ANDROID")) && t.contains(QStringLiteral("ADB")))
        || t.contains(QStringLiteral("ADB INTERFACE"))) {
        return DeviceType::Adb;
    }

    if ((t.contains(QStringLiteral("ANDROID")) && t.contains(QStringLiteral("FASTBOOT")))
        || t.contains(QStringLiteral("FASTBOOT INTERFACE"))) {
        return DeviceType::Fastboot;
    }

    return DeviceType::Unknown;
}

// 综合识别设备类型（对应 C# IdentifyDeviceType）。
DeviceType identifyDeviceType(const QString& vid, const QString& pid, const QString& caption,
                              const QString& name, const QString& manufacturer,
                              const QString& service, const QString& deviceId)
{
    // 1. VID/PID 精确匹配。
    if (!vid.isEmpty() && !pid.isEmpty()) {
        const auto it = DeviceWatcher::knownDevices().constFind(vid + pid);
        if (it != DeviceWatcher::knownDevices().constEnd()) return it.value();
    }

    // 2. 驱动服务名匹配。
    const QString svc = service.toUpper();
    if (svc.contains(QStringLiteral("QCUSB")) || svc.contains(QStringLiteral("QDBUSB"))
        || (svc == QStringLiteral("USBSER") && vid == QStringLiteral("05C6"))) {
        return DeviceType::Edl9008;
    }
    if (svc.contains(QStringLiteral("MTKUSB")) || svc.contains(QStringLiteral("MTKCDC"))) {
        return DeviceType::MtkPreloader;
    }

    // 3. 设备名称 / 描述匹配。
    const QString text = QStringLiteral("%1 %2 %3 %4")
                             .arg(caption, name, manufacturer, deviceId)
                             .toUpper();
    return identifyByDescription(text);
}

#ifdef Q_OS_WIN
// 读一个 SetupAPI 注册表属性为字符串。
QString devRegString(HDEVINFO set, SP_DEVINFO_DATA* data, DWORD prop)
{
    wchar_t buf[1024] = { 0 };
    DWORD type = 0;
    DWORD size = sizeof(buf);
    if (SetupDiGetDeviceRegistryPropertyW(set, data, prop, &type,
                                          reinterpret_cast<PBYTE>(buf), size, &size)) {
        return QString::fromWCharArray(buf);
    }
    return QString();
}

// 读 REG_MULTI_SZ 的 HardwareID 首个条目。
QString devHardwareIdFirst(HDEVINFO set, SP_DEVINFO_DATA* data)
{
    wchar_t buf[4096] = { 0 };
    DWORD type = 0;
    DWORD size = sizeof(buf);
    if (!SetupDiGetDeviceRegistryPropertyW(set, data, SPDRP_HARDWAREID, &type,
                                           reinterpret_cast<PBYTE>(buf), size, &size)) {
        return QString();
    }
    return QString::fromWCharArray(buf); // 多个以 \0 分隔，首个即本体。
}
#endif

} // namespace

// ── DeviceInfo ──────────────────────────────────────────────────────────────
QString DeviceInfo::typeName() const
{
    switch (type) {
    case DeviceType::Edl9008:      return QStringLiteral("高通 9008");
    case DeviceType::Diag:         return QStringLiteral("高通 Diag");
    case DeviceType::MtkPreloader: return QStringLiteral("MTK Preloader");
    case DeviceType::MtkBrom:      return QStringLiteral("MTK Brom");
    case DeviceType::Spreadtrum:   return QStringLiteral("展讯下载");
    case DeviceType::Adb:          return QStringLiteral("ADB");
    case DeviceType::Fastboot:     return QStringLiteral("Fastboot");
    default:                       return QStringLiteral("未知");
    }
}

// ── 已知 VID/PID 表（原样保留 C# 的 KnownDevices）────────────────────────────
QHash<QString, DeviceType> DeviceWatcher::knownDevices()
{
    static const QHash<QString, DeviceType> table = {
        // 高通 9008 EDL 模式（各种厂商）。key = VID + PID（4+4 位大写十六进制）。
        { QStringLiteral("05C69008"), DeviceType::Edl9008 },
        { QStringLiteral("05C69006"), DeviceType::Edl9008 },
        { QStringLiteral("05C69007"), DeviceType::Edl9008 },
        { QStringLiteral("05C69025"), DeviceType::Edl9008 },
        { QStringLiteral("05C69091"), DeviceType::Edl9008 },
        { QStringLiteral("05C6900E"), DeviceType::Diag },
        { QStringLiteral("2A709008"), DeviceType::Edl9008 },
        { QStringLiteral("2A709011"), DeviceType::Edl9008 },
        { QStringLiteral("22D99008"), DeviceType::Edl9008 },
        { QStringLiteral("1BBB9008"), DeviceType::Edl9008 },
        { QStringLiteral("19D29008"), DeviceType::Edl9008 },
        { QStringLiteral("0FCE9008"), DeviceType::Edl9008 },
        { QStringLiteral("27179008"), DeviceType::Edl9008 },
        { QStringLiteral("27179091"), DeviceType::Edl9008 },
        { QStringLiteral("2717F003"), DeviceType::Edl9008 },
        { QStringLiteral("18D19008"), DeviceType::Edl9008 },
        { QStringLiteral("04E86800"), DeviceType::Edl9008 },
        { QStringLiteral("04E8685D"), DeviceType::Edl9008 },
        { QStringLiteral("2C7C9008"), DeviceType::Edl9008 },
        { QStringLiteral("1EAC9008"), DeviceType::Edl9008 },
        { QStringLiteral("17EF9008"), DeviceType::Edl9008 },
        { QStringLiteral("1E689008"), DeviceType::Edl9008 },
        { QStringLiteral("1D4D9008"), DeviceType::Edl9008 },
        { QStringLiteral("2AB89008"), DeviceType::Edl9008 },

        // MTK
        { QStringLiteral("0E8D0003"), DeviceType::MtkPreloader },
        { QStringLiteral("0E8D2000"), DeviceType::MtkPreloader },
        { QStringLiteral("0E8D2001"), DeviceType::MtkPreloader },
        { QStringLiteral("0E8D2005"), DeviceType::MtkPreloader },
        { QStringLiteral("0E8D2006"), DeviceType::MtkPreloader },
        { QStringLiteral("0E8D0002"), DeviceType::MtkBrom },
        { QStringLiteral("0E8D20FF"), DeviceType::MtkBrom },

        // 展讯
        { QStringLiteral("17824D00"), DeviceType::Spreadtrum },
        { QStringLiteral("17824D01"), DeviceType::Spreadtrum },

        // Google ADB/Fastboot
        { QStringLiteral("18D1D00D"), DeviceType::Fastboot },
        { QStringLiteral("18D14EE0"), DeviceType::Adb },
        { QStringLiteral("18D14EE7"), DeviceType::Adb },
    };
    return table;
}

// ── DeviceWatcher ───────────────────────────────────────────────────────────
DeviceWatcher::DeviceWatcher(QObject* parent) : QObject(parent)
{
    m_pollTimer.setInterval(2000);
    connect(&m_pollTimer, &QTimer::timeout, this, &DeviceWatcher::onPoll);
}

DeviceWatcher::~DeviceWatcher()
{
    stop();
}

void DeviceWatcher::start()
{
    if (m_started) return;
    m_started = true;

    // 初始化端口集并立即扫描已连接设备。
    QSet<QString> ports;
    const QList<DeviceInfo> devices = enumerateDevices();
    for (const DeviceInfo& d : devices) {
        if (!d.portName.isEmpty()) {
            ports.insert(d.portName);
            testDevicePort(const_cast<DeviceInfo&>(d));
            emit deviceConnected(d);
        }
    }
    m_lastPorts = ports;
    if (!devices.isEmpty()) emit devicesChanged(devices);

    m_pollTimer.start();
}

void DeviceWatcher::stop()
{
    if (!m_started) return;
    m_started = false;
    m_pollTimer.stop();
}

void DeviceWatcher::onPoll()
{
    QSet<QString> ports;
    const QList<DeviceInfo> devices = enumerateDevices();
    for (const DeviceInfo& d : devices)
        if (!d.portName.isEmpty()) ports.insert(d.portName);

    // 新增端口。
    for (const QString& port : ports) {
        if (!m_lastPorts.contains(port)) {
            DeviceInfo d = deviceInfo(port);
            testDevicePort(d);
            emit deviceConnected(d);
        }
    }

    // 移除端口。
    for (const QString& port : m_lastPorts) {
        if (!ports.contains(port)) {
            DeviceInfo d;
            d.portName = port;
            emit deviceDisconnected(d);
        }
    }

    const bool changed = (ports != m_lastPorts);
    m_lastPorts = ports;
    if (changed) emit devicesChanged(devices);
}

// 枚举所有设备（对应 C# GetAllDevices）：SetupAPI 遍历 USB / Ports 类设备，
// 再补上系统串口列表里未识别的端口。
QList<DeviceInfo> DeviceWatcher::enumerateDevices() const
{
    QList<DeviceInfo> devices;
    QSet<QString> addedPorts;

#ifdef Q_OS_WIN
    // 遍历「设备接口类」根集：这里用 USB 设备类 GUID 加 Ports 类 GUID 两次抓取。
    // 用 GUID_DEVCLASS_PORT_NAME（串口）与 USB 类。为简单稳健，直接枚举所有设备
    // 再按 DeviceID 过滤含 USB / VID_ 的项。
    static const GUID kUsbClass = { 0xA5DCBF10, 0x6530, 0x11D2,
                                    { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

    HDEVINFO set = SetupDiGetClassDevsW(&kUsbClass, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData;
        ifData.cbSize = sizeof(ifData);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kUsbClass, i, &ifData); ++i) {
            DWORD req = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &ifData, nullptr, 0, &req, nullptr);
            if (req == 0) continue;
            QByteArray buf(req, 0);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA infoData;
            infoData.cbSize = sizeof(infoData);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &ifData, detail, req, &req, &infoData))
                continue;

            const QString deviceId = devRegString(set, &infoData, SPDRP_DEVICEDESC).isEmpty()
                                         ? devRegString(set, &infoData, SPDRP_FRIENDLYNAME)
                                         : devRegString(set, &infoData, SPDRP_DEVICEDESC);
            const QString caption = devRegString(set, &infoData, SPDRP_FRIENDLYNAME);
            const QString manufacturer = devRegString(set, &infoData, SPDRP_MFG);
            const QString service = devRegString(set, &infoData, SPDRP_SERVICE);
            const QString hardwareId = devHardwareIdFirst(set, &infoData);

            QString portName = extractPortName(caption);
            if (portName.isEmpty()) portName = extractPortName(deviceId);

            QString vid, pid;
            extractVidPid(hardwareId.isEmpty() ? deviceId : hardwareId, &vid, &pid);

            const DeviceType type =
                identifyDeviceType(vid, pid, caption, deviceId, manufacturer, service, deviceId);

            if (!portName.isEmpty() || type != DeviceType::Unknown) {
                DeviceInfo d;
                d.portName = portName;
                d.deviceId = deviceId;
                d.description = caption;
                d.manufacturer = manufacturer;
                d.vid = vid;
                d.pid = pid;
                d.type = type;
                if (!portName.isEmpty()) {
                    if (addedPorts.contains(portName)) continue;
                    addedPorts.insert(portName);
                }
                devices.append(d);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
#endif

    // 补上系统串口列表里未被 SetupAPI 命中的端口。
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        const QString portName = info.portName();
        if (portName.isEmpty() || addedPorts.contains(portName)) continue;
        addedPorts.insert(portName);

        DeviceInfo d;
        d.portName = portName;
        d.description = info.description();
        d.manufacturer = info.manufacturer();
        d.deviceId = info.systemLocation();

        QString vid, pid;
        if (info.hasVendorIdentifier())
            vid = QStringLiteral("%1").arg(info.vendorIdentifier(), 4, 16, QLatin1Char('0')).toUpper();
        if (info.hasProductIdentifier())
            pid = QStringLiteral("%1").arg(info.productIdentifier(), 4, 16, QLatin1Char('0')).toUpper();
        d.vid = vid;
        d.pid = pid;

        d.type = identifyDeviceType(vid, pid, d.description, d.description,
                                    d.manufacturer, QString(), d.deviceId);
        devices.append(d);
    }

    return devices;
}

QList<DeviceInfo> DeviceWatcher::allDevices() const
{
    return enumerateDevices();
}

DeviceInfo DeviceWatcher::deviceInfo(const QString& portName) const
{
    const QList<DeviceInfo> devices = enumerateDevices();
    for (const DeviceInfo& d : devices)
        if (d.portName == portName) return d;
    DeviceInfo d;
    d.portName = portName;
    return d;
}

QList<DeviceInfo> DeviceWatcher::findDevicesByType(DeviceType type) const
{
    QList<DeviceInfo> result;
    const QList<DeviceInfo> devices = enumerateDevices();
    for (const DeviceInfo& d : devices)
        if (d.type == type) result.append(d);
    return result;
}

bool DeviceWatcher::waitForDevice(DeviceType type, int timeoutMs, DeviceInfo* device,
                                  const std::function<bool()>& isCancelled)
{
    QElapsedTimer elapsed;
    elapsed.start();
    int scanCount = 0;

    while (elapsed.elapsed() < timeoutMs) {
        if (isCancelled && isCancelled()) return false;
        ++scanCount;

        const QList<DeviceInfo> all = enumerateDevices();
        QList<DeviceInfo> devices;
        for (const DeviceInfo& d : all)
            if (d.type == type) devices.append(d);

        // 9008 设备额外按 VID 匹配。
        if (type == DeviceType::Edl9008 && devices.isEmpty()) {
            for (const DeviceInfo& d : all) {
                if (d.vid == QStringLiteral("05C6") || d.vid == QStringLiteral("2717")
                    || d.vid == QStringLiteral("2A70")
                    || d.description.contains(QStringLiteral("9008"), Qt::CaseInsensitive)) {
                    devices.append(d);
                }
            }
        }

        if (!devices.isEmpty()) {
            DeviceInfo d = devices.first();
            testDevicePort(d);
            if (device) *device = d;
            return true;
        }

        // 动态扫描间隔：开始时更频繁，后来减慢。
        const int delay = scanCount <= 5 ? 200 : (scanCount <= 10 ? 500 : 1000);
        QThread::msleep(delay);
        if (QCoreApplication::instance())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return false;
}

QList<DeviceInfo> DeviceWatcher::allDevicesWithTest() const
{
    QList<DeviceInfo> devices = enumerateDevices();
    for (DeviceInfo& d : devices) testDevicePort(d);
    return devices;
}

bool DeviceWatcher::findAvailable9008Device(DeviceInfo* device, int maxRetries) const
{
    for (int retry = 0; retry < maxRetries; ++retry) {
        QList<DeviceInfo> devices = findDevicesByType(DeviceType::Edl9008);

        if (devices.isEmpty()) {
            const QList<DeviceInfo> all = enumerateDevices();
            for (const DeviceInfo& d : all) {
                if (d.type == DeviceType::Edl9008 || d.vid == QStringLiteral("05C6")
                    || d.vid == QStringLiteral("2717")
                    || d.description.contains(QStringLiteral("9008"), Qt::CaseInsensitive)
                    || d.description.contains(QStringLiteral("QDLOADER"), Qt::CaseInsensitive)) {
                    devices.append(d);
                }
            }
        }

        for (DeviceInfo& d : devices) {
            testDevicePort(d);
            if (d.isPortAvailable) {
                if (device) *device = d;
                return true;
            }
        }

        if (!devices.isEmpty()) {
            if (device) *device = devices.first(); // 端口被占用也返回第一个。
            return true;
        }

        if (retry < maxRetries - 1) QThread::msleep(500);
    }
    return false;
}

bool DeviceWatcher::findAvailableMtkDevice(DeviceInfo* device) const
{
    QList<DeviceInfo> devices = findDevicesByType(DeviceType::MtkPreloader);
    for (DeviceInfo& d : devices) {
        testDevicePort(d);
        if (d.isPortAvailable) {
            if (device) *device = d;
            return true;
        }
    }

    devices = findDevicesByType(DeviceType::MtkBrom);
    for (DeviceInfo& d : devices) {
        testDevicePort(d);
        if (d.isPortAvailable) {
            if (device) *device = d;
            return true;
        }
    }
    return false;
}

// 【非侵入式】只确认端口存在，绝不打开端口。
bool DeviceWatcher::testPortAvailable(const QString& portName, QString* status)
{
    if (status) status->clear();
    if (portName.isEmpty()) {
        if (status) *status = QStringLiteral("端口名为空");
        return false;
    }

    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        if (info.portName().compare(portName, Qt::CaseInsensitive) == 0) {
            if (status) *status = QStringLiteral("端口存在");
            return true;
        }
    }
    if (status) *status = QStringLiteral("端口不存在");
    return false;
}

// 【侵入式】真正打开端口 —— 会触发 qcusbser 的 DTR 边沿，可能吃掉一次性 HELLO_REQ。
bool DeviceWatcher::probePortByOpening(const QString& portName, QString* status)
{
    if (status) status->clear();
    if (portName.isEmpty()) {
        if (status) *status = QStringLiteral("端口名为空");
        return false;
    }

#ifdef Q_OS_WIN
    // 用 Win32 打开串口，确认可独占。
    const QString native = QStringLiteral("\\\\.\\") + portName;
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()),
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (status) {
            if (err == ERROR_ACCESS_DENIED)
                *status = QStringLiteral("端口被占用");
            else
                *status = QStringLiteral("错误: %1").arg(err);
        }
        return false;
    }
    CloseHandle(h);
    if (status) *status = QStringLiteral("端口可用");
    return true;
#else
    Q_UNUSED(status);
    return false;
#endif
}

void DeviceWatcher::testDevicePort(DeviceInfo& device)
{
    if (device.portName.isEmpty()) return;
    QString status;
    device.isPortAvailable = testPortAvailable(device.portName, &status);
    device.portStatus = status;
}

} // namespace kc
