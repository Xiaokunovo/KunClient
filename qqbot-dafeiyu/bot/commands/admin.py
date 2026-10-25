"""进阶群管：权限查询、授权管理员、警告台账、刷屏参数、入群验证。"""

from __future__ import annotations

from ..onebot import ApiError
from ..permission import LEVELS
from ..router import Context, router
from ..targets import display_name, resolve_target

CAT = "群管理"
CAT_SET = "群设置"

ROLE_LABEL = {
    "super": "主人",
    "admin": "机器人管理员",
    "group.owner": "群主",
    "group.admin": "群管",
    "member": "普通群友",
}


# ==================== 权限 ====================
@router.command(
    "权限",
    aliases=["我是谁", "权限查询"],
    perm="member",
    scope="group",
    usage="权限 [@某人]",
    desc="看看自己（或 TA）在机器人这里是什么身份",
    category=CAT,
)
async def cmd_whoami(ctx: Context):
    target = ctx.user_id
    if ctx.ev.at_ids or ctx.ev.reply_id:
        uid, _ = await resolve_target(ctx, ctx.args)
        if uid:
            target = uid

    if target == ctx.user_id:
        actor = ctx.actor
    else:
        # 别人的身份只能从配置 + 群角色推
        level = LEVELS["member"]
        src = "member"
        if target in ctx.bot.perms.superusers:
            level, src = LEVELS["super"], "super"
        elif target in ctx.bot.perms.admins:
            level, src = LEVELS["admin"], "admin"
        else:
            try:
                info = await ctx.api.get_group_member_info(ctx.group_id, target)
                role = str(info.get("role") or "member")
                level = {"owner": LEVELS["group.owner"], "admin": LEVELS["group.admin"]}.get(role, LEVELS["member"])
                src = {"owner": "group.owner", "admin": "group.admin"}.get(role, "member")
            except ApiError:
                pass
        from ..permission import Actor

        actor = Actor(target, level, src)

    name = ctx.ev.nickname if target == ctx.user_id else await display_name(ctx, target)
    lines = [
        f"{name}（{target}）",
        f"身份：{ROLE_LABEL.get(actor.source, '普通群友')}（等级 {actor.level}）",
    ]
    if target in ctx.bot.perms.superusers:
        lines.append("· 主人：全部权限，可重载配置、改人设、授权管理")
    elif target in ctx.bot.perms.admins:
        lines.append("· 机器人管理员：跨群可用群管命令")
    if actor.level >= LEVELS["group.admin"]:
        lines.append("· 可以禁言/踢人/撤回/改群设置")
    lines.append(f"· 可用命令 {len([c for c in ctx.bot.router.unique() if c.level <= actor.level and not c.hidden])} 个")
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "设管理",
    aliases=["授权管理"],
    perm="super",
    usage="设管理 @某人",
    desc="把某人设成机器人管理员（跨群可用群管命令）",
    category=CAT,
)
async def cmd_add_admin(ctx: Context):
    uid, _ = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要给谁授权？@ 一下、回复消息，或者给 QQ 号。")
        return
    if uid in ctx.bot.perms.superusers:
        await ctx.reply("他是主人，权限本来就比你高。")
        return
    admins = [int(x) for x in (ctx.cfg.get("permission.admins") or []) if str(x).isdigit()]
    if uid in admins:
        await ctx.reply("他已经是管理员了。")
        return
    admins.append(uid)
    ctx.cfg.set("permission.admins", admins, save=True)
    ctx.bot.perms.reload()
    await ctx.reply(f"已把 {await display_name(ctx, uid)} 设为机器人管理员。")


@router.command(
    "取消管理",
    aliases=["撤销管理"],
    perm="super",
    usage="取消管理 @某人",
    desc="取消某人的机器人管理员身份",
    category=CAT,
)
async def cmd_del_admin(ctx: Context):
    uid, _ = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要取消谁？")
        return
    admins = [int(x) for x in (ctx.cfg.get("permission.admins") or []) if str(x).isdigit()]
    if uid not in admins:
        await ctx.reply("他本来就不是管理员。")
        return
    admins.remove(uid)
    ctx.cfg.set("permission.admins", admins, save=True)
    ctx.bot.perms.reload()
    await ctx.reply(f"已取消 {await display_name(ctx, uid)} 的管理员身份。")


