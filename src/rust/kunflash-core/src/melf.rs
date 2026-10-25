//! `.melf`（**M**ulti-image **ELF**）容器解析 —— 红魔/努比亚这条线的引导文件。
//!
//! ## 实测结构（2026-10-08，样本 `devprg_NP05J.melf`，1,659,300 B）
//!
//! 这不是猜测，是逐字段量出来的：
//!
//! | # | 偏移 | 类别 | machine | phnum | 长度 | 判断 |
//! |---|---|---|---|---|---|---|
//! | 0 | `0x0` | ELF32 | `0xF3` 高通 | 2 | 2,308 | 外层头 + Hexagon stub |
//! | 1 | `0x904` | **ELF32** | **`0xF3`** | **12** | 195,696 | ← **firehose 编程器** |
//! | 2 | `0x30574` | ELF64 | `0xB7` AArch64 | 9 | 961,504 | ARM64 侧镜像 |
//! | 3 | `0x11B154` | ELF64 | `0xB7` AArch64 | 7 | 303,096 | ARM64 侧镜像 |
//! | 4 | `0x16514C` | ELF64 | `0x1` | 9 | 196,696 | 未知侧 |
//!
//! 三条关键事实（都影响实现方式，别凭印象改）：
//!
//! 1. **容器里没有镜像目录表**。我把每个内嵌偏移（`0x904`…）与各镜像长度按 32 位小端
//!    在表头区（`0x0..0x904`）里搜过，**一次都没命中**；全文件也搜不到 `MELF`/`melf` 字样。
//!    也就是说：**只能靠扫 ELF 魔数 + 校验头来枚举镜像**。所以 [`crate::elf::parse_header`]
//!    那套「结构自洽才算真头」的判据不是洁癖，是防误判的必需品。
//! 2. **外层 ELF 的两个 LOAD 段正好铺到 `0x904`**（`0x84+1536 = 0x684`，`0x684+640 = 0x904`）
//!    —— 外层就是「头 + Hexagon stub（2176 B 代码）」，紧接着才是第一个内嵌镜像。
//! 3. **编程器是 `machine == 0xF3` 且 `phnum >= 4` 的那个内嵌 ELF**。外层也是 `0xF3`
//!    但 `phnum == 2`（只有 stub 的两段），所以判据必须带上 phnum 门槛，不能只比 machine。
//!    实测编程器：12 个程序头，其中 `PT_LOAD` 4 段、合计 filesz = 177,184 B；
//!    还带一个高通私有段（`type=0`、`filesz=436`、`flags=0x7000000`）与一段落在 `0x2D000`
//!    的 `type=0 / filesz=11376`。**送 Sahara 时到底发哪些段，要在真机上验**（见待办）。
//!
//! ## 机器码 0xF3 是什么
//!
//! 高通给 Hexagon 的官方 `e_machine` 是 164(`0xA4`)，但**编程器 ELF 用的是 243(`0xF3`)**。
//! 别「修正」成 164 —— 换成 164 会把这个文件判成非高通镜像。
//!
//! ## 还没验的（真机才能定，别当已实现）
//!
//! - 送 Sahara 的段集合：是按 `PT_LOAD` 全发，还是**也要发**那两个 `type=0` 的私有段。
//! - 镜像 #2/#3/#4（ARM64 侧）在刷写流程里到底要不要发、以及在哪个阶段发。
//!   红魔的 `devprg` 是「一个 melf 顶一套工具」的形态，这几段很可能是给
//!   某些存储类型/调试路径用的，**没有真机不能猜**。

use crate::elf::{self, ElfHeader, PT_LOAD};

/// 高通编程器的 `e_machine`。见模块注释：不是 164。
pub const QUALCOMM_MACHINE: u16 = 0xF3;

/// 「这个内嵌头算编程器」的 phnum 门槛。
///
/// 外层 stub 只有 2 个段（实测），真正的编程器有 12 个。取 4 是为了留余量：
/// 未来若出现精简过的编程器（比如只有 5 段）也不会被误滤掉，
/// 而随机字节冒充一个 `phnum >= 4` 且头结构完全自洽的 ELF 的概率可以忽略。
pub const MIN_PROGRAMMER_PHNUM: u16 = 4;

/// 容器里的一个内嵌镜像。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MelfImage {
    /// 该镜像在容器里的起始偏移。
    pub offset: usize,
    /// 到下一个镜像（或文件尾）的长度；**只是长度上限提示**，
    /// 真正要发多少字节看程序头，不要拿它当「镜像大小」用。
    pub span: usize,
    pub header: ElfHeader,
}

impl MelfImage {
    pub fn is_qualcomm(&self) -> bool {
        self.header.machine == QUALCOMM_MACHINE
    }

    /// 像不像一个「可执行的编程器」：高通机器码 + 真有程序头。
    pub fn looks_like_programmer(&self) -> bool {
        self.is_qualcomm() && self.header.phnum >= MIN_PROGRAMMER_PHNUM
    }

