//! 极小的 ELF32 / ELF64 头与程序头读取器。
//!
//! 为什么要自己写：红魔（努比亚）的 9008 引导是 `.melf`（Multi-image ELF 容器），
//! 要从中挑出真正送 Sahara 的 firehose 编程器，必须能**校验**每个候选头的合理性 ——
//! 靠「扫 `\x7fELF` 魔数」是不够的：代码段里出现 4 个连续魔数虽少见，但一旦误判，
//! 后果是把随机字节当 loader 发给设备。
//!
//! 所以判定「这是不是一个真 ELF 头」用的是**结构自洽**，不是魔数：
//! `e_ehsize` 必须等于 52(32 位)/64(64 位)、`e_phentsize` 必须等于 32/56、
//! 程序头表必须完整落在文件内。这四个条件同时成立的概率在随机字节上可以忽略。
//!
//! 只读不写、不依赖任何 crate；只覆盖 32/64 位小端（高通工具链全是小端）。

/// ELF class 字节（`e_ident[EI_CLASS]`）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ElfClass {
    Elf32,
    Elf64,
}

impl ElfClass {
    pub fn as_str(self) -> &'static str {
        match self {
            ElfClass::Elf32 => "ELF32",
            ElfClass::Elf64 => "ELF64",
        }
    }
}

/// 解析出来的 ELF 头（只留我们要用的字段）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ElfHeader {
    pub class: ElfClass,
    pub etype: u16,
    /// 机器码。高通用的**不是**标准 `EM_HEXAGON(164)`，而是 **0xF3(243)** —— 见 [`crate::melf`]。
    pub machine: u16,
    pub entry: u64,
    pub phoff: u64,
    pub phentsize: u16,
    pub phnum: u16,
}

/// 程序头（段描述）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProgramHeader {
    pub p_type: u32,
    pub offset: u64,
    pub vaddr: u64,
    pub filesz: u64,
    pub memsz: u64,
    pub flags: u32,
}

/// `PT_LOAD`：唯一一类「文件里真有数据、必须发给设备」的段。
pub const PT_LOAD: u32 = 1;

/// 这儿是不是 ELF 魔数（只判 4 字节，不校验结构）。
pub fn has_magic(data: &[u8], at: usize) -> bool {
    matches!(data.get(at..at + 4), Some(b"\x7fELF"))
}

