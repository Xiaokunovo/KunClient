"""定时提醒。

存在 data/reminders.json，机器人每 10 秒扫一次到期的。
不用 asyncio.call_later：进程重启后内存里的定时器全丢，落盘才扛得住重启。
"""

from __future__ import annotations

import itertools
import threading
import time
from pathlib import Path

from .storage import JsonStore

MAX_PER_GROUP = 50


class Reminders:
    def __init__(self, path: Path):
        self.store = JsonStore(path, default={"items": [], "seq": 0})
        self._lock = threading.RLock()

    # ---- 写 ---------------------------------------------------------
    def add(self, group_id: int, user_id: int, text: str, seconds: int) -> int | None:
        """返回提醒编号；本群提醒太多返回 None。"""
        with self._lock:
            data = self.store.all()
            items = data.get("items", [])
            if sum(1 for i in items if i.get("gid") == group_id) >= MAX_PER_GROUP:
                return None
            seq = int(data.get("seq", 0)) + 1
            items.append({
                "id": seq,
                "gid": group_id,
                "uid": user_id,
                "text": text[:200],
                "due": time.time() + max(1, seconds),
            })
            self.store.update({"items": items, "seq": seq})
            return seq

    def cancel(self, group_id: int, user_id: int, rid: int) -> bool:
        with self._lock:
            data = self.store.all()
            items = data.get("items", [])
            keep = [i for i in items if not (i.get("id") == rid and i.get("gid") == group_id
                                             and i.get("uid") == user_id)]
            if len(keep) == len(items):
                return False
            self.store.update({"items": keep})
            return True

    def list_of(self, group_id: int, user_id: int | None = None) -> list[dict]:
        items = self.store.get("items", []) or []
        out = [i for i in items if i.get("gid") == group_id
               and (user_id is None or i.get("uid") == user_id)]
        out.sort(key=lambda i: i.get("due", 0))
        return out

    # ---- 读 ---------------------------------------------------------
    def pop_due(self, now: float | None = None) -> list[dict]:
        """取出所有已到期的并删掉。"""
        now = now or time.time()
        with self._lock:
            data = self.store.all()
            items = data.get("items", [])
            due = [i for i in items if float(i.get("due", 0)) <= now]
            if not due:
                return []
            keep = [i for i in items if float(i.get("due", 0)) > now]
            self.store.update({"items": keep})
            return due

    def count(self) -> int:
        return len(self.store.get("items", []) or [])


def fmt_left(seconds: float) -> str:
    seconds = int(max(0, seconds))
    if seconds >= 86400:
        return f"{seconds // 86400}天{seconds % 86400 // 3600}小时"
    if seconds >= 3600:
        return f"{seconds // 3600}小时{seconds % 3600 // 60}分"
    if seconds >= 60:
        return f"{seconds // 60}分钟"
    return f"{seconds}秒"
