"""全局黑名单：跨群生效，只有主人能维护。

为什么单独放一个文件而不是塞进 groups.json：
它是「全局」的，跟某个群无关；放群里会导致每个群都存一份，改一次要遍历所有群。
"""

from __future__ import annotations

import time
from pathlib import Path

from .storage import JsonStore


class GlobalBans:
    def __init__(self, path: Path):
        self.store = JsonStore(path, default={"global": {}})

    def _all(self) -> dict[str, dict]:
        return dict(self.store.get("global", {}) or {})

    def add(self, user_id: int, reason: str = "", by: int = 0) -> bool:
        data = self._all()
        if str(user_id) in data:
            return False
        data[str(user_id)] = {"reason": reason[:100], "by": by, "ts": int(time.time())}
        self.store.update({"global": data})
        return True

    def remove(self, user_id: int) -> bool:
        data = self._all()
        if str(user_id) not in data:
            return False
        data.pop(str(user_id))
        self.store.update({"global": data})
        return True

    def get(self, user_id: int) -> dict | None:
        return self._all().get(str(user_id))

    def items(self) -> list[tuple[int, dict]]:
        out = []
        for k, v in self._all().items():
            try:
                out.append((int(k), v))
            except (TypeError, ValueError):
                continue
        out.sort(key=lambda x: -x[1].get("ts", 0))
        return out
