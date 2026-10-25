#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkTypes — MTK（联发科）刷写功能的公共类型与协议常量
//
//  移植参照：SP_Flash_Tool_src_6.2228（C++/Qt 应用层源码，用户提供）。
//  ⚠️ 架构决策（2026-10-12）：
//    上游 SP Flash Tool 的协议核心是 **32 位闭源二进制 flash.dll**（纯 C API、
//    无 Qt 依赖、x86）。KunClient 是 x64 单文件静态构建 —— ① x86 DLL 无法进
//    x64 进程；② 捆绑 MediaTek 闭源组件有明确许可风险（其 SLA_Challenge 正是
//    许可执行件）。因此本目录为**自研协议层**：以上游源码为行为蓝本（命令词汇、
//    scatter 格式、设备阶段语义逐条对照），字节级协议取自社区公开实现
//    （bkerler/mtkclient，GPLv3；本仓库仅对照其**协议常量与线格式**，不含其代码）。
//
//  🔴 2026-10-13 协议重写（v2）—— 上一版的 BROM 层整体是错的，本文件是唯一口径：
//    · 握手不是「魔数 0xE0 → 应答 0x5A」，而是 host 逐字节发 A0 0A 50 05、
//      device 逐字节回**按位取反**（~b & 0xFF）；
//    · 命令**不是** [cmd][len:4 LE][payload] 帧，而是**回显式**：发命令字节 →
//      读回同一字节做确认；多字节字段一律**大端**（">I" / ">H"）；
//    · USB 路径波特率 921600，串口（CDC）路径 115200；
//    · scatter 的 physical_start_addr 是**存储偏移**，刷写必须经 DA 的
//      WRITE_DATA（不是 BROM 的 0xD4 内存写）。
//
//  🔴 状态标注：全部协议常量取自社区实现，**本机无真机可验**。真机行为不符时
//    先改这里（或 MtkBrom.cpp 的线格式），不要散着打补丁。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace kc::mtk {

// ── 存储类型（口径对齐上游 HW_StorageType_E / mtkclient StorageType）────────
//  ⚠️ 这是 **DA 硬件类型**（MTK_DA_HW_STORAGE_*），不是 XFlash 参数里的
//     DaStorage —— 后者见下方 enum class DaStorage，两者数值不同，别混用。
enum class StorageType {
    None = 0,
    Nor = 1,
    Nand = 2,
    Emmc = 3,
    Sdmmc = 4,
    Ufs = 5,
};

inline QString storageTypeName(StorageType type)
{
    switch (type) {
    case StorageType::Nor:   return QStringLiteral("NOR");
    case StorageType::Nand:  return QStringLiteral("NAND");
    case StorageType::Emmc:  return QStringLiteral("EMMC");
    case StorageType::Sdmmc: return QStringLiteral("SDMMC");
    case StorageType::Ufs:   return QStringLiteral("UFS");
    case StorageType::None:  break;
    }
    return QStringLiteral("-");
}

/// 名称 → 存储类型。兼容三种写法："EMMC" / "HW_STORAGE_EMMC" / "emmc"（大小写不敏感）。
inline StorageType storageTypeFromName(const QString& rawName)
{
    const QString name = rawName.trimmed().toUpper();
    if (name.endsWith(QStringLiteral("UFS")))    return StorageType::Ufs;
    if (name.endsWith(QStringLiteral("EMMC")))   return StorageType::Emmc;
    if (name.endsWith(QStringLiteral("SDMMC")))  return StorageType::Sdmmc;
    if (name.endsWith(QStringLiteral("NAND")))   return StorageType::Nand;
    if (name.endsWith(QStringLiteral("NOR")))    return StorageType::Nor;
    return StorageType::None;
}

// ── 设备阶段（上游 CONN_END_STAGE 的对外可见子集）───────────────────────────
enum class DeviceStage {
    Disconnected,  // 未见 MTK 设备
    Brom,          // BROM 模式（USB PID 0x0003 及变体）：只认握手
    Preloader,     // Preloader/下载代理已驻留（USB PID 0x2000）
    Da,            // DA（Download Agent）已加载（USB PID 0x2001）：高速通道
};

