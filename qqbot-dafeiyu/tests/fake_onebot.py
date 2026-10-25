"""假 OneBot 11 服务端：用于离线跑通「事件进来 → 机器人处理 → API 调用出去」全链路。

它只实现机器人用到的那部分 action，其余一律回空数据。测试里可以：
    srv.push(event)          推一条事件给机器人
    srv.calls                [(action, params), ...]  按顺序记录
    srv.sent                 只挑 send_group_msg / send_private_msg
"""

from __future__ import annotations

import asyncio
import itertools
import json
import time

import websockets

SELF_ID = 10000


class FakeOneBot:
    def __init__(self, port: int = 30099):
        self.port = port
        self.calls: list[tuple[str, dict]] = []
        self.sent: list[dict] = []
        self.members = {
            10000: {"user_id": 10000, "nickname": "大肥鱼", "card": "", "role": "admin"},
            20001: {"user_id": 20001, "nickname": "群主老王", "card": "", "role": "owner"},
            20002: {"user_id": 20002, "nickname": "管理员小李", "card": "", "role": "admin"},
            20003: {"user_id": 20003, "nickname": "普通群友", "card": "小鱼干", "role": "member"},
            20004: {"user_id": 20004, "nickname": "捣乱的", "card": "", "role": "member"},
            # 两个「死人」：100 天没说话
            20005: {"user_id": 20005, "nickname": "潜水王", "card": "", "role": "member",
                    "last_sent_time": int(time.time()) - 100 * 86400},
            20006: {"user_id": 20006, "nickname": "从没说过话", "card": "", "role": "member",
                    "last_sent_time": 0, "join_time": int(time.time()) - 200 * 86400},
            # 哨兵测试用：一个会发违规内容的，一个只发轻度不当的
            20007: {"user_id": 20007, "nickname": "暴躁老哥", "card": "", "role": "member"},
            20008: {"user_id": 20008, "nickname": "嘴臭一点", "card": "", "role": "member"},
        }
        # 活跃成员补上「刚说过话」，否则会被当成死人
        for _uid in (20001, 20002, 20003, 20004, 10000, 20007, 20008):
            self.members[_uid]["last_sent_time"] = int(time.time())
            self.members[_uid].setdefault("join_time", int(time.time()) - 300 * 86400)
        self._clients: set = set()
        self._server = None
        self._echo = itertools.count(1)
        self._msg_id = itertools.count(9000)
        self.connected = asyncio.Event()

    # ---- 生命周期 ---------------------------------------------------
    async def start(self) -> None:
        self._server = await websockets.serve(self._handle, "127.0.0.1", self.port)
        await self._server.start_serving()

    async def stop(self) -> None:
        for ws in list(self._clients):
            await ws.close()
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def _handle(self, ws) -> None:
        self._clients.add(ws)
        self.connected.set()
        try:
            async for raw in ws:
                payload = json.loads(raw)
                action = payload.get("action")
                params = payload.get("params") or {}
                echo = payload.get("echo")
                self.calls.append((action, params))
                data = self._reply_data(action, params)
                await ws.send(json.dumps({"status": "ok", "retcode": 0, "data": data, "echo": echo}))
        except Exception:  # noqa: BLE001
            pass
        finally:
            self._clients.discard(ws)

    def _reply_data(self, action: str, params: dict):
        if action == "get_login_info":
            return {"user_id": SELF_ID, "nickname": "大肥鱼"}
        if action in ("send_group_msg", "send_private_msg"):
            self.sent.append({"action": action, **params})
            return {"message_id": next(self._msg_id)}
        if action == "get_group_member_info":
            uid = int(params.get("user_id") or 0)
            return self.members.get(uid, {"user_id": uid, "nickname": str(uid), "role": "member"})
        if action == "get_group_member_list":
            return list(self.members.values())
        if action == "get_group_info":
            return {"group_id": params.get("group_id"), "group_name": "测试群", "member_count": len(self.members),
                    "max_member_count": 500}
        if action == "get_group_list":
            return [
                {"group_id": 555000111, "group_name": "测试群", "member_count": len(self.members)},
                {"group_id": 555000222, "group_name": "第二个群", "member_count": 12},
            ]
        if action == "get_msg":
            return {"message_id": params.get("message_id"), "sender": {"user_id": 20003}, "message": []}
        return None

    # ---- 推送事件 ---------------------------------------------------
    async def push(self, event: dict) -> None:
        event.setdefault("self_id", SELF_ID)
        for ws in list(self._clients):
            await ws.send(json.dumps(event))

    async def push_group_msg(self, user_id: int, text: str, *, at: bool = False, message_id: int | None = None,
                             extra_segments: list[dict] | None = None) -> int:
        mid = message_id or next(self._msg_id)
        member = self.members.get(user_id, {"role": "member", "nickname": str(user_id)})
        segments: list[dict] = []
        if at:
            segments.append({"type": "at", "data": {"qq": str(SELF_ID)}})
            segments.append({"type": "text", "data": {"text": " "}})
        segments.append({"type": "text", "data": {"text": text}})
        if extra_segments:
            segments.extend(extra_segments)
        await self.push({
            "post_type": "message",
            "message_type": "group",
            "sub_type": "normal",
            "message_id": mid,
            "group_id": 555000111,
            "user_id": user_id,
            "message": segments,
            "raw_message": text,
            "sender": {"user_id": user_id, "nickname": member.get("nickname"),
                       "card": member.get("card", ""), "role": member.get("role", "member")},
            "time": 1700000000,
        })
        return mid

    async def push_private_msg(self, user_id: int, text: str) -> int:
        mid = next(self._msg_id)
        await self.push({
            "post_type": "message",
            "message_type": "private",
            "sub_type": "friend",
            "message_id": mid,
            "user_id": user_id,
            "message": [{"type": "text", "data": {"text": text}}],
            "raw_message": text,
            "sender": {"user_id": user_id, "nickname": "私聊的人", "role": "member"},
            "time": 1700000000,
        })
        return mid

    async def push_group_request(self, user_id: int, comment: str, flag: str = "FLAG123") -> None:
        await self.push({
            "post_type": "request",
            "request_type": "group",
            "sub_type": "add",
            "group_id": 555000111,
            "user_id": user_id,
            "comment": comment,
            "flag": flag,
            "time": 1700000000,
        })

    async def push_group_increase(self, user_id: int) -> None:
        await self.push({
            "post_type": "notice",
            "notice_type": "group_increase",
            "sub_type": "approve",
            "group_id": 555000111,
            "user_id": user_id,
            "time": 1700000000,
        })

    # ---- 断言辅助 ---------------------------------------------------
    def actions(self) -> list[str]:
        return [a for a, _ in self.calls]

    def find(self, action: str) -> list[dict]:
        return [p for a, p in self.calls if a == action]

    def last_text(self) -> str:
        if not self.sent:
            return ""
        msg = self.sent[-1].get("message")
        if isinstance(msg, str):
            return msg
        return "".join(str(s.get("data", {}).get("text", "")) for s in msg or [] if s.get("type") == "text")

    def last_segments(self) -> list[dict]:
        if not self.sent:
            return []
        msg = self.sent[-1].get("message")
        return msg if isinstance(msg, list) else []

    def reset(self) -> None:
        self.calls.clear()
        self.sent.clear()