    /// 送 Sahara 时要发的段（`PT_LOAD`），并给出文件内 [起, 止) 的绝对范围。
    pub fn load_segments(&self, data: &[u8]) -> Result<Vec<(crate::elf::ProgramHeader, usize, usize)>, String> {
        let phs = elf::program_headers(data, self.offset)?;
        let mut out = Vec::new();
        for ph in phs.into_iter().filter(|p| p.p_type == PT_LOAD && p.filesz > 0) {
            let start = self.offset + ph.offset as usize;
            let end = start + ph.filesz as usize;
            if end > data.len() {
                return Err(format!(
                    "段越界：@0x{start:X}..0x{end:X}，文件只有 {} B",
                    data.len()
                ));
            }
            out.push((ph, start, end));
        }
        Ok(out)
    }

    /// `PT_LOAD` 段的 filesz 合计 —— 送设备的净数据量（用于日志/进度分母）。
    pub fn load_payload_bytes(&self, data: &[u8]) -> Result<u64, String> {
        Ok(self.load_segments(data)?.iter().map(|(p, _, _)| p.filesz).sum())
    }
}

/// 解析出来的 MELF 容器。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Melf {
    /// 按偏移升序的全部内嵌镜像（含最外层的 stub）。
    pub images: Vec<MelfImage>,
    pub total_len: usize,
}

impl Melf {
    /// 送 Sahara 的 firehose 编程器。取第一个「像编程器」的镜像。
    pub fn firehose_programmer(&self) -> Option<&MelfImage> {
        self.images.iter().find(|i| i.looks_like_programmer())
    }

    /// 全部高通的 `machine == 0xF3` 镜像（含外层 stub），用于诊断输出。
    pub fn qualcomm_images(&self) -> Vec<&MelfImage> {
        self.images.iter().filter(|i| i.is_qualcomm()).collect()
    }
}

/// 这个 buffer 看起来是不是 MELF 容器。
///
/// 判据：外层是 ELF、`machine == 0xF3`、且**至少有一个**内嵌的编程器镜像。
/// 只看扩展名是不够的 —— `.melf` / `.elf` / `.mbn` 在不同厂商手里混用。
pub fn is_melf(data: &[u8]) -> bool {
    parse(data).map(|m| m.firehose_programmer().is_some()).unwrap_or(false)
}

/// 扫描并解析 MELF 容器（含最外层）。
pub fn parse(data: &[u8]) -> Result<Melf, String> {
    let outer = elf::parse_header(data, 0).map_err(|e| format!("外层不是合法 ELF：{e}"))?;

    // 找所有魔数出现位置，再逐个用「结构自洽」筛掉假头
    let mut offsets = Vec::new();
    let mut i = 0usize;
    while let Some(rel) = data[i..].windows(4).position(|w| w == b"\x7fELF") {
        let at = i + rel;
        if elf::parse_header(data, at).is_ok() {
            offsets.push(at);
        }
        i = at + 4;
        if i >= data.len() {
            break;
        }
    }

    if offsets.is_empty() || offsets[0] != 0 {
        return Err("容器里找不到任何结构自洽的 ELF 镜像".to_string());
    }

    let mut images = Vec::with_capacity(offsets.len());
    for (idx, &off) in offsets.iter().enumerate() {
        let next = offsets.get(idx + 1).copied().unwrap_or(data.len());
        let header = elf::parse_header(data, off).expect("上一步已校验过");
        images.push(MelfImage { offset: off, span: next - off, header });
    }

    let melf = Melf { images, total_len: data.len() };

    // 外层只应是 stub：这里**不**强制要求（万一某厂商把编程器放在 0 偏移就得让它过），
    // 但一个 0xF3 的容器连编程器都找不到基本就是拿错文件了，早点报错比后面发错数据好。
    if melf.firehose_programmer().is_none() {
        return Err(format!(
            "外层是 0x{:X} 且未找到编程器镜像（machine==0x{F3:X} 且 phnum >= {MIN_PROGRAMMER_PHNUM}）",
            outer.machine, F3 = QUALCOMM_MACHINE
        ));
    }

    Ok(melf)
}

/// 诊断用的表格行（探针与 UI 都用它，别在两处各拼一遍）。
pub fn describe(images: &[MelfImage]) -> Vec<String> {
    images
        .iter()
        .enumerate()
        .map(|(i, img)| {
            format!(
                "#{i} @0x{:X} {} machine=0x{:X} entry=0x{:X} phnum={} span={}",
                img.offset,
                img.header.class.as_str(),
                img.header.machine,
                img.header.entry,
                img.header.phnum,
                img.span
            )
        })
        .collect()
}

