"""事件解析：把 OneBot 的原始 dict 包成好用的对象。

重点解决三件事：
1. 消息段（array）与 CQ 码（string）两种上报格式都能吃；
2. 群里 @ 了谁、回复了哪条、有没有图片，一眼能拿到；
3. 命令解析时文本已经去掉 @机器人 和前缀，直接就是参数。
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Any

CQ_RE = re.compile(r"\[CQ:([a-zA-Z_]+)((?:,[^\]]*)?)\]")


def _cq_unescape(text: str) -> str:
    return (
        text.replace("&#91;", "[")
        .replace("&#93;", "]")
        .replace("&#44;", ",")
        .replace("&amp;", "&")
    )


def parse_cq_string(message: str) -> list[dict]:
    """把 CQ 码字符串转成消息段数组。"""
    segments: list[dict] = []
    pos = 0
    for m in CQ_RE.finditer(message):
        if m.start() > pos:
            segments.append({"type": "text", "data": {"text": _cq_unescape(message[pos:m.start()])}})
        params: dict[str, str] = {}
        raw = m.group(2).lstrip(",")
        for pair in raw.split(","):
            if "=" in pair:
                k, v = pair.split("=", 1)
                params[k] = _cq_unescape(v)
        seg_type = m.group(1)
        if seg_type == "at":
            segments.append({"type": "at", "data": {"qq": params.get("qq", "")}})
        elif seg_type == "reply":
            segments.append({"type": "reply", "data": {"id": params.get("id", "")}})
        elif seg_type == "image":
            segments.append({"type": "image", "data": params})
        elif seg_type == "face":
            segments.append({"type": "face", "data": params})
        else:
            segments.append({"type": seg_type, "data": params})
        pos = m.end()
    if pos < len(message):
        segments.append({"type": "text", "data": {"text": _cq_unescape(message[pos:])}})
    return segments


def segments_of(raw: dict) -> list[dict]:
    msg = raw.get("message")
    if isinstance(msg, list):
        return msg
    if isinstance(msg, str):
        return parse_cq_string(msg)
    # 有些实现只给 raw_message
    raw_message = raw.get("raw_message")
    if isinstance(raw_message, str):
        return parse_cq_string(raw_message)
    return []


@dataclass
class Event:
    raw: dict
    segments: list[dict] = field(default_factory=list)

    # ---- 基础字段 ---------------------------------------------------
    @property
    def post_type(self) -> str:
        return self.raw.get("post_type", "")

    @property
    def message_type(self) -> str:
        return self.raw.get("message_type", "")

    @property
    def notice_type(self) -> str:
        return self.raw.get("notice_type", "")

    @property
    def request_type(self) -> str:
        return self.raw.get("request_type", "")

    @property
    def sub_type(self) -> str:
        return self.raw.get("sub_type", "")

    @property
    def self_id(self) -> int:
        return int(self.raw.get("self_id") or 0)

    @property
    def user_id(self) -> int:
        return int(self.raw.get("user_id") or 0)

    @property
    def group_id(self) -> int:
        return int(self.raw.get("group_id") or 0)

    @property
    def message_id(self) -> int:
        return int(self.raw.get("message_id") or 0)

    @property
    def is_group(self) -> bool:
        return self.message_type == "group"

    @property
    def is_private(self) -> bool:
        return self.message_type == "private"

    @property
    def sender(self) -> dict:
        return self.raw.get("sender") or {}

    @property
    def nickname(self) -> str:
        return self.sender.get("card") or self.sender.get("nickname") or str(self.user_id)

    @property
    def role(self) -> str:
        """member / admin / owner"""
        return self.sender.get("role") or "member"

    # ---- 消息内容 ---------------------------------------------------
    @property
    def text(self) -> str:
        """纯文本（@ 段变成空格，回复段丢弃）。"""
        parts: list[str] = []
        for seg in self.segments:
            t = seg.get("type")
            if t == "text":
                parts.append(str(seg.get("data", {}).get("text", "")))
            elif t == "at":
                parts.append(" ")
            elif t == "face":
                parts.append(" ")
        return "".join(parts)

    @property
    def at_ids(self) -> list[int]:
        out: list[int] = []
        for seg in self.segments:
            if seg.get("type") == "at":
                qq = str(seg.get("data", {}).get("qq", ""))
                if qq.isdigit():
                    out.append(int(qq))
        return out

    @property
    def at_all(self) -> bool:
        return any(str(seg.get("data", {}).get("qq", "")) == "all" for seg in self.segments if seg.get("type") == "at")

    @property
    def at_me(self) -> bool:
        return self.self_id in self.at_ids

    @property
    def reply_id(self) -> int | None:
        for seg in self.segments:
            if seg.get("type") == "reply":
                val = str(seg.get("data", {}).get("id", ""))
                if val.isdigit():
                    return int(val)
        return None

    @property
    def images(self) -> list[dict]:
        return [seg for seg in self.segments if seg.get("type") == "image"]

    @property
    def has_media(self) -> bool:
        return any(seg.get("type") in {"image", "record", "video", "file"} for seg in self.segments)

    @property
    def time(self) -> float:
        return float(self.raw.get("time") or time.time())

    # ---- 会话键 -----------------------------------------------------
    @property
    def session_key(self) -> str:
        """同一个群共用一个上下文；私聊按人隔离。"""
        if self.is_group:
            return f"group:{self.group_id}"
        return f"private:{self.user_id}"


def wrap(raw: dict) -> Event:
    return Event(raw=raw, segments=segments_of(raw))


# ---- 消息构造 -------------------------------------------------------
def at(user_id: int) -> dict:
    return {"type": "at", "data": {"qq": str(user_id)}}


def text(content: str) -> dict:
    return {"type": "text", "data": {"text": content}}


def reply(message_id: int) -> dict:
    return {"type": "reply", "data": {"id": str(message_id)}}


def image(file: str) -> dict:
    return {"type": "image", "data": {"file": file}}


def build(*segments: Any) -> list[dict]:
    """把 str / int(当 @) / 消息段混着写，拼成消息段数组。"""
    out: list[dict] = []
    for seg in segments:
        if isinstance(seg, str):
            if seg:
                out.append(text(seg))
        elif isinstance(seg, int):
            out.append(at(seg))
        elif isinstance(seg, dict):
            out.append(seg)
        elif isinstance(seg, (list, tuple)):
            out.extend(build(*seg))
    return out


def plain(message: Any) -> str:
    """把消息段数组还原成可读文本（用于日志/引用）。"""
    if isinstance(message, str):
        return message
    parts = []
    for seg in message or []:
        t = seg.get("type")
        d = seg.get("data", {})
        if t == "text":
            parts.append(str(d.get("text", "")))
        elif t == "at":
            parts.append(f"@{d.get('qq', '')}")
        elif t == "image":
            parts.append("[图片]")
        elif t == "face":
            parts.append("[表情]")
    return "".join(parts)
