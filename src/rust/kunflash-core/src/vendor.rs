//! 厂商（vendor）维度：EDL 刷写要适配的四家 —— 小米 / 欧加 / 联想 / 红魔。
//!
//! ## 为什么要单独建一个模块
//!
//! 四家的差异**不在刷写协议本身**（都是高通 Sahara → Firehose），而在三段：
//!
//! | 环节 | 差异 |
//! |---|---|
//! | ① 进 9008 | 小米：`getvar token` → 填 sig → `stage` → `oem edl`；其余三家多为 fastboot `oem edl` 或直插 9008 |
//! | ② loader 形态 | **小米/欧加/联想：单个 Hexagon ELF（`.elf`/`.mbn`）**；**红魔：`.melf` 多镜像容器**（见 [`crate::melf`]） |
//! | ③ 签名 / 鉴权 | 小米要 FDL Auth 填 sig；欧加另需 ofp/ops 包解密；联想/红魔按各自 loader 的签名要求 |
//!
//! 所以 vendor 不是一个「用来打日志的标签」，它要**驱动这三段的分支**。
//!
//! ## 当前掌握程度（诚实标注，别把待定当已实现）
//!
//! - 小米、欧加：**已有一版能用的 C# 实现**（`XiaomiEdlPage` / `OfpDecryptor` / `OpsDecryptor`），
//!   移植时以它们为准。
//! - 红魔：拿到了真实 loader（`devprg_NP05J.melf`），容器已拆清；
//!   **loader 之后的鉴权/下发细节尚无真机验证**。
//! - 联想：**只有用户一句「进 9008 和红魔/联想一样、刷写包都差不多」**，没有样本、没有代码。
//!   所以这里只把它接进枚举、接口留好，**实现状态标成 [`Support::InterfaceOnly`]**。
//!
//! ⚠️ 上表只描述「已知的差异点」，不代表流程细节都对 —— 每一条都要在真机上过一遍才作数。

use crate::elf::has_magic;
use crate::melf;

/// 支持的四家厂商。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Vendor {
    Xiaomi,
    /// 欧加（OPPO / 一加 / realme）。
    Oplus,
    Lenovo,
    /// 红魔（努比亚）。
    RedMagic,
}

/// 实现状态 —— 直接把「有没有验过」写进代码，避免后面把待定当已完成。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Support {
    /// 有可参考的既有实现（移植即可）。
    Ported,
    /// 有真实样本、容器/格式已实测，但端到端未上真机。
    SampleVerified,
    /// 只有接口与枚举，实现待补。
    InterfaceOnly,
}

impl Vendor {
    pub const ALL: [Vendor; 4] = [Vendor::Xiaomi, Vendor::Oplus, Vendor::Lenovo, Vendor::RedMagic];

    /// FFI / 配置里用的稳定标识。**别改这些字符串**：它们会出现在 json 配置与日志里。
    pub fn as_str(self) -> &'static str {
        match self {
            Vendor::Xiaomi => "xiaomi",
            Vendor::Oplus => "oplus",
            Vendor::Lenovo => "lenovo",
            Vendor::RedMagic => "redmagic",
        }
    }

    /// 中文显示名（UI 用；跨 FFI 传的是 [`Self::as_str`]）。
    pub fn display_name(self) -> &'static str {
        match self {
            Vendor::Xiaomi => "小米",
            Vendor::Oplus => "欧加",
            Vendor::Lenovo => "联想",
            Vendor::RedMagic => "红魔",
        }
    }

    pub fn support(self) -> Support {
        match self {
            Vendor::Xiaomi | Vendor::Oplus => Support::Ported,
            Vendor::RedMagic => Support::SampleVerified,
            Vendor::Lenovo => Support::InterfaceOnly,
        }
    }

    /// loader 预期形态。**只是提示**：真正判定一律走内容嗅探（[`LoaderFormat::detect`]），
    /// 因为扩展名在四家手里混用（`.elf` / `.mbn` / `.bin` / `.melf` 都出现过）。
    pub fn expected_loader(self) -> LoaderFormat {
        match self {
            Vendor::RedMagic => LoaderFormat::MelfContainer,
            _ => LoaderFormat::SingleElf,
        }
    }

    pub fn parse(s: &str) -> Result<Vendor, String> {
        let t = s.trim().to_ascii_lowercase();
        Vendor::ALL
            .into_iter()
            .find(|v| v.as_str() == t)
            .ok_or_else(|| format!("未知厂商：{s}（可选：xiaomi/oplus/lenovo/redmagic）"))
    }
}

