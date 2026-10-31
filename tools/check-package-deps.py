#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
KunClient 分发包依赖完整性闸门（Windows）

干什么：把一个「要发出去」的目录 / 压缩包 / 单文件 exe，按 PE 导入表递归求
        依赖闭包，逐个比对**包内是否真的有那个 DLL**，缺一个就报错退出。

为什么需要它（2026-10-31）：
  用户反馈「由于找不到 liblzma-5.dll，无法继续执行代码」。根因不是代码，是
  **打包时 DLL 闭包算漏了一个**：ldd 把 MSYS2 的 DLL 报成 `/mingw64/bin/xxx`
  （MSYS 挂载点），而脚本拿 `/e/msys64/mingw64/bin` 去比对，永远匹配不上；
  恰好躺在 exe 旁边的那几个 DLL 因为「已经在目录里」被放过，唯独不在旁边、
  又真被 exe 直接导入的 liblzma-5.dll 被静默丢弃。

  这类缺陷在开发机上永远复现不了（开发机 PATH 里有 mingw64，系统直接找到了），
  只有用户机器才会炸。所以必须有一道**构建期**的闸门，而不是靠人眼。

用法：
  python tools/check-package-deps.py <目录 | .exe | .7z | .zip> [更多...]
  python tools/check-package-deps.py src/cpp/dist-offline src/cpp/KunClient-2.0.6-offline.7z

退出码：0 = 全部完整；1 = 有缺失（明细已打印）；2 = 用法/工具缺失。

🔴 判据是「包内有没有这个文件」，不是「本机能不能找到」—— 后者在开发机上恒真，
   正是漏检的来源。
🔴 为什么不用 bash 写：第一版用 shell 循环调 objdump，每个依赖 fork 三四个进程，
   一个 45 文件的包要跑 2~3 分钟，接进构建脚本等于每次构建多等几分钟。
   这里直接按 PE 结构解析导入表（纯读字节，不 spawn），整个包 ~100ms。
"""

from __future__ import annotations

import os
import io as _io
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

# ── Windows 自带 DLL 白名单 ───────────────────────────────────────────────────
# 只列「真的会随系统提供」的。宁缺毋滥：多放一个，就少一次真漏检。
SYSTEM_DLLS = {
    "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll", "gdi32full.dll",
    "advapi32.dll", "shell32.dll", "shlwapi.dll", "ole32.dll", "oleaut32.dll",
    "comdlg32.dll", "comctl32.dll", "ws2_32.dll", "winmm.dll", "imm32.dll",
    "uxtheme.dll", "version.dll", "setupapi.dll", "crypt32.dll", "secur32.dll",
    "iphlpapi.dll", "dwrite.dll", "winhttp.dll", "mfplat.dll", "mfreadwrite.dll",
    "wtsapi32.dll", "netapi32.dll", "shcore.dll", "userenv.dll", "authz.dll",
    "bcrypt.dll", "bcryptprimitives.dll", "ncrypt.dll", "dwmapi.dll", "d3d9.dll",
    "d3d11.dll", "d3d12.dll", "dxgi.dll", "mscms.dll", "ntdll.dll", "msvcrt.dll",
    "msvcp_win.dll", "ucrtbase.dll", "cfgmgr32.dll", "win32u.dll", "sechost.dll",
    "combase.dll", "rpcrt4.dll", "wintrust.dll", "dbghelp.dll", "psapi.dll",
    "powrprof.dll", "propsys.dll", "dnsapi.dll", "wininet.dll", "wldap32.dll",
    "urlmon.dll", "oleacc.dll", "usp10.dll", "mswsock.dll", "winspool.drv",
    "hid.dll", "devobj.dll", "ktmw32.dll", "mfuuid.dll", "mpr.dll", "winusb.dll",
    "d3dcompiler_47.dll", "opengl32.dll", "glu32.dll",
}


def is_system_dll(name: str) -> bool:
    low = name.lower()
    return low.startswith("api-ms-win-") or low in SYSTEM_DLLS


# ── PE 导入表解析 ─────────────────────────────────────────────────────────────
def pe_imports(path: str) -> list[str]:
    """读 PE 的导入表，返回 DLL 名列表（原始大小写）。非 PE / 解析失败返回 []。"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if len(data) < 0x40 or data[:2] != b"MZ":
        return []
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            return []
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        optsize = struct.unpack_from("<H", data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        ddoff = opt + (112 if magic == 0x20B else 96)
        imp_rva = struct.unpack_from("<I", data, ddoff + 8)[0]
        if imp_rva == 0:
            return []
        sec = opt + optsize
        sections = []
        for i in range(nsec):
            base = sec + i * 40
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, base + 8)
            sections.append((vaddr, vsize, rawptr, rawsize))
    except (struct.error, IndexError):
        return []

    def rva2off(rva: int):
        for vaddr, vsize, rawptr, rawsize in sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + (rva - vaddr)
        return None

    off = rva2off(imp_rva)
    if off is None:
        return []

    out = []
    while off + 20 <= len(data):
        ilt, _ts, _fc, name_rva, iat = struct.unpack_from("<IIIII", data, off)
        if name_rva == 0 and ilt == 0 and iat == 0:
            break
        noff = rva2off(name_rva)
        if noff is None:
            break
        end = data.find(b"\0", noff)
        if end < 0:
            break
        out.append(data[noff:end].decode("ascii", "replace"))
        off += 20
    return out


