"""入口：python -m bot.main

用法：
    python -m bot.main           正常启动
    python -m bot.main --check   只检查配置和命令注册，不连 QQ
    python -m bot.main --help    看参数
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import logging.handlers
import os
import signal
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from bot.app import Bot  # noqa: E402
from bot.config import Config, ensure_config  # noqa: E402
from bot.router import router  # noqa: E402


def setup_logging(cfg: Config) -> None:
    level = getattr(logging, str(cfg.get("log.level", "INFO")).upper(), logging.INFO)
    log_dir = ROOT / "data" / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)

    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(name)-9s %(message)s", "%H:%M:%S")

    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(fmt)
    console.setLevel(level)

    file_handler = logging.handlers.TimedRotatingFileHandler(
        log_dir / "bot.log", when="midnight", backupCount=int(cfg.get("log.keep_days", 7)), encoding="utf-8"
    )
    file_handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(name)-9s %(message)s"))
    file_handler.setLevel(logging.DEBUG)

    root = logging.getLogger()
    root.setLevel(logging.DEBUG)
    root.handlers.clear()
    root.addHandler(console)
    root.addHandler(file_handler)

    # aiohttp / websockets 的 DEBUG 太吵
    logging.getLogger("websockets").setLevel(logging.WARNING)
    logging.getLogger("aiohttp").setLevel(logging.WARNING)
    logging.getLogger("asyncio").setLevel(logging.WARNING)


def load_commands() -> None:
    """import 一次命令包，装饰器才会把命令注册进 router。

    必须在 banner / check 之前调用 —— 否则横幅上的「命令 0 个已注册」会骗人。
    """
    import bot.commands  # noqa: F401


def banner(cfg: Config, problems: list[str]) -> None:
    load_commands()
    name = cfg.get("bot.name", "大肥鱼")
    line = "═" * 52
    print(f"\n{line}")
    print(f"  🐳  {name}  QQ 机器人  v1.0")
    print(f"{line}")
    print(f"  协议端   {cfg.get('onebot.ws_url')}")
    print(f"  模型     {cfg.get('deepseek.model')}  (key: {'已配置' if cfg.get('deepseek.api_key') else '未配置'})")
    supers = cfg.get("permission.superusers") or []
    print(f"  主人     {', '.join(map(str, supers)) if supers else '未设置（私聊我发「绑定主人」）'}")
    print(f"  命令     {len(router.unique())} 个已注册")
    if cfg.get("webui.enabled", True):
        token = cfg.ensure_webui_token()
        print(f"  控制台   http://{cfg.get('webui.host', '127.0.0.1')}:{cfg.get('webui.port', 8088)}")
        print(f"  口令     {token}")
    print(f"{line}")
    for p in problems:
        print(f"  ⚠  {p}")
    if problems:
        print(f"{line}")
    print()


def check_mode(cfg: Config, problems: list[str]) -> int:
    load_commands()
    print("── 自检 ──")
    print(f"配置：{cfg.path}")
    print(f"命令：{len(router.unique())} 个")
    by_cat: dict[str, list[str]] = {}
    for cmd in router.unique():
        by_cat.setdefault(cmd.category, []).append(cmd.name)
    for cat, names in by_cat.items():
        print(f"  {cat:<8} {' '.join(sorted(set(names)))}")
    print(f"DeepSeek Key：{'OK' if cfg.get('deepseek.api_key') else '缺失'}")
    print(f"主人：{cfg.get('permission.superusers') or '未设置'}")
    if problems:
        print("问题：")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("自检通过。")
    return 0


async def amain(cfg: Config) -> None:
    import bot.commands  # noqa: F401  触发命令注册

    bot = Bot(cfg, router)
    loop = asyncio.get_running_loop()
    stop = asyncio.Event()

    def _shutdown(*_args):
        if not stop.is_set():
            print("\n正在退出…")
            stop.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _shutdown)
        except (NotImplementedError, RuntimeError):
            signal.signal(sig, _shutdown)  # Windows 上 add_signal_handler 不支持

    run_task = asyncio.create_task(bot.start())
    stop_task = asyncio.create_task(stop.wait())
    done, _ = await asyncio.wait({run_task, stop_task}, return_when=asyncio.FIRST_COMPLETED)

    if run_task in done and run_task.exception():
        logging.getLogger("main").error("主循环异常退出", exc_info=run_task.exception())

    run_task.cancel()
    stop_task.cancel()
    await bot.shutdown()
    print("已退出。")


def main() -> int:
    if sys.platform == "win32":
        try:
            sys.stdout.reconfigure(encoding="utf-8")
            sys.stderr.reconfigure(encoding="utf-8")
        except Exception:  # noqa: BLE001
            pass
        os.system("")  # 打开 Windows 终端的 ANSI 支持

    parser = argparse.ArgumentParser(description="大肥鱼 QQ 机器人")
    parser.add_argument("--check", action="store_true", help="只做配置/命令自检，不连接 QQ")
    parser.add_argument("--config", default=None, help="指定配置文件路径")
    args = parser.parse_args()

    cfg = Config.load(Path(args.config)) if args.config else ensure_config()
    setup_logging(cfg)

    import bot.commands  # noqa: F401

    problems = cfg.validate()

    if args.check:
        return check_mode(cfg, problems)

    banner(cfg, problems)
    try:
        asyncio.run(amain(cfg))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
