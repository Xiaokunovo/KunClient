"""简单 JSON 持久化：群管理员、关键词回复、黑名单、欢迎语。

⚠️ schema v2 起，所有**用户维度**的数据（管理员、黑名单、KunClient 账号、
签到）都以**真实 QQ 号**为主键 uid，不再用平台 openid。
未绑定QQ 的用户临时用 "openid:" + openid 占位，绑定后由
Storage.claim_user() 把数据认领到 QQ 号上。openid 只保留在
qq_bind / roster 这两张「平台观测表」里。

⚠️ schema v3（权限体系重做）起：
- ``roles`` 是唯一授权表：``{"*": {uid: "super"}, "群openid": {uid: "group_admin"|"member"}}``。
  旧键 admins / super_admins 已迁空，仅保留字段兼容读取；
- ``identity_ban``：openid 级全局拦截兜底（未绑定 QQ 号时的跨群封禁）；
- ``audit``：授权/封禁/解绑等关键操作的环形审计缓冲。

data.json 结构：
{
  "schema":   3,
  "roles":    { "*": {"QQ号": "super"}, "群openid": {"QQ号": "group_admin"} },
  "blacklist":{ "群openid": ["QQ号或占位uid", ...] },
  "global_blacklist": { "QQ号": {"at": "...", "by": "...", "reason": "..."} },
  "identity_ban": { "openid": {"at": "...", "by": "...", "reason": "..."} },
  "audit":    [ {"ts": "...", "op": "...", "by": "...", "target": "...", ...} ],
  "keywords": { "群openid": [ {"t": "触发词", "r": "回复内容"}, ... ] },
  "welcome":  { "群openid": {"on": true, "text": "..."} },
  "qq_bind":  { "QQ号": {"qq": "QQ号", "at": "...", "openids": ["openid", ...]} },
  "roster":   { "群openid": { "openid": {"username": "...", "role": "...", "qq": "..."} } }
}
"""
import asyncio
import json
import logging
import os
import time

from .identity import SCHEMA_VERSION

log = logging.getLogger("qq.storage")

# data.json 的完整默认结构（schema v3：用户维度一律以 QQ 号为主键，
# 授权一律走 roles 显式角色表；平台群主身份默认不自动提权，
# 见 config.json 的 permissions.trust_platform_roles）
DEFAULT_DATA: dict = {
    "roles": {}, "keywords": {}, "blacklist": {}, "global_blacklist": {},
    "identity_ban": {}, "audit": [], "welcome": {},
    "admins": {}, "super_admins": [],   # v3 起已迁空，仅保留字段兼容
    "features": {}, "notice": {}, "kun_users": {},
    "sign": {}, "roster": {}, "qq_bind": {},
    "point_shop": {},
    # 客户端发版自动播报状态（见 core/release_watch.py）：上次播报版本 / 开关 / 目标群
    "release": {},
}


def fresh_data() -> dict:
    """一份全新的空数据（内层是新容器，可直接安全改写）。"""
    return {k: (list(v) if isinstance(v, list) else dict(v) if isinstance(v, dict) else v)
            for k, v in DEFAULT_DATA.items()}


# 已废弃、从 data.json 里清掉的键。
# 为什么需要显式清理：_load() 是 self.data.update(loaded)，只删 DEFAULT_DATA 里的键
# **不会**动到已有文件里已存在的键 —— 死字段会一直躺在磁盘上（实测线上
# super_bind_used 就是这样留下来的）。这里在加载时剔除，下次 save 就写不回去了。
OBSOLETE_KEYS = ("super_bind_used", "rom_fav")


