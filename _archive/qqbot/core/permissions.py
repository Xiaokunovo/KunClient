"""统一权限体系（RBAC）：角色分层 + 权限节点 + 目标保护 + 审计日志。

为什么重做（旧设计的实际问题）：
1. 只有「管理员 / 超级管理员」两档，QQ 群主/群管理员被平台**无条件自动提权**
   为机器人管理员——关不掉、逐群控制不了，私聊里又自动失效，行为不一致；
2. 权限判定散落三处：router 的 admin=True 判一遍，命令内部再 is_super 复查，
   「积分商店」又在命令里查 is_admin——新命令漏查一个就是越权；
3. 权限倒挂：群管理员能把超级管理员/主人拉进本群黑名单，被反杀；
4. 全局封号依赖对方自报 QQ 号，未绑定时只能封当前群一个 openid，换群即失效；
5. 授权/封禁/解绑不留任何持久记录，出问题无法追溯。

新模型：
- 四个角色，数值越高权限越大：owner(100) > super(80) > group_admin(40) > user(0)
- 每条命令声明一个权限节点 perm，由 Router **统一判定**，命令内部不再自查权限；
- 授权全部显式、持久化在 data.json 的 roles 表；平台群主身份默认不再自动提权
  （config.permissions.trust_platform_roles=true 可开启，且可被逐群降级覆盖）；
- 目标保护：不能对权限 >= 自己的成员执行管理操作，主人永不被管理操作命中；
- 审计：授权/封禁/解绑等关键操作全部落 data.json audit 环形缓冲，可回看。

data.json roles 结构：
    roles = {
      "*":        { "QQ号": "super" },                 # 全局角色
      "群openid":  { "QQ号": "group_admin" | "member" } # 群角色（member=显式降级）
    }
"""
import logging
import time

from core.identity import OPENID_PREFIX, is_qq

log = logging.getLogger("qq.perm")

# ---------- 角色 ----------

ROLE_OWNER = ("owner", 100)        # 主人：config owner_qq，只能改配置，永不被管理操作命中
                                    # （支持多主人：owner_qq 可写字符串或 QQ 号数组）
ROLE_SUPER = ("super", 80)         # 超级管理员：config admins + roles["*"]
ROLE_GROUP = ("group_admin", 40)   # 群管理员：roles[群] 显式授予（可选信任平台群主身份）
ROLE_USER = ("user", 0)            # 普通用户

ROLE_LABEL = {100: "主人", 80: "超级管理员", 40: "群管理员", 0: "普通用户"}

# ---------- 权限节点 -> 所需角色等级 / 中文名 ----------

PERM_NODES = {
    "group.manage":   (40, "群管理"),      # 关键词/开关/公告/欢迎语
    "group.ban":      (40, "本群拉黑"),     # 本群黑名单
    "points.manage":  (40, "积分管理"),     # 积分调整/积分商店设置
    # 群管理平台接口（2026-08 官方开放）：撤回 / 禁言 / 移除成员 / 入群审批
    "group.recall":   (40, "消息撤回"),     # 撤回群消息（机器人需为群管理员）
    "group.mute":     (40, "群禁言"),       # 禁言 / 解除禁言 / 查禁言状态
    "group.member":   (40, "群成员移除"),   # 群成员批量移除
    "group.join":     (40, "入群审批"),     # 入群申请列表 / 通过 / 拒绝
    "group.grant":    (80, "群管理授权"),   # 设置/移除群管理员
    "global.ban":     (80, "全局封号"),     # 跨群+私聊封禁
    "identity.manage": (80, "身份维护"),    # 绑定表/解绑/设QQ号
    "bot.manage":     (80, "机器人管理"),   # 状态/统计/日志/重连/重载/菜单/入群策略
    "server.manage":  (80, "服务端管理"),   # KunClient 服务端管理命令
    "role.super":     (100, "超管授权"),    # 添加/移除超级管理员（仅主人）
}

# 每个角色默认拥有的权限描述（供「角色」命令展示）
ROLE_PERM_HINT = {
    100: "全部权限（含超管授权）",
    80: "群管理 / 本群拉黑 / 全局封号 / 身份维护 / 机器人管理 / 服务端管理 / 入群策略",
    40: ("本群：关键词 / 开关 / 公告 / 欢迎语 / 拉黑 / 积分管理 / "
         "撤回 / 禁言 / 移除成员 / 入群审批"),
    0: "基础功能（签到 / 账号绑定）",
}

AUDIT_KEEP = 200   # audit 环形缓冲上限

