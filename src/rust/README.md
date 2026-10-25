# KunFlash Rust 刷写栈

KunFlashTool 的 **Rust 实现** —— EDL / Firehose 协议、镜像解析与 FFI 导出。

> ⚠️ **状态：已冻结**
> 这套实现完成了核心协议与解析，但最终产品线转向了 C++/Qt6 版本
> （见 [`../cpp/`](../cpp/)）。本目录作为技术存档保留，**不再维护**。

---

## 结构

```
src/rust/
├── Cargo.toml              workspace 定义
├── kunflash-core/          核心库（纯 Rust，零第三方依赖）
│   ├── src/
│   │   ├── lib.rs          入口
│   │   ├── sahara.rs       Sahara 协议（握手 / 镜像上传）
│   │   ├── session.rs      刷写会话状态机
│   │   ├── melf.rs         MELf 镜像格式（小米 EDL 鉴权文件）
│   │   ├── elf.rs          ELF 解析
│   │   ├── crc32.rs        CRC32
│   │   ├── json.rs         极简 JSON 序列化（手写，避免 serde 依赖）
│   │   ├── xml.rs          XML 属性扫描（手写）
│   │   └── vendor.rs       厂商识别
│   └── examples/
│       └── melf_inspect.rs 命令行 MELf 检查工具
└── kunflash-ffi/           C ABI 动态库（供 .NET / C++ 调用）
    └── src/lib.rs
```

---

## 设计取舍

**零第三方依赖**是刻意的：

- `cargo build --offline` 必须能过（本机 crates.io 走镜像，离线只有本地缓存命中才能编译）；
- FFI 层只需要「XML 属性扫描 + JSON 输出」，手写两百行足够；
- 不把 serde 那一套（几百 KB）塞进 dll —— 用户对产物体积有硬要求。

**`panic = "unwind"` 不能改成 `abort`**：

FFI 侧每个导出都靠 `catch_unwind` 把 panic 转成 JSON 错误信封。
`panic=abort` 会让这条护栏直接失效 —— 刷机工具最可能 panic 的时刻正是「super 写了一半」。

---

## 构建

```bash
cd src/rust

# 核心库
cargo build --release -p kunflash-core

# FFI 动态库（cdylib）
cargo build --release -p kunflash-ffi

# 示例工具
cargo run --release --example melf_inspect -- <path-to-melf>
```

产物：
- `target/release/libkunflash_core.rlib`
- `target/release/kunflash_ffi.dll`（Windows）

### 工具链

- Rust 2024 edition（需要 rustc 1.85+）
- target：`x86_64-pc-windows-gnu`（不拖 mingw 运行时）

---

## 测试

```bash
cd src/rust
cargo test --workspace
```

---

## 与 C++ 版的关系

两套实现覆盖同样的协议，但**代码不共享**：

| 能力 | Rust 版 | C++ 版 |
|---|---|---|
| Sahara 握手 | ✅ | ✅ |
| Firehose | 部分 | ✅ 完整 |
| GPT / rawprogram | ❌ | ✅ |
| MELf / 小米鉴权 | ✅ | ✅ |
| MTK 支持 | ❌ | ✅ |
| 厂商专用流程 | ❌ | ✅ 欧加/小米 |

结论：**实际产品用 C++ 版**。Rust 版的价值在于协议实现的另一种参考，
以及「零依赖 + 小体积 dll」的工程示范。

---

## 协议

MIT License（同 KunClient 主项目）。