# ==================== 警告台账 ====================
@router.command(
    "警告列表",
    aliases=["警告记录"],
    perm="group.admin",
    scope="group",
    usage="警告列表",
    desc="看看本群谁被警告过、几次",
    category=CAT,
)
async def cmd_warn_list(ctx: Context):
    warns = (ctx.bot.state._get(ctx.group_id).get("warns") or {})
    rows = []
    import time as _t

    for uid, item in warns.items():
        ts = float(item.get("ts", 0))
        if _t.time() - ts > 7 * 86400:
            continue
        try:
            rows.append((int(uid), int(item.get("n", 0)), ts))
        except (TypeError, ValueError):
            continue
    if not rows:
        await ctx.reply("本群最近 7 天没有警告记录。")
        return
    rows.sort(key=lambda r: -r[1])
    names = {}
    try:
        for m in await ctx.api.get_group_member_list(ctx.group_id):
            uid = int(m.get("user_id") or 0)
            if uid:
                names[uid] = str(m.get("card") or m.get("nickname") or uid)
    except ApiError:
        pass
    threshold = int(ctx.cfg.get("group.warn_mute_threshold", 3))
    lines = [f"本群警告记录（满 {threshold} 次自动禁言）"]
    for uid, n, _ts in rows[:20]:
        lines.append(f"· {names.get(uid, uid)}  {n}/{threshold} 次")
    await ctx.reply("\n".join(lines), at=False)


# ==================== 刷屏参数 ====================
@router.command(
    "刷屏",
    aliases=["反刷屏"],
    perm="group.admin",
    scope="group",
    usage="刷屏 开|关|8秒6条",
    desc="调整本群反刷屏阈值（只改机器人自己的判断，不落盘）",
    category=CAT,
)
async def cmd_antispam(ctx: Context):
    arg = ctx.args.strip()
    enabled = bool(ctx.cfg.get("group.anti_spam.enabled", True))
    window = int(ctx.cfg.get("group.anti_spam.window", 8))
    limit = int(ctx.cfg.get("group.anti_spam.limit", 6))

    if not arg:
        await ctx.reply(
            f"本群反刷屏：{'开' if enabled else '关'}\n"
            f"规则：{window} 秒内超过 {limit} 条 → 禁言 {ctx.cfg.get('group.anti_spam.mute')} 秒\n"
            f"改：刷屏 开 / 刷屏 关 / 刷屏 8秒6条"
        )
        return

    if arg in ("开", "开启", "on"):
        ctx.cfg.set("group.anti_spam.enabled", True, save=True)
        await ctx.reply("反刷屏已开启。")
        return
    if arg in ("关", "关闭", "off"):
        ctx.cfg.set("group.anti_spam.enabled", False, save=True)
        await ctx.reply("反刷屏已关闭。")
        return

    import re

    m = re.match(r"^(\d+)\s*秒?\s*(\d+)\s*条?$", arg)
    if m:
        w, lim = int(m.group(1)), int(m.group(2))
        if not (1 <= w <= 120 and 2 <= lim <= 50):
            await ctx.reply("窗口 1~120 秒，条数 2~50，别太离谱。")
            return
        ctx.cfg.set("group.anti_spam.window", w, save=True)
        ctx.cfg.set("group.anti_spam.limit", lim, save=True)
        await ctx.reply(f"已改为：{w} 秒内超过 {lim} 条算刷屏。")
        return

    await ctx.reply("用法：刷屏 / 刷屏 开 / 刷屏 关 / 刷屏 8秒6条")


