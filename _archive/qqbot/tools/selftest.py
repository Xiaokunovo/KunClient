"""自检：QQ 凭证 + 网关鉴权。运行：py main.py --selftest"""
import asyncio
import json
import logging
import os
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(BASE)
if BASE not in sys.path:
    sys.path.insert(0, BASE)

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

API_BASE = "https://api.bot.qq.com"
API_BASE_FALLBACK = "https://api.sgroup.qq.com"


def run():
    try:
        ok = asyncio.run(_main())
    except KeyboardInterrupt:
        ok = False
    print("\n自检结论：" + ("全部通过 ✅" if ok else "存在失败项 ❌（见上方输出）"))
    sys.exit(0 if ok else 1)


async def _main() -> bool:
    logging.basicConfig(level=logging.WARNING, stream=sys.stdout)
    with open("config.json", "r", encoding="utf-8") as f:
        cfg = json.load(f)

    print("=" * 56)
    print("① QQ 凭证自检（getAppAccessToken）")
    print("=" * 56)
    from core.auth import TokenManager
    tokens = TokenManager(cfg["app_id"], cfg["app_secret"])
    token = None
    try:
        token = await tokens.get_token()
        print(f"[PASS] access_token 获取成功：{token[:12]}…")
    except Exception as e:
        print(f"[FAIL] 获取 access_token 失败：{e}")
        return False

    print()
    print("=" * 56)
    print("② QQ 网关自检（REST /gateway + WebSocket 鉴权）")
    print("=" * 56)
    ws_ok = await _ws_test(cfg, token)
    return ws_ok


async def _ws_test(cfg, token) -> bool:
    import aiohttp
    from core.gateway import INTENT_GROUP_AND_C2C
    async with aiohttp.ClientSession() as session:
        base = API_BASE
        try:
            async with session.get(f"{base}/gateway",
                                   headers={"Authorization": f"QQBot {token}"},
                                   timeout=aiohttp.ClientTimeout(total=15)) as r:
                if r.status >= 400:
                    raise RuntimeError(f"HTTP {r.status}")
                gw_url = (await r.json())["url"]
            print(f"[PASS] 获取网关地址：{gw_url}")
        except Exception as e:
            base = API_BASE_FALLBACK
            try:
                async with session.get(f"{base}/gateway",
                                       headers={"Authorization": f"QQBot {token}"},
                                       timeout=aiohttp.ClientTimeout(total=15)) as r:
                    gw_url = (await r.json())["url"]
                print(f"[PASS] 主域名异常({e})，旧域名获取网关成功：{gw_url}")
            except Exception as e2:
                print(f"[FAIL] 获取网关地址失败：{e} / {e2}")
                return False
        try:
            async with session.ws_connect(gw_url) as ws:
                hello = await ws.receive_json()
                if hello.get("op") != 10:
                    raise RuntimeError(f"首包异常: {hello}")
                interval = hello["d"].get("heartbeat_interval")
                await ws.send_json({"op": 2, "d": {
                    "token": f"QQBot {token}", "intents": INTENT_GROUP_AND_C2C, "shard": [0, 1]}})
                print(f"[..] 已发送 Identify，等待 READY…（心跳周期 {interval}ms）")

                async def wait_ready():
                    async for msg in ws:
                        if msg.type != aiohttp.WSMsgType.TEXT:
                            continue
                        p = msg.json()
                        if p.get("op") == 9:
                            raise RuntimeError("Invalid Session（intents 无权限或 token 错误）")
                        if p.get("t") == "READY":
                            return p["d"]
                ready = await asyncio.wait_for(wait_ready(), timeout=20)
                user = ready.get("user") or {}
                print(f"[PASS] WebSocket 鉴权成功：机器人 {user.get('username')} (id={user.get('id')})，"
                      f"session={ready.get('session_id')}")
                return True
        except asyncio.TimeoutError:
            print("[FAIL] 20 秒内未收到 READY")
        except Exception as e:
            print(f"[FAIL] WebSocket 鉴权失败：{e}")
        return False