inline QString deviceStageName(DeviceStage stage)
{
    switch (stage) {
    case DeviceStage::Brom:      return QStringLiteral("BROM");
    case DeviceStage::Preloader: return QStringLiteral("Preloader");
    case DeviceStage::Da:        return QStringLiteral("DA");
    case DeviceStage::Disconnected: break;
    }
    return QStringLiteral("未连接");
}

// ── USB 识别常量（MTK 厂牌 VID，全社区实现一致）────────────────────────────
constexpr uint16_t kVidMtk = 0x0E8D;
constexpr uint16_t kPidBrom = 0x0003;      // BROM 模式（主）
constexpr uint16_t kPidPreloader = 0x2000; // Preloader
constexpr uint16_t kPidDa = 0x2001;        // DA 高速模式

/// BROM 阶段可能出现的全部 PID（mtkclient Port.run_handshake 的 brom_pids）。
/// 非该集合的 PID 需要先单独发一个 0xA0 做探测（见 MtkBrom::handshake）。
inline bool isBromPid(uint16_t pid)
{
    switch (pid) {
    case 0x0003:
    case 0xF200:
    case 0xD1E9:
    case 0xD1E2:
    case 0xD1EC:
    case 0xD1DD:
        return true;
    default:
        return false;
    }
}

// ── BROM 协议常量（回显式；来源：mtkclient Port/preloader + penumbra 文档）───
//  🔴 波特率有两个口径：USB bulk 路径 set_line_coding(921600)，
//     串口（usbser/CDC → COMx）路径 SerialClass 开在 115200。
//     KunClient 走 COM 口（Windows 下 MTK 口经 usbser 呈现），故取 **115200**；
//     若真机在 115200 握不上手，改这个常量试 921600（不改其余逻辑）。
constexpr int kBromBaud = 115200;

/// 握手串：host 逐字节发；device 逐字节回 ~b & 0xFF（= 5F F5 AF FA）。
/// ⚠️ 这是**回显**语义，不是"魔数 + 固定应答"。上一版把它写成 0xE0/0x5A 是错的。
constexpr uint8_t kBromHandshakeBytes[4] = { 0xA0, 0x0A, 0x50, 0x05 };
constexpr int kBromHandshakeLen = 4;

/// BROM 单条命令字（mtkclient Preloader.Cmd）。
enum BromCmd : uint8_t {
    kCmdRead16           = 0xD0,
    kCmdRead32           = 0xD1,
    kCmdWrite16          = 0xD2,
    kCmdWrite16NoEcho    = 0xD3,
    kCmdWrite32          = 0xD4,
    kCmdJumpDa           = 0xD5,
    kCmdJumpBl           = 0xD6,
    kCmdSendDa           = 0xD7,
    kCmdGetTargetConfig  = 0xD8,
    kCmdSendEnvPrepare   = 0xD9,
    kCmdBromRegisterAccess = 0xDA,
    kCmdJumpDa64         = 0xDE,
    kCmdGetHwSwVer       = 0xFC,  // GET_HW_SW_VER
    kCmdGetHwCode        = 0xFD,  // GET_HW_CODE（识别芯片，读 4 字节 → 两个 u16 大端）
    kCmdGetBlVer         = 0xFE,  // GET_BL_VER
    kCmdGetVersion       = 0xFF,  // GET_VERSION
};

/// SEND_DA 之后设备若返回此状态 = 要求 SLA（Secure Legacy Auth / 签名校验）。
constexpr uint16_t kStatusSlaRequired = 0x1D0D;

/// JUMP_DA 之后设备侧会发 1 字节 0xC0 表示 DA 已起来（DA 自身再回 SYNC 包）。
constexpr uint8_t kDaReadyByte = 0xC0;

/// BROM 阶段单次 USB 写入上限（无 EP 信息时的保守回退，见 mtkclient upload_data）。
constexpr int kBromFallbackMaxPacket = 0x400;

