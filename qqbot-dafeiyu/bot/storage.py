"""持久化小工具：原子写 JSON + 线程安全的内存台账。"""

from __future__ import annotations

import json
import os
import tempfile
import threading
import time
from collections import deque
from pathlib import Path
from typing import Any


def atomic_write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


class JsonStore:
    """一个 JSON 文件 = 一个 dict，读一次缓存在内存，写时整体落盘。"""

    def __init__(self, path: Path, default: dict | None = None):
        self.path = path
        self._lock = threading.RLock()
        self._data: dict = {}
        if path.exists():
            try:
                self._data = json.loads(path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                self._data = dict(default or {})
        else:
            self._data = dict(default or {})

    def get(self, key: str, default: Any = None) -> Any:
        with self._lock:
            return self._data.get(key, default)

    def set(self, key: str, value: Any, save: bool = True) -> None:
        with self._lock:
            self._data[key] = value
            if save:
                atomic_write_json(self.path, self._data)

    def update(self, patch: dict, save: bool = True) -> None:
        with self._lock:
            self._data.update(patch)
            if save:
                atomic_write_json(self.path, self._data)

    def all(self) -> dict:
        with self._lock:
            return dict(self._data)

    def flush(self) -> None:
        with self._lock:
            atomic_write_json(self.path, self._data)


class MessageLedger:
    """内存消息台账：为「撤回最近 N 条」提供 message_id → 发送者 的映射。

    OneBot 的 delete_msg 只需要 message_id，但要判断「能不能撤别人的」就得知道发送者。
    每个群保留最近 200 条，进程重启即清空（够用，且不落盘避免膨胀）。
    """

    def __init__(self, per_group: int = 200):
        self.per_group = per_group
        self._data: dict[int, deque] = {}
        self._lock = threading.Lock()

    def add(self, group_id: int, message_id: int, user_id: int, text: str = "") -> None:
        with self._lock:
            dq = self._data.setdefault(group_id, deque(maxlen=self.per_group))
            dq.append({"id": message_id, "uid": user_id, "t": time.time(), "text": text[:80]})

    def recent(self, group_id: int, limit: int = 10) -> list[dict]:
        with self._lock:
            dq = self._data.get(group_id)
            if not dq:
                return []
            return list(dq)[-limit:]

    def find(self, group_id: int, message_id: int) -> dict | None:
        with self._lock:
            for item in self._data.get(group_id, ()):
                if item["id"] == message_id:
                    return item
        return None

    def clear(self, group_id: int) -> None:
        with self._lock:
            self._data.pop(group_id, None)


class Counter:
    """滑动窗口计数器：刷屏检测 / 冷却 / 频率限制都用它。"""

    def __init__(self, window: float, limit: int):
        self.window = window
        self.limit = limit
        self._hits: dict[str, deque] = {}
        self._lock = threading.Lock()

    def hit(self, key: str) -> int:
        """记一次，返回窗口内的总次数。"""
        now = time.time()
        with self._lock:
            dq = self._hits.setdefault(key, deque())
            dq.append(now)
            while dq and now - dq[0] > self.window:
                dq.popleft()
            return len(dq)

    def exceeded(self, key: str) -> bool:
        return self.hit(key) > self.limit

    def reset(self, key: str) -> None:
        with self._lock:
            self._hits.pop(key, None)

    def sweep(self) -> None:
        """清掉过期 key，防止长时间运行内存膨胀。"""
        now = time.time()
        with self._lock:
            for k in [k for k, dq in self._hits.items() if not dq or now - dq[-1] > self.window * 4]:
                self._hits.pop(k, None)
