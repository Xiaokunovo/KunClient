#include "kc/mtk/MtkDevice.h"

#include <QRegularExpression>

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

namespace kc::mtk {
namespace {

// 本地定义 GUID（避免依赖 devguid.h / initguid.h，这两个头在 MinGW 下不全）。
//   GUID_DEVCLASS_PORTS = {4D36E978-E325-11CE-BFC1-08002BE10318}
//   GUID_DEVINTERFACE_USB_DEVICE = {A5DCBF10-6530-11D2-901F-00C04FB951ED}
const GUID kGuidPorts = { 0x4D36E978, 0xE325, 0x11CE,
                          { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };

constexpr int kHwIdBufferChars = 1024;

/// 在 REG_MULTI_SZ 缓冲里找 "VID_0E8D&PID_xxxx"（大小写不敏感）。
/// 命中返回 true 并输出 vid/pid；pid=0 表示只认出了厂商。
bool findVidPid(const QVector<wchar_t>& multiSz, uint16_t* vid, uint16_t* pid)
{
    const QString text = QString::fromWCharArray(multiSz.constData()).toUpper();
    const int idx = text.indexOf(QStringLiteral("VID_0E8D&PID_"));
    if (idx < 0) return false;
    if (vid) *vid = kVidMtk;
    if (pid) {
        const QString hex = text.mid(idx + 13, 4);
        bool ok = false;
        const int value = hex.toInt(&ok, 16);
        *pid = ok ? static_cast<uint16_t>(value) : 0;
    }
    return true;
}

/// 读设备实例的 REG_MULTI_SZ 硬件 ID。
QVector<wchar_t> readHardwareIds(HDEVINFO infoSet, SP_DEVINFO_DATA* devInfo)
{
    QVector<wchar_t> buffer(kHwIdBufferChars, 0);
    DWORD got = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(infoSet, devInfo, SPDRP_HARDWAREID, nullptr,
                                           reinterpret_cast<PBYTE>(buffer.data()),
                                           static_cast<DWORD>(buffer.size()) * sizeof(wchar_t),
                                           &got)
        || got < 2) {
        return {};
    }
    // 双 null 结尾保护
    if (buffer.back() != 0) buffer.append(0);
    return buffer;
}

/// 从友好名里抠 COM 口号："MTK USB Port (COM7)" → "COM7"。
QString comPortFromFriendlyName(const QString& friendly)
{
    static const QRegularExpression re(QStringLiteral("\\bCOM\\d+\\b"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(friendly);
    return m.hasMatch() ? m.captured(0).toUpper() : QString();
}

DeviceStage stageFromPid(uint16_t pid)
{
    switch (pid) {
    case kPidBrom:      return DeviceStage::Brom;
    case kPidPreloader: return DeviceStage::Preloader;
    case kPidDa:        return DeviceStage::Da;
    default:            return DeviceStage::Disconnected;
    }
}

} // namespace

QVector<MtkUsbDevice> scanMtkDevices()
{
    QVector<MtkUsbDevice> result;

    HDEVINFO infoSet = SetupDiGetClassDevsW(&kGuidPorts, nullptr, nullptr, DIGCF_PRESENT);
    if (infoSet == INVALID_HANDLE_VALUE) return result;

    SP_DEVINFO_DATA devInfo{};
    devInfo.cbSize = sizeof(devInfo);

    for (DWORD index = 0; SetupDiEnumDeviceInfo(infoSet, index, &devInfo); ++index) {
        MtkUsbDevice device;

        // ① 端口设备自身的硬件 ID（有的驱动直接写 USB 硬件 ID）
        uint16_t vid = 0;
        uint16_t pid = 0;
        const QVector<wchar_t> ownIds = readHardwareIds(infoSet, &devInfo);
        if (!findVidPid(ownIds, &vid, &pid) || vid != kVidMtk) {
            // ② 查父设备（usbser 端口的父就是 USB 设备本身）
            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, devInfo.DevInst, 0) != CR_SUCCESS) continue;
            QVector<wchar_t> parentIds(kHwIdBufferChars, 0);
            // 签名（cfgmgr32.h）：(DEVINST, ULONG, PULONG pulRegDataType, PVOID Buffer,
            //                       PULONG pulLength, ULONG ulFlags)
            // 第 5 参是**入出**长度指针，第 6 参是 flags —— 原写法把长度值当指针传、
            // 把 &size 当 flags 传，两者对调导致编译失败。
            ULONG length = static_cast<ULONG>(parentIds.size()) * sizeof(wchar_t);
            if (CM_Get_DevNode_Registry_PropertyW(parent, CM_DRP_HARDWAREID, nullptr,
                                                  parentIds.data(), &length, 0)
                    != CR_SUCCESS
                || length < 2) {
                continue;
            }
            if (!findVidPid(parentIds, &vid, &pid) || vid != kVidMtk) continue;
        }

        const DeviceStage stage = stageFromPid(pid);
        if (stage == DeviceStage::Disconnected) continue;

        // 友好名拿 COM 口号（无端口/无驱动时留空，UI 明示）
        wchar_t friendly[512] = {};
        if (SetupDiGetDeviceRegistryPropertyW(infoSet, &devInfo, SPDRP_FRIENDLYNAME, nullptr,
                                              reinterpret_cast<PBYTE>(friendly), sizeof(friendly),
                                              nullptr)) {
            device.comPort = comPortFromFriendlyName(QString::fromWCharArray(friendly));
        }

        device.stage = stage;
        device.vid = vid;
        device.pid = pid;
        wchar_t instanceId[512] = {};
        if (CM_Get_Device_IDW(devInfo.DevInst, instanceId, 512, 0) == CR_SUCCESS) {
            device.devicePath = QString::fromWCharArray(instanceId);
        }
        result.append(device);
    }

    SetupDiDestroyDeviceInfoList(infoSet);
    return result;
}

bool findMtkDevice(DeviceStage atLeast, MtkUsbDevice* out)
{
    const QVector<MtkUsbDevice> devices = scanMtkDevices();

    const MtkUsbDevice* best = nullptr;
    for (const MtkUsbDevice& device : devices) {
        if (atLeast != DeviceStage::Disconnected
            && static_cast<int>(device.stage) < static_cast<int>(atLeast)) {
            continue;
        }
        if (!best) {
            best = &device;
            continue;
        }
        // 优先：有 COM 口 > 阶段更高
        const bool hasPort = !device.comPort.isEmpty();
        const bool bestHasPort = !best->comPort.isEmpty();
        if (hasPort != bestHasPort) {
            if (hasPort) best = &device;
            continue;
        }
        if (static_cast<int>(device.stage) > static_cast<int>(best->stage)) best = &device;
    }

    if (!best) return false;
    if (out) *out = *best;
    return true;
}

} // namespace kc::mtk
