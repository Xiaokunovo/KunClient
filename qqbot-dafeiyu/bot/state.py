"""群级状态：欢迎语、违禁词、警告计数、静音开关等，按群隔离并落盘。"""

from __future__ import annotations

import time
from pathlib import Path

from .storage import JsonStore

WARN_TTL = 7 * 24 * 3600  # 警告 7 天后自动作废


class GroupState:
    def __init__(self, path: Path):
        self.store = JsonStore(path, default={})

    # ---- 底层 -------------------------------------------------------
    def _get(self, group_id: int) -> dict:
        return self.store.get(str(group_id), {})

    def _patch(self, group_id: int, **kw) -> dict:
        data = self._get(group_id)
        data.update(kw)
        self.store.set(str(group_id), data)
        return data

    def get(self, group_id: int, key: str, default=None):
        return self._get(group_id).get(key, default)

    def set(self, group_id: int, key: str, value) -> None:
        self._patch(group_id, **{key: value})

    # ---- 欢迎 / 告别 ------------------------------------------------
    def welcome_on(self, group_id: int, default: bool = True) -> bool:
        return bool(self._get(group_id).get("welcome", default))

    def welcome_text(self, group_id: int, default: str) -> str:
        return str(self._get(group_id).get("welcome_text") or default)

    def farewell_on(self, group_id: int, default: bool = False) -> bool:
        return bool(self._get(group_id).get("farewell", default))

    def farewell_text(self, group_id: int, default: str) -> str:
        return str(self._get(group_id).get("farewell_text") or default)

    # ---- 违禁词 -----------------------------------------------------
    def words(self, group_id: int) -> list[str]:
        return list(self._get(group_id).get("words", []))

    def add_word(self, group_id: int, word: str) -> bool:
        words = self.words(group_id)
        if word in words:
            return False
        words.append(word)
        self._patch(group_id, words=words)
        return True

    def del_word(self, group_id: int, word: str) -> bool:
        words = self.words(group_id)
        if word not in words:
            return False
        words.remove(word)
        self._patch(group_id, words=words)
        return True

    # ---- 黑名单 -----------------------------------------------------
    def bans(self, group_id: int) -> dict[str, dict]:
        return dict(self._get(group_id).get("bans", {}))

    def add_ban(self, group_id: int, user_id: int, reason: str = "", by: int = 0) -> bool:
        bans = self.bans(group_id)
        if str(user_id) in bans:
            return False
        bans[str(user_id)] = {"reason": reason[:100], "by": by, "ts": int(time.time())}
        self._patch(group_id, bans=bans)
        return True

    def del_ban(self, group_id: int, user_id: int) -> bool:
        bans = self.bans(group_id)
        if str(user_id) not in bans:
            return False
        bans.pop(str(user_id))
        self._patch(group_id, bans=bans)
        return True

    def is_banned(self, group_id: int, user_id: int) -> dict | None:
        return self.bans(group_id).get(str(user_id))

    # ---- 关键词自动回复 ---------------------------------------------
    def keywords(self, group_id: int) -> dict[str, str]:
        return dict(self._get(group_id).get("keywords", {}))

    def add_keyword(self, group_id: int, word: str, reply: str) -> str:
        """返回状态：added / replaced / full"""
        kws = self.keywords(group_id)
        if word not in kws and len(kws) >= 50:
            return "full"
        status = "replaced" if word in kws else "added"
        kws[word] = reply
        self._patch(group_id, keywords=kws)
        return status

    def del_keyword(self, group_id: int, word: str) -> bool:
        kws = self.keywords(group_id)
        if word not in kws:
            return False
        kws.pop(word)
        self._patch(group_id, keywords=kws)
        return True

    def match_keyword(self, group_id: int, text: str) -> tuple[str, str] | None:
        """长词优先，避免「你好」抢走「你好呀」。"""
        kws = self.keywords(group_id)
        for word in sorted(kws, key=len, reverse=True):
            if word and word in text:
                return word, kws[word]
        return None

    # ---- 警告 -------------------------------------------------------
    def warn(self, group_id: int, user_id: int) -> int:
        warns = dict(self._get(group_id).get("warns", {}))
        now = time.time()
        item = warns.get(str(user_id)) or {"n": 0, "ts": now}
        if now - float(item.get("ts", 0)) > WARN_TTL:
            item = {"n": 0, "ts": now}
        item["n"] = int(item.get("n", 0)) + 1
        item["ts"] = now
        warns[str(user_id)] = item
        # 顺手清理过期项，别让 json 无限长大
        warns = {k: v for k, v in warns.items() if now - float(v.get("ts", 0)) <= WARN_TTL}
        self._patch(group_id, warns=warns)
        return item["n"]

    def warn_count(self, group_id: int, user_id: int) -> int:
        item = (self._get(group_id).get("warns") or {}).get(str(user_id)) or {}
        if time.time() - float(item.get("ts", 0)) > WARN_TTL:
            return 0
        return int(item.get("n", 0))

    def clear_warn(self, group_id: int, user_id: int) -> None:
        warns = dict(self._get(group_id).get("warns", {}))
        warns.pop(str(user_id), None)
        self._patch(group_id, warns=warns)

    # ---- 群统计 -----------------------------------------------------
    def bump_message(self, group_id: int, user_id: int) -> None:
        data = self._get(group_id)
        stats = data.setdefault("stats", {})
        stats["messages"] = int(stats.get("messages", 0)) + 1
        stats["since"] = stats.get("since") or int(time.time())
        users = stats.setdefault("users", {})
        users[str(user_id)] = int(users.get(str(user_id), 0)) + 1
        # 只留前 200 个活跃用户，避免文件膨胀
        if len(users) > 200:
            top = sorted(users.items(), key=lambda kv: kv[1], reverse=True)[:200]
            stats["users"] = dict(top)

        # 今日榜：跨天自动清零
        today = time.strftime("%Y-%m-%d")
        daily = stats.get("daily") or {}
        if daily.get("date") != today:
            daily = {"date": today, "users": {}}
        du = daily.setdefault("users", {})
        du[str(user_id)] = int(du.get(str(user_id), 0)) + 1
        stats["daily"] = daily

        self.store.set(str(group_id), data, save=False)

    def stats(self, group_id: int) -> dict:
        return self._get(group_id).get("stats", {})

    def top(self, group_id: int, scope: str = "total", limit: int = 10) -> list[tuple[int, int]]:
        """活跃榜。scope=total 总榜 / today 今日榜。"""
        stats = self.stats(group_id)
        if scope == "today":
            daily = stats.get("daily") or {}
            if daily.get("date") != time.strftime("%Y-%m-%d"):
                return []
            users = daily.get("users") or {}
        else:
            users = stats.get("users") or {}
        rows = []
        for k, v in users.items():
            try:
                rows.append((int(k), int(v)))
            except (TypeError, ValueError):
                continue
        rows.sort(key=lambda x: -x[1])
        return rows[:limit]

    # ---- 签到 / 积分 -------------------------------------------------
    BASE_POINTS = 10
    STREAK_BONUS = 2
    STREAK_BONUS_CAP = 20
    FIRST_BONUS = 20

    def checkin(self, group_id: int, user_id: int) -> dict:
        """签到。返回 {ok, gained, streak, points, already}。"""
        today = time.strftime("%Y-%m-%d")
        yesterday = time.strftime("%Y-%m-%d", time.localtime(time.time() - 86400))
        data = self._get(group_id)
        recs = dict(data.get("checkin", {}))
        rec = recs.get(str(user_id)) or {}

        if rec.get("date") == today:
            return {
                "ok": False,
                "already": True,
                "gained": 0,
                "streak": int(rec.get("streak", 0)),
                "points": int((data.get("points") or {}).get(str(user_id), 0)),
            }

        streak = int(rec.get("streak", 0)) + 1 if rec.get("date") == yesterday else 1
        gained = self.BASE_POINTS + min(self.STREAK_BONUS * (streak - 1), self.STREAK_BONUS_CAP)
        if streak == 1 and not rec:
            gained += self.FIRST_BONUS

        points = dict(data.get("points", {}))
        points[str(user_id)] = int(points.get(str(user_id), 0)) + gained
        recs[str(user_id)] = {"date": today, "streak": streak}
        data["checkin"] = recs
        data["points"] = points
        self.store.set(str(group_id), data)
        return {"ok": True, "already": False, "gained": gained, "streak": streak,
                "points": points[str(user_id)]}

    def points_of(self, group_id: int, user_id: int) -> int:
        return int((self._get(group_id).get("points") or {}).get(str(user_id), 0))

    def points_top(self, group_id: int, limit: int = 10) -> list[tuple[int, int]]:
        pts = self._get(group_id).get("points") or {}
        rows = []
        for k, v in pts.items():
            try:
                rows.append((int(k), int(v)))
            except (TypeError, ValueError):
                continue
        rows.sort(key=lambda x: -x[1])
        return rows[:limit]

    def streak_of(self, group_id: int, user_id: int) -> int:
        rec = (self._get(group_id).get("checkin") or {}).get(str(user_id)) or {}
        if rec.get("date") not in (
            time.strftime("%Y-%m-%d"),
            time.strftime("%Y-%m-%d", time.localtime(time.time() - 86400)),
        ):
            return 0
        return int(rec.get("streak", 0))

    def flush(self) -> None:
        self.store.flush()
