"""事件路由、命令解析与回复发送。

官方平台限制：
- 群内只有 @机器人 的消息会推送到 GROUP_AT_MESSAGE_CREATE；
- 被动回复必须带收到消息的 msg_id，同一 msg_id 最多回复 5 条（msg_seq 区分）；
- 主动消息需要平台另行开通，默认只能被动回复。

易用性设计：
- 未命中命令的消息交给 bot.on_keyword（关键词回复 + 一次性引导）；
- 命令打错时按编辑距离给出「你是想发 xxx 吗」提示。
"""
import asyncio
import json
import logging
import re
import time

log = logging.getLogger("qq.router")

MAX_REPLY_CHUNKS = 5   # 同一 msg_id 被动回复上限
CHUNK_LEN = 800        # 单条消息内容长度（保守值，超出自动分段）

MENTION_RE = re.compile(r"<@([A-Za-z0-9_\-]+)>")

PREFIXES = ("/", "！", "!", "／")

# 全角 → 半角（数字/字母/常用符号）：QQ 用户常打出全角字符导致命令无法识别
_FW_MAP = {ord(f): ord(t) for f, t in zip(
    "０１２３４５６７８９"
    "ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ"
    "ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ",
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz")}
_FW_MAP.update({ord("！"): "!", ord("／"): "/", ord("？"): "?", ord("："): ":", ord("－"): "-"})


def _normalize(text: str) -> str:
    return (text or "").translate(_FW_MAP)


def split_text(text: str, limit: int = CHUNK_LEN) -> list[str]:
    """按行边界切分长文本。"""
    parts = []
    while len(text) > limit:
        cut = text.rfind("\n", int(limit * 0.5), limit)
        if cut <= 0:
            cut = limit
        parts.append(text[:cut])
        text = text[cut:].lstrip("\n")
    parts.append(text)
    return parts


def _levenshtein(a: str, b: str) -> int:
    if abs(len(a) - len(b)) > 3:
        return 99
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[-1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


class Context:
    """一次消息事件上下文。"""

    def __init__(self, event_type: str, d: dict):
        self.event_type = event_type
        self.msg_id = d.get("id") or ""
        self.content = (d.get("content") or "").strip()
        self.group_openid = d.get("group_openid")
        self.user_openid = d.get("user_openid")
        self.author_id = (d.get("author") or {}).get("id") or self.user_openid or ""
        _au = d.get("author") or {}
        self.username = _au.get("username") or ""
        self.member_role = _au.get("member_role") or ""   # member/admin/owner
        self.is_group = event_type == "GROUP_AT_MESSAGE_CREATE"
        self.chat_key = self.group_openid or self.user_openid or self.author_id
        # 被 @ 的用户：优先事件体 mentions 字段（群聊 @ 别人时 content 里的 @ 会被平台剥掉），
        # 其次解析 content 里的 <@openid> 标签
        ms = []
        for u in d.get("mentions") or []:
            if isinstance(u, dict):
                for k in ("id", "openid", "user_openid", "member_openid"):
                    v = u.get(k)
                    if v and v not in ms:
                        ms.append(v)
        for v in MENTION_RE.findall(self.content):
            if v not in ms:
                ms.append(v)
        self.mentions = ms

        # 用户主键 uid：已绑定 QQ 号时等于 QQ 号，否则是 "openid:" + openid 占位。
        # 由 Router.handle_event 收到消息后按绑定表回填（见 Bot.identity.uid_of_openid）。
        self.uid = self.author_id
        # 角色与等级：由 Router.handle_event 经 Bot.perms.resolve 回填（RBAC，见 core/permissions.py）
        self.role_label = "普通用户"
        self.level = 0

    @property
    def sender(self) -> str:
        return self.author_id

    @property
    def session_key(self) -> str:
        """交互会话隔离键：群内按「群+用户」隔离（多人同时查询互不打扰），单聊按用户。

        用 uid（QQ 号）而非 openid，这样同一人在同群的会话不会被换设备打断。
        """
        if self.is_group and self.group_openid:
            return f"{self.group_openid}:{self.uid}"
        return self.user_openid or self.author_id or self.chat_key


class Command:
    def __init__(self, fn, help_text: str, usage: str, perm: str | None):
        self.fn = fn
        self.help = help_text
        self.usage = usage
        self.perm = perm          # 所需权限节点（None=公开），见 core/permissions.PERM_NODES
        self.admin_only = bool(perm)   # 兼容旧字段：有 perm 即需权限

    @property
    def perm_label(self) -> str:
        from core.permissions import PERM_NODES
        return PERM_NODES.get(self.perm or "", (0, "无"))[1]


class Router:
    def __init__(self, bot):
        self.bot = bot
        self.commands: dict[str, Command] = {}
        self._seq: dict[str, int] = {}   # msg_id -> 已使用的被动回复条数

    def command(self, name: str, *, aliases=(), help="", usage="", perm=None):
        """注册命令。perm 为权限节点名（如 "global.ban"），None 表示公开命令。

        权限判定**只在这里做一次**（中心化）：命令内部不再自查 is_super/is_admin，
        避免旧设计里「router 判一遍、命令里再判一遍、还有第三种判法」的漏查越权。
        """
        def deco(fn):
            cmd = Command(fn, help, usage, perm)
            self.commands[name] = cmd
            for a in aliases:
                self.commands[a] = cmd
            return fn
        return deco

    def _suggest(self, name: str) -> str | None:
        name = name.lower()
        best, best_d = None, 99
        for cname in self.commands:
            d = _levenshtein(name, cname.lower())
            if d < best_d:
                best, best_d = cname, d
        if best and best_d <= (1 if len(name) <= 2 else 2):
            return best
        return None

    # ---------- 事件入口 ----------

    async def handle_event(self, event_type: str, d: dict):
        if event_type not in ("GROUP_AT_MESSAGE_CREATE", "C2C_MESSAGE_CREATE"):
            await self.bot.on_other_event(event_type, d)
            return
        ctx = Context(event_type, d)
        if not ctx.content or not ctx.sender:
            return
        # 用户主键：绑定过 QQ 号就用 QQ 号，否则用 openid 占位键
        ctx.uid = self.bot.identity.uid_of_openid(ctx.sender)
        # 角色解析（RBAC）：ctx.role_label / ctx.level 供权限判定与目标保护用
        ctx.role_label, ctx.level = self.bot.perms.resolve(ctx)
        self.bot.stats["msg_recv"] += 1
        log.info("收到%s消息 [%s]: %s", "群" if ctx.is_group else "私聊",
                 ctx.uid, ctx.content[:60])
        # 诊断：需要指定目标的命令才记录原始事件体（平时降噪，排查 @ 识别时够用）
        if ctx.content.startswith(("拉黑", "取消拉黑", "全局拉黑", "设置管理", "移除管理")):
            try:
                log.info("原始事件: %s", json.dumps(d, ensure_ascii=False)[:2500])
            except Exception:
                pass
        # 群名册：记录 @ 过机器人的成员（昵称 ↔ openid），供昵称匹配与显示
        if ctx.is_group and ctx.group_openid and ctx.username:
            roster = self.bot.storage.data.setdefault("roster", {}).setdefault(ctx.group_openid, {})
            entry = roster.get(ctx.sender)
            qq = self.bot.identity.qq_of(ctx.sender) or ""
            if not entry or entry.get("username") != ctx.username or entry.get("role") != ctx.member_role \
                    or entry.get("qq") != qq:
                roster[ctx.sender] = {"username": ctx.username, "role": ctx.member_role, "qq": qq}
                asyncio.create_task(self.bot.storage.save())
        # 消息 ID 台账：撤回命令要按 message_id 操作，而事件里的 d.id 只在这一刻可得
        if ctx.is_group and ctx.group_openid:
            msglog = getattr(self.bot, "msglog", None)
            if msglog:
                msglog.record(ctx.group_openid, ctx.msg_id, openid=ctx.sender,
                              name=ctx.username, text=ctx.content)
        if ctx.is_group and ctx.group_openid:
            g = self.bot.groups.setdefault(ctx.group_openid, {"count": 0, "first": time.time(), "last": 0})
            g["count"] += 1
            g["last"] = time.time()
        # 拦截：① 全局黑名单（按 QQ 号，跨群跨私聊）② 本群黑名单
        if self.bot.is_blacklisted(ctx):
            self.bot.stats["blocked"] += 1
            log.info("已忽略消息 [%s]：%s", ctx.uid,
                     "全局黑名单" if self.bot.identity.is_globally_blocked(ctx.sender) else "本群黑名单")
            return

        # 全角归一后再解析（不影响关键词原文匹配）
        text = _normalize(ctx.content)

        name, args_text = self._parse(text)
        if name is None:
            await self.bot.on_keyword(ctx)
            return
        cmd = self.commands.get(name) or self.commands.get(name.lower())
        if cmd is None:
            looks_like_cmd = name.lower().startswith(("kun", "bot", "帮", "管理", "菜单", "功能", "命令")) \
                or text.startswith(PREFIXES)
            if looks_like_cmd:
                sug = self._suggest(name)
                msg = f"没有「{name}」这个命令" + (f"，你是想发「{sug}」吗？" if sug else "。")
                await self.reply(ctx, msg + "\n发送「帮助」查看全部命令。")
            else:
                await self.bot.on_keyword(ctx)
            return
        if cmd.perm and not self.bot.perms.allows(ctx, cmd.perm):
            # 中心化鉴权：未取得所需权限节点，统一拒绝（命令内部不再自查权限）
            self.bot.stats["denied"] = self.bot.stats.get("denied", 0) + 1
            log.info("权限不足 [%s/%s] 命令 %s 需要 %s",
                     ctx.uid, ctx.role_label, name, cmd.perm)
            return await self.reply(ctx, self.bot.perms.deny_text(ctx, cmd.perm))
        self.bot.stats["cmd"] += 1
        try:
            await cmd.fn(ctx, args_text)
        except Exception as e:
            self.bot.stats["errors"] += 1
            log.exception("命令 %s 执行失败", name)
            await self.reply(ctx, f"命令执行出错：{e}")

    @staticmethod
    def _parse(text: str):
        raw = text.strip()
        if raw[:1] in PREFIXES:
            raw = raw[1:].strip()
        parts = raw.split(None, 1)
        if not parts:
            return None, ""
        return parts[0], parts[1].strip() if len(parts) > 1 else ""

    # ---------- 回复 ----------

    async def reply(self, ctx: Context, text: str):
        chunks = split_text(str(text))
        seq = self._seq.get(ctx.msg_id, 0)
        sent = 0
        for chunk in chunks:
            seq += 1
            if seq > MAX_REPLY_CHUNKS:
                log.warning("msg_id %s 被动回复超过 %d 条，丢弃剩余内容", ctx.msg_id, MAX_REPLY_CHUNKS)
                break
            try:
                await self._send_one(ctx, chunk, seq)
                sent += 1
            except Exception as e:
                log.warning("发送消息失败，1 秒后重试一次: %s", e)
                await asyncio.sleep(1)
                try:
                    await self._send_one(ctx, chunk, seq)
                    sent += 1
                except Exception as e2:
                    seq -= 1  # 失败的 seq 未消费，下一条重试
                    log.warning("发送消息最终失败（msg_id=%s seq=%s）: %s", ctx.msg_id, seq, e2)
                    break
        self._seq[ctx.msg_id] = seq
        if len(self._seq) > 5000:
            self._seq.clear()
        self.bot.stats["replies"] += sent
        return sent

    async def _send_one(self, ctx: Context, chunk: str, seq: int):
        if ctx.is_group and ctx.group_openid:
            resp = await self.bot.api.send_group_message(
                ctx.group_openid, content=chunk, msg_id=ctx.msg_id, msg_seq=seq)
            self._remember_sent(ctx, resp, chunk)
        elif ctx.user_openid or ctx.author_id:
            # C2C 场景部分事件不带 user_openid，author.id 即用户 openid
            await self.bot.api.send_c2c_message(
                ctx.user_openid or ctx.author_id, content=chunk, msg_id=ctx.msg_id, msg_seq=seq)
        else:
            raise RuntimeError(f"无法确定回复目标（事件 {ctx.event_type}）")

    def _remember_sent(self, ctx: Context, resp, chunk: str):
        """把机器人自己发出的 message_id 记进台账，「撤回」命令靠它撤自己的话。

        发送接口成功响应形如 {"id": "...", "timestamp": ...}；C2C 撤回官方未开放，
        所以只记群消息。异常一律吞掉——记台账失败不能影响消息已经发出的事实。
        """
        msglog = getattr(self.bot, "msglog", None)
        if not msglog or not ctx.group_openid:
            return
        try:
            mid = (resp or {}).get("id") or ""
        except AttributeError:
            mid = ""
        if mid:
            msglog.record(ctx.group_openid, mid, mine=True, text=chunk)
