"""OneBot 11 客户端（正向 WebSocket）。

为什么用正向 WS：机器人主动连 NapCat，不用在公网开端口，也不用反向 WS 的服务端。
一条连接同时承载「事件推送」和「API 调用」，靠 echo 字段把响应和请求对上。
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
from typing import Any, Awaitable, Callable

import websockets

log = logging.getLogger("onebot")


class ApiError(RuntimeError):
    def __init__(self, action: str, retcode: Any, message: str = ""):
        self.action = action
        self.retcode = retcode
        super().__init__(f"{action} 失败 retcode={retcode} {message}".strip())


class OneBot:
    def __init__(
        self,
        ws_url: str,
        token: str = "",
        reconnect_min: float = 2,
        reconnect_max: float = 30,
        call_timeout: float = 15,
    ):
        self.ws_url = ws_url
        self.token = token
        self.reconnect_min = reconnect_min
        self.reconnect_max = reconnect_max
        self.call_timeout = call_timeout

        self.self_id: int = 0
        self.connected = False
        self._ws: Any = None
        self._echo = itertools.count(1)
        self._pending: dict[str, asyncio.Future] = {}
        self._handlers: list[Callable[[dict], Awaitable[None]]] = []
        self._stop = asyncio.Event()
        self._tasks: set[asyncio.Task] = set()

    # ---- 生命周期 ---------------------------------------------------
    def on_event(self, handler: Callable[[dict], Awaitable[None]]) -> None:
        self._handlers.append(handler)

    async def run(self) -> None:
        backoff = self.reconnect_min
        while not self._stop.is_set():
            try:
                await self._connect_once()
                backoff = self.reconnect_min  # 连上过就重置退避
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - 任何连接异常都走重连
                log.warning("连接断开：%s（%.0fs 后重连）", exc, backoff)
            self.connected = False
            try:
                await asyncio.wait_for(self._stop.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 1.8, self.reconnect_max)

    async def _connect_once(self) -> None:
        headers = {"Authorization": f"Bearer {self.token}"} if self.token else None
        kwargs: dict[str, Any] = {
            "ping_interval": 20,
            "ping_timeout": 20,
            "max_size": 32 * 1024 * 1024,
        }
        if headers:
            kwargs["additional_headers"] = headers
        try:
            conn = websockets.connect(self.ws_url, **kwargs)  # type: ignore[attr-defined]
        except TypeError:  # websockets < 14 用的是 extra_headers
            if headers:
                kwargs.pop("additional_headers", None)
                kwargs["extra_headers"] = headers
            conn = websockets.connect(self.ws_url, **kwargs)  # type: ignore[attr-defined]

        async with conn as ws:
            self._ws = ws
            self.connected = True
            log.info("已连上 OneBot 端：%s", self.ws_url)

            # 🔴 必须先起读循环，再发 API 请求。
            # 否则 get_login_info 的响应没人读，call() 会一直等到超时（15s），
            # 表现为「连上了但前 15 秒什么事件都不处理」。
            reader = asyncio.create_task(self._reader(ws))
            try:
                info = await self.call("get_login_info")
                self.self_id = int(info.get("user_id") or 0)
                log.info("登录账号：%s（%s）", self.self_id, info.get("nickname", ""))
            except Exception as exc:  # noqa: BLE001
                log.warning("取登录信息失败（不影响收发）：%s", exc)
            await reader
        self._ws = None

    async def _reader(self, ws) -> None:
        async for raw in ws:
            self._spawn(self._on_raw(raw))

    def _spawn(self, coro: Awaitable[None]) -> None:
        task = asyncio.create_task(coro)  # type: ignore[arg-type]
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _on_raw(self, raw: str | bytes) -> None:
        try:
            payload = json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            log.debug("收到非 JSON 帧：%r", raw[:120])
            return

        echo = payload.get("echo")
        if echo is not None and str(echo) in self._pending:
            fut = self._pending.pop(str(echo))
            if not fut.done():
                fut.set_result(payload)
            return

        post_type = payload.get("post_type")
        if post_type == "meta_event":
            if payload.get("meta_event_type") == "lifecycle" and payload.get("sub_type") == "connect":
                self.self_id = int(payload.get("self_id") or self.self_id)
            return

        for handler in self._handlers:
            try:
                await handler(payload)
            except Exception:  # noqa: BLE001 - 单个 handler 崩了不能影响其它
                log.exception("事件处理异常：%s", post_type)

    async def close(self) -> None:
        self._stop.set()
        if self._ws is not None:
            await self._ws.close()

    # ---- API 调用 ---------------------------------------------------
    async def call(self, action: str, **params: Any) -> Any:
        if self._ws is None:
            raise ApiError(action, "disconnected", "OneBot 未连接")
        echo = str(next(self._echo))
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[echo] = fut
        try:
            await self._ws.send(json.dumps({"action": action, "params": params, "echo": echo}))
            payload = await asyncio.wait_for(fut, timeout=self.call_timeout)
        except asyncio.TimeoutError as exc:
            raise ApiError(action, "timeout", f"{self.call_timeout}s 无响应") from exc
        finally:
            self._pending.pop(echo, None)

        retcode = payload.get("retcode", 0)
        if retcode not in (0, 1):  # 1 = 已异步提交，也算成功
            raise ApiError(action, retcode, str(payload.get("message") or payload.get("wording") or ""))
        return payload.get("data")

    # ---- 常用封装 ---------------------------------------------------
    async def send_group(self, group_id: int, message: Any, auto_escape: bool = False) -> int:
        data = await self.call("send_group_msg", group_id=group_id, message=message, auto_escape=auto_escape)
        return int((data or {}).get("message_id") or 0)

    async def send_private(self, user_id: int, message: Any, auto_escape: bool = False) -> int:
        data = await self.call("send_private_msg", user_id=user_id, message=message, auto_escape=auto_escape)
        return int((data or {}).get("message_id") or 0)

    async def delete_msg(self, message_id: int) -> None:
        await self.call("delete_msg", message_id=message_id)

    async def set_group_ban(self, group_id: int, user_id: int, duration: int) -> None:
        await self.call("set_group_ban", group_id=group_id, user_id=user_id, duration=int(duration))

    async def set_group_whole_ban(self, group_id: int, enable: bool = True) -> None:
        await self.call("set_group_whole_ban", group_id=group_id, enable=bool(enable))

    async def set_group_kick(self, group_id: int, user_id: int, reject_add_request: bool = False) -> None:
        await self.call(
            "set_group_kick",
            group_id=group_id,
            user_id=user_id,
            reject_add_request=bool(reject_add_request),
        )

    async def set_group_card(self, group_id: int, user_id: int, card: str) -> None:
        await self.call("set_group_card", group_id=group_id, user_id=user_id, card=card)

    async def set_group_name(self, group_id: int, name: str) -> None:
        await self.call("set_group_name", group_id=group_id, group_name=name)

    async def set_group_special_title(self, group_id: int, user_id: int, title: str) -> None:
        await self.call("set_group_special_title", group_id=group_id, user_id=user_id, special_title=title)

    async def send_group_notice(self, group_id: int, content: str) -> None:
        await self.call("_send_group_notice", group_id=group_id, content=content)

    async def set_essence_msg(self, message_id: int) -> None:
        await self.call("set_essence_msg", message_id=message_id)

    async def get_group_member_info(self, group_id: int, user_id: int) -> dict:
        return await self.call("get_group_member_info", group_id=group_id, user_id=user_id, no_cache=True) or {}

    async def get_group_member_list(self, group_id: int) -> list[dict]:
        return await self.call("get_group_member_list", group_id=group_id) or []

    async def get_group_info(self, group_id: int) -> dict:
        return await self.call("get_group_info", group_id=group_id, no_cache=True) or {}

    async def get_group_list(self) -> list[dict]:
        return await self.call("get_group_list") or []

    async def get_msg(self, message_id: int) -> dict:
        return await self.call("get_msg", message_id=message_id) or {}

    async def set_group_add_request(self, flag: str, approve: bool, reason: str = "") -> None:
        await self.call("set_group_add_request", flag=flag, sub_type="add", approve=bool(approve), reason=reason)

    async def send_like(self, user_id: int, times: int = 10) -> None:
        await self.call("send_like", user_id=user_id, times=times)

    async def set_msg_emoji_like(self, message_id: int, emoji_id: str = "128077") -> None:
        await self.call("set_msg_emoji_like", message_id=message_id, emoji_id=emoji_id)