# ==================== 入群验证 ====================
@router.command(
    "入群验证",
    aliases=["入群答", "加群验证"],
    perm="group.owner",
    scope="group",
    usage="入群验证 设置 问题=答案",
    desc="设置入群问题，附言答对自动通过；答错只通知主人不拒绝",
    category=CAT_SET,
)
async def cmd_verify(ctx: Context):
    st = ctx.bot.state
    parts = ctx.args.split(None, 1)
    action = parts[0] if parts else ""
    body = parts[1].strip() if len(parts) > 1 else ""
    cur = st.get(ctx.group_id, "verify") or {}

    if not action or action in ("查看", "show"):
        if not cur:
            await ctx.reply("本群没设入群验证。\n用法：入群验证 设置 问题=答案")
            return
        await ctx.reply(
            f"入群验证：{'开' if cur.get('on', True) else '关'}\n"
            f"问题：{cur.get('question') or '（无）'}\n"
            f"答案包含：{cur.get('answer')}\n"
            f"改：入群验证 设置 问题=答案　｜　入群验证 开 / 关"
        )
        return

    if action in ("开", "开启", "on"):
        if not cur:
            await ctx.reply("先设置问题和答案：入群验证 设置 问题=答案")
            return
        cur["on"] = True
        st.set(ctx.group_id, "verify", cur)
        await ctx.reply("入群验证已开启。")
        return

    if action in ("关", "关闭", "off"):
        if cur:
            cur["on"] = False
            st.set(ctx.group_id, "verify", cur)
        await ctx.reply("入群验证已关闭。")
        return

    if action in ("设置", "set"):
        if "=" not in body:
            await ctx.reply("格式：入群验证 设置 进群暗号是什么=kunawa")
            return
        q, a = body.split("=", 1)
        q, a = q.strip(), a.strip()
        if not q or not a:
            await ctx.reply("问题和答案都不能为空。")
            return
        st.set(ctx.group_id, "verify", {"on": True, "question": q, "answer": a})
        await ctx.reply(f"已设置：问题「{q}」，答案包含「{a}」。申请人附言里带上答案就自动通过。")
        return

    await ctx.reply("用法：入群验证 / 入群验证 设置 问题=答案 / 入群验证 开 / 关")


# ==================== 入群申请处理 ====================
@router.command(
    "同意",
    aliases=["通过申请"],
    perm="super",
    usage="同意 <flag>",
    desc="同意一条入群申请（flag 在通知里）",
    category=CAT,
)
async def cmd_approve(ctx: Context):
    flag = ctx.args.strip()
    if not flag:
        await ctx.reply("用法：同意 <flag>（flag 在我发给你的入群通知里）")
        return
    try:
        await ctx.api.set_group_add_request(flag, True)
        await ctx.reply("已同意。")
    except ApiError as exc:
        await ctx.reply(f"操作失败：{exc}")


@router.command(
    "拒绝",
    aliases=["驳回申请"],
    perm="super",
    usage="拒绝 <flag> [理由]",
    desc="拒绝一条入群申请",
    category=CAT,
)
async def cmd_reject(ctx: Context):
    parts = ctx.args.strip().split(None, 1)
    if not parts:
        await ctx.reply("用法：拒绝 <flag> [理由]")
        return
    flag = parts[0]
    reason = parts[1] if len(parts) > 1 else "管理员拒绝"
    try:
        await ctx.api.set_group_add_request(flag, False, reason)
        await ctx.reply("已拒绝。")
    except ApiError as exc:
        await ctx.reply(f"操作失败：{exc}")