// ── DA 文件（MTK_AllInOne_DA.bin / MTK_DA_*.bin）结构 ───────────────────────
//  来源：mtkclient DA/daconfig.py parse_da_loader + DA.__init__。
constexpr uint32_t kDaFileHeaderSize = 0x68;   // magic + ID 区，含 "MTK_DA_v6" 判定
constexpr uint32_t kDaEntryCountOffset = 0x68; // 此处读 4 字节 LE = DA 条目数
constexpr uint32_t kDaEntryTableOffset = 0x6C; // DA 条目表起点
constexpr uint32_t kDaEntrySize = 0xDC;        // 新式条目长度（v5/v6）
constexpr uint32_t kDaEntrySizeLegacy = 0xD8;  // 旧式条目长度
constexpr uint16_t kDaEntryMagic = 0xDADA;     // 条目起始的 u16 magic（LE 存为 DA DA）

// ── XFlash DA2 会话常量（来源：mtkclient DA/xflash/xflash_lib.py + xflash_param.py）─
constexpr uint32_t kXflashMagic = 0xFEEEEEEF;      // 每个包头中的 magic
constexpr uint32_t kXflashSyncSignal = 0x434E5953; // "SYNC" 小端
constexpr uint32_t kXflashPacketHeaderLen = 12;    // <III = magic / datatype / length
constexpr int kXflashParamChunk = 0x200;           // send_param 每块字节数
constexpr uint32_t kXflashExtensionsAddress = 0x4FFF0000;
constexpr uint32_t kXflashMaxPacketFallback = 0x100000; // get_packet_length 失败时的读回退（1MB）

/// XFlash 命令字（mtkclient Cmd）。
enum class XCmd : uint32_t {
    Unknown = 0x010000,
    Download = 0x010001,
    Upload = 0x010002,
    Format = 0x010003,
    WriteData = 0x010004,
    ReadData = 0x010005,
    FormatPartition = 0x010006,
    Shutdown = 0x010007,
    BootTo = 0x010008,
    DeviceCtrl = 0x010009,
    InitExtRam = 0x01000A,
    SwitchUsbSpeed = 0x01000B,
    ReadOtpZone = 0x01000C,
    WriteOtpZone = 0x01000D,
    WriteEfuse = 0x01000E,
    ReadEfuse = 0x01000F,
    SetupEnvironment = 0x010100,
    SetupHwInitParams = 0x010101,

    SetBatteryOpt = 0x020002,
    SetChecksumLevel = 0x020003,
    SetResetKey = 0x020004,
    SetHostInfo = 0x020005,
    SetMetaBootMode = 0x020006,

    GetEmmcInfo = 0x040001,
    GetNandInfo = 0x040002,
    GetNorInfo = 0x040003,
    GetUfsInfo = 0x040004,
    GetDaVersion = 0x040005,
    GetExpireData = 0x040006,
    GetPacketLength = 0x040007,
    GetRandomId = 0x040008,
    GetPartitionTblCata = 0x040009,
    GetConnectionAgent = 0x04000A,
    GetUsbSpeed = 0x04000B,
    GetRamInfo = 0x04000C,
    GetChipId = 0x04000D,
    GetExpireDate = 0x040011,
    GetDramType = 0x040012,
    GetDevFwInfo = 0x040013,
    SlaEnabledStatus = 0x040016,

    StartDlInfo = 0x080001,
    EndDlInfo = 0x080002,
    CcOptionalDownloadAct = 0x080005,
};

/// 包头 DataType（mtkclient DataType）。
enum class XDataType : uint32_t {
    ProtocolFlow = 1,
    Message = 2,
};

/// XFlash 参数里的 storage 值（mtkclient DaStorage）。**不是** StorageType。
enum class DaStorage : uint32_t {
    Emmc = 0x1,
    Sdmmc = 0x2,
    Nand = 0x10,
    NandSlc = 0x11,
    NandMlc = 0x12,
    NandTlc = 0x13,
    NandAmlc = 0x14,
    NandSpi = 0x15,
    Nor = 0x20,
    NorSerial = 0x21,
    NorParallel = 0x22,
    Ufs = 0x30,
};

