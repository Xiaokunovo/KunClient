"""一键启动：拉起 NapCat（协议端）→ 弹二维码 → 启动机器人。

    python run.py                 全部启动
    python run.py --no-napcat     只启机器人（NapCat 已经单独在跑）
    python run.py --setup         先装/更新 NapCat 再启动
    python run.py --check         体检完就退出

为什么要这个脚本：NapCat 的二维码是它自己弹的，但第一次装完目录结构不一定对、
OneBot 的 WS 服务也没开。这里把「补配置 → 启动 → 等二维码 → 弹图 → 起机器人」
串成一条链，省得手动折腾。
"""

from __future__ import annotations

import argparse
import asyncio
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import napcat_setup  # noqa: E402

NAPCAT_DIR = ROOT / "napcat"
# NapCat 的 cachePath = <binaryPath>/cache。Windows Node 版的 binaryPath 是 napcat/napcat/，
# 所以二维码实际落在 napcat/napcat/cache/ 下；其余几个是老版本/别的打包方式的兜底。
QR_CANDIDATES = [
    NAPCAT_DIR / "napcat" / "cache" / "qrcode.png",
    NAPCAT_DIR / "cache" / "qrcode.png",
    NAPCAT_DIR / "napcat" / "tmp" / "qrcode.png",
    NAPCAT_DIR / "tmp" / "qrcode.png",
    NAPCAT_DIR / "qrcode.png",
    NAPCAT_DIR / "napcat" / "cache" / "qrcode.jpg",
]
LAUNCHER_NAMES = [
    "launcher.bat",
    "napcat.bat",
    "NapCatWinBootMain.exe",
    "napcat.exe",
    "start.bat",
    "run.bat",
]


def find_launcher() -> Path | None:
    """找 NapCat 的正确启动器。

    Windows Node 版解压后有两层：
        napcat/napcat.bat            —— 直接跑 node，不注入 QQ，登录不了
        napcat/napcat/launcher.bat   —— 注入 QQ NT 的真正启动器（要管理员）
    所以必须优先找子目录里的 launcher，否则会「启动成功但永远没有二维码」。
    """
    sub = NAPCAT_DIR / "napcat"
    # launcher.bat 用 wt.exe 重开窗口；没装 Windows Terminal 就用 powershell 版
    has_wt = shutil.which("wt.exe") is not None
    preferred = ["launcher.bat", "launcher-win10.bat"] if has_wt else ["launcher-win10.bat", "launcher.bat"]
    for name in preferred:
        p = sub / name
        if p.exists():
            return p
    for name in preferred:
        p = NAPCAT_DIR / name
        if p.exists():
            return p
    if not NAPCAT_DIR.exists():
        return None
    for name in LAUNCHER_NAMES:
        p = NAPCAT_DIR / name
        if p.exists():
            return p
    for p in sorted(NAPCAT_DIR.iterdir()):
        if p.suffix.lower() in (".bat", ".exe") and p.is_file():
            return p
    return None


def port_in_use(port: int = 3001) -> bool:
    import socket

    with socket.socket() as s:
        s.settimeout(0.4)
        return s.connect_ex(("127.0.0.1", port)) == 0


def launch_napcat() -> subprocess.Popen | None:
    if port_in_use():
        print("→ 端口 3001 已经在监听，说明 NapCat 已启动过，直接复用")
        return None
    exe = find_launcher()
    if not exe:
        print("× 找不到 NapCat 启动器。先跑：python tools/napcat_setup.py --download")
        return None
    print(f"→ 启动 NapCat：{exe.name}")
    cwd = exe.parent
    if exe.suffix.lower() in (".bat", ".cmd"):
        cmd = ["cmd.exe", "/c", exe.name]
    else:
        cmd = [str(exe)]
    kwargs = {"cwd": str(cwd)}
    if sys.platform == "win32":
        kwargs["creationflags"] = subprocess.CREATE_NEW_CONSOLE  # 二维码在它自己的窗口里
    return subprocess.Popen(cmd, **kwargs)


def _newest(paths: list[Path]) -> tuple[Path | None, float]:
    best, best_t = None, 0.0
    for p in paths:
        if p.exists():
            t = p.stat().st_mtime
            if t > best_t:
                best, best_t = p, t
    return best, best_t


async def watch_qr(timeout: float = 180) -> None:
    """等 NapCat 把二维码写出来，复制到 data/qrcode.png 并自动打开。"""
    print("→ 等 NapCat 出二维码…（最多 3 分钟）")
    seen = 0.0
    start = time.time()
    shown = False
    while time.time() - start < timeout:
        await asyncio.sleep(1.0)
        path, mtime = _newest(QR_CANDIDATES)
        if not path or mtime <= seen:
            continue
        seen = mtime
        dest = ROOT / "data" / "qrcode.png"
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            shutil.copy2(path, dest)
        except OSError:
            continue
        print("\n" + "=" * 46)
        print("  请用手机 QQ 扫码登录（只扫一次，之后免扫）")
        print(f"  二维码：{dest}")
        print("=" * 46 + "\n")
        if not shown:
            shown = True
            try:
                os.startfile(dest)  # type: ignore[attr-defined]
            except Exception:  # noqa: BLE001
                print("（自动打开失败，手动双击 data/qrcode.png）")
        if time.time() - start > 20:
            return


async def amain(args) -> None:
    from bot.main import amain as bot_amain, banner, setup_logging
    from bot.config import ensure_config

    cfg = ensure_config()

    if args.setup:
        napcat_setup.download()

    proc = None
    if not args.no_napcat:
        print("→ 检查 OneBot 配置…")
        napcat_setup.ensure()
        proc = launch_napcat()
        if proc is None:
            print("  （继续启动机器人：它会一直重试连接，等 NapCat 起来后自动连上）")

    qr_task = asyncio.create_task(watch_qr()) if proc else None

    setup_logging(cfg)
    problems = cfg.validate()
    banner(cfg, problems)

    try:
        await bot_amain(cfg)
    finally:
        if qr_task:
            qr_task.cancel()
        if proc and proc.poll() is None:
            print("→ 关闭 NapCat…")
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()


def main() -> int:
    if sys.platform == "win32":
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except Exception:  # noqa: BLE001
            pass

    ap = argparse.ArgumentParser(description="大肥鱼 QQ 机器人启动器")
    ap.add_argument("--no-napcat", action="store_true", help="不启动 NapCat（已单独运行）")
    ap.add_argument("--setup", action="store_true", help="启动前下载/更新 NapCat")
    ap.add_argument("--check", action="store_true", help="只体检")
    args = ap.parse_args()

    if args.check:
        napcat_setup.check()
        from bot.config import ensure_config

        return 0 if not ensure_config().validate() else 1

    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
