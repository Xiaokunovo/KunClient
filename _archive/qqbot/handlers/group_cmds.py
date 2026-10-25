"""群管理平台命令：撤回消息 / 入群审批 / 群禁言 / 群成员移除 / 群信息。

官方 2026-08-10 与 09-03 两批开放了**群聊**管理接口（此前只有频道体系有 mute/kick），
本模块把它们接成命令：

    撤回群聊消息        DELETE /v2/groups/{g}/messages/{id}
    入群申请列表        GET    /v2/groups/{g}/join_request_list
    入群申请审批        POST   /v2/groups/{g}/approval_join_request/{member}
    入群自动审批策略    GET/POST /v2/groups/join_approval_strategy[/{id}/execute]
    查询群禁言状态      GET    /v2/groups/{g}/restrict_chat_setting
    设置群成员禁言      POST   /v2/groups/{g}/restrict_chat_setting
    群成员批量移除      POST   /v2/groups/{g}/batch_remove_members
    群基本信息/机器人状态 GET  /v2/groups/{g}/info ｜ /bot_state

几条硬约束（踩过才知道）：
- 这些接口全部要求**机器人在群内是管理员**（否则 40062003）；「批量移除 / 成员列表」
  目前是内邀能力，没开白名单会返回 11253（应用无接口访问权限）——错误文案已翻成中文。
- 目标必须是 **member_openid**（平台群管理标识），不是 QQ 号。QQ 号是机器人自己的
  身份层，平台不认，所以这里绝不做 QQ→openid 换算。
- 撤回窗口只有 **2 分钟**，且 id 只在事件/发送响应里出现一次 → 由 core/msglog.py 记账。
- 禁言只能作用于普通成员，最长 30 天。
- 权限节点在 @router.command 上声明，由 Router 中心化判定，命令内部不复查。
- 处置类操作（撤回/禁言/移除/审批）一律过 bot.guard_target 并落审计。
"""
import logging
import re
import time
from datetime import datetime, timedelta, timezone

from core.identity import is_openid
from .router import MENTION_RE

log = logging.getLogger("qq.group")

TZ = timezone(timedelta(hours=8))
RECALL_MAX = 5              # 一次最多撤几条（防手滑刷屏）
MUTE_DEFAULT = 10 * 60      # 默认禁言 10 分钟
MUTE_MAX = 30 * 86400       # 平台上限 30 天
BATCH_MAX = 20              # 禁言/移除单次上限
JOIN_TTL = 900              # 入群申请缓存有效期（秒）
STATE_TTL = 600             # 机器人群内身份缓存有效期（秒）

_DUR_RE = re.compile(r"^(\d+)(s|m|h|d|秒|分|分钟|时|小时|天|日)$", re.I)
_DUR_UNIT = {"s": 1, "秒": 1, "m": 60, "分": 60, "分钟": 60,
             "h": 3600, "时": 3600, "小时": 3600, "d": 86400, "天": 86400, "日": 86400}

# 🔴 平台硬限制（2026-10-17 用 79 条线上原始事件核实）：群消息事件体里**没有 mentions
# 字段**，`content` 里被 @ 的那个人的昵称会被整段抹掉（只剩空格）。也就是说
# 「@ 某人」这条路在群聊里根本走不通 —— 机器人永远拿不到被 @ 者的 member_openid。
# 唯一可行的目标来源是**群名册**：谁 @ 过机器人一次，谁的 openid 就从 author.id 落进名册。
NO_TARGET_TIP = (
    "没找到这个人。发「群成员」看编号，再发「禁言 3 10分」。\n"
    "名册里没有他？让他 @我 发一句「签到」，我就能认出他了。\n"
    "（平台不提供 @ 对象，只能这样认人）")


# ---------- 纯函数（便于离线测试） ----------

def parse_duration(text: str) -> tuple[int | None, str]:
    """解析时长，返回 (秒, 备注)。识别不了返回 (None, "")。

    支持 30s / 10m / 1h / 2d 与 30秒 / 10分 / 1小时 / 2天；
    「永久」按平台上限 30 天处理（官方不支持真·永久）。
    """
    raw = (text or "").strip().lower()
    if not raw:
        return None, ""
    if raw in ("永久", "长期", "forever", "inf"):
        return MUTE_MAX, "（平台上限 30 天，不支持真·永久）"
    m = _DUR_RE.match(raw)
    if m:
        return int(m.group(1)) * _DUR_UNIT[m.group(2).lower()], ""
    if raw.isdigit():
        return int(raw) * 60, ""
    return None, ""


def rfc3339(seconds: float) -> str:
    """未来 N 秒的 RFC3339 时间（+08:00），禁言接口要求的格式。"""
    return (datetime.now(TZ) + timedelta(seconds=float(seconds))).isoformat(timespec="seconds")


def short_time(value: str) -> str:
    """RFC3339 -> '08-05 14:19'（北京时间）。解析失败原样截断。"""
    if not value:
        return ""
    try:
        dt = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return str(value)[:16]
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=TZ)
    return dt.astimezone(TZ).strftime("%m-%d %H:%M")


