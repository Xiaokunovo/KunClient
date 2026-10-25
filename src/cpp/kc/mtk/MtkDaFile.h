#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkDaFile — MTK DA 文件（MTK_AllInOne_DA.bin / MTK_DA_*.bin）解析
//
//  结构（来源：mtkclient DA/daconfig.py parse_da_loader + DA.__init__）：
//    0x00 … 0x68  文件头（含 "MTK_DA_v6" 判定；"MTK_DOWNLOAD_AGENT" magic 在前）
//    0x68 … 0x6C  u32 LE = DA 条目数
//    0x6C …       DA 条目表，每条 0xDC（新式）或 0xD8（旧式）
//    条目（全部 u16 LE）：
//      [0x00] magic(0xDADA) [0x02] hw_code [0x04] hw_sub_code [0x06] hw_version
//      [0x08] sw_version [0x0A] reserved        ← 仅新式有这两字段
//      [..]   pagesize [..] reserved3 [..] entry_region_index [..] entry_region_count
//      然后 entry_region_count × EntryRegion（5 × u32 LE）：
//        buf(文件偏移) / len / start_addr(RAM 目标地址) / start_offset / sig_len
//
//  region 下标语义（xflash_lib.upload_da1）：
//    region[1] = DA1（SEND_DA 上传 + JUMP_DA 跳转）
//    region[2] = DA2（DA 起来后 BOOT_TO 引导到 stage2）
//    region[0] 是引导/占位区，不直接上传。
//
//  🔴 纯解析，无 IO 副作用；单测用合成 DA 文件覆盖（见 tests/MtkProtocolTest.cpp）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

namespace kc::mtk {

/// DA 条目里的一个 region 描述。
struct DaRegion {
    uint32_t buf = 0;         // 在 DA 文件内的偏移
    uint32_t len = 0;         // 字节长度（含尾部签名）
    uint32_t startAddr = 0;   // 目标内存地址（JUMP_DA / BOOT_TO 用）
    uint32_t startOffset = 0; // 备用起始偏移
    uint32_t sigLen = 0;      // 尾部签名字节数

    bool isSane(quint64 fileSize) const;
};

/// 一个 SoC 的 DA 条目。
struct DaEntry {
    uint16_t magic = 0;
    uint16_t hwCode = 0;
    uint16_t hwSubCode = 0;
    uint16_t hwVersion = 0;
    uint16_t swVersion = 0;
    uint16_t pagesize = 0;
    uint16_t entryRegionIndex = 0;
    uint16_t entryRegionCount = 0;
    bool v6 = false;     // 文件里含 "MTK_DA_v6"（XML/DA2 体系）
    bool legacy = false; // 旧式条目（0xD8，无 sw_version 字段）
    QVector<DaRegion> regions;

    const DaRegion* regionAt(int index) const;
    const DaRegion* da1() const { return regionAt(1); }
    const DaRegion* da2() const { return regionAt(2); }
};

class DaFile {
public:
    /// 解析 DA 文件；返回全部条目（按文件顺序）。
    /// 解析不出任何条目 → 返回 false 并写 *error。
    static bool parse(const QString& path, QVector<DaEntry>* entries, QString* error);

    /// 取某个 region 的原始字节（已按 buf/len 定位并做边界校验）。
    static bool readRegion(const QString& path, const DaRegion& region, QByteArray* out, QString* error);

    /// 按 hw code 选条目：完全匹配优先，其次取 hw_version 最大者。
    /// 找不到时返回 nullptr 并写 *why（给 UI 直接展示）。
    static const DaEntry* pickForHwCode(const QVector<DaEntry>& entries, uint16_t hwCode, QString* why);

    /// 文件是否像 DA：读前 0x20 字节，看 "MTK_DOWNLOAD_AGENT" / "MTK_DA" 前缀。
    static bool looksLikeDaFile(const QString& path, QString* error);
};

/// 从 preloader 镜像里抠出 DRAM 参数（EMI）。
///   来源：mtkclient DA/daconfig.py m_extract_emi。两种形态：
///   ① 先找 4D 4D 4D 01 38 00 00 00，按 mlen/siglen 截断再取尾部 dramsize 段；
///   ② 找 "MTK_BLOADER_INFO_v"：在开头 → 整个 data 就是 EMI；否则取 "MTK_BIN"+0xC 之后。
///   抠不出来返回空（调用方只警告，不判死 —— 部分平台不需要 EMI）。
///   放在这里而不是 MtkDeviceSession，是为了让协议单测不需要链接 win32 设备层。
QByteArray extractEmiFromPreloader(const QByteArray& preloader);

} // namespace kc::mtk