/// loader 文件的容器形态。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoaderFormat {
    /// 单个 Hexagon ELF（可能叫 `.elf` / `.mbn` / `.bin`）。小米/欧加/联想 的常态。
    SingleElf,
    /// MELF 多镜像容器（红魔 `devprg_*.melf`）。见 [`crate::melf`]。
    MelfContainer,
    /// 不是 ELF。厂商若给的是别的封装（比如压缩包里的 `.bin`），走这条。
    Unknown,
}

impl LoaderFormat {
    pub fn as_str(self) -> &'static str {
        match self {
            LoaderFormat::SingleElf => "single-elf",
            LoaderFormat::MelfContainer => "melf",
            LoaderFormat::Unknown => "unknown",
        }
    }
}

/// **按内容**判定 loader 形态。
///
/// 🔴 不要按扩展名判定：同一个 `.mbn` 在不同厂商手里可能是单 ELF 也可能是容器，
/// 而红魔的 `.melf` 本身就是个 ELF 文件（`7f 45 4c 46` 开头），
/// 所以「是不是 melf」必须靠**内层有没有编程器镜像**来定，不能靠后缀。
pub fn detect_loader(data: &[u8]) -> LoaderFormat {
    if !has_magic(data, 0) {
        return LoaderFormat::Unknown;
    }
    if melf::is_melf(data) {
        LoaderFormat::MelfContainer
    } else {
        LoaderFormat::SingleElf
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::melf::synth_melf;

    #[test]
    fn all_four_vendors_are_wired() {
        assert_eq!(Vendor::ALL.len(), 4, "小米/欧加/联想/红魔 四家一个都不能少");
        let names: Vec<_> = Vendor::ALL.iter().map(|v| v.as_str()).collect();
        assert_eq!(names, vec!["xiaomi", "oplus", "lenovo", "redmagic"]);
    }

    #[test]
    fn parse_accepts_case_and_space() {
        assert_eq!(Vendor::parse(" Xiaomi ").unwrap(), Vendor::Xiaomi);
        assert_eq!(Vendor::parse("REDMAGIC").unwrap(), Vendor::RedMagic);
        assert!(Vendor::parse("htc").is_err());
    }

    #[test]
    fn support_status_is_honest() {
        // 联想没有样本 → 必须标 InterfaceOnly，别把它当已实现
        assert_eq!(Vendor::Lenovo.support(), Support::InterfaceOnly);
        assert_eq!(Vendor::RedMagic.support(), Support::SampleVerified);
    }

    #[test]
    fn detect_loader_is_content_based_not_extension_based() {
        // melf 容器（内层有编程器）→ MelfContainer
        assert_eq!(detect_loader(&synth_melf(12, true)), LoaderFormat::MelfContainer);
        // 只有外层 stub 的 0xF3 ELF → 不是容器，按单 ELF 处理
        assert_eq!(detect_loader(&synth_melf(2, false)), LoaderFormat::SingleElf);
        // 非 ELF
        assert_eq!(detect_loader(b"not an elf at all"), LoaderFormat::Unknown);
    }

    #[test]
    fn redmagic_expects_melf_others_single_elf() {
        assert_eq!(Vendor::RedMagic.expected_loader(), LoaderFormat::MelfContainer);
        for v in [Vendor::Xiaomi, Vendor::Oplus, Vendor::Lenovo] {
            assert_eq!(v.expected_loader(), LoaderFormat::SingleElf);
        }
    }
}