/// 只给测试用：造一个「外层 stub + programming 镜像」的最小 MELF。
#[cfg(test)]
pub(crate) fn synth_melf(programmer_phnum: u16, extra_aarch64: bool) -> Vec<u8> {
    fn elf32(machine: u16, phnum: u16, entry: u32) -> Vec<u8> {
        let mut v = vec![0u8; 52 + phnum as usize * 32];
        v[0..4].copy_from_slice(b"\x7fELF");
        v[4] = 1;
        v[5] = 1;
        v[6] = 1;
        v[16..18].copy_from_slice(&2u16.to_le_bytes());
        v[18..20].copy_from_slice(&machine.to_le_bytes());
        v[20..24].copy_from_slice(&1u32.to_le_bytes());
        v[24..28].copy_from_slice(&entry.to_le_bytes());
        v[28..32].copy_from_slice(&52u32.to_le_bytes());
        v[40..42].copy_from_slice(&52u16.to_le_bytes());
        v[42..44].copy_from_slice(&32u16.to_le_bytes());
        v[44..46].copy_from_slice(&phnum.to_le_bytes());
        v
    }
    fn elf64(machine: u16, phnum: u16) -> Vec<u8> {
        let mut v = vec![0u8; 64 + phnum as usize * 56];
        v[0..4].copy_from_slice(b"\x7fELF");
        v[4] = 2;
        v[5] = 1;
        v[6] = 1;
        v[16..18].copy_from_slice(&2u16.to_le_bytes());
        v[18..20].copy_from_slice(&machine.to_le_bytes());
        v[20..24].copy_from_slice(&1u32.to_le_bytes());
        v[32..40].copy_from_slice(&64u64.to_le_bytes()); // e_phoff
        v[52..54].copy_from_slice(&64u16.to_le_bytes()); // e_ehsize
        v[54..56].copy_from_slice(&56u16.to_le_bytes()); // e_phentsize
        v[56..58].copy_from_slice(&phnum.to_le_bytes());
        v
    }

    let mut out = elf32(QUALCOMM_MACHINE, 2, 0x2212_6000); // 外层 stub
    out.extend_from_slice(&elf32(QUALCOMM_MACHINE, programmer_phnum, 0x2041_5100));
    if extra_aarch64 {
        out.extend_from_slice(&elf64(0xB7, 9));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::elf::ElfClass;

    #[test]
    fn parses_synthetic_container_and_finds_programmer() {
        let data = synth_melf(12, true);
        let m = parse(&data).expect("应能解析");
        assert_eq!(m.images.len(), 3, "外层 + 编程器 + 一个 AArch64 镜像");
        assert_eq!(m.images[0].offset, 0);
        let prog = m.firehose_programmer().expect("必须能挑出编程器");
        assert!(prog.offset > 0, "编程器不能是外层 stub");
        assert_eq!(prog.header.machine, QUALCOMM_MACHINE);
        assert_eq!(prog.header.phnum, 12);
        assert_eq!(prog.header.class, ElfClass::Elf32);
        // ARM64 侧那个不该被当成编程器
        assert_eq!(m.images[2].header.machine, 0xB7);
        assert!(!m.images[2].looks_like_programmer());
    }

    #[test]
    fn outer_stub_alone_is_not_a_programmer() {
        // 外层也是 0xF3，但 phnum == 2 → 不能只比 machine 码，必须带 phnum 门槛
        let data = synth_melf(2, false);
        assert!(parse(&data).is_err(), "只有一个 2 段的 0xF3 镜像时应报错");
        assert!(!is_melf(&data));
    }

    #[test]
    fn phnum_threshold_is_four() {
        assert!(parse(&synth_melf(4, false)).is_ok(), "4 段应算编程器");
        assert!(parse(&synth_melf(3, false)).is_err(), "3 段不算");
    }

    #[test]
    fn random_bytes_with_magic_are_not_an_elf() {
        let mut junk = vec![0u8; 4096];
        junk[0..4].copy_from_slice(b"\x7fELF");
        assert!(parse(&junk).is_err());
        assert!(!is_melf(&junk));
    }

    #[test]
    fn load_segments_use_absolute_range_and_reject_out_of_bounds() {
        // 造一个带 1 个 PT_LOAD 的编程器，filesz 故意越界 → 必须报错（防止把随机内存发给设备）
        let mut data = synth_melf(4, false);
        let prog_off = 52 + 2 * 32; // 紧跟在 stub 之后
        let ph = prog_off + 52; // 编程器自己的程序头表起点
        data[ph..ph + 4].copy_from_slice(&PT_LOAD.to_le_bytes());
        data[ph + 4..ph + 8].copy_from_slice(&0x100u32.to_le_bytes()); // ph.offset
        data[ph + 16..ph + 20].copy_from_slice(&0xFFFF_FFFFu32.to_le_bytes()); // filesz 离谱
        let m = parse(&data).unwrap();
        let prog = m.firehose_programmer().unwrap();
        assert!(prog.load_segments(&data).is_err(), "越界段必须被拒");
    }

    #[test]
    fn describe_rows_are_stable() {
        let data = synth_melf(12, true);
        let m = parse(&data).unwrap();
        let rows = describe(&m.images);
        assert_eq!(rows.len(), 3);
        assert!(rows[0].starts_with("#0 @0x0 ELF32 machine=0xF3"));
        assert!(rows[2].contains("machine=0xB7"));
    }
}