/// 校验并解析 `at` 处的 ELF 头。**这是「真头」的判据**，见模块注释。
pub fn parse_header(data: &[u8], at: usize) -> Result<ElfHeader, String> {
    if !has_magic(data, at) {
        return Err(format!("偏移 {at:#x} 处不是 ELF 魔数"));
    }

    // e_ident：class / data
    let class_byte = *data.get(at + 4).ok_or("文件在 e_ident[EI_CLASS] 处截断")?;
    let data_byte = *data.get(at + 5).ok_or("文件在 e_ident[EI_DATA] 处截断")?;
    let class = match class_byte {
        1 => ElfClass::Elf32,
        2 => ElfClass::Elf64,
        other => return Err(format!("未知 ELF class：{other}")),
    };
    match data_byte {
        1 => {}                                        // 小端
        2 => return Err("ELF 是大端（高通工具链不会出现）".to_string()),
        other => return Err(format!("未知 ELF 字节序：{other}")),
    }

    let want_phentsize: u16 = match class {
        ElfClass::Elf32 => 32,
        ElfClass::Elf64 => 56,
    };
    let want_ehsize: u16 = match class {
        ElfClass::Elf32 => 52,
        ElfClass::Elf64 => 64,
    };

    let rd16 = |off: usize| -> Result<u16, String> {
        let b = data.get(at + off..at + off + 2).ok_or("ELF 头截断")?;
        Ok(u16::from_le_bytes([b[0], b[1]]))
    };
    let rd32 = |off: usize| -> Result<u32, String> {
        let b = data.get(at + off..at + off + 4).ok_or("ELF 头截断")?;
        Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    let rd64 = |off: usize| -> Result<u64, String> {
        let b = data.get(at + off..at + off + 8).ok_or("ELF 头截断")?;
        Ok(u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
    };

    let etype = rd16(16)?;
    let machine = rd16(18)?;

    let (entry, phoff, ehsize, phentsize, phnum) = match class {
        // ELF32：e_entry@24(4) e_phoff@28(4) e_ehsize@40(2) e_phentsize@42(2) e_phnum@44(2)
        ElfClass::Elf32 => (rd32(24)? as u64, rd32(28)? as u64, rd16(40)?, rd16(42)?, rd16(44)?),
        // ELF64：e_entry@24(8) e_phoff@32(8) e_ehsize@52(2) e_phentsize@54(2) e_phnum@56(2)
        ElfClass::Elf64 => (rd64(24)?, rd64(32)?, rd16(52)?, rd16(54)?, rd16(56)?),
    };

    if ehsize != want_ehsize {
        return Err(format!("e_ehsize={ehsize}，{class:?} 应为 {want_ehsize}（判定为非 ELF）"));
    }
    if phentsize != want_phentsize {
        return Err(format!("e_phentsize={phentsize}，{class:?} 应为 {want_phentsize}（判定为非 ELF）"));
    }
    // 程序头表必须完整落在文件里；phnum 上限顺手挡一下离谱值
    if phnum > 128 {
        return Err(format!("e_phnum={phnum} 不合理"));
    }
    let table_end = phoff
        .checked_add(phnum as u64 * phentsize as u64)
        .ok_or("程序头表偏移溢出")?;
    if table_end > data.len() as u64 {
        return Err(format!(
            "程序头表越界：需要 {table_end} B，文件只有 {} B",
            data.len()
        ));
    }

    Ok(ElfHeader { class, etype, machine, entry, phoff, phentsize, phnum })
}

/// 读出全部程序头。
pub fn program_headers(data: &[u8], at: usize) -> Result<Vec<ProgramHeader>, String> {
    let h = parse_header(data, at)?;
    let mut out = Vec::with_capacity(h.phnum as usize);
    for i in 0..h.phnum as usize {
        let off = at + h.phoff as usize + i * h.phentsize as usize;
        let rd32 = |o: usize| -> Result<u32, String> {
            data.get(off + o..off + o + 4)
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                .ok_or_else(|| "程序头截断".to_string())
        };
        let rd64 = |o: usize| -> Result<u64, String> {
            data.get(off + o..off + o + 8)
                .map(|b| u64::from_le_bytes(b.try_into().expect("长度已按 8 校验")))
                .ok_or_else(|| "程序头截断".to_string())
        };

        out.push(match h.class {
            // Elf32_Phdr: type@0 off@4 vaddr@8 paddr@12 filesz@16 memsz@20 flags@24
            ElfClass::Elf32 => ProgramHeader {
                p_type: rd32(0)?,
                offset: rd32(4)? as u64,
                vaddr: rd32(8)? as u64,
                filesz: rd32(16)? as u64,
                memsz: rd32(20)? as u64,
                flags: rd32(24)?,
            },
            // Elf64_Phdr: type@0 flags@4 off@8 vaddr@16 paddr@24 filesz@32 memsz@40
            ElfClass::Elf64 => ProgramHeader {
                p_type: rd32(0)?,
                flags: rd32(4)?,
                offset: rd64(8)?,
                vaddr: rd64(16)?,
                filesz: rd64(32)?,
                memsz: rd64(40)?,
            },
        });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 造一个结构自洽的假 ELF32 头（52 B）+ 一张程序头表。
    fn fake_elf32(machine: u16, phnum: u16) -> Vec<u8> {
        let mut v = vec![0u8; 52 + phnum as usize * 32];
        v[0..4].copy_from_slice(b"\x7fELF");
        v[4] = 1; // ELF32
        v[5] = 1; // 小端
        v[6] = 1;
        v[16..18].copy_from_slice(&2u16.to_le_bytes()); // e_type = EXEC
        v[18..20].copy_from_slice(&machine.to_le_bytes());
        v[20..24].copy_from_slice(&1u32.to_le_bytes()); // e_version
        v[28..32].copy_from_slice(&52u32.to_le_bytes()); // e_phoff
        v[40..42].copy_from_slice(&52u16.to_le_bytes()); // e_ehsize
        v[42..44].copy_from_slice(&32u16.to_le_bytes()); // e_phentsize
        v[44..46].copy_from_slice(&phnum.to_le_bytes());
        v
    }

    #[test]
    fn parses_elf32_header() {
        let h = parse_header(&fake_elf32(0xF3, 2), 0).expect("应当解析成功");
        assert_eq!(h.class, ElfClass::Elf32);
        assert_eq!(h.machine, 0xF3);
        assert_eq!(h.phnum, 2);
        assert_eq!(h.phentsize, 32);
    }

    #[test]
    fn rejects_random_bytes_with_elf_magic() {
        // 这是本模块存在的理由：光有魔数不算 ELF
        let mut v = vec![0u8; 128];
        v[0..4].copy_from_slice(b"\x7fELF");
        v[4] = 1;
        v[5] = 1;
        // e_ehsize / e_phentsize 全是 0 → 必须被拒
        assert!(parse_header(&v, 0).is_err());
    }

    #[test]
    fn rejects_truncated_and_out_of_range() {
        let good = fake_elf32(0xF3, 4);
        // 头被截断
        assert!(parse_header(&good[..40], 0).is_err());
        // 程序头表越界（声称 64 个但文件里没有）
        let mut lie = good.clone();
        lie[44..46].copy_from_slice(&64u16.to_le_bytes());
        assert!(parse_header(&lie, 0).is_err());
    }

    #[test]
    fn rejects_big_endian_and_bad_class() {
        let mut v = fake_elf32(0xF3, 2);
        v[5] = 2;
        assert!(parse_header(&v, 0).is_err());
        let mut v2 = fake_elf32(0xF3, 2);
        v2[4] = 7;
        assert!(parse_header(&v2, 0).is_err());
    }

    #[test]
    fn reads_program_headers() {
        let mut v = fake_elf32(0xF3, 2);
        // 第 0 个程序头：PT_LOAD, off=0x100, filesz=0x40
        let ph = 52;
        v[ph..ph + 4].copy_from_slice(&1u32.to_le_bytes());
        v[ph + 4..ph + 8].copy_from_slice(&0x100u32.to_le_bytes());
        v[ph + 16..ph + 20].copy_from_slice(&0x40u32.to_le_bytes());
        let phs = program_headers(&v, 0).expect("应当解析成功");
        assert_eq!(phs.len(), 2);
        assert_eq!(phs[0].p_type, PT_LOAD);
        assert_eq!(phs[0].offset, 0x100);
        assert_eq!(phs[0].filesz, 0x40);
        assert_eq!(phs[1].p_type, 0);
    }
}