def human_duration(seconds: int) -> str:
    """秒 -> '1 小时 30 分' 这类中文时长。"""
    seconds = max(0, int(seconds))
    if seconds < 60:
        return f"{seconds} 秒"
    parts, rest = [], seconds
    for unit, size in (("天", 86400), ("小时", 3600), ("分钟", 60)):
        if rest >= size:
            parts.append(f"{rest // size} {unit}")
            rest %= size
    if rest and len(parts) < 2:
        parts.append(f"{rest} 秒")
    return " ".join(parts[:2])


# ---------- 命令注册 ----------

def register(bot, router):
    ident = bot.identity
    # 缓存挂在 bot 上（而不是纯闭包变量）：测试能一键清空，运维排查时也能直接看。
    join_cache = bot.join_cache = {}      # group_openid -> {list, ts, cursor}
    state_cache = bot.bot_state_cache = {}  # group_openid -> (ts, bot_state)
    strat_cache = bot.strategy_cache = {}   # "global" -> {list, ts}

    # ---------- 通用小工具 ----------

    async def _need_group(ctx, label="该命令") -> bool:
        """群管理接口全依赖 group_openid，私聊里没有群上下文，直接拒绝。"""
        if ctx.is_group and ctx.group_openid:
            return True
        await router.reply(ctx, f"{label}只能在群里用（私聊没有群上下文）。")
        return False

    async def _bot_state(ctx) -> dict:
        """机器人在本群的身份信息，缓存 10 分钟。失败返回 {}（不阻塞后续操作）。"""
        group = ctx.group_openid or ""
        hit = state_cache.get(group)
        now = time.time()
        if hit and now - hit[0] < STATE_TTL:
            return hit[1]
        try:
            st = await bot.api.get_bot_state(group)
        except Exception as e:
            log.info("获取群内机器人状态失败（不影响后续操作）: %s", e)
            return {}
        if not isinstance(st, dict):
            return {}
        state_cache[group] = (now, st)
        return st

    async def _bot_role(ctx) -> str:
        return ((await _bot_state(ctx)) or {}).get("member_role") or ""

    async def _admin_hint(ctx) -> str:
        """失败后的补充说明：能确定不是管理员就直说，查不到就给通用提示。"""
        role = await _bot_role(ctx)
        if role in ("admin", "owner"):
            return ""
        if role:
            return ("\n💡 机器人在本群是普通成员 —— 请群主在「群设置 → 群成员 → 机器人」里"
                    "把它设为管理员后再试。")
        return "\n💡 若提示「无操作权限」，请把机器人设为群管理员。"

    async def _require_admin(ctx, action: str = "该操作") -> bool:
        """平台要求「机器人在群内是管理员」—— 能确定不是，就当场拒绝。

        为什么要前置拦：接口只会回 `11703 机器人不是群管理员`，翻成中文也还是
        「操作失败」，用户不知道下一步该干什么（2026-10-17 线上实测踩到）。
        查不到身份（bot_state 没开白名单等）时**不拦**，交给接口自己报错 ——
        否则一个辅助接口不可用就把所有群管命令全挡死了。
        """
        role = await _bot_role(ctx)
        if role in ("admin", "owner") or not role:
            return True
        await router.reply(
            ctx,
            f"⛔ 我现在不是本群管理员，没法{action}。\n"
            "请在「群设置 → 群成员 → 机器人」里把我设为管理员，再发一次。\n"
            "（发「群信息」可随时查看我当前身份）")
        return False

    def _self_openid(ctx) -> str:
        """机器人自己的 member_openid（用来把「@机器人」从目标里剔掉）。"""
        hit = state_cache.get(ctx.group_openid or "")
        return ((hit[1] or {}).get("member_openid") if hit else "") or ""

    def _roster_rows(ctx):
        """群名册 → [(openid, 昵称, 身份, QQ号)]，顺序固定（身份高的在前，同身份按昵称）。

        「群成员」列表与「禁言 #3」的编号必须来自同一份排序，否则按编号会点错人。
        名册只收录**@ 过机器人**的成员 —— 平台不提供群成员列表接口（`/members` 报 11253）。
        """
        roster = bot.storage.data.get("roster", {}).get(ctx.group_openid or "", {}) or {}
        rank = {"owner": 0, "admin": 1, "member": 2}
        rows = []
        for oid, info in roster.items():
            info = info or {}
            name = info.get("username") or ident.display(oid, ctx.group_openid or "")
            rows.append((str(oid), name, info.get("role") or "", info.get("qq") or ""))
        rows.sort(key=lambda r: (rank.get(r[2], 3), r[1]))
        return rows

    def _resolve(ctx, args):
        """解析目标 → ([(openid, 显示名)], 提示文案)。

        平台群管理接口只认 **member_openid**，而群事件又不告诉我们 @ 了谁
        （见 NO_TARGET_TIP 上的说明），所以这里按「能在本地确定身份」的顺序来：
            mentions / <@id>  →  裸 openid  →  #序号  →  QQ 号  →  昵称（精确/唯一子串）
        多个目标用空格隔开逐个解析；解析不出来的 token 汇总成提示。
        """
        out: list[tuple[str, str]] = []
        seen: set[str] = set()
        self_oid = _self_openid(ctx)
        rows = _roster_rows(ctx)

        def add(oid, name=""):
            oid = str(oid or "").strip()
            if not oid or oid == self_oid or oid in seen:
                return
            seen.add(oid)
            out.append((oid, name or ident.display(oid, ctx.group_openid or "")))

        # ① 平台给了 mentions（频道消息才有，群聊恒为空）或文本里带 <@id>
        for oid in list(ctx.mentions) + MENTION_RE.findall(args or ""):
            add(oid)
        if out:
            return out, None

        raw = re.sub(r"^\s*[@＠]\s*", "", (args or "").strip()).strip()
        if not raw:
            return [], None

        # ② 先整串试一次（昵称里带空格的情况，例如「河南省省长-青墨」不会被空格切开）
        whole = next((r for r in rows if r[1] == raw), None)
        if whole:
            add(whole[0], whole[1])
            return out, None

        missing = []
        for tok in raw.split():
            tok = tok.strip()
            if not tok:
                continue
            hit = None
            if tok.startswith("#") and tok[1:].isdigit():
                idx = int(tok[1:])
                if 1 <= idx <= len(rows):
                    hit = rows[idx - 1]
            elif is_openid(tok):
                hit = next((r for r in rows if r[0] == tok), None) or (tok, "", "", "")
            elif tok.isdigit():
                # QQ 号优先于序号：「禁言 12345」里 12345 几乎不可能是第 12345 个成员
                hit = next((r for r in rows if r[3] and r[3] == tok), None)
                if not hit:
                    # 全局绑定表兜底：qq_bind 是跨群/私聊的 QQ号 → openid 映射，
                    # 所以「在别的群 @ 过我并绑过 QQ」的人这里也能按号找到
                    # （实测 member_openid 不随群变化，同一个人各群一致）。
                    oids = ident.openids_of(tok)
                    if oids:
                        for oid in oids:
                            add(oid)
                        continue
                if not hit and int(tok) <= len(rows):
                    hit = rows[int(tok) - 1]
            if not hit:
                exact = [r for r in rows if r[1] == tok]
                if len(exact) == 1:
                    hit = exact[0]
                elif len(exact) > 1:
                    return [], f"有 {len(exact)} 个人都叫「{tok}」，用「群成员」看编号后发「#编号」。"
                else:
                    sub = [r for r in rows if tok in r[1]]
                    if len(sub) == 1:
                        hit = sub[0]
                    elif len(sub) > 1:
                        names = "、".join(r[1] for r in sub[:8])
                        return [], f"「{tok}」匹配到多个人（{names}），用更完整的昵称或「群成员」看编号。"
            if hit:
                add(hit[0], hit[1])
            else:
                missing.append(tok)
        if not out and missing:
            return [], NO_TARGET_TIP
        return out, None


    def _guard(ctx, targets, action: str):
        """目标保护：不能对主人 / 权限 >= 自己的成员执行（返回拒绝文案或 None）。

        另外先用**群名册里的平台身份**挡一层：平台不允许禁言/移除群主与管理员
        （只会回 `40062003 无操作权限`，看不出到底是谁的问题），提前说清楚更省事。
        """
        role_of = {r[0]: r[2] for r in _roster_rows(ctx)}
        for oid, name in targets:
            role = role_of.get(oid) or ""
            if role in ("admin", "owner"):
                who = "群主" if role == "owner" else "管理员"
                return f"⛔ {name} 是本群{who}，平台不允许对{who}执行「{action}」。"
            deny = bot.perms.target_guard(ctx, ident.uid_of_openid(oid), action)
            if deny:
                return deny
        return None

    def _names(targets) -> str:
        return "、".join(n for _o, n in targets)

    # ================= 撤回群聊消息 =================

    async def _recall_one(ctx, entry):
        try:
            await bot.api.delete_group_message(ctx.group_openid, entry["id"])
        except Exception as e:
            return False, str(e)
        bot.msglog.drop(ctx.group_openid, entry["id"])
        return True, ""

    @router.command("撤回", aliases=("撤回消息", "recall"), perm="group.recall",
                    usage="撤回 ｜ 撤回 <1-5> ｜ 撤回 #编号",
                    help="撤回机器人最近一条回复；#编号 = 撤该成员最近一条消息（2 分钟内）")
    async def recall_cmd(ctx, args):
        if not await _need_group(ctx, "撤回"):
            return
        text = (args or "").strip()
        msglog = bot.msglog

        # ① 指定了人 → 撤他最近一条消息（机器人是管理员才行，普通成员只能撤自己）
        if ctx.mentions or MENTION_RE.search(text) or (text and not text.isdigit()):
            await _bot_state(ctx)   # 先拿到机器人自己的 openid，才能把「@机器人」剔出目标
            if not await _require_admin(ctx, "撤回成员的消息"):
                return
            targets, tip = _resolve(ctx, text)
            if tip:
                return await router.reply(ctx, tip)
            if targets:
                oid, name = targets[0]
                entry = msglog.last(ctx.group_openid, openid=oid)
                if not entry:
                    return await router.reply(
                        ctx, f"没找到 {name} 最近的消息。\n"
                             "只有他 @ 过我、且还在 2 分钟内的消息才能撤。")
                if not msglog.recallable(entry):
                    return await router.reply(
                        ctx, f"⏱ 那条消息是 {msglog.age_text(entry)}发的，"
                             "已超过平台 2 分钟撤回时限。")
                ok, err = await _recall_one(ctx, entry)
                if not ok:
                    return await router.reply(ctx, f"❌ 撤回失败：{err}{await _admin_hint(ctx)}")
                bot.perms.audit("group.recall", ctx, target=oid,
                                detail=f"撤回成员消息（{msglog.age_text(entry)}）")
                await bot.storage.save()
                return await router.reply(ctx, f"✅ 已撤回 {name} 的消息。")

        # ② 撤机器人自己的消息：撤回 / 撤回 3
        n = max(1, min(int(text), RECALL_MAX)) if text.isdigit() else 1
        entries = msglog.recent(ctx.group_openid, mine=True, n=n)
        if not entries:
            return await router.reply(
                ctx, "我这边没有可撤回的发送记录。\n"
                     "（只保留本次运行期间、最近 40 条消息；重启后清空）")

        ok_n, timeout_n, errors = 0, 0, []
        for entry in reversed(entries):      # 从最新的开始撤
            if not msglog.recallable(entry):
                timeout_n += 1
                continue
            ok, err = await _recall_one(ctx, entry)
            if ok:
                ok_n += 1
            else:
                errors.append(err)
        if ok_n:
            bot.perms.audit("group.recall", ctx, detail=f"撤回机器人消息 {ok_n} 条")
            await bot.storage.save()
        lines = []
        if ok_n:
            lines.append(f"✅ 已撤回我的 {ok_n} 条消息。")
        if timeout_n:
            lines.append(f"⏱ {timeout_n} 条已超过 2 分钟撤回时限，跳过。")
        if errors:
            lines.append(f"❌ {len(errors)} 条撤回失败：{errors[0]}")
            lines.append((await _admin_hint(ctx)).strip())
        await router.reply(ctx, "\n".join(l for l in lines if l))

    # ================= 入群申请 =================

    async def _fetch_join(ctx, pages: int = 1):
        """拉取入群申请（可翻页），并缓存供「入群同意 3」按序号回查 join_request_id。"""
        items: list = []
        cursor = ""
        for _ in range(max(1, min(int(pages or 1), 5))):
            data = await bot.api.list_join_requests(ctx.group_openid, cursor=cursor, limit=20)
            if not isinstance(data, dict):
                break
            items.extend(data.get("list") or [])
            cursor = data.get("next_cursor") or ""
            if not cursor:
                break
        join_cache[ctx.group_openid] = {"list": items, "ts": time.time(), "cursor": cursor}
        return items, cursor

    async def _pick_requests(ctx, args):
        """把「1 2 3」或 member_openid 解析成申请条目；缓存过期自动重拉。"""
        cache = join_cache.get(ctx.group_openid or "")
        items = (cache or {}).get("list") or []
        if not items or time.time() - float((cache or {}).get("ts") or 0) > JOIN_TTL:
            try:
                items, _cursor = await _fetch_join(ctx)
            except Exception as e:
                return [], f"❌ 拉取申请列表失败：{e}{await _admin_hint(ctx)}"
        picked, bad, done = [], [], []
        for tok in (args or "").split():
            if tok.isdigit():
                idx = int(tok)
                if 1 <= idx <= len(items):
                    # 序号必须和「入群申请」列出来的保持一致：处理过的条目**留在原位**
                    # 只打标记，否则通过第 1 条后第 2 条会变成第 1 条，管理员按旧序号就错了。
                    if items[idx - 1].get("_done"):
                        done.append(tok)
                    else:
                        picked.append(items[idx - 1])
                else:
                    bad.append(tok)
                continue
            hit = [it for it in items
                   if it.get("member_openid") == tok or it.get("join_request_id") == tok]
            if hit:
                if hit[0].get("_done"):
                    done.append(tok)
                else:
                    picked.append(hit[0])
            else:
                bad.append(tok)
        if not picked:
            if done:
                return [], "这几条已经处理过了：" + "、".join(done) + "\n发「入群申请」可刷新列表。"
            tip = ("序号超出范围：" + "、".join(bad)) if bad else "没有匹配的申请。"
            return [], tip + "\n先发「入群申请」刷新列表。"
        return picked, None

    async def _approve(ctx, items, *, decline: bool = False,
                       reason: str = "", blacklist: bool = False):
        lines, failed = [], 0
        for it in items:
            oid = it.get("member_openid") or ""
            name = it.get("username") or (oid[:8] + "…" if oid else "未知")
            try:
                await bot.api.approval_join_request(
                    ctx.group_openid, oid, "decline" if decline else "approve",
                    join_request_id=it.get("join_request_id") or "",
                    reject_reason=reason, add_to_member_blacklist=blacklist)
            except Exception as e:
                failed += 1
                lines.append(f"❌ {name}：{e}")
                continue
            lines.append(("🚫 已拒绝：" if decline else "✅ 已通过：") + name
                         + ("（已加入群黑名单）" if (decline and blacklist) else ""))
            bot.perms.audit("group.join.decline" if decline else "group.join.approve",
                            ctx, target=oid,
                            detail=(f"拒绝入群：{reason}" if decline else "通过入群申请"))
            # 打标记而不是从缓存里删：序号要保持和列表一致（见 _pick_requests）
            it["_done"] = "decline" if decline else "approve"
        if lines:
            await bot.storage.save()
        if failed:
            lines.append((await _admin_hint(ctx)).strip())
        await router.reply(ctx, "\n".join(l for l in lines if l))

    @router.command("入群申请", aliases=("申请列表", "入群请求", "待审申请"), perm="group.join",
                    usage="入群申请 [页数]", help="拉取待审批的入群申请（机器人需为群管理员）")
    async def join_list_cmd(ctx, args):
        if not await _need_group(ctx, "入群申请"):
            return
        if not await _require_admin(ctx, "拉取入群申请"):
            return
        pages = int(args.strip()) if args.strip().isdigit() else 1
        try:
            items, cursor = await _fetch_join(ctx, pages)
        except Exception as e:
            return await router.reply(ctx, f"❌ 拉取失败：{e}{await _admin_hint(ctx)}")
        if not items:
            return await router.reply(ctx, "✅ 当前没有待审批的入群申请。")
        lines = [f"📥 待审批入群申请（{len(items)} 条）："]
        for i, it in enumerate(items, 1):
            head = f"{i}. {it.get('username') or '（无昵称）'}"
            if it.get("_done"):
                head += "（已处理）"
            bits = [head]
            src = {"self_apply": "主动申请", "invited": "被邀请"}.get(
                it.get("apply_source") or "", it.get("apply_source") or "")
            if src:
                bits.append(src)
            at = short_time(it.get("apply_at") or "")
            if at:
                bits.append(at)
            if it.get("bot"):
                bits.append("🤖机器人")
            lines.append("｜".join(bits))
            risk = (it.get("risk_tips") or "").strip()
            if risk:
                lines.append(f"   ⚠️ {risk[:60]}")
            vi = it.get("verify_info") or {}
            if vi.get("verify_message"):
                lines.append(f"   验证消息：{str(vi['verify_message'])[:60]}")
            for qa in (vi.get("review_qa_list") or [])[:3]:
                lines.append(f"   ❓{qa.get('question', '')} → {qa.get('answer', '')}")
        lines.append("")
        lines.append("通过：入群同意 序号 ｜ 拒绝：入群拒绝 序号 [理由] [拉黑]")
        if cursor:
            lines.append("还有更多：入群申请 2")
        await router.reply(ctx, "\n".join(lines))

    @router.command("入群同意", aliases=("同意入群", "通过申请", "批准入群"), perm="group.join",
                    usage="入群同意 <序号> [序号…]", help="通过入群申请（可一次给多个序号）")
    async def join_approve_cmd(ctx, args):
        if not await _need_group(ctx, "入群审批"):
            return
        if not await _require_admin(ctx, "审批入群申请"):
            return
        if not args.strip():
            return await router.reply(ctx, "用法：入群同意 1 2 3\n（先发「入群申请」看序号）")
        items, tip = await _pick_requests(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        await _approve(ctx, items)

    @router.command("入群拒绝", aliases=("拒绝入群", "驳回申请"), perm="group.join",
                    usage="入群拒绝 <序号> [理由] [拉黑]",
                    help="拒绝入群申请；加「拉黑」同时加入群黑名单")
    async def join_decline_cmd(ctx, args):
        if not await _need_group(ctx, "入群审批"):
            return
        if not await _require_admin(ctx, "审批入群申请"):
            return
        if not args.strip():
            return await router.reply(ctx, "用法：入群拒绝 1 广告号 拉黑")
        blacklist = any(w in args for w in ("拉黑", "黑名单"))
        text = args.replace("拉黑", " ").replace("黑名单", " ")
        tokens = text.split()
        idx = [t for t in tokens if t.isdigit()]
        reason = " ".join(t for t in tokens if not t.isdigit()).strip() or "管理员拒绝"
        if not idx:
            return await router.reply(ctx, "用法：入群拒绝 <序号> [理由]\n（先发「入群申请」看序号）")
        items, tip = await _pick_requests(ctx, " ".join(idx))
        if tip:
            return await router.reply(ctx, tip)
        await _approve(ctx, items, decline=True, reason=reason[:60], blacklist=blacklist)

    # ================= 入群自动审批策略（机器人级） =================

    async def _load_strategies():
        cache = strat_cache.get("global")
        if cache and time.time() - float(cache.get("ts") or 0) < JOIN_TTL:
            return cache.get("list") or []
        data = await bot.api.list_join_strategies(limit=20)
        items = (data or {}).get("strategies") or [] if isinstance(data, dict) else []
        strat_cache["global"] = {"list": items, "ts": time.time()}
        return items

    @router.command("自动审批", aliases=("审批策略", "入群策略"), perm="bot.manage",
                    usage="自动审批 ｜ 自动审批 执行 <序号|策略ID>",
                    help="查看入群自动审批策略；执行 = 全量扫描白名单自动通过（约 10 分钟）")
    async def strategy_cmd(ctx, args):
        parts = args.split(None, 1)
        sub = parts[0] if parts else ""
        rest = parts[1].strip() if len(parts) > 1 else ""
        try:
            items = await _load_strategies()
        except Exception as e:
            return await router.reply(ctx, f"❌ 查询策略失败：{e}")

        if sub in ("执行", "run", "exec"):
            if not items:
                return await router.reply(ctx, "当前没有生效中的入群自动审批策略。")
            if not rest:
                return await router.reply(ctx, "用法：自动审批 执行 <序号|策略ID>")
            target = None
            if rest.isdigit():
                idx = int(rest)
                if 1 <= idx <= len(items):
                    target = items[idx - 1]
            else:
                target = next((s for s in items
                               if s.get("strategy_id") == rest), None)
            if not target:
                return await router.reply(ctx, f"没找到策略「{rest}」。发「自动审批」看列表。")
            sid = target.get("strategy_id") or ""
            groups = len(target.get("group_openids") or target.get("group_ids") or [])
            summary = (f"执行入群自动审批策略 {sid}"
                       f"（关联 {groups} 个群，白名单 {target.get('whitelist_user_count', 0)} 人，"
                       "全量扫描约 10 分钟）")

            async def _run(c):
                try:
                    await bot.api.execute_join_strategy(sid)
                except Exception as e:
                    return await router.reply(c, f"❌ 执行失败：{e}")
                bot.perms.audit("group.join.strategy.exec", c, detail=summary)
                await bot.storage.save()
                await router.reply(c, f"▶️ 已提交执行：{sid}\n"
                                      "白名单内的入群申请会被自动通过，官方说明约 10 分钟完成。")

            bot.confirms.arm(ctx.session_key, command="自动审批 执行", summary=summary,
                             run=_run, uid=ctx.uid, perm="bot.manage")
            return await router.reply(ctx, f"⚠️ 即将执行：{summary}\n"
                                          "回「确认」执行，「取消」放弃（90 秒内有效）。")

        if not items:
            return await router.reply(ctx, "当前没有生效中的入群自动审批策略。")
        lines = [f"🤖 入群自动审批策略（{len(items)} 个）："]
        for i, s in enumerate(items, 1):
            gs = s.get("group_openids") or []
            gids = s.get("group_ids") or []
            scope = f"{len(gs)} 个群" if gs else (f"群号 {len(gids)} 个" if gids else "未关联群")
            bits = [f"{i}. {s.get('strategy_id') or '?'}", scope,
                    f"白名单 {s.get('whitelist_user_count', 0)} 人",
                    "启用" if s.get("is_enable") == "on" else "已关闭"]
            exp = short_time(s.get("expire_at") or "")
            if exp:
                bits.append(f"至 {exp}")
            lines.append("｜".join(bits))
            if s.get("remark"):
                lines.append(f"   {str(s['remark'])[:40]}")
        lines.append("")
        lines.append("执行（全量扫描白名单并自动通过）：自动审批 执行 <序号>")
        await router.reply(ctx, "\n".join(lines))

    # ================= 群禁言 =================

    def _split_duration(args):
        """从参数里摘出时长 → (剩余文本, 秒数, 备注)。

        时长**从后往前找第一个能解析的 token**（`禁言 3 10分` / `禁言 10分 3` 都对），
        纯数字只有在**还剩别的 token** 时才当分钟 —— 否则「禁言 12345」里的数字更可能是
        目标（QQ 号 / 名册编号），不能一口吞掉。找不到时长就用默认 10 分钟。
        """
        seconds, note = MUTE_DEFAULT, ""
        tokens = (args or "").split()
        for i in range(len(tokens) - 1, -1, -1):
            tok = tokens[i]
            sec, extra = parse_duration(tok)
            if not sec:
                continue
            if tok.strip().isdigit() and len(tokens) < 2:
                continue          # 只有数字本身 → 当成目标，不给时长
            if sec > MUTE_MAX:
                seconds, note = MUTE_MAX, "（超过平台 30 天上限，已按 30 天设置）"
            else:
                seconds, note = sec, extra
            del tokens[i]
            break
        return " ".join(tokens), seconds, note

    async def _apply_mute(ctx, targets, *, seconds: int, note: str, unmute: bool = False):
        members = []
        for oid, _name in targets[:BATCH_MAX]:
            if unmute:
                members.append({"op": "del", "member_openid": oid, "mute_expire_at": ""})
            else:
                members.append({"op": "add", "member_openid": oid,
                                "mute_expire_at": rfc3339(seconds)})
        try:
            await bot.api.set_member_mute(ctx.group_openid, members)
        except Exception as e:
            return await router.reply(ctx, f"❌ 操作失败：{e}{await _admin_hint(ctx)}")
        for oid, _name in targets:
            bot.perms.audit("group.mute.del" if unmute else "group.mute.set",
                            ctx, target=oid,
                            detail=("解除禁言" if unmute else f"禁言 {human_duration(seconds)}"))
        await bot.storage.save()
        head = ("🔊 已解除禁言：" if unmute
                else f"🔇 已禁言 {human_duration(seconds)}：")
        tail = ""
        if not unmute:
            expire = rfc3339(seconds)
            tail = f"\n到期时间：{short_time(expire)}{note}"
        if len(targets) > BATCH_MAX:
            tail += f"\n（单次最多 {BATCH_MAX} 人，其余请再发一次）"
        await router.reply(ctx, head + _names(targets) + tail)

    @router.command("禁言", aliases=("mute", "设置禁言"), perm="group.mute",
                    usage="禁言 <#编号|昵称|QQ号> [时长] ｜ 禁言 3 10分 ｜ 禁言 甲 乙 1小时",
                    help="禁言群成员（默认 10 分钟，最长 30 天；只能对普通成员生效）")
    async def mute_cmd(ctx, args):
        if not await _need_group(ctx, "禁言"):
            return
        if not await _require_admin(ctx, "禁言成员"):
            return
        await _bot_state(ctx)      # 拿机器人自己的 openid，别把自己当目标
        text, seconds, note = _split_duration(args)
        targets, tip = _resolve(ctx, text)
        if tip:
            return await router.reply(ctx, tip)
        if not targets:
            return await router.reply(
                ctx, "用法：禁言 <#编号|昵称|QQ号> [时长]\n"
                     "时长：30秒 / 10分 / 1小时 / 2天 / 永久（=30 天），纯数字按分钟\n"
                     "注意：只能禁言普通成员（群主、管理员、机器人不行）。\n\n" + NO_TARGET_TIP)
        deny = _guard(ctx, targets, "禁言")
        if deny:
            return await router.reply(ctx, deny)
        await _apply_mute(ctx, targets, seconds=seconds, note=note)

    @router.command("解除禁言", aliases=("unmute", "取消禁言"), perm="group.mute",
                    usage="解除禁言 <#编号|昵称|QQ号> […]", help="解除群成员禁言")
    async def unmute_cmd(ctx, args):
        if not await _need_group(ctx, "解除禁言"):
            return
        if not await _require_admin(ctx, "解除禁言"):
            return
        text, _seconds, _note = _split_duration(args)
        targets, tip = _resolve(ctx, text)
        if tip:
            return await router.reply(ctx, tip)
        if not targets:
            return await router.reply(ctx, "用法：解除禁言 <#编号|昵称|QQ号> […]")
        await _apply_mute(ctx, targets, seconds=0, note="", unmute=True)

    @router.command("禁言列表", aliases=("禁言状态", "查询禁言"), perm="group.mute",
                    help="查询群禁言状态（全员禁言模式 + 被禁言成员）")
    async def mute_list_cmd(ctx, args):
        if not await _need_group(ctx, "禁言查询"):
            return
        if not await _require_admin(ctx, "查询禁言状态"):
            return
        try:
            data = await bot.api.get_restrict_chat_setting(ctx.group_openid)
        except Exception as e:
            return await router.reply(ctx, f"❌ 查询失败：{e}{await _admin_hint(ctx)}")
        data = data if isinstance(data, dict) else {}
        rule = data.get("global_rule") or {}
        mode = rule.get("mode") or "none"
        mode_text = {"none": "未开启全员禁言", "always": "全员禁言中",
                     "schedule": "定时禁言（按规则生效）"}.get(mode, mode)
        lines = [f"🔇 群禁言状态：{mode_text}"]
        sched = rule.get("schedule_rules") or []
        recur = rule.get("recurring_rules") or []
        if sched:
            lines.append(f"· 定时规则 {len(sched)} 条：")
            for r in sched[:5]:
                lines.append(f"  {short_time(r.get('start_at') or '')} → "
                             f"{short_time(r.get('end_at') or '')}"
                             f"{'' if r.get('enabled') else '（未启用）'}")
        if recur:
            lines.append(f"· 周期规则 {len(recur)} 条：")
            for r in recur[:5]:
                days = "/".join(str(d) for d in (r.get("weekdays") or []))
                lines.append(f"  周{days} {r.get('start_time')} → {r.get('end_time')}"
                             f"{'' if r.get('enabled') else '（未启用）'}")
        members = data.get("members") or []
        if members:
            lines.append("")
            lines.append(f"👤 被禁言成员（{len(members)} 人）：")
            for i, m in enumerate(members[:20], 1):
                who = m.get("username") or ident.display(m.get("member_openid") or "",
                                                         ctx.group_openid or "")
                exp = short_time(m.get("mute_expire_at") or "")
                lines.append(f"{i}. {who}" + (f"｜至 {exp}" if exp else ""))
        else:
            lines.append("当前没有被禁言的成员。")
        await router.reply(ctx, "\n".join(lines))

    # ================= 群成员名册（禁言/撤回/移除的目标来源） =================

    @router.command("群成员", aliases=("成员列表", "名册"), perm="group.mute",
                    usage="群成员 ｜ 群成员 召集",
                    help="名册（编号用于禁言/撤回/移除）；加「召集」请全群 @我 一次，把名册养起来")
    async def members_cmd(ctx, args):
        if not await _need_group(ctx, "群成员"):
            return
        rows = _roster_rows(ctx)

        # 平台不给群成员列表，只能靠「谁 @ 过我」认人 → 给管理员一个一键召集的入口，
        # 否则名册永远是空的，禁言/踢人根本没目标可选（2026-10-18 用户反馈「好难用」）。
        if (args or "").strip() in ("召集", "拉人", "招呼", "邀请"):
            return await router.reply(
                ctx,
                "📣 想让我能管到你，请在群里 @我 发一句「签到」\n"
                "（平台不提供群成员列表，只有 @ 过我的人我才认得出来）\n"
                f"现在名册里 {len(rows)} 人，都 @ 一次就能全员覆盖。")

        if not rows:
            return await router.reply(
                ctx, "名册还是空的。\n"
                     "发「群成员 召集」，我在群里喊一声，大家 @我 发个「签到」就进来了。")
        role_text = {"owner": "群主", "admin": "管理员", "member": "成员"}
        total = ""
        try:
            info = await bot.api.get_group_info(ctx.group_openid)
            if isinstance(info, dict) and info.get("group_member_num"):
                total = f"/{info['group_member_num']}"
        except Exception:
            pass
        lines = [f"👥 名册 {len(rows)}{total} 人"]
        for i, (oid, name, role, qq) in enumerate(rows[:40], 1):
            bits = [f"{i}. {name}"]
            if role and role != "member":
                bits.append(role_text.get(role, role))
            if qq:
                bits.append(qq)
            lines.append("｜".join(bits))
        if len(rows) > 40:
            lines.append(f"（还有 {len(rows) - 40} 人未显示）")
        lines.append("")
        lines.append("禁言 3 10分 · 解除禁言 3 · 移除成员 3 · 撤回 3")
        lines.append("人没在名册里？发「群成员 召集」")
        await router.reply(ctx, "\n".join(lines))

    # ================= 群成员批量移除 =================

    @router.command("移除成员", aliases=("踢人", "移出群聊"), perm="group.member",
                    usage="移除成员 <#编号|昵称|QQ号> […] [拉黑]",
                    help="批量移出群聊（单次最多 20 人；平台内邀能力，需开通白名单）")
    async def remove_members_cmd(ctx, args):
        if not await _need_group(ctx, "移除成员"):
            return
        if not await _require_admin(ctx, "移出群成员"):
            return
        blacklist = any(w in args for w in ("拉黑", "黑名单"))
        text = args.replace("拉黑", " ").replace("黑名单", " ")
        targets, tip = _resolve(ctx, text)
        if tip:
            return await router.reply(ctx, tip)
        if not targets:
            return await router.reply(
                ctx, "用法：移除成员 <#编号|昵称|QQ号> […] [拉黑]\n"
                     "（单次最多 20 人；加「拉黑」= 同时加入群黑名单）\n\n" + NO_TARGET_TIP)
        deny = _guard(ctx, targets, "移出群聊")
        if deny:
            return await router.reply(ctx, deny)
        oids = [oid for oid, _n in targets[:BATCH_MAX]]
        try:
            resp = await bot.api.batch_remove_members(
                ctx.group_openid, oids, add_to_member_blacklist=blacklist)
        except Exception as e:
            return await router.reply(ctx, f"❌ 移除失败：{e}{await _admin_hint(ctx)}")
        resp = resp if isinstance(resp, dict) else {}
        failed = resp.get("add_to_member_blacklist_fail_openids") or []
        for oid in oids:
            bot.perms.audit("group.member.remove", ctx, target=oid,
                            detail="移出群聊" + ("（并拉黑）" if blacklist else ""))
        await bot.storage.save()
        lines = [f"🚪 已移除 {len(oids)} 人：{_names(targets[:BATCH_MAX])}"]
        if blacklist:
            lines.append("同时已加入群黑名单。" if not failed
                         else f"⚠️ 有 {len(failed)} 人拉黑失败。")
        if len(targets) > BATCH_MAX:
            lines.append(f"（单次最多 {BATCH_MAX} 人，其余请再发一次）")
        await router.reply(ctx, "\n".join(lines))

    # ================= 群信息 / 机器人身份自检 =================

    @router.command("群信息", aliases=("群状态",), perm="group.manage",
                    help="群基本信息 + 机器人在本群的身份（排查禁言/撤回失败用）")
    async def group_info_cmd(ctx, args):
        if not await _need_group(ctx, "群信息"):
            return
        lines = []
        try:
            info = await bot.api.get_group_info(ctx.group_openid)
            if isinstance(info, dict) and info:
                lines.append(f"👥 群名：{info.get('group_name') or '（未知）'}")
                if info.get("group_member_num") is not None:
                    lines.append(f"人数：{info.get('group_member_num')}")
                if info.get("group_class_text"):
                    lines.append(f"分类：{info.get('group_class_text')}")
                if info.get("group_tags"):
                    lines.append("标签：" + "、".join(info.get("group_tags") or []))
                if info.get("group_finger_memo"):
                    lines.append(f"简介：{info['group_finger_memo']}")
            else:
                lines.append("👥 群信息：接口未返回数据（可能未开通该接口权限）")
        except Exception as e:
            lines.append(f"👥 群信息获取失败：{e}")
        st = await _bot_state(ctx)
        role = (st or {}).get("member_role") or ""
        role_text = {"admin": "管理员", "owner": "群主", "member": "普通成员"}.get(role, "未知")
        lines.append("")
        lines.append(f"🤖 机器人在本群：{role_text}")
        if role in ("admin", "owner"):
            lines.append("能力：可撤回成员消息 / 禁言 / 入群审批 ✅")
        elif role:
            lines.append("能力：只能撤回自己发的消息；禁言、入群审批、撤别人消息都会失败 ❌")
            lines.append("→ 请群主在「群设置 → 群成员 → 机器人」里把我设为管理员")
        if st.get("joined_at"):
            lines.append(f"入群时间：{short_time(st['joined_at'])}")
        if st.get("recv_msg_setting"):
            recv = {"all": "群内全部消息", "only_mention": "仅 @ 我",
                    "mention_and_context": "@ 我 + 上下文"}.get(
                st["recv_msg_setting"], st["recv_msg_setting"])
            lines.append(f"收消息设置：{recv}")
        if st.get("allow_proactive_msg") is not None:
            lines.append("主动推送：" + ("已开通" if st["allow_proactive_msg"] else "未开通"))
        await router.reply(ctx, "\n".join(lines))


def reset_caches(bot=None):
    """测试/运维用：清空模块级缓存（devtest 每个用例之间调用，避免互相污染）。"""
    for name in ("join_cache", "bot_state_cache", "strategy_cache"):
        cache = getattr(bot, name, None)
        if isinstance(cache, dict):
            cache.clear()