AUDIT_OPS = (
    "role.super.grant", "role.super.revoke",
    "role.group.grant", "role.group.revoke",
    "ban.group.add", "ban.group.del",
    "ban.global.add", "ban.global.del",
    "ban.identity.add", "ban.identity.del",
    "identity.bind", "identity.unbind", "identity.link",
    "config.reload",
    # 群管理平台接口（撤回 / 禁言 / 移除成员 / 入群审批）——都是对群成员的实际处置，必须留痕
    "group.recall", "group.mute.set", "group.mute.del", "group.member.remove",
    "group.join.approve", "group.join.decline", "group.join.strategy.exec",
    # 服务端侧关键操作（改公告 / 切强制更新 / 手动发版播报）——影响全体客户端，必须留痕
    "server.notice.set", "server.force_update.set", "server.release.broadcast",
)


class Permissions:
    """角色解析、权限判定、目标保护、审计。无内存缓存，全部现算（数据量极小）。"""

    def __init__(self, bot):
        self.bot = bot

    # ---------- 配置 ----------

    def _perm_cfg(self) -> dict:
        cfg = self.bot.config.get("permissions")
        return cfg if isinstance(cfg, dict) else {}

    @property
    def trust_platform_roles(self) -> bool:
        """是否信任平台群主/管理员身份（默认 false：授权必须显式）。"""
        return bool(self._perm_cfg().get("trust_platform_roles", False))

    # ---------- 角色解析 ----------

    def _config_supers(self) -> tuple[list[str], list[str]]:
        """(主人 QQ 号列表, config admins 列表)。

        owner_qq 兼容两种写法：字符串（单主人，旧配置）或 QQ 号数组（多主人）。
        admins 同样兼容 openid 旧配置。
        """
        raw = self.bot.config.get("owner_qq")
        if isinstance(raw, (list, tuple)):
            owners = [str(a).strip() for a in raw if str(a).strip()]
        else:
            one = str(raw or "").strip()
            owners = [one] if one else []
        admins = [str(a).strip() for a in (self.bot.config.get("admins") or []) if str(a).strip()]
        return owners, admins

    def owners(self) -> list[str]:
        """全部主人 QQ 号（config owner_qq，字符串或数组写法都已归一成列表）。"""
        return self._config_supers()[0]

    def is_owner_uid(self, uid: str, sender: str = "") -> bool:
        """该 uid/openid 是否主人。多主人时代的口径统一入口。"""
        owners = self.owners()
        if not owners:
            return False
        uid = str(uid or "")
        if uid in owners:
            return True
        if sender:
            qq = self.bot.identity.qq_of(sender)
            return bool(qq) and qq in owners
        return False

    def _roles_global(self) -> dict:
        return (self.bot.storage.data.get("roles") or {}).get("*") or {}

    # 公开只读访问（handler 展示用）
    def roles_global(self) -> dict:
        return dict(self._roles_global())

    def roles_group(self, group: str) -> dict:
        return dict(self._roles_group(group))

    def _roles_group(self, group: str) -> dict:
        if not group:
            return {}
        return (self.bot.storage.data.get("roles") or {}).get(group) or {}

    def _super_admins_legacy(self) -> list:
        return self.bot.storage.data.get("super_admins") or []

    def resolve_ids(self, uid: str, sender: str, group: str = "",
                    member_role: str = "") -> tuple[str, int]:
        """(uid, openid) -> (角色名, 等级)。uid 是 QQ 号或 openid: 占位键。"""
        uid = str(uid or "")
        sender = str(sender or "")
        # openid 占位键里能还原出 openid（config admins 旧配置可能存 openid）
        if not sender and uid.startswith(OPENID_PREFIX):
            sender = uid[len(OPENID_PREFIX):]
        owners, cfg_admins = self._config_supers()
        ident = self.bot.identity
        if owners and (uid in owners or (sender and ident.qq_of(sender) in owners)):
            return ROLE_OWNER
        if uid in cfg_admins or (sender and sender in cfg_admins):
            return ROLE_SUPER
        if self._roles_global().get(uid) == "super" or uid in self._super_admins_legacy():
            return ROLE_SUPER
        if group:
            r = self._roles_group(group).get(uid)
            if r == "group_admin":
                return ROLE_GROUP
            if r == "member":
                return ROLE_USER   # 显式降级：压过 trust_platform_roles
            if self.trust_platform_roles and member_role in ("admin", "owner"):
                return ROLE_GROUP
        return ROLE_USER

    def resolve(self, ctx) -> tuple[str, int]:
        # getattr 防御：测试与旧调用方可能传入只带 uid/sender 的轻量 ctx
        _role, level = self.resolve_ids(ctx.uid, ctx.sender,
                                        getattr(ctx, "group_openid", "") or "",
                                        getattr(ctx, "member_role", "") or "")
        # 对外一律给**中文角色名**：ctx.role_label 是直接展示给用户的（角色卡片、拒绝文案、
        # 日志），曾经返回内部英文名 owner/super/group_admin/user，用户在群里看到
        # 「你的角色：group_admin」——中文映射表 ROLE_LABEL 那时根本没被用上。
        # 内部英文名只保留在 resolve_ids 里，供程序判断（idtest 等按元组取值）。
        return ROLE_LABEL.get(level, "普通用户"), level

    def level_of_uid(self, uid: str, group: str = "") -> int:
        """只按 uid 判等级（目标保护用；平台身份对目标不可知，不参与）。"""
        uid = str(uid or "")
        owners, cfg_admins = self._config_supers()
        if uid in owners:
            return ROLE_OWNER[1]
        if uid in cfg_admins:
            return ROLE_SUPER[1]
        if self._roles_global().get(uid) == "super" or uid in self._super_admins_legacy():
            return ROLE_SUPER[1]
        if group and self._roles_group(group).get(uid) == "group_admin":
            return ROLE_GROUP[1]
        return ROLE_USER[1]

    # ---------- 权限判定 ----------

    def allows(self, ctx, node: str) -> bool:
        need = PERM_NODES.get(node, (80, node))[0]
        return ctx.level >= need

    def deny_text(self, ctx, node: str) -> str:
        label = PERM_NODES.get(node, (80, node))[1]
        return (f"⛔ 该命令需要「{label}」权限。\n"
                f"你的角色：{ctx.role_label}。\n"
                f"（授权请联系主人；查看自己的角色发「角色」）")

    # ---------- 目标保护（防权限倒挂） ----------

    def target_guard(self, ctx, target_uid: str, action: str = "该操作") -> str | None:
        """对「针对人」的管理操作做保护。返回 None 放行，否则返回拒绝文案。

        规则：
        - 主人（owner）永不被管理操作命中（封禁/授权/调整都不行）；
        - 目标权限 >= 操作者权限且不是操作者本人 → 拒绝（同级互咬、下克上都不行）；
        - 占位键目标（未绑定）等级为 user，放行。
        """
        target_uid = str(target_uid or "")
        if not target_uid:
            return "目标不存在或无法识别。"
        tl = self.level_of_uid(target_uid, ctx.group_openid or "")
        if tl >= ROLE_OWNER[1]:
            return "⛔ 不能对主人执行该操作。"
        if tl >= ctx.level and target_uid != ctx.uid:
            tname = ROLE_LABEL.get(tl, "未知")
            return (f"⛔ {action}不能对权限相同或更高的成员执行"
                    f"（对方是{tname}）。")
        return None

    # ---------- 授权 ----------

    async def grant_super(self, ctx, uid: str) -> tuple[bool, str]:
        """授予超级管理员（仅主人可达，Router 已挡 role.super 节点）。"""
        guard = self.target_guard(ctx, uid, "授权")
        if guard:
            return False, guard
        roles = self.bot.storage.data.setdefault("roles", {}).setdefault("*", {})
        if roles.get(uid) == "super":
            return False, "ℹ️ 该成员已经是超级管理员。"
        roles[uid] = "super"
        self._audit("role.super.grant", ctx, target=uid, detail="授予超级管理员")
        await self.bot.storage.save()
        return True, f"✅ 已授予超级管理员：{self.bot.identity.display(uid, ctx.group_openid)}"

    async def revoke_super(self, ctx, uid: str) -> tuple[bool, str]:
        owners, cfg_admins = self._config_supers()
        if uid in owners or uid in cfg_admins:
            return False, ("该管理来自 config.json（owner_qq / admins），"
                           "机器人命令无法移除——请编辑 config.json 后发「重载配置」。")
        guard = self.target_guard(ctx, uid, "撤销授权")
        if guard:
            return False, guard
        roles = (self.bot.storage.data.get("roles") or {}).get("*") or {}
        if roles.pop(uid, None) != "super":
            return False, "ℹ️ 该成员不是机器人授予的超级管理员。"
        self._audit("role.super.revoke", ctx, target=uid, detail="撤销超级管理员")
        await self.bot.storage.save()
        return True, f"✅ 已撤销超级管理员：{self.bot.identity.display(uid, ctx.group_openid)}"

    async def grant_group_admin(self, ctx, uid: str) -> tuple[bool, str]:
        guard = self.target_guard(ctx, uid, "授权")
        if guard:
            return False, guard
        roles = self.bot.storage.data.setdefault("roles", {}).setdefault(ctx.group_openid or "", {})
        if roles.get(uid) == "group_admin":
            return False, "ℹ️ 该成员已经是本群管理员。"
        roles[uid] = "group_admin"
        self._audit("role.group.grant", ctx, target=uid, detail="授予群管理员")
        await self.bot.storage.save()
        return True, f"✅ 已设置本群管理员：{self.bot.identity.display(uid, ctx.group_openid)}"

    async def revoke_group_admin(self, ctx, uid: str) -> tuple[bool, str]:
        guard = self.target_guard(ctx, uid, "撤销授权")
        if guard:
            return False, guard
        # ⚠️ 必须用 setdefault 链拿**挂在 data 上**的 dict：直接 .get(...) or {}
        # 在群没有任何 roles 记录时会拿到临时 dict，写 "member" 降级标记会静默丢失。
        roles = (self.bot.storage.data.setdefault("roles", {})
                 .setdefault(ctx.group_openid or "", {}))
        if roles.pop(uid, None) != "group_admin":
            # 信任平台身份时，移除授权要显式写成 member，否则平台群主身份会让他复活
            if self.trust_platform_roles:
                roles[uid] = "member"
                self._audit("role.group.revoke", ctx, target=uid, detail="降级为普通成员（压过平台群主身份）")
                await self.bot.storage.save()
                return True, (f"✅ 已撤销 {self.bot.identity.display(uid, ctx.group_openid)} 的群管理员，"
                              "并已压过平台群主/管理员身份。")
            return False, "ℹ️ 该成员不是本群管理员。"
        self._audit("role.group.revoke", ctx, target=uid, detail="撤销群管理员")
        await self.bot.storage.save()
        return True, f"✅ 已移除本群管理员：{self.bot.identity.display(uid, ctx.group_openid)}"

    # ---------- 审计 ----------

    def _audit(self, op: str, ctx=None, *, by: str = "", target: str = "", detail: str = ""):
        """关键操作落审计环形缓冲（同步写内存，调用方负责 save）。"""
        rec = {
            "ts": time.strftime("%Y-%m-%d %H:%M"),
            "op": op,
            "by": str(by or (ctx.uid if ctx else "") or ""),
            "target": str(target or ""),
            "group": str((ctx.group_openid if ctx else "") or ""),
            "detail": str(detail or ""),
        }
        buf = self.bot.storage.data.setdefault("audit", [])
        buf.append(rec)
        if len(buf) > AUDIT_KEEP:
            del buf[:-AUDIT_KEEP]

    def audit_records(self, n: int = 10) -> list:
        buf = self.bot.storage.data.get("audit") or []
        return buf[-max(1, min(n, 50)):]

    def audit(self, op: str, ctx=None, *, by: str = "", target: str = "", detail: str = ""):
        """公开审计入口：供角色授权以外的关键操作（改公告/切强制更新等）调用。

        与 _audit 的区别只是可见性——审计记录必须由调用方自行 `await storage.save()`
        （与角色操作一致），这里不隐式落盘，避免在只读路径上触发写盘。
        """
        self._audit(op, ctx, by=by, target=target, detail=detail)

    # ---------- 按「人」归并（展示用） ----------

    def person_key(self, uid: str) -> str:
        """把一个标识归到「人」——同一个人的 QQ 号与 openid 必须得到同一个 key。

          "2089449185"                       -> "2089449185"（本身就是号）
          "F74BE2BA8F1918522972FE13CF3AB495" -> "2089449185"（已绑定的 openid）
          "openid:XXXX"                      -> QQ 号或占位键

        否则「管理列表」会把同一个人列成好几条：config 的 admins 里存的是 openid 时，
        主人会在「主人」段以 QQ 号出现、又在「超级管理员」段以 openid 出现，各一条。
        """
        uid = str(uid or "")
        if is_qq(uid):
            return uid
        return self.bot.identity.qq_of(uid) or uid

    def persons(self, uids, group: str = "") -> list[tuple[str, str]]:
        """标识列表 -> [(显示名, person_key)]，同一人只留第一条（保序）。"""
        out, seen = [], set()
        for u in uids:
            key = self.person_key(u)
            if not key or key in seen:
                continue
            seen.add(key)
            out.append((self.bot.identity.display(u, group), key))
        return out

    # ---------- 「角色」命令输出 ----------

    def role_card(self, ctx) -> str:
        ident = self.bot.identity
        qq = ident.qq_of(ctx.sender)
        lines = [
            f"👤 你的角色：{ctx.role_label}",
            f"权限范围：{ROLE_PERM_HINT.get(ctx.level, '—')}",
        ]
        if qq:
            lines.append(f"QQ 号：{qq}（已登记）")
        else:
            lines.append("QQ 号：未登记（发「绑定QQ <QQ号>」可跨群通用，不影响使用）")
        if ctx.is_group:
            g = self._roles_group(ctx.group_openid).get(ctx.uid)
            if g:
                lines.append(f"本群授权：{'群管理员' if g == 'group_admin' else '普通成员（已显式降级）'}")
        if ctx.level < ROLE_SUPER[1]:
            lines.append("（需要更高权限请联系主人 config.json 的 owner_qq）")
        return "\n".join(lines)