# ==================== 黑名单 ====================
@router.command(
    "拉黑",
    aliases=["ban", "封禁"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="拉黑 @某人 [原因]",
    desc="踢出并记入本群黑名单，以后申请入群自动拒绝",
    category=CAT,
)
async def cmd_ban(ctx: Context):
    uid, rest = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要拉黑谁？@ 一下、回复消息，或者给 QQ 号。")
        return
    if uid == ctx.api.self_id:
        await ctx.reply("我不拉黑自己。")
        return
    if uid in ctx.bot.perms.superusers:
        await ctx.reply("他是主人，拉不了。")
        return
    if ctx.is_group and uid == ctx.user_id:
        await ctx.reply("拉黑自己？不了吧。")
        return

    name = await display_name(ctx, uid)
    reason = rest.strip() or "违反群规"
    added = ctx.bot.state.add_ban(ctx.group_id, uid, reason, ctx.user_id)

    kicked = False
    try:
        await ctx.api.set_group_kick(ctx.group_id, uid, reject_add_request=True)
        kicked = True
    except ApiError as exc:
        await ctx.reply(f"已记入黑名单，但踢出失败：{exc}")
        return

    tip = f"已拉黑 {name}（{reason}）" if added else f"{name} 本来就在黑名单里"
    await ctx.reply(tip + ("，已踢出并拒绝再次加群。" if kicked else "。"))


@router.command(
    "解黑",
    aliases=["取消拉黑", "unban"],
    perm="group.admin",
    scope="group",
    usage="解黑 @某人",
    desc="从本群黑名单里移除（不会自动拉回群）",
    category=CAT,
)
async def cmd_unban(ctx: Context):
    uid, _ = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要解黑谁？")
        return
    name = await display_name(ctx, uid)
    ok = ctx.bot.state.del_ban(ctx.group_id, uid)
    if not ok and ctx.bot.global_bans.get(uid):
        await ctx.reply(f"{name} 在**全局**黑名单里，本群解不了，要主人发「全局解黑」。")
        return
    await ctx.reply(f"已把 {name} 移出本群黑名单，他重新申请就能进来了。" if ok else "他不在本群黑名单里。")


@router.command(
    "黑名单",
    aliases=["封禁列表"],
    perm="group.admin",
    scope="group",
    usage="黑名单",
    desc="看看本群和全局的黑名单",
    category=CAT,
)
async def cmd_banlist(ctx: Context):
    local = ctx.bot.state.bans(ctx.group_id)
    glob = dict(ctx.bot.global_bans.items())

    names: dict[int, str] = {}
    try:
        for m in await ctx.api.get_group_member_list(ctx.group_id):
            uid = int(m.get("user_id") or 0)
            if uid:
                names[uid] = str(m.get("card") or m.get("nickname") or uid)
    except ApiError:
        pass

    lines: list[str] = []
    if glob:
        lines.append(f"全局黑名单（{len(glob)}）：")
        for uid, info in glob[:15]:
            lines.append(f"· {uid}　{info.get('reason') or '无理由'}")
        if len(glob) > 15:
            lines.append(f"… 还有 {len(glob) - 15} 个")
    if local:
        if lines:
            lines.append("")
        lines.append(f"本群黑名单（{len(local)}）：")
        for uid, info in list(local.items())[:15]:
            try:
                iuid = int(uid)
            except ValueError:
                continue
            lines.append(f"· {names.get(iuid, uid)}（{uid}）　{info.get('reason') or '无理由'}")
        if len(local) > 15:
            lines.append(f"… 还有 {len(local) - 15} 个")

    if not lines:
        await ctx.reply("本群和全局都没有黑名单。\n用法：拉黑 @某人 原因")
        return
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "全局拉黑",
    aliases=["全局ban"],
    perm="super",
    usage="全局拉黑 @某人 [原因]",
    desc="跨所有群拉黑，所有群的入群申请都会被拒",
    category=CAT,
)
async def cmd_gban(ctx: Context):
    uid, rest = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要全局拉黑谁？")
        return
    if uid in ctx.bot.perms.superusers:
        await ctx.reply("他是主人，拉不了。")
        return
    reason = rest.strip() or "全局封禁"
    added = ctx.bot.global_bans.add(uid, reason, ctx.user_id)

    kicked = ""
    if ctx.is_group:
        try:
            await ctx.api.set_group_kick(ctx.group_id, uid, reject_add_request=True)
            kicked = "，并从本群踢出"
        except ApiError:
            pass

    name = await display_name(ctx, uid) if ctx.is_group else str(uid)
    await ctx.reply(
        (f"已全局拉黑 {name}（{reason}）{kicked}。" if added else f"{name} 已在全局黑名单里{kicked}。")
        + "\n所有群的入群申请都会被自动拒绝。"
    )


@router.command(
    "全局解黑",
    aliases=["全局unban"],
    perm="super",
    usage="全局解黑 @某人",
    desc="从全局黑名单移除",
    category=CAT,
)
async def cmd_gunban(ctx: Context):
    uid, _ = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要解黑谁？")
        return
    ok = ctx.bot.global_bans.remove(uid)
    name = await display_name(ctx, uid) if ctx.is_group else str(uid)
    await ctx.reply(f"已把 {name} 移出全局黑名单。" if ok else "他不在全局黑名单里。")