# ── 目录闭包 ──────────────────────────────────────────────────────────────────
def check_tree(root: str) -> bool:
    # ① 包内文件索引（文件名小写 → 相对路径）。Windows 文件系统不区分大小写，
    #    比对必须跟着不区分，否则 Qt6Core.dll 与 qt6core.dll 会被当成两个东西。
    pkg: dict[str, str] = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            pkg.setdefault(fn.lower(), os.path.relpath(full, root))

    # ② 入口：包内所有 PE（exe + dll，含 platforms/ tls/ imageformats/ 等插件子目录）
    entries = sorted(
        os.path.join(dirpath, fn)
        for dirpath, _dn, fns in os.walk(root)
        for fn in fns
        if fn.lower().endswith((".exe", ".dll"))
    )

    seen: set[str] = set()
    resolved = 0
    missing: list[tuple[str, str]] = []
    queue = list(entries)

    while queue:
        cur = queue.pop(0)
        cur_name = os.path.basename(cur)
        for dep in pe_imports(cur):
            low = dep.lower()
            if low in seen:
                continue
            seen.add(low)
            if is_system_dll(dep):
                continue
            rel = pkg.get(low)
            if rel is not None:
                resolved += 1
                queue.append(os.path.join(root, rel))
                continue
            missing.append((dep, cur_name))

    print(f"  文件数 {len(pkg)}（PE {len(entries)}），包内解析到的第三方 DLL {resolved} 个")
    if missing:
        print(f"  [FAIL] 缺失 {len(missing)} 个：")
        for dep, who in missing:
            print(f"    - {dep}  <- 被 {who} 依赖")
        return False
    print("  [OK] 依赖闭包完整")
    return True


def check_single_exe(exe: str) -> bool:
    """静态包承诺「一个文件就能跑」，所以只查它自己的导入表。"""
    third = [d for d in pe_imports(exe) if not is_system_dll(d)]
    if third:
        print("  [FAIL] 单文件却导入第三方 DLL（不是真单文件，用户机器上会缺 DLL）：")
        for d in third:
            print(f"    - {d}")
        return False
    print("  [OK] 导入表无第三方 DLL（真单文件）")
    return True


# ── 压缩包解包 ────────────────────────────────────────────────────────────────
def find_7z() -> str | None:
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    candidates = [
        os.path.join(repo, "src", "cpp", "7z-extra", "7za.exe"),
        r"C:\Windows\system32\7z.exe",
        r"C:\Program Files\7-Zip\7z.exe",
        r"C:\Program Files (x86)\7-Zip\7z.exe",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def extract(archive: str, dest: str) -> bool:
    low = archive.lower()
    try:
        if low.endswith(".zip"):
            with zipfile.ZipFile(archive) as z:
                z.extractall(dest)
            return True
        seven = find_7z()
        if not seven:
            print("  [错误] 需要 7-Zip 解包，但没找到 7za.exe / 7z.exe", file=sys.stderr)
            return False
        r = subprocess.run(
            [seven, "x", "-y", os.path.abspath(archive), f"-o{os.path.abspath(dest)}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        return r.returncode == 0
    except (zipfile.BadZipFile, OSError) as e:
        print(f"  [错误] 解包失败：{e}", file=sys.stderr)
        return False


def main(argv: list[str]) -> int:
    # Windows 控制台默认 GBK，中文 + 特殊符号会 UnicodeEncodeError 直接崩。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass
    if not argv:
        print("用法：check-package-deps.py <目录 | .exe | .7z | .zip> [更多...]",
              file=sys.stderr)
        return 2

    fails = 0
    for target in argv:
        print(f"-- {target}")

        if os.path.isdir(target):
            if not check_tree(target):
                fails += 1
            continue

        if not os.path.isfile(target):
            print("  [错误] 路径不存在", file=sys.stderr)
            fails += 1
            continue

        low = target.lower()
        if low.endswith(".exe"):
            if not check_single_exe(target):
                fails += 1
        elif low.endswith((".7z", ".zip")):
            tmp = tempfile.mkdtemp(prefix="kcdep-")
            try:
                if not extract(target, tmp):
                    fails += 1
                    continue
                if not check_tree(tmp):
                    fails += 1
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("  [错误] 不认识的类型（只支持目录 / .exe / .7z / .zip）", file=sys.stderr)
            fails += 1

    sys.stdout.flush()
    print("-" * 60)
    if fails:
        print(f"[FAIL] 有 {fails}/{len(argv)} 个包依赖不完整 —— 不要分发"
              f"（用户机器上会直接起不来）")
        return 1
    print(f"[OK] 全部 {len(argv)} 个包依赖完整")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