class Storage:
    def __init__(self, path: str):
        self.path = path
        self._lock = asyncio.Lock()
        self.data: dict = fresh_data()
        self.dropped_keys: list = []   # 本次加载清掉的废弃键，非空则值得落盘一次
        self._load()

    def _load(self):
        # 全新库（无文件）直接视为 v2；已有文件以其自身声明的 schema 为准，
        # 缺 schema 字段的旧文件按 v1 处理，交给 Identity.migrate_legacy 换算主键。
        if not os.path.exists(self.path):
            self.data["schema"] = SCHEMA_VERSION
            return
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                self.data.update(loaded)
                self.data["schema"] = loaded.get("schema", 1)
                if self.data["schema"] != SCHEMA_VERSION:
                    log.info("data.json 为 v%s 结构，用户主键将由 openid 换算为 QQ 号", self.data["schema"])
                self.dropped_keys = [k for k in OBSOLETE_KEYS if self.data.pop(k, None) is not None]
                if self.dropped_keys:
                    log.info("已从 data.json 清除废弃字段：%s", ", ".join(self.dropped_keys))
        except Exception as e:
            log.warning("读取 %s 失败（将使用空数据）: %s", self.path, e)
            self.data = fresh_data()
            self.data["schema"] = SCHEMA_VERSION

    @property
    def stem(self) -> str:
        """数据文件主干名，如 data.json -> data（用于按日备份命名隔离不同库）。"""
        return os.path.splitext(os.path.basename(self.path))[0] or "data"

    async def save(self):
        async with self._lock:
            # 保留上一份好数据作为备份，防止写入中断损坏
            if os.path.exists(self.path):
                try:
                    import shutil
                    shutil.copyfile(self.path, self.path + ".bak")
                except Exception:
                    pass
                # 每天首写时留一份按日备份，保留最近 7 天
                # （按文件名主干隔离，devtest 等临时库不会污染正式 backups/）
                try:
                    bdir = os.path.join(os.path.dirname(self.path) or ".", "backups")
                    os.makedirs(bdir, exist_ok=True)
                    prefix = f"{self.stem}-"
                    bpath = os.path.join(bdir, f"{prefix}{time.strftime('%Y%m%d')}.json")
                    if not os.path.exists(bpath):
                        import shutil
                        shutil.copyfile(self.path, bpath)
                        backups = sorted(f for f in os.listdir(bdir) if f.startswith(prefix))
                        for old in backups[:-7]:
                            os.remove(os.path.join(bdir, old))
                except Exception:
                    pass
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, ensure_ascii=False, indent=2)
            os.replace(tmp, self.path)

    async def reload(self, path: str | None = None):
        """就地把磁盘数据重读进**当前对象**（配置热重载用），返回是否有内容变化。

        ⚠️ 绝对不要用 `bot.storage = Storage(...)` 来做重载，那样会踩两个数据丢失的坑：
          1. _lock 是**实例字段**（storage.py:60）。新对象拿的是一把全新的锁，与旧对象
             毫无互斥关系；旧对象上还在飞的 save()（router.py:178 的 create_task）会在
             重载之后才拿到锁，用「reload 之前的内存快照」把磁盘覆盖回去 ——
             QQ 绑定、积分、名册、kun_users 里的账号 token 被静默回滚。
          2. 未落盘的内存改动会被直接丢掉，且没有备份。
        复用同一实例 + 同一把锁，上面两条都不成立：在途 save() 要么先跑完（正常落盘），
        要么排在我们之后（那时 self.data 已经是新数据了）。
        调用方需自己保证 data.json 没有悬空未落盘的改动 —— Bot.reload() 会先 save()。
        """
        async with self._lock:
            before = json.dumps(self.data, ensure_ascii=False, sort_keys=True)
            if path is not None:
                self.path = path
            self.data = fresh_data()
            self.dropped_keys = []
            self._load()
            changed = json.dumps(self.data, ensure_ascii=False, sort_keys=True) != before
            return changed

    def group_section(self, section: str, group_id: str):
        """取某个群的配置列表/字典（引用语义，改完记得 save）。"""
        return self.data.setdefault(section, {}).setdefault(group_id, [] if section != "welcome" else {})

    # ---------- 主键迁移 ----------

    def claim_user(self, old_uid: str, new_uid: str):
        """把 old_uid 名下的用户数据并入 new_uid（绑定QQ 时认领历史数据）。

        场景：用户先用占位键 "openid:xxx" 签到攒了积分，
        后来绑定 QQ 号后，历史数据要跟着走到 QQ 号上。
        """
        if not old_uid or not new_uid or old_uid == new_uid:
            return
        data = self.data

        rec = (data.get("kun_users") or {}).pop(old_uid, None)
        if rec is not None:
            data.setdefault("kun_users", {}).setdefault(new_uid, rec)

        for section in ("admins", "blacklist"):
            for members in (data.get(section) or {}).values():
                if old_uid in members:
                    members.remove(old_uid)
                    if new_uid not in members:
                        members.append(new_uid)

        # roles 授权表：键跟着人走（群管理员/超级管理员授权不因换身份失效）
        for members in (data.get("roles") or {}).values():
            if old_uid in members:
                role_val = members.pop(old_uid)
                if new_uid not in members:
                    members[new_uid] = role_val

        supers = data.setdefault("super_admins", [])
        if old_uid in supers:
            supers.remove(old_uid)
            if new_uid not in supers:
                supers.append(new_uid)

        for table in (data.get("sign") or {}).values():
            rec = (table or {}).pop(old_uid, None)
            if rec is not None:
                table.setdefault(new_uid, rec)
