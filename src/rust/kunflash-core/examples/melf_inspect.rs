//! loader 检查工具（dev 用，不进 exe）：
//!
//! ```bash
//! cd src/rust && cargo run --offline -p kunflash-core --example melf_inspect -- "C:\path\devprg_xxx.melf"
//! ```
//!
//! 输出：loader 形态（单 ELF / MELF 容器）、内嵌镜像表、被选中的 firehose 编程器
//! 及其 `PT_LOAD` 段清单。**这是「适配红魔」时唯一能离线自查的入口** ——
//! 拿到新机型的 devprg 先跑它，确认编程器挑对了再上真机。

use std::process::ExitCode;

use kunflash_core::elf::{self, PT_LOAD};
use kunflash_core::{melf, vendor};

fn main() -> ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("用法: melf_inspect <loader 文件路径>");
        return ExitCode::from(2);
    };

    let data = match std::fs::read(&path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("读不到文件 {path}：{e}");
            return ExitCode::from(2);
        }
    };

    println!("文件      : {path}");
    println!("大小      : {} B ({:.2} MB)", data.len(), data.len() as f64 / 1048576.0);

    let format = vendor::detect_loader(&data);
    println!("形态(内容): {}", format.as_str());
    // 扩展名只作参考：.melf 本身就是 ELF 文件，后缀不可信
    println!(
        "扩展名    : {}（仅供参考，判定一律看内容）",
        std::path::Path::new(&path)
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("(无)")
    );

    println!();
    println!("四家厂商（接口与实现状态）：");
    for v in vendor::Vendor::ALL {
        println!(
            "  - {:9} {:4}  support={:?}  loader={}",
            v.as_str(),
            v.display_name(),
            v.support(),
            v.expected_loader().as_str()
        );
    }
    println!();

    match format {
        vendor::LoaderFormat::Unknown => {
            println!("不是 ELF —— 四家的 loader 都应是 ELF（单 ELF 或 .melf 容器），请确认选对了文件");
            ExitCode::from(1)
        }
        vendor::LoaderFormat::SingleElf => {
            let h = match elf::parse_header(&data, 0) {
                Ok(h) => h,
                Err(e) => {
                    eprintln!("ELF 头解析失败：{e}");
                    return ExitCode::from(1);
                }
            };
            println!("单 ELF 编程器：{} machine=0x{:X} entry=0x{:X} phnum={}",
                     h.class.as_str(), h.machine, h.entry, h.phnum);
            if let Err(e) = dump_segments(&data, 0, "  ") {
                eprintln!("  段解析失败：{e}");
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        vendor::LoaderFormat::MelfContainer => {
            let m = match melf::parse(&data) {
                Ok(m) => m,
                Err(e) => {
                    eprintln!("MELF 解析失败：{e}");
                    return ExitCode::from(1);
                }
            };
            println!("MELF 容器：共 {} 个内嵌镜像（已按结构自洽校验，非仅魔数匹配）", m.images.len());
            for (i, img) in m.images.iter().enumerate() {
                let mark = if img.looks_like_programmer() { "   ← 编程器" } else { "" };
                println!(
                    "  #{i} @0x{:<8X} {} machine=0x{:<3X} entry=0x{:<10X} phnum={:<3} span={}{mark}",
                    img.offset,
                    img.header.class.as_str(),
                    img.header.machine,
                    img.header.entry,
                    img.header.phnum,
                    img.span
                );
            }

            let prog = m.firehose_programmer().expect("parse 已保证存在");
            println!();
            println!("选中的 firehose 编程器：@0x{:X}（第 {} 个镜像）",
                     prog.offset, m.images.iter().position(|i| i.offset == prog.offset).unwrap_or(0));
            println!("  machine=0x{:X} phnum={} entry=0x{:X}", prog.header.machine, prog.header.phnum, prog.header.entry);
            println!("  送 Sahara 要发的 PT_LOAD 段：");
            match dump_segments(&data, prog.offset, "    ") {
                Ok(payload) => {
                    println!("  合计净数据 = {payload} B（{:.0} KB）", payload as f64 / 1024.0);
                    println!();
                    println!("⚠️ 容器里那两个 type=0 的高通私有段（如 filesz=436 / 11376）**未**含在上面；");
                    println!("   到底要不要一起发，必须真机验证 —— 见 melf.rs 模块注释的「还没验的」。");
                    ExitCode::SUCCESS
                }
                Err(e) => {
                    eprintln!("  段解析失败：{e}");
                    ExitCode::from(1)
                }
            }
        }
    }
}

/// 打印某个 ELF 的 `PT_LOAD` 段，返回 filesz 合计。
fn dump_segments(data: &[u8], at: usize, indent: &str) -> Result<u64, String> {
    let phs = elf::program_headers(data, at)?;
    let mut total = 0u64;
    let mut shown = 0usize;
    for (i, ph) in phs.iter().enumerate() {
        if ph.p_type != PT_LOAD || ph.filesz == 0 {
            continue;
        }
        total += ph.filesz;
        shown += 1;
        println!(
            "{indent}[{i:2}] off=0x{:<8X} vaddr=0x{:<10X} filesz={:<9} memsz={:<9} flags=0x{:X}",
            ph.offset, ph.vaddr, ph.filesz, ph.memsz, ph.flags
        );
    }
    if shown == 0 {
        println!("{indent}（没有带数据的 PT_LOAD 段 —— 这个文件不能直接当编程器发）");
    }
    Ok(total)
}
