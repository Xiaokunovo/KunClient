"""QQ 机器人 WebSocket 网关客户端。

协议（官方文档）：连接 /gateway 拿 wss 地址后——
  1. 收到 Hello(op=10, 含 heartbeat_interval)
  2. 发送 Identify(op=2, token="QQBot xxx", intents, shard) ；断线重连可改为 Resume(op=6)
  3. 收到 READY(op=0, t=READY, 含 session_id)
  4. 按周期发心跳 op=1（d 为最新 s，首次 null），服务端回 op=11
  5. 服务端推送事件 op=0（t 为事件类型，d 为数据）
异常断线自动重连：优先 Resume 补发漏掉的事件，失败则重新 Identify。
"""
import asyncio
import json
import logging

import aiohttp

log = logging.getLogger("qq.ws")

OP_DISPATCH = 0
OP_HEARTBEAT = 1
OP_IDENTIFY = 2
OP_RESUME = 6
OP_RECONNECT = 7
OP_INVALID_SESSION = 9
OP_HELLO = 10
OP_HEARTBEAT_ACK = 11

# 群聊 + 单聊事件（GROUP_AT_MESSAGE_CREATE / C2C_MESSAGE_CREATE / 机器人进退群 / 好友增删等）
INTENT_GROUP_AND_C2C = 1 << 25


class Gateway:
    def __init__(self, api, tokens, intents: int, on_dispatch):
        self.api = api
        self.tokens = tokens
        self.intents = intents
        self.on_dispatch = on_dispatch  # async fn(event_type, data)
        self.session_id: str | None = None
        self.last_seq = None
        self.bot_user: dict = {}
        self._hb_acked = asyncio.Event()
        self._stop = asyncio.Event()
        self._ready = asyncio.Event()   # READY/RESUMED 后才允许发心跳
        self.connected = False          # READY 之后为 True
        self.reconnect_count = 0        # 累计重连次数
        self._ws = None                 # 当前连接，供 force_reconnect 使用

    async def run(self):
        backoff = 5
        while not self._stop.is_set():
            try:
                await self._connect_once()
                backoff = 5
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("网关连接断开: %s", e)
            if self._stop.is_set():
                break
            self.reconnect_count += 1
            log.info("%.0f 秒后重连网关...", backoff)
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)

    async def _connect_once(self):
        self._ready.clear()
        url = await self.api.gateway_url()
        log.info("连接网关 %s", url)
        kwargs = {}
        ws_timeout = getattr(aiohttp, "ClientWSTimeout", None)
        if ws_timeout is not None:
            kwargs["timeout"] = ws_timeout(ws_receive=None, ws_close=15)
        async with self.api.session.ws_connect(url, max_msg_size=16 * 1024 * 1024, **kwargs) as ws:
            self._ws = ws
            first = await ws.receive()
            if first.type != aiohttp.WSMsgType.TEXT:
                raise RuntimeError(f"网关首包异常: {first}")
            hello = json.loads(first.data)
            if hello.get("op") != OP_HELLO:
                raise RuntimeError(f"网关首包 op 错误: {hello}")
            interval_ms = (hello.get("d") or {}).get("heartbeat_interval", 30000)

            if self.session_id and self.last_seq is not None:
                token = await self.tokens.get_token()
                await ws.send_json({"op": OP_RESUME, "d": {
                    "token": f"QQBot {token}", "session_id": self.session_id, "seq": self.last_seq}})
                log.info("尝试恢复会话 %s (seq=%s)", self.session_id, self.last_seq)
            else:
                await self._identify(ws)

            hb_task = asyncio.create_task(self._heartbeat_loop(ws, interval_ms / 1000))
            try:
                async for msg in ws:
                    if msg.type != aiohttp.WSMsgType.TEXT:
                        if msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.CLOSING,
                                        aiohttp.WSMsgType.ERROR):
                            break
                        continue
                    payload = json.loads(msg.data)
                    op = payload.get("op")
                    if op == OP_DISPATCH:
                        self.last_seq = payload.get("s")
                        event_type = payload.get("t")
                        data = payload.get("d") or {}
                        if event_type == "READY":
                            self.session_id = data.get("session_id")
                            self.bot_user = data.get("user") or {}
                            self.connected = True
                            self._ready.set()
                            log.info("鉴权成功：机器人 %s（session=%s）",
                                     self.bot_user.get("username"), self.session_id)
                        elif event_type == "RESUMED":
                            self._ready.set()
                            log.info("会话已恢复，补发完成")
                        try:
                            await self.on_dispatch(event_type, data)
                        except Exception:
                            log.exception("处理事件 %s 出错", event_type)
                    elif op == OP_HEARTBEAT_ACK:
                        self._hb_acked.set()
                    elif op == OP_RECONNECT:
                        log.info("服务端要求重连")
                        break
                    elif op == OP_INVALID_SESSION:
                        log.warning("会话无效，改为重新鉴权")
                        self.session_id = None
                        self.last_seq = None
                        break
                    elif op == OP_HELLO:
                        await self._identify(ws)
            finally:
                hb_task.cancel()
                self.connected = False

    async def force_reconnect(self):
        """主动断开当前连接，触发自动重连（含会话恢复）。"""
        ws = self._ws
        if ws is not None and not ws.closed:
            await ws.close(code=4000)

    async def _identify(self, ws):
        token = await self.tokens.get_token()
        await ws.send_json({"op": OP_IDENTIFY, "d": {
            "token": f"QQBot {token}",
            "intents": self.intents,
            "shard": [0, 1],
        }})
        log.info("已发送 Identify（intents=%d）", self.intents)

    async def _heartbeat_loop(self, ws, interval: float):
        try:
            # 关键：READY 之前发的第一跳心跳会被网关忽略（实测），
            # 导致永远等不到 op11 而反复误判超时，必须等鉴权完成后再跳。
            try:
                await asyncio.wait_for(self._ready.wait(), timeout=interval)
            except asyncio.TimeoutError:
                log.warning("等待 READY 超时，仍尝试发送心跳")
            while True:
                self._hb_acked.clear()
                try:
                    await ws.send_json({"op": OP_HEARTBEAT, "d": self.last_seq})
                except Exception as e:
                    log.warning("心跳发送失败: %s", e)
                    await ws.close()
                    return
                try:
                    await asyncio.wait_for(self._hb_acked.wait(), timeout=interval + 10)
                except asyncio.TimeoutError:
                    log.warning("心跳超时无应答，触发重连")
                    await ws.close()
                    return
                await asyncio.sleep(interval)
        except asyncio.CancelledError:
            pass
