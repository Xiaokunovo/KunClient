"""NapCat 协议端安装 / 配置工具。

    python tools/napcat_setup.py --download     下载最新 NapCat（Windows Node 版）
    python tools/napcat_setup.py --ensure       写入 / 修补 OneBot11 配置（WS 3001）
    python tools/napcat_setup.py --list         看看现在有哪些配置
    python tools/napcat_setup.py --check        体检：目录 / 配置 / 端口

设计取舍：
- 用 Windows Node 版（自带 node 运行时），不依赖本机 Node 版本；
- 配置是「合并」而不是覆盖 —— 用户自己在 WebUI 里加的 HTTP 服务不会被抹掉；
- 登录前 NapCat 还没有 onebot11_<QQ>.json，所以同时写一份 onebot11.json 当模板，
  登录后再跑一次 --ensure 就能补到具体账号上。
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAPCAT_DIR = ROOT / "napcat"


def detect_config_dir() -> Path:
    """NapCat 的 configPath = <binaryPath>/config。

    Windows Node 版解压后 binaryPath 是 `napcat/napcat/`（napcat.mjs 所在处），
    所以真正读的是 `napcat/napcat/config/`。老版本 / 其它打包方式可能是 `napcat/config/`，
    这里两种都认，优先已经存在的那个。
    """
    for cand in (NAPCAT_DIR / "napcat" / "config", NAPCAT_DIR / "config"):
        if cand.exists():
            return cand
    return NAPCAT_DIR / "napcat" / "config"


CONFIG_DIR = detect_config_dir()
RELEASE_API = "https://api.github.com/repos/NapNeko/NapCatQQ/releases/latest"
PREFERRED_ASSET = "NapCat.Shell.Windows.Node.zip"

# GitHub 直连在国内经常拉不动，按顺序回退。空串 = 直连。
MIRRORS = [
    "",
    "https://gh-proxy.com/",
    "https://ghproxy.net/",
    "https://ghfast.top/",
]

WS_PORT = 3001
WS_NAME = "dafeiyu-ws"


def onebot_template(port: int = WS_PORT, token: str = "") -> dict:
    return {
        "network": {
            "httpServers": [],
            "httpSseServers": [],
            "httpClients": [],
            "websocketServers": [
                {
                    "name": WS_NAME,
                    "enable": True,
                    "host": "127.0.0.1",
                    "port": port,
                    "messagePostFormat": "array",
                    "reportSelfMessage": False,
                    "token": token,
                    "enableForcePushEvent": True,
                    "debug": False,
                    "heartInterval": 30000,
                }
            ],
            "websocketClients": [],
            "plugins": [],
        },
        "musicSignUrl": "",
        "enableLocalFile2Url": False,
        "parseMultMsg": False,
    }


def merge_config(path: Path, port: int = WS_PORT, token: str = "") -> bool:
    """把我们的 WS 服务合并进已有配置，返回是否有改动。"""
    data: dict = {}
    if path.exists():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            backup = path.with_suffix(".broken.json")
            shutil.copy2(path, backup)
            print(f"  ! {path.name} 不是合法 JSON，已备份到 {backup.name} 并重建")
            data = {}

    network = data.setdefault("network", {})
    for key, default in (
        ("httpServers", []),
        ("httpSseServers", []),
        ("httpClients", []),
        ("websocketClients", []),
        ("plugins", []),
    ):
        network.setdefault(key, default)
    servers = network.setdefault("websocketServers", [])

    desired = onebot_template(port, token)["network"]["websocketServers"][0]
    changed = False
    for item in servers:
        if item.get("name") == WS_NAME:
            for k, v in desired.items():
                if item.get(k) != v:
                    item[k] = v
                    changed = True
            break
    else:
        servers.append(desired)
        changed = True

    data.setdefault("musicSignUrl", "")
    data.setdefault("enableLocalFile2Url", False)
    data.setdefault("parseMultMsg", False)

    if changed or not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    return changed


def ensure(port: int = WS_PORT, token: str = "") -> int:
    if not NAPCAT_DIR.exists():
        print("× 还没装 NapCat。先跑：python tools/napcat_setup.py --download")
        return 2
    config_dir = detect_config_dir()
    config_dir.mkdir(parents=True, exist_ok=True)

    targets = sorted(config_dir.glob("onebot11_*.json"))
    if not targets:
        print("· 还没有 onebot11_<QQ>.json（说明还没扫码登录过）")
        print("· 先写一份 onebot11.json 当默认模板")
        targets = [config_dir / "onebot11.json"]
    else:
        # 模板也同步一份，换号时省事
        merge_config(config_dir / "onebot11.json", port, token)

    for path in targets:
        changed = merge_config(path, port, token)
        print(f"  {'✓ 已更新' if changed else '· 已是最新'}  {path.name}  → ws://127.0.0.1:{port}")
    return 0


def list_configs() -> int:
    config_dir = detect_config_dir()
    if not config_dir.exists():
        print("配置目录不存在：", config_dir)
        return 2
    files = sorted(config_dir.glob("*.json"))
    if not files:
        print("配置目录是空的。")
        return 0
    for f in files:
        print(f"{f.name:<28} {f.stat().st_size:>7} B")
    return 0


def check(port: int = WS_PORT) -> int:
    print("── NapCat 体检 ──")
    ok = True
    print(f"目录：{NAPCAT_DIR}  {'存在' if NAPCAT_DIR.exists() else '缺失'}")
    if not NAPCAT_DIR.exists():
        return 2

    launchers = [p.name for p in NAPCAT_DIR.iterdir() if p.suffix.lower() in (".bat", ".exe", ".cmd")]
    print(f"启动器：{', '.join(launchers) or '未找到'}")
    if not launchers:
        ok = False

    configs = sorted(detect_config_dir().glob("onebot11_*.json"))
    print(f"账号配置：{[c.name for c in configs] or '无（还没登录过）'}")
    for c in configs:
        try:
            data = json.loads(c.read_text(encoding="utf-8"))
            servers = data.get("network", {}).get("websocketServers", [])
            mine = [s for s in servers if s.get("name") == WS_NAME]
            print(f"  {c.name}: websocketServers={len(servers)}，本机器人={'已配置' if mine else '未配置'}")
            if not mine:
                ok = False
        except json.JSONDecodeError:
            print(f"  {c.name}: JSON 解析失败")
            ok = False

    import socket

    with socket.socket() as s:
        s.settimeout(0.4)
        in_use = s.connect_ex(("127.0.0.1", port)) == 0
    print(f"端口 {port}：{'已被监听（NapCat 应该在跑）' if in_use else '空闲（NapCat 没启动）'}")
    print("结论：", "可以启动" if ok else "还需要处理上面标出的项")
    return 0 if ok else 1


def _fetch_release_info() -> dict:
    req = urllib.request.Request(RELEASE_API, headers={"User-Agent": "dafeiyu-bot/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        print(f"  ! 查不到最新版本（{exc}），改用内置的 v4.18.41")
        return {
            "tag_name": "v4.18.41",
            "assets": [
                {
                    "name": PREFERRED_ASSET,
                    "size": 124_000_000,
                    "browser_download_url": (
                        "https://github.com/NapNeko/NapCatQQ/releases/download/v4.18.41/" + PREFERRED_ASSET
                    ),
                }
            ],
        }


def _download_file(url: str, dest: Path, expect: int = 0) -> bool:
    """依次尝试直连 + 镜像，成功返回 True。"""
    for prefix in MIRRORS:
        target = (prefix + url) if prefix else url
        label = prefix.rstrip("/").split("//")[-1] if prefix else "GitHub 直连"
        try:
            print(f"  尝试 {label} …", flush=True)
            req = urllib.request.Request(target, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=45) as resp, open(dest, "wb") as fh:
                total = int(resp.headers.get("Content-Length") or expect or 0)
                done, last = 0, -1
                while chunk := resp.read(256 * 1024):
                    fh.write(chunk)
                    done += len(chunk)
                    if total:
                        pct = int(done * 100 / total)
                        if pct != last and pct % 10 == 0:
                            print(f"    {pct:>3}%  {done / 1048576:.1f}/{total / 1048576:.1f} MB", flush=True)
                            last = pct
                if done < 1024:
                    raise OSError("下载内容过小，多半被拦截了")
            print(f"  ✓ 完成（{done / 1048576:.1f} MB）")
            return True
        except Exception as exc:  # noqa: BLE001
            print(f"  × {label} 失败：{exc}")
            if dest.exists():
                dest.unlink()
    return False


def download(force: bool = False) -> int:
    print("查询 NapCat 最新版本…")
    info = _fetch_release_info()
    tag = info.get("tag_name", "?")
    assets = {a["name"]: a for a in info.get("assets", [])}
    asset = assets.get(PREFERRED_ASSET)
    if not asset:
        print(f"× 没找到 {PREFERRED_ASSET}，可选：{list(assets)}")
        return 2
    print(f"最新版本：{tag}  {asset['name']}  {asset['size'] / 1048576:.1f} MB")

    if NAPCAT_DIR.exists() and any(NAPCAT_DIR.iterdir()) and not force:
        print(f"· {NAPCAT_DIR} 已存在内容，跳过下载（要覆盖请加 --force）")
        return ensure()

    tmp = ROOT / "_dl" / asset["name"]
    tmp.parent.mkdir(parents=True, exist_ok=True)
    print(f"下载到 {tmp}")
    if not _download_file(asset["browser_download_url"], tmp, asset["size"]):
        print("× 所有线路都失败了。可以手动下载后放到 _dl/ 目录再跑 --ensure：")
        print(f"  {asset['browser_download_url']}")
        return 3

    print("解压…")
    NAPCAT_DIR.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(tmp) as zf:
        zf.extractall(NAPCAT_DIR)
    print("✓ 完成")
    return ensure()


def main() -> int:
    ap = argparse.ArgumentParser(description="NapCat 安装/配置工具")
    ap.add_argument("--download", action="store_true", help="下载并解压最新 NapCat")
    ap.add_argument("--ensure", action="store_true", help="写入/修补 OneBot11 配置")
    ap.add_argument("--list", action="store_true", help="列出配置文件")
    ap.add_argument("--check", action="store_true", help="体检")
    ap.add_argument("--port", type=int, default=WS_PORT)
    ap.add_argument("--token", default="", help="WS 鉴权 token（可选）")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if args.download:
        return download(args.force)
    if args.list:
        return list_configs()
    if args.check:
        return check(args.port)
    return ensure(args.port, args.token)


if __name__ == "__main__":
    raise SystemExit(main())
