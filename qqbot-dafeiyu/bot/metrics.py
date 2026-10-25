"""轻量指标：只记「每天多少条消息」，给控制台画趋势用。

不落库到 groups.json 里 —— 那是按群存的，画全站趋势得再汇总一遍，没必要。
单独一个小文件，读的时候整个加载，写的时候整块覆盖（几十个 key，开销可以忽略）。
"""

from __future__ import annotations

import threading
import time
from pathlib import Path

from .storage import JsonStore

KEEP_DAYS = 30


class Metrics:
    def __init__(self, path: Path):
        self.store = JsonStore(path, default={"days": {}})
        self._lock = threading.Lock()

    def bump(self, n: int = 1) -> None:
        day = time.strftime("%Y-%m-%d")
        with self._lock:
            days = dict(self.store.get("days", {}) or {})
            days[day] = int(days.get(day, 0)) + n
            # 只留最近 30 天，别让文件无限长
            if len(days) > KEEP_DAYS:
                keep = sorted(days)[-KEEP_DAYS:]
                days = {k: days[k] for k in keep}
            self.store.update({"days": days}, save=False)
            self._dirty = True

    def flush(self) -> None:
        if getattr(self, "_dirty", False):
            self.store.flush()
            self._dirty = False

    def series(self, days: int = 7) -> list[dict]:
        """返回最近 N 天（含今天），缺的那天补 0。"""
        raw = self.store.get("days", {}) or {}
        out = []
        now = time.time()
        for i in range(days - 1, -1, -1):
            d = time.strftime("%Y-%m-%d", time.localtime(now - i * 86400))
            out.append({"date": d, "count": int(raw.get(d, 0))})
        return out

    def today(self) -> int:
        return int((self.store.get("days", {}) or {}).get(time.strftime("%Y-%m-%d"), 0))
