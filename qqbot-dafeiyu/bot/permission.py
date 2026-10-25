"""权限判定。

等级模型（数值越大权限越高）：
  100 主人 superuser —— 配置里写死的 QQ 号，通吃
   60 机器人管理 admin —— 配置里写死的协助管理者，跨群生效
   50 群主 owner      —— 由 OneBot 事件的 sender.role 给出
   40 群管理 admin    —— 同上
   10 普通成员 member

注意：群管命令除了「操作者够权限」，还要求「机器人自己在群里是管理员」，
这两件事是分开的 —— 操作者是群主但机器人被撤了管理，照样执行不了。
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass

from .event import Event

log = logging.getLogger("perm")

LEVELS = {
    "member": 10,
    "group.admin": 40,
    "group.owner": 50,
    "admin": 60,
    "super": 100,
}

ROLE_LEVEL = {"member": 10, "admin": 40, "owner": 50}


@dataclass
class Actor:
    user_id: int
    level: int
    source: str  # super / admin / group.owner / group.admin / member

    @property
    def is_super(self) -> bool:
        return self.level >= LEVELS["super"]

    @property
    def is_group_admin(self) -> bool:
        return self.level >= LEVELS["group.admin"]


class Permissions:
    def __init__(self, cfg):
        self.cfg = cfg
        self._bot_role_cache: dict[int, tuple[str, float]] = {}
        self._role_ttl = 120  # 机器人自身群身份缓存 2 分钟

    # ---- 配置热更新 -------------------------------------------------
    def reload(self) -> None:
        self._bot_role_cache.clear()

    @property
    def superusers(self) -> set[int]:
        return {int(x) for x in (self.cfg.get("permission.superusers") or []) if str(x).isdigit()}

    @property
    def admins(self) -> set[int]:
        return {int(x) for x in (self.cfg.get("permission.admins") or []) if str(x).isdigit()}

    # ---- 操作者身份 -------------------------------------------------
    def resolve(self, ev: Event) -> Actor:
        uid = ev.user_id
        if uid in self.superusers:
            return Actor(uid, LEVELS["super"], "super")
        if uid in self.admins:
            return Actor(uid, LEVELS["admin"], "admin")
        if ev.is_group and self.cfg.get("permission.allow_group_admin", True):
            role = ev.role
            if role == "owner":
                return Actor(uid, LEVELS["group.owner"], "group.owner")
            if role == "admin":
                return Actor(uid, LEVELS["group.admin"], "group.admin")
        return Actor(uid, LEVELS["member"], "member")

    def require(self, actor: Actor, need: str) -> bool:
        return actor.level >= LEVELS.get(need, 100)

    def threshold(self, need: str) -> int:
        """取某档权限的等级值（外部要拿它做比较时用，别自己去摸 LEVELS 常量）。"""
        return LEVELS.get(need, 100)

    # ---- 机器人自身群身份 -------------------------------------------
    async def bot_role(self, api, group_id: int) -> str:
        """返回机器人在该群的 role：owner / admin / member。查不到就当 member。"""
        now = time.time()
        cached = self._bot_role_cache.get(group_id)
        if cached and now - cached[1] < self._role_ttl:
            return cached[0]
        role = "member"
        try:
            info = await api.get_group_member_info(group_id, api.self_id)
            role = str(info.get("role") or "member")
        except Exception as exc:  # noqa: BLE001
            log.debug("查机器人群身份失败 group=%s：%s", group_id, exc)
        self._bot_role_cache[group_id] = (role, now)
        return role

    async def can_manage(self, api, group_id: int) -> tuple[bool, str]:
        role = await self.bot_role(api, group_id)
        if role in ("admin", "owner"):
            return True, role
        return False, "我在这个群不是管理员，先给我设个管理再使唤我（咕噜）"

    def invalidate_bot_role(self, group_id: int) -> None:
        self._bot_role_cache.pop(group_id, None)
