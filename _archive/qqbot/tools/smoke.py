"""冒烟测试：启动完整机器人主循环，保持 15 秒无异常即通过。

运行：py main.py --smoke
"""
import asyncio
import sys

from main import Bot, load_config, setup_logging

SECONDS = 15


def run():
    config = load_config()
    setup_logging(config.get("log_file", "bot.log"))
    print(f"[..] 启动机器人主循环，保持 {SECONDS} 秒…")

    async def _run():
        bot = Bot(config)
        task = asyncio.create_task(bot.run())
        await asyncio.sleep(SECONDS)
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass

    try:
        asyncio.run(_run())
        print(f"冒烟测试通过 ✅（{SECONDS} 秒内网关连接与心跳无异常）")
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print(f"冒烟测试失败 ❌：{e}")
        sys.exit(1)
