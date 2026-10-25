#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ScatterParser — MTK scatter 文件解析（双格式）
//
//  scatter 是 MTK 线刷包的「分区表 + 镜像清单」，两种形态都要吃：
//   ① 经典 .txt（用户从固件包拿到的就是这种，格式公开、稳定）：
//        - partition_index: 0x0
//          partition_name: PRELOADER
//          file_name: preloader_mt6768.bin
//          is_download: true
//          physical_start_addr: 0x0
//          partition_size: 0x80000
//          region: EMMC_BOOT_1
//          storage: HW_STORAGE_EMMC
//          operation_type: BOOTLOADERS
//      兼容「字段全带 - 前缀」与「字段缩进无前缀」两种变体（不同打包工具产出不同）。
//   ② v6 XML（上游 SP Flash Tool 6.2228 的内部格式，蓝本 XML/Parser/ScatterXMLParser.cpp
//      逐字段对照移植）：GENERAL/PLATFORM、STORAGE_TYPE name=… 下挂
//      PARTITION_INDEX name=…（is_download / partition_name / physical_start_addr /
//      file_name / op_type / partition_size / region）。
//
//  解析是纯函数、无 IO 副作用，可单测（见 tests/mtk/ScatterParserTests.cpp）。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"

#include <QString>

namespace kc::mtk {

class ScatterParser {
public:
    /// 解析文件（按扩展名判别：.xml 走 XML 解析，其余按经典 .txt）。
    /// 失败时返回 valid=false + errorMessage。
    static ScatterInfo parseFile(const QString& filePath);

    /// 经典 .txt 文本解析。baseDir 用于把 file_name 解析成绝对路径并测存在性。
    static ScatterInfo parseClassicText(const QString& text, const QString& baseDir);

    /// v6 XML 文本解析。baseDir 语义同上。
    static ScatterInfo parseXmlText(const QString& text, const QString& baseDir);

    /// "0x1F"/"0X1f"/"31"/" 0x20 " → 数值。失败返回 false。
    static bool parseNumber(const QString& text, uint64_t* out);
};

} // namespace kc::mtk
