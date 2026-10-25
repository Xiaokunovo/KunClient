"""对话上下文：按会话（群/私聊）维护滑动窗口，带 TTL 和落盘。"""

from __future__ import annotations

import json
import time
from collections import deque
from pathlib import Path
from typing import Iterable

from .storage import atomic_write_json


class ContextStore:
    def __init__(self, path: Path, rounds: int = 8, ttl: float = 1800):
        self.path = path
        self.rounds = max(1, rounds)
        self.ttl = ttl
        # session_key -> {"messages": [...], "ts": float}
        self._data: dict[str, dict] = {}
        self._dirty = False
        self._load()

    def _load(self) -> None:
        if not self.path.exists():
            return
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            now = time.time()
            for key, item in raw.items():
                if now - float(item.get("ts", 0)) <= self.ttl:
                    self._data[key] = item
        except (json.JSONDecodeError, OSError, TypeError, ValueError):
            self._data = {}

    def flush(self) -> None:
        if not self._dirty:
            return
        atomic_write_json(self.path, self._data)
        self._dirty = False

    # ---- 读 ---------------------------------------------------------
    def history(self, key: str) -> list[dict]:
        item = self._data.get(key)
        if not item:
            return []
        if time.time() - float(item.get("ts", 0)) > self.ttl:
            self._data.pop(key, None)
            self._dirty = True
            return []
        return list(item.get("messages", []))

    def turns(self, key: str) -> int:
        return len(self.history(key)) // 2

    # ---- 写 ---------------------------------------------------------
    def append(self, key: str, user_text: str, assistant_text: str) -> None:
        item = self._data.setdefault(key, {"messages": [], "ts": time.time()})
        msgs: deque = deque(item.get("messages", []), maxlen=self.rounds * 2)
        msgs.append({"role": "user", "content": user_text})
        msgs.append({"role": "assistant", "content": assistant_text})
        item["messages"] = list(msgs)
        item["ts"] = time.time()
        self._dirty = True

    def clear(self, key: str) -> int:
        item = self._data.pop(key, None)
        self._dirty = True
        return len(item.get("messages", [])) if item else 0

    def clear_prefix(self, prefix: str) -> int:
        keys = [k for k in self._data if k.startswith(prefix)]
        for k in keys:
            self._data.pop(k, None)
        self._dirty = bool(keys)
        return len(keys)

    def build_messages(self, key: str, system_prompt: str, user_text: str, extra: Iterable[dict] = ()) -> list[dict]:
        msgs: list[dict] = [{"role": "system", "content": system_prompt}]
        msgs.extend(self.history(key))
        msgs.extend(extra)
        msgs.append({"role": "user", "content": user_text})
        return msgs

    def sweep(self) -> int:
        now = time.time()
        dead = [k for k, v in self._data.items() if now - float(v.get("ts", 0)) > self.ttl]
        for k in dead:
            self._data.pop(k, None)
        if dead:
            self._dirty = True
        return len(dead)
