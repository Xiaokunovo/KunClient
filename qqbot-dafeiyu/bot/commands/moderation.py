"""群管理命令：禁言 / 踢人 / 撤回 / 警告 / 违禁词。"""

from __future__ import annotations

from ..onebot import ApiError
from ..permission import LEVELS
from ..router import Context, router
from ..targets import display_name, fmt_duration, parse_duration, resolve_target

CAT = "群管理"


async def _role_of(ctx: Context, user_id: int) -> str:
    try:
        info = await ctx.api.get_group_member_info(ctx.group_id, user_id)
        return str(info.get("role") or "member")
    except ApiError:
        return "member"


async def _guard_hierarchy(ctx: Context, target: int) -> str | None:
    """返回错误文案；None 表示可以动手。"""
    if target == ctx.api.self_id:
        return "我不打自己。"
    if target == ctx.user_id:
        return "对自己下手这么狠？算了，我不同意。"
    role = await _role_of(ctx, target)
    if role == "owner":
        return "对方是群主，我动不了。"
    if role == "admin" and ctx.actor.level < LEVELS["group.owner"]:
        return "对方也是管理员，同级之间不能互相操作。"
    return None


# ==================== 禁言 ====================
@router.command(
    "禁言",
    aliases=["口球", "mute"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="禁言 @某人 10分",
    desc="禁言成员，可全员禁言",
    category=CAT,
)
async def cmd_mute(ctx: Context):
    args = ctx.args.strip()
    if not args and not ctx.ev.at_ids and not ctx.ev.reply_id:
        await ctx.reply("用法：禁言 @某人 10分 / 禁言 全员 1小时 / 回复某人消息发「禁言 10分」")
        return

    # 全员禁言
    if args.startswith("全员") or args.startswith("全体") or args.startswith("全"):
        rest = args[2:] if args.startswith(("全员", "全体")) else args[1:]
        secs = parse_duration(rest) or 600
        await ctx.api.set_group_whole_ban(ctx.group_id, True)
        await ctx.reply(f"全员禁言已开启，{fmt_duration(secs)}后记得手动解（发「解禁 全员」）。")
        # QQ 的全体禁言没有自动到期，这里起个定时任务兜底
        ctx.bot._spawn(_auto_unmute_all(ctx, secs))
        return

    target, rest = await resolve_target(ctx, args)
    if not target:
        await ctx.reply("没找到要禁言的人。@ 一下、回复他的消息，或者直接给 QQ 号。")
        return
    why = await _guard_hierarchy(ctx, target)
    if why:
        await ctx.reply(why)
        return

    secs = parse_duration(rest) if rest.strip() else None
    if secs is None:
        secs = 600
    name = await display_name(ctx, target)

    if secs == 0:
        await ctx.api.set_group_ban(ctx.group_id, target, 0)
        await ctx.reply(f"已给 {name} 解禁。")
        return

    await ctx.api.set_group_ban(ctx.group_id, target, secs)
    await ctx.reply(f"已禁言 {name} {fmt_duration(secs)}。")


async def _auto_unmute_all(ctx: Context, secs: int):
    import asyncio

    await asyncio.sleep(max(1, secs))
    try:
        await ctx.api.set_group_whole_ban(ctx.group_id, False)
        await ctx.api.send_group(ctx.group_id, [{"type": "text", "data": {"text": "全员禁言时间到，已自动解除。"}}])
    except ApiError:
        pass


@router.command(
    "解禁",
    aliases=["解除禁言", "unmute"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="解禁 @某人",
    desc="解除禁言，可解全员",
    category=CAT,
)
async def cmd_unmute(ctx: Context):
    args = ctx.args.strip()
    if args.startswith("全"):
        await ctx.api.set_group_whole_ban(ctx.group_id, False)
        await ctx.reply("全员禁言已关闭。")
        return

    target, _ = await resolve_target(ctx, args)
    if not target:
        await ctx.reply("没找到人。@ 一下、回复消息，或者给 QQ 号。")
        return
    name = await display_name(ctx, target)
    await ctx.api.set_group_ban(ctx.group_id, target, 0)
    await ctx.reply(f"{name} 已解禁，可以说话了。")


# ==================== 踢人 ====================
@router.command(
    "踢人",
    aliases=["踢", "t", "kick"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="踢人 @某人 [拉黑]",
    desc="移出群聊，可拉黑",
    category=CAT,
)
async def cmd_kick(ctx: Context):
    target, rest = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("没找到要踢的人。@ 一下、回复消息，或者给 QQ 号。")
        return
    why = await _guard_hierarchy(ctx, target)
    if why:
        await ctx.reply(why)
        return
    reject = any(k in rest for k in ("拉黑", "黑名单", "拒绝"))
    name = await display_name(ctx, target)
    await ctx.api.set_group_kick(ctx.group_id, target, reject_add_request=reject)
    await ctx.reply(f"已把 {name} 请出群聊" + ("，并拉黑。" if reject else "。"))


# ==================== 撤回 ====================
@router.command(
    "撤回",
    aliases=["撤", "recall"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="撤回 [@某人] [条数]",
    desc="撤回消息 / 某人最近 N 条 / 最近 N 条",
    category=CAT,
)
async def cmd_recall(ctx: Context):
    args = ctx.args.strip()

    # 回复某条 → 只撤那一条
    if ctx.ev.reply_id and not args:
        try:
            await ctx.api.delete_msg(ctx.ev.reply_id)
            await ctx.reply("撤了。")
        except ApiError as exc:
            await ctx.reply(f"撤回失败：{exc}")
        return

    # 带 @ / 回复 / QQ号 → 只撤这个人的
    target = None
    if ctx.ev.at_ids or args:
        uid, rest = await resolve_target(ctx, args)
        if uid:
            target = uid
            args = rest

    n = 1
    if args.strip().isdigit():
        n = max(1, min(int(args.strip()), 20))

    pool = ctx.bot.ledger.recent(ctx.group_id, 200 if target else n + 1)
    if target:
        pool = [i for i in pool if i["uid"] == target]
    if not pool:
        await ctx.reply("没有可撤回的记录（我重启过的话台账会清空）。")
        return

    ok = 0
    for item in reversed(pool):
        if item["id"] == ctx.ev.message_id:
            continue
        try:
            await ctx.api.delete_msg(item["id"])
            ok += 1
        except ApiError:
            continue
        if ok >= n:
            break

    if target:
        name = await display_name(ctx, target)
        await ctx.reply(f"撤了 {name} 的 {ok} 条。" if ok else "一条都没撤掉，可能超过 2 分钟或没权限。")
    else:
        await ctx.reply(f"撤回了 {ok} 条。" if ok else "一条都没撤掉，可能超过 2 分钟或没权限。")


# ==================== 警告 ====================
@router.command(
    "警告",
    aliases=["warn"],
    perm="group.admin",
    scope="group",
    usage="警告 @某人 [原因]",
    desc=f"记一次警告，累计到阈值自动禁言",
    category=CAT,
)
async def cmd_warn(ctx: Context):
    target, rest = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("没找到人。@ 一下、回复消息，或者给 QQ 号。")
        return
    why = await _guard_hierarchy(ctx, target)
    if why:
        await ctx.reply(why)
        return
    n = ctx.bot.state.warn(ctx.group_id, target)
    name = await display_name(ctx, target)
    threshold = int(ctx.cfg.get("group.warn_mute_threshold", 3))
    reason = f"（{rest}）" if rest else ""
    if n >= threshold:
        ok, _ = await ctx.bot.perms.can_manage(ctx.api, ctx.group_id)
        if ok:
            secs = int(ctx.cfg.get("group.warn_mute_seconds", 600))
            try:
                await ctx.api.set_group_ban(ctx.group_id, target, secs)
                ctx.bot.state.clear_warn(ctx.group_id, target)
                await ctx.reply(f"{name} 警告已满 {threshold} 次{reason}，禁言 {fmt_duration(secs)}。")
                return
            except ApiError:
                pass
    await ctx.reply(f"{name} 第 {n}/{threshold} 次警告{reason}。")


@router.command(
    "清警告",
    aliases=["清除警告"],
    perm="group.admin",
    scope="group",
    usage="清警告 @某人",
    desc="把某人的警告计数清零",
    category=CAT,
)
async def cmd_clear_warn(ctx: Context):
    target, _ = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("没找到人。")
        return
    ctx.bot.state.clear_warn(ctx.group_id, target)
    await ctx.reply(f"已清空 {await display_name(ctx, target)} 的警告记录。")


# ==================== 违禁词 ====================
@router.command(
    "违禁词",
    aliases=["敏感词", "屏蔽词"],
    perm="group.admin",
    scope="group",
    usage="违禁词 添加|删除|列表",
    desc="命中自动撤回并警告",
    category=CAT,
)
async def cmd_words(ctx: Context):
    parts = ctx.arg_list()
    if not parts or parts[0] in ("列表", "list", "查看"):
        words = ctx.bot.state.words(ctx.group_id)
        if not words:
            await ctx.reply("本群还没设违禁词。用「违禁词 添加 词」加。")
            return
        shown = "、".join(words[:50])
        await ctx.reply(f"本群违禁词（{len(words)}）：{shown}" + ("…" if len(words) > 50 else ""))
        return

    action = parts[0]
    word = " ".join(parts[1:]).strip()
    if not word:
        await ctx.reply("用法：违禁词 添加 词 / 违禁词 删除 词 / 违禁词 列表")
        return
    if action in ("添加", "加", "add"):
        added = ctx.bot.state.add_word(ctx.group_id, word)
        await ctx.reply(f"已添加违禁词「{word}」。" if added else "这个词已经在列表里了。")
    elif action in ("删除", "删", "del", "移除"):
        removed = ctx.bot.state.del_word(ctx.group_id, word)
        await ctx.reply(f"已删除「{word}」。" if removed else "列表里没这个词。")
    else:
        await ctx.reply("用法：违禁词 添加 词 / 违禁词 删除 词 / 违禁词 列表")
