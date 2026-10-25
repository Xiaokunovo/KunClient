#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 server/assets/scrcpy.zip 里**与 platform-tools 完全重复**的 adb 剔掉。

为什么：scrcpy 官方 win64 包自带一份 adb.exe + AdbWinApi.dll + AdbWinUsbApi.dll，
而我们的 tools.zip 里已经有一份一模一样的（客户端启动时必下）。两份都在包里就是白传。

实测（2026-10-08，11.3MB 的包，压缩后）：adb.exe 3.89MB + 两个 DLL 0.09MB ≈ 3.98MB，
占整包 **37%**。剔掉后 **10.78MB → 6.78MB**。

── 为什么剔掉之后 scrcpy 还能用 ──
scrcpy **4.1 没有 `--adb` 参数**（实测 `--help` 里查无此项），但它认 **ADB 环境变量**：
    Environment variables:
        ADB
            Path to adb executable
所以客户端在启动 scrcpy 时把 ADB 指到自己的 platform-tools（见
KunClient.App/Pages/ScreenMirrorPage.xaml.cs 的 StartScrcpyAsync）。
实测：ADB 指向不存在路径 → 「Command not found … define its fullpath in the ADB
environment variable」；指向真实 adb → 「daemon started successfully」。

── 🔴 上线顺序（顺序反了会让在用的老客户端投屏直接起不来）──
    1) 先发**新客户端**（会设 ADB 环境变量）
    2) 再 `node tools/deploy-server.js` 把瘦包推上服务器（该脚本已包含 assets/scrcpy.zip）
原因：老客户端不设 ADB，只能靠 scrcpy 自己目录里的 adb —— 瘦包里那份被剔掉了。
新客户端对新旧两种包都能跑（旧包多一份 adb 不影响），所以顺序只能是「先客户端」。

用法：
    python tools/slim-scrcpy-package.py            # 预览（只报数字，不改文件）
    python tools/slim-scrcpy-package.py --apply    # 原地替换 server/assets/scrcpy.zip
原包备份写到 .workbuddy/tmp/，**不放在 server/assets/ 下** —— 那个目录是公网可直接
下载的（/assets/*），留个 .bak 等于多发一份 11MB。
"""

import argparse
import os
import shutil
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(REPO, "server", "assets", "scrcpy.zip")
BACKUP_DIR = os.path.join(REPO, ".workbuddy", "tmp", "scrcpy_slim")

# 与 tools.zip 重复、且能由 ADB 环境变量替代的三件套
DROP = {"adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll"}


def human(n: int) -> str:
    return f"{n / 1048576:.2f} MB"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="原地替换 server/assets/scrcpy.zip")
    args = ap.parse_args()

    if not os.path.exists(TARGET):
        print(f"找不到 {TARGET}", file=sys.stderr)
        return 1

    before = os.path.getsize(TARGET)
    src = zipfile.ZipFile(TARGET)

    kept, dropped = [], []
    for info in src.infolist():
        base = os.path.basename(info.filename.rstrip("/"))
        (dropped if base in DROP else kept).append(info)

    print(f"包：{os.path.relpath(TARGET, REPO)}")
    print(f"  现状        {human(before):>10}   共 {len(src.infolist())} 项")
    for i in dropped:
        print(f"  将剔除      {human(i.compress_size):>10}   {i.filename}（解压后 {human(i.file_size)}）")
    print(f"  预计瘦身后  {human(before - sum(i.compress_size for i in dropped)):>10}   "
          f"（-{sum(i.compress_size for i in dropped) / before * 100:.0f}%）")

    if not dropped:
        print("包里已经没有要剔的东西了，什么都没做。")
        return 0

    if not args.apply:
        print("\n这是预览。要真正替换请加 --apply。")
        return 0

    os.makedirs(BACKUP_DIR, exist_ok=True)
    backup = os.path.join(BACKUP_DIR, "scrcpy.orig.zip")
    shutil.copy2(TARGET, backup)

    tmp_out = os.path.join(tempfile.mkdtemp(prefix="scrcpy_slim_"), "scrcpy.zip")
    with zipfile.ZipFile(tmp_out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as dst:
        for info in kept:
            # 逐条重新压缩（而不是 copy 原条目）：原包的 adb 是 store/低压缩率，
            # 全量重压能让剩下那些 DLL 也吃到 level=9。
            if info.is_dir():
                dst.writestr(info.filename, b"")
            else:
                dst.writestr(info.filename, src.read(info.filename))

    src.close()
    shutil.move(tmp_out, TARGET)

    after = os.path.getsize(TARGET)
    print(f"\n已替换：{human(before)} → {human(after)}（省 {human(before - after)}，"
          f"-{(before - after) / before * 100:.0f}%）")
    print(f"原包备份：{os.path.relpath(backup, REPO)}")
    print("\n🔴 记住上线顺序：先发新客户端（设 ADB 环境变量），再 deploy-server.js 推这个包。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
