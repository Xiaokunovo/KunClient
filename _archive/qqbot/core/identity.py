"""身份层：用真实 QQ 号作为全站用户主键，平台 openid 只负责路由。

QQ 官方机器人开放平台**不会下发真实 QQ 号**——事件体与 OpenAPI 里只有
openid / member_openid / union_openid（官方「唯一身份机制」文档明确说明：
不同 bot 的 openid 不同，同一人在不同群的 member_openid 也不同）。
所以 QQ 号只能由本人自报登记一次，本模块把它变成可信且唯一的身份：

- ``qq_bind``：``QQ 号 -> 该号在各群/私聊的 openid 列表``。
  一人一号、锁定不可改绑：既杜绝冒领他人 QQ 号，也让「全局拉黑」无法被
  「换个号重新绑定」绕过。
- ``uid``：全站用户主键。已绑定 = QQ 号；未绑定 = ``openid:`` + openid 占位，
  对方日后绑定QQ 时把占位键下的历史数据自动认领到 QQ 号。
- ``global_blacklist``：全局黑名单，按 **QQ 号** 判定，跨群跨私聊一律拦截。

⚠️ 平台给同一用户在**不同群**的是不同 openid，所以映射必须是「号 -> 多个
openid」的多对一结构。旧版本拿单个 openid 当主键，导致同一个人换个群就
「变成另一个人」（积分、收藏、账号绑定都不跟人走），v2 一并修正。

v3（权限体系重做）新增：
- ``identity_ban``：**openid 级全局拦截兜底**。对方没绑定 QQ 号时按号封不了，
  先把他的 openid 加进来（至少所有已知身份场景全部拦截）；一旦他绑定 QQ
  或被超管登记（link_openid），自动升级为按 QQ 号的全局封禁并清除 openid 记录。
- ``roles``：显式角色表（v2 的 admins / super_admins 两个旧键迁到这里，见
  Permissions），授权不再依赖平台身份自动提权。
"""
import logging
import re
import time

log = logging.getLogger("qq.identity")

SCHEMA_VERSION = 3
OPENID_PREFIX = "openid:"
QQ_MIN_LEN = 5
QQ_MAX_LEN = 11

OPENID_RE = re.compile(r"[A-Za-z0-9_\-]{16,}")


def is_qq(text: str) -> bool:
    """是否是合法 QQ 号（5-11 位纯数字）。"""
    t = str(text or "").strip()
    return t.isdigit() and QQ_MIN_LEN <= len(t) <= QQ_MAX_LEN


def is_openid(text: str) -> bool:
    """是否是平台 openid（长度 ≥16 的字母数字串）。"""
    return bool(OPENID_RE.fullmatch(str(text or "").strip()))


def now() -> str:
    return time.strftime("%Y-%m-%d %H:%M")