/// eMMC 分区（mtkclient EmmcPartitionType）。
enum class EmmcPartitionType : uint32_t {
    Boot1 = 1,
    Boot2 = 2,
    Rpmb = 3,
    Gp1 = 4,
    Gp2 = 5,
    Gp3 = 6,
    Gp4 = 7,
    User = 8,
    End = 9,
};

/// UFS 分区（mtkclient UFSPartitionType）。
enum class UfsPartitionType : uint32_t {
    Boot1 = 1,
    Boot2 = 2,
    User = 3,
    Rpmb = 4,
};

/// 长操作进度状态：0x40040004 = 进行中（后面跟毫秒或百分比），0x40040005 = 完成。
constexpr uint32_t kXflashStatusContinue = 0x40040004;
constexpr uint32_t kXflashStatusComplete = 0x40040005;

/// 错误码编码：severity<<30 | domain<<16 | code（mtkclient ErrorHandler）。
/// severity: 0=Success 1=Info 2=Warning 3=Error（存的是 >>30 后的值）。
inline uint32_t xflashStatusCode(uint32_t status) { return status & 0xFFFFu; }

// ── scatter 分区条目（经典 .txt 与 v6 XML 的统一产物）───────────────────────
struct ScatterPartition {
    QString name;             // 分区名（PRELOADER / boot / …）
    QString fileName;         // 镜像文件名（相对 scatter 所在目录）
    QString absoluteFilePath; // 解析后的绝对路径（可能不存在）
    uint64_t startAddress = 0;// physical_start_addr（**存储偏移**，不是内存地址）
    uint64_t size = 0;        // partition_size
    QString region;           // EMMC_USER / EMMC_BOOT_1 / UFS_LU0 / …
    StorageType storage = StorageType::None;
    QString operationType;    // BOOTLOADERS / UPDATE / BASIC / RESERVED …
    bool isDownload = false;  // scatter 标记是否参与下载
    bool isReserved = false;  // 保留分区
    bool enabled = false;     // UI 勾选态（默认跟随 is_download）
    bool fileExists = false;  // absoluteFilePath 是否真实存在
};

// ── scatter 解析产物 ────────────────────────────────────────────────────────
struct ScatterInfo {
    bool valid = false;
    QString errorMessage;
    QString chipName;          // platform（MT6768 / MT6785 …）
    StorageType storage = StorageType::None;
    QString storageName;       // 原始 storage 串
    QVector<ScatterPartition> partitions;
    QStringList protectedParts; // BINREGION / PROTECTED 类分区（上游保护名单口径）
    QStringList warnings;       // 非致命问题（如被跳过的无名分区块）——页面按警告显示
    bool skipPtOperation = false;
};

// ── scatter region → DA 三元组 ──────────────────────────────────────────────
//  刷写/擦除的 XFlash 参数是 pack("<IIQQ", storage, parttype, addr, size)。
//  region 串决定 parttype（LUN / boot 区），addr 是**该 region 内的偏移**。
struct DaTarget {
    DaStorage storage = DaStorage::Emmc; // XFlash 参数 storage
    uint32_t partType = 0;               // EmmcPartitionType / UfsPartitionType / NandCellUsage
    QString label;                       // 供日志显示（如 "EMMC-USER"）
};

/// 把 scatter 的 region 串映射成 DA 目标。
///   region 取值（MTK scatter 惯例）：EMMC_USER / EMMC_BOOT_1 / EMMC_BOOT_2 /
///   EMMC_RPMB / EMMC_GP1..4 / UFS_LU0..LU5 / NAND / NOR（大小写不敏感）。
///   storageHint 是 scatter 顶层的 storage 字段，region 认不出来时作为兜底。
/// 🔴 UFS 的 LU→parttype 映射沿用 mtkclient 口径（LU0=user / LU1=boot1 /
///   LU2=boot2 / LU3=rpmb）；不同 DA 版本对 LU 的解释可能不同，真机需复核。
bool regionToTarget(const QString& region, StorageType storageHint, DaTarget* out);

} // namespace kc::mtk