class Identity:
    """QQ 号 ↔ openid 映射、用户主键解析、全局黑名单判定。"""

    def __init__(self, storage):
        self.storage = storage
        self._oid2qq: dict[str, str] = {}      # openid -> QQ 号
        self._qq2oids: dict[str, list[str]] = {}  # QQ 号 -> openid 列表
        self._blocked_oids: set[str] = set()   # 被全局拉黑命中的 openid（QQ 号封禁 ∪ openid 封禁）
        self.rebuild()

    # ---------- 索引 ----------

    def rebuild(self):
        """重建全部内存索引（绑定数据被改动或热重载后调用）。

        ⚠️ 前提是 self.storage 是**同一个实例** —— 索引是从 storage.data 现算的，
        换 Storage 对象必须换 Identity 对象，否则索引和数据会错位。
        """
        binds = self.storage.data.setdefault("qq_bind", {})
        clean: dict[str, dict] = {}
        self._oid2qq, self._qq2oids = {}, {}
        for key, rec in list(binds.items()):
            if not isinstance(rec, dict):
                continue
            key = str(key)
            qq = str(rec.get("qq") or (key if is_qq(key) else "")).strip()
            if not is_qq(qq):
                log.warning("丢弃非法 QQ 号绑定记录: %r", key)
                continue
            # v1 里同一个 QQ 号可能按多个 openid 分散登记，这里合并成一个号一条
            oids: list[str] = list((clean.get(qq) or {}).get("openids") or [])
            if not is_qq(key) and key not in oids:
                oids.append(key)  # v1：以 openid 为键的旧结构
            for oid in rec.get("openids") or []:
                oid = str(oid).strip()
                if oid and oid not in oids:
                    oids.append(oid)
            prev = clean.get(qq)
            if prev:  # 同号多条：保留最早那条的登记时间
                rec["at"] = prev.get("at") or rec.get("at")
            rec["qq"], rec["openids"] = qq, oids
            clean[qq] = rec
            self._qq2oids[qq] = list(oids)
            for oid in oids:
                self._oid2qq[oid] = qq
        self.storage.data["qq_bind"] = clean
        self._rebuild_blocked()

    def _rebuild_blocked(self):
        gb = self.storage.data.get("global_blacklist") or {}
        ib = self.storage.data.get("identity_ban") or {}
        self._blocked_oids = set()
        for qq in gb:
            self._blocked_oids.update(self._qq2oids.get(str(qq), ()))
        # openid 级兜底拦截（未绑定 QQ 号的全局封禁）
        for oid in ib:
            oid = str(oid).strip()
            if oid:
                self._blocked_oids.add(oid)
        log.info("全局黑名单 %d 个 QQ 号 + %d 个 openid，已覆盖已知身份 %d 个",
                 len(gb), len(ib), len(self._blocked_oids))

    # ---------- 主键解析 ----------

    def qq_of(self, openid: str) -> str | None:
        """openid -> 已登记的 QQ 号；未登记返回 None。"""
        return self._oid2qq.get(str(openid or ""))

    def openids_of(self, qq: str) -> list[str]:
        """QQ 号 -> 已关联的 openid 列表（各群/私聊各一个）。"""
        return list(self._qq2oids.get(str(qq).strip(), ()))

    def is_openid_uid(self, uid) -> bool:
        return isinstance(uid, str) and uid.startswith(OPENID_PREFIX)

    def uid_of_openid(self, openid: str) -> str:
        """平台 openid -> 用户主键：已绑定返回 QQ 号，未绑定返回占位键。"""
        openid = str(openid or "")
        return self._oid2qq.get(openid) or (OPENID_PREFIX + openid)

    def uid_of_qq(self, qq: str) -> str:
        qq = str(qq or "").strip()
        return qq if is_qq(qq) else OPENID_PREFIX + qq

    def oids_of_uid(self, uid: str) -> list[str]:
        """用户主键 -> 已知 openid 列表。"""
        uid = str(uid or "")
        if self.is_openid_uid(uid):
            return [uid[len(OPENID_PREFIX):]]
        return self.openids_of(uid)

    # ---------- 显示 ----------

    def display(self, uid: str, group_id: str = "") -> str:
        """人类可读标识：本群昵称 → 任意群昵称 → QQ:号 → 截断 openid。"""
        uid = str(uid or "")
        if not uid:
            return "未知用户"
        oids = self.oids_of_uid(uid)
        # config.json 里可以直接存**裸 openid**（没有 "openid:" 前缀，如 admins 的旧写法）——
        # 它既不是 QQ 号、也不在 _qq2oids 里，oids_of_uid 会返回空，于是名册里明明有这个人
        # 却被显示成 "QQ:F74BE2BA…"。这里把原串当 openid 再试一次。
        if not oids and is_openid(uid):
            oids = [uid]
        roster = self.storage.data.get("roster") or {}
        if group_id and oids:
            members = roster.get(group_id) or {}
            for oid in oids:
                name = (members.get(oid) or {}).get("username")
                if name:
                    return name
        for members in roster.values():
            for oid in oids:
                name = (members.get(oid) or {}).get("username")
                if name:
                    return name
        if self.is_openid_uid(uid):
            oid = uid[len(OPENID_PREFIX):]
            return oid[:12] + "…" if len(oid) > 12 else oid
        return f"QQ:{uid}"

    # ---------- 绑定（一号一绑，锁定不可改绑） ----------

    def bind(self, openid: str, qq: str) -> tuple[str, str]:
        """把当前 openid 绑定到 QQ 号，返回 (状态码, 回复文案)。

        状态码：ok / already / format / rebind_conflict / qq_taken
        """
        openid, qq = str(openid or "").strip(), str(qq or "").strip()
        if not openid or not is_qq(qq):
            return "format", f"用法：绑定QQ <你的QQ号>（{QQ_MIN_LEN}-{QQ_MAX_LEN} 位数字）"
        binds = self.storage.data.setdefault("qq_bind", {})
        mine = self._oid2qq.get(openid)
        if mine == qq:
            is_new_alias = openid not in (self._qq2oids.get(qq) or ())
            if is_new_alias:
                rec = binds.setdefault(qq, {"qq": qq, "at": now(), "openids": []})
                rec.setdefault("openids", []).append(openid)
                self._qq2oids.setdefault(qq, []).append(openid)
                if qq in (self.storage.data.get("global_blacklist") or {}):
                    self._blocked_oids.add(openid)
            tail = ("你在这个群的身份已并入该 QQ 号，资料/积分/账号跨群通用。"
                    if is_new_alias else "你的资料/积分/账号已经跨群通用。")
            return "already", f"✅ 你已绑定 QQ {qq}。{tail}"
        if mine:
            return "rebind_conflict", (
                f"❌ 你当前身份已绑定 QQ {mine}，不允许改绑到 {qq}。\n"
                f"（一个身份只能对应一个 QQ 号。如需更换，请让超级管理员发送「解绑QQ {mine}」）")
        if self._qq2oids.get(qq):
            return "qq_taken", (
                f"❌ QQ {qq} 已被其他身份登记，不能重复绑定。\n"
                f"（一号一绑，防冒领。如果你确认这是你的号，"
                f"请让超级管理员发送「解绑QQ {qq}」后再试）")
        rec = binds.setdefault(qq, {"qq": qq, "at": now(), "openids": []})
        rec.setdefault("openids", []).append(openid)
        self._oid2qq[openid] = qq
        self._qq2oids.setdefault(qq, []).append(openid)
        if qq in (self.storage.data.get("global_blacklist") or {}):
            self._blocked_oids.add(openid)
        # 该 openid 若在兜底拦截里，拿到 QQ 号立刻升级为按号封禁
        self._upgrade_identity_ban(openid)
        return "ok", (f"✅ 已绑定 QQ {qq}\n"
                      "以后你的积分、收藏、KunClient 账号在各个群都通用。"
                      "（不绑定也能正常用机器人；绑定后不可自行改绑）")

    def link_openid(self, openid: str, qq: str, *, create: bool = False) -> tuple[bool, str]:
        """把一个 openid 并入 QQ 号。create=True 时允许超管直接新建该 QQ 号的绑定。

        为什么需要 create：机器人自己永远拿不到 QQ 号，而「让对方先发绑定QQ」
        意味着管理员要等别人配合。超管本人知道对方的 QQ 号（头像/名片/私聊），
        可以直接登记，不必求人 —— 这是让「封号 @某人」立刻能全局生效的关键。
        仅超管可调用，普通用户走 bind() 仍受一号一绑保护。
        """
        openid, qq = str(openid or "").strip(), str(qq or "").strip()
        if not openid:
            return False, "缺少目标身份。"
        if not is_qq(qq):
            return False, f"QQ 号格式不对：{qq}"
        binds = self.storage.data.setdefault("qq_bind", {})
        mine = self._oid2qq.get(openid)
        if mine == qq:
            return False, f"该身份已经在 QQ {qq} 名下了。"
        if mine:
            return False, f"该身份已绑定 QQ {mine}，请先「解绑QQ {mine}」。"
        rec = binds.get(qq)
        if rec is None:
            if not create:
                return False, f"QQ {qq} 还没有绑定记录。\n让本人发一次「绑定QQ {qq}」，或由超管发「设QQ号 {qq} @某人」直接登记。"
            rec = {"qq": qq, "at": now(), "openids": [], "by_admin": True}
            binds[qq] = rec
        oids = rec.setdefault("openids", [])
        if openid not in oids:
            oids.append(openid)
        self._qq2oids[qq] = list(oids)
        self._oid2qq[openid] = qq
        if qq in (self.storage.data.get("global_blacklist") or {}):
            self._blocked_oids.add(openid)
        # 超管登记 QQ 号 = 拿到可信身份：兜底拦截同步升级为按号封禁
        self._upgrade_identity_ban(openid)
        return True, f"已把该身份并入 QQ {qq}（现共 {len(oids)} 个身份）。"

    def unbind_qq(self, qq: str, *, force: bool = False) -> tuple[bool, str]:
        """解除某个 QQ 号的绑定（仅超管）。数据保留在原 QQ 号键下，重新绑定即恢复。"""
        qq = str(qq or "").strip()
        gb = self.storage.data.get("global_blacklist") or {}
        if qq in gb and not force:
            return False, f"QQ {qq} 正在全局黑名单中，请先「全局解黑 {qq}」再解绑。"
        rec = (self.storage.data.get("qq_bind") or {}).pop(qq, None)
        if not rec:
            return False, f"QQ {qq} 没有绑定记录。"
        n = len(rec.get("openids") or [])
        for oid in rec.get("openids") or []:
            self._oid2qq.pop(str(oid), None)
        self._qq2oids.pop(qq, None)
        return True, (f"已解绑 QQ {qq}（解除 {n} 个身份）。\n"
                      "该号可重新绑定；其名下的积分/收藏/账号数据会保留并在重新绑定后恢复。")

    # ---------- 全局黑名单（按 QQ 号） ----------

    def global_blacklist(self) -> dict:
        return self.storage.data.get("global_blacklist") or {}

    def is_globally_blocked(self, openid: str) -> bool:
        """该身份是否被全局拦截：QQ 号封禁 或 openid 兜底封禁。"""
        openid = str(openid or "")
        if not openid:
            return False
        if openid in self._blocked_oids:
            return True
        qq = self._oid2qq.get(openid)
        return bool(qq) and qq in self.global_blacklist()

    # ---------- openid 级全局拦截兜底（未绑定 QQ 号时） ----------

    def identity_ban_list(self) -> dict:
        return self.storage.data.get("identity_ban") or {}

    def identity_block(self, openid: str, by_uid: str, reason: str = "") -> tuple[bool, str]:
        """把单个 openid 加入全局拦截（对方没绑 QQ 号时的跨群封禁兜底）。

        一旦该 openid 日后绑定 QQ 号（bind）或被超管登记（link_openid），
        会自动升级为按 QQ 号的全局封禁并清除本记录。
        """
        openid = str(openid or "").strip()
        if not openid:
            return False, "缺少目标身份。"
        ib = self.storage.data.setdefault("identity_ban", {})
        if openid in ib:
            at = (ib[openid] or {}).get("at", "?")
            return False, f"该身份早已在全局拦截中（{at} 加入）。"
        ib[openid] = {"openid": openid, "at": now(), "by": str(by_uid or ""),
                      "reason": str(reason or "")}
        self._rebuild_blocked()
        return True, ("该身份已加入全局拦截（所有群与私聊生效）。\n"
                      "注意：平台只给机器人每群一个加密 ID，对方若从未绑定 QQ 号，"
                      "他在其他群的另一个加密 ID 暂不受影响——让他发一次「绑定QQ」"
                      "或被「设QQ号」登记后，拦截会自动升级为按 QQ 号永久生效。")

    def identity_unblock(self, openid: str) -> tuple[bool, str]:
        ib = self.storage.data.setdefault("identity_ban", {})
        if openid not in ib:
            return False, "该身份不在全局拦截中。"
        ib.pop(openid)
        self._rebuild_blocked()
        return True, "已把该身份移出全局拦截。"

    def _upgrade_identity_ban(self, openid: str):
        """openid 兜底拦截 -> QQ 号全局封禁（拿到 QQ 号的那一刻升级）。"""
        ib = self.storage.data.get("identity_ban") or {}
        rec = ib.pop(openid, None)
        if not rec:
            return None
        qq = self._oid2qq.get(openid)
        if not qq:
            return None
        gb = self.storage.data.setdefault("global_blacklist", {})
        if qq not in gb:
            gb[qq] = {"qq": qq, "at": now(),
                      "by": str((rec or {}).get("by") or ""),
                      "reason": str((rec or {}).get("reason") or "") + "（openid 拦截升级）"}
        self._rebuild_blocked()
        log.info("openid 拦截已升级为 QQ 号全局封禁：%s… -> QQ %s", openid[:8], qq)
        return qq

    def global_block(self, qq: str, by_uid: str, reason: str = "") -> tuple[bool, str]:
        """按 QQ 号全局拉黑。返回 (是否新增, 回复文案)。"""
        qq = str(qq or "").strip()
        gb = self.storage.data.setdefault("global_blacklist", {})
        if qq in gb:
            at = (gb[qq] or {}).get("at", "?")
            return False, f"QQ {qq} 早已在全局黑名单中（{at} 加入）。"
        gb[qq] = {"qq": qq, "at": now(), "by": str(by_uid or ""), "reason": str(reason or "")}
        self._rebuild_blocked()
        return True, f"QQ {qq} 已加入全局黑名单，机器人在所有群与私聊中都会忽略该号的消息。"

    def global_unblock(self, qq: str) -> tuple[bool, str]:
        qq = str(qq or "").strip()
        gb = self.storage.data.setdefault("global_blacklist", {})
        if qq not in gb:
            return False, f"QQ {qq} 不在全局黑名单中。"
        gb.pop(qq)
        self._rebuild_blocked()
        return True, f"已把 QQ {qq} 移出全局黑名单。"

    # ---------- v1 -> v2 -> v3 迁移 ----------

    def migrate_legacy(self) -> dict:
        """旧数据升级。幂等，已是 v3 时直接返回。

        v1 -> v2：openid 主键换算成 QQ 号主键（同一人多个 openid 合并）。
        v2 -> v3：admins / super_admins 两个旧授权键迁进 roles 显式角色表，
                  新增 identity_ban（openid 兜底拦截）与 audit（审计）空表。
        ⚠️ v1 的键换算**只能对 v1 数据做**：v2 数据的键已是 QQ 号，
        再过一遍 uid_of_openid 会被包成 "openid:QQ号" 占位键，必须分流。
        """
        data = self.storage.data
        cur = data.get("schema")
        try:
            cur = int(cur)
        except (TypeError, ValueError):
            cur = 1
        if cur >= SCHEMA_VERSION:
            return {}
        stats: dict = {}

        if cur < 2:
            self._migrate_v1_to_v2(stats)
            data["schema"] = 2
        self._migrate_v2_to_v3(stats)
        data["schema"] = SCHEMA_VERSION
        # 只要 schema 版本变了就必须落盘 —— 即使没有任何可迁条目（stats 为空）也要表达出来。
        # 否则 main.py 的 `bool(self.identity.migrate_legacy())` 判据为假 → 不 save，
        # 升级结果只活在内存里：磁盘 schema 停在旧版、每次重启重跑迁移。
        # 2026-10-12 线上实测踩到：线上 admins 的群列表为空 + super_admins 为空
        # → moved=0 → 返回 {} → schema 卡在 v2，要靠之后某次偶然的 save() 才写盘。
        stats["_schema"] = "v%s -> v%s" % (cur, SCHEMA_VERSION)
        log.info("数据已升级到 v%s：%s", SCHEMA_VERSION, stats)
        return stats

    def _migrate_v1_to_v2(self, stats: dict):
        """v1：所有 openid 键换算成 QQ 号键。"""
        data = self.storage.data

        def _map(oid: str) -> str:
            return self.uid_of_openid(oid)

        def _rekey_dict(section: str):
            """{openid: 值} -> {uid: 值}，同一人多份记录保留先出现的那份。"""
            src = data.get(section) or {}
            if not src:
                return
            new: dict = {}
            moved = 0
            for oid, val in src.items():
                uid = _map(oid)
                if uid not in new:
                    new[uid] = val
                    moved += 1
            data[section] = new
            if moved:
                stats[section] = moved

        def _rekey_lists(section: str):
            """{群: [openid]} -> {群: [uid]}，组内去重。"""
            src = data.get(section) or {}
            if not src:
                return
            moved = 0
            for gid, lst in src.items():
                out: list = []
                for oid in lst or []:
                    uid = _map(oid)
                    if uid not in out:
                        out.append(uid)
                        moved += 1
                src[gid] = out
            if moved:
                stats[section] = moved

        _rekey_dict("kun_users")
        supers: list = []
        for oid in data.get("super_admins") or []:
            uid = _map(oid)
            if uid not in supers:
                supers.append(uid)
                stats["super_admins"] = stats.get("super_admins", 0) + 1
        data["super_admins"] = supers
        _rekey_lists("admins")
        _rekey_lists("blacklist")
        # 签到：{chat_key: {openid: rec}}，同一人同群保留积分更高的一条
        sign = data.get("sign") or {}
        moved_sign = 0
        for chat_key, table in sign.items():
            out: dict = {}
            for oid, rec in (table or {}).items():
                uid = _map(oid)
                cur = out.get(uid)
                if cur is None or (rec or {}).get("total", 0) > (cur or {}).get("total", 0):
                    out[uid] = rec
                moved_sign += 1
            sign[chat_key] = out
        if moved_sign:
            stats["sign"] = moved_sign
        # roster 保持 openid 为键（平台原始观测），补写 qq 便于显示
        for members in (data.get("roster") or {}).values():
            for oid, info in (members or {}).items():
                if isinstance(info, dict):
                    qq = self._oid2qq.get(oid)
                    if qq:
                        info["qq"] = qq
        data.setdefault("global_blacklist", {})
        log.info("主键 openid -> QQ 号（v1->v2）：%s", stats)

    def _migrate_v2_to_v3(self, stats: dict):
        """v2 -> v3：授权体系换成 roles 显式角色表，补 identity_ban / audit。"""
        data = self.storage.data
        roles = data.setdefault("roles", {})
        moved = 0
        for gid, lst in (data.get("admins") or {}).items():
            if not isinstance(lst, list):
                continue
            bucket = roles.setdefault(str(gid), {})
            for uid in lst:
                uid = str(uid or "")
                if uid and bucket.get(uid) != "super":
                    bucket[uid] = "group_admin"
                    moved += 1
        if moved:
            stats["roles.group"] = moved
        glob = roles.setdefault("*", {})
        supers_moved = 0
        for uid in data.get("super_admins") or []:
            uid = str(uid or "")
            if uid and glob.get(uid) != "group_admin":
                glob[uid] = "super"
                supers_moved += 1
        if supers_moved:
            stats["roles.super"] = supers_moved
        # 旧键清空（内容已进 roles；Storage.OBSOLETE_KEYS 不再需要它们复活）
        data["admins"] = {}
        data["super_admins"] = []
        data.setdefault("identity_ban", {})
        data.setdefault("audit", [])