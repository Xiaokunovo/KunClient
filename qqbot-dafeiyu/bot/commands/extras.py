"""实用功能：关键词自动回复、群活跃排行、定时提醒。"""

from __future__ import annotations

import time

from .. import event as ev_mod
from ..onebot import ApiError
from ..reminders import fmt_left
from ..router import Context, router
from ..targets import display_name, fmt_duration, parse_duration

CAT_GROUP = "群设置"
CAT_MISC = "其它"


# ==================== 关键词自动回复 ====================
@router.command(
    "关键词",
    aliases=["关键字", "kw"],
    perm="group.admin",
    scope="group",
    usage="关键词 添加 词=回复",
    desc="命中关键词自动回一句",
    category=CAT_GROUP,
)
async def cmd_keyword(ctx: Context):
    parts = ctx.args.split(None, 1)
    action = parts[0] if parts else ""
    body = parts[1].strip() if len(parts) > 1 else ""

    if not action or action in ("列表", "查看", "list"):
        kws = ctx.bot.state.keywords(ctx.group_id)
        if not kws:
            await ctx.reply("本群还没配关键词。\n用法：关键词 添加 你好=你好呀 {at}")
            return
        lines = [f"本群关键词（{len(kws)}）："]
        for i, (w, r) in enumerate(sorted(kws.items(), key=lambda kv: kv[0]), 1):
            lines.append(f"{i}. {w} → {r}")
        lines.append("\n改：关键词 添加 词=新回复　删：关键词 删除 词")
        await ctx.reply("\n".join(lines), at=False)
        return

    if action in ("添加", "加", "add", "设置"):
        if "=" not in body:
            await ctx.reply("格式：关键词 添加 触发词=回复内容\n回复里可以写 {at} 来 @ 提问的人")
            return
        word, reply = body.split("=", 1)
        word, reply = word.strip(), reply.strip()
        if not word or not reply:
            await ctx.reply("触发词和回复都不能为空。")
            return
        status = ctx.bot.state.add_keyword(ctx.group_id, word, reply)
        if status == "full":
            await ctx.reply("关键词太多了（上限 50 个），先删几个。")
        elif status == "replaced":
            await ctx.reply(f"已更新「{word}」的回复。")
        else:
            await ctx.reply(f"已添加「{word}」。")
        return

    if action in ("删除", "删", "del", "移除"):
        word = body.strip()
        if not word:
            await ctx.reply("用法：关键词 删除 词")
            return
        ok = ctx.bot.state.del_keyword(ctx.group_id, word)
        await ctx.reply(f"已删除「{word}」。" if ok else "没这个词。")
        return

    await ctx.reply("用法：关键词 / 关键词 添加 词=回复 / 关键词 删除 词")


# ==================== 群活跃排行 ====================
@router.command(
    "排行",
    aliases=["排行榜", "rank"],
    perm="member",
    scope="group",
    usage="排行 [今日]",
    desc="看看本群谁最能说",
    category=CAT_GROUP,
)
async def cmd_rank(ctx: Context):
    scope = "today" if "今日" in ctx.args or "今天" in ctx.args else "total"
    rows = ctx.bot.state.top(ctx.group_id, scope, 10)
    if not rows:
        await ctx.reply("还没有统计数据，聊几句再来。" if scope == "total" else "今天还没人说话。")
        return

    # 一次拿成员列表做 uid → 群名片 的映射，别逐个查（10 次 API 太慢）
    names: dict[int, str] = {}
    try:
        for m in await ctx.api.get_group_member_list(ctx.group_id):
            uid = int(m.get("user_id") or 0)
            if uid:
                names[uid] = str(m.get("card") or m.get("nickname") or uid)
    except ApiError:
        pass

    medal = ["🥇", "🥈", "🥉"]
    lines = [f"本群{ '今日' if scope=='today' else '总' }活跃榜"]
    for i, (uid, cnt) in enumerate(rows):
        mark = medal[i] if i < 3 else f"{i + 1}."
        lines.append(f"{mark} {names.get(uid, uid)}  {cnt} 条")
    stats = ctx.bot.state.stats(ctx.group_id)
    lines.append(f"\n本群累计 {stats.get('messages', 0)} 条消息")
    await ctx.reply("\n".join(lines), at=False)


# ==================== 定时提醒 ====================
@router.command(
    "提醒",
    aliases=["remind", "定时"],
    perm="member",
    scope="group",
    usage="提醒 10分 开会",
    desc="到点 @ 你一下",
    category=CAT_MISC,
)
async def cmd_remind(ctx: Context):
    parts = ctx.args.split(None, 1)
    if len(parts) < 2:
        await ctx.reply("用法：提醒 10分 开会\n（时长支持 30秒 / 10分 / 1小时 / 1天）")
        return
    secs = parse_duration(parts[0])
    text = parts[1].strip()
    if not secs:
        await ctx.reply(f"看不懂「{parts[0]}」这个时长。试试 10分 / 1小时 / 30秒")
        return
    if secs > 30 * 86400:
        await ctx.reply("最多只能提醒 30 天以内的事。")
        return

    rid = ctx.bot.reminders.add(ctx.group_id, ctx.user_id, text, secs)
    ctx.bot.wake_reminders()
    if rid is None:
        await ctx.reply("本群待提醒太多了，先等几个到点。")
        return
    await ctx.reply(f"好，#{rid} 记下了：{fmt_duration(secs)}后提醒你「{text}」。")


@router.command(
    "提醒列表",
    aliases=["我的提醒"],
    perm="member",
    scope="group",
    usage="提醒列表",
    desc="看看还没到点的提醒",
    category=CAT_MISC,
)
async def cmd_remind_list(ctx: Context):
    items = ctx.bot.reminders.list_of(ctx.group_id, ctx.user_id)
    if not items:
        await ctx.reply("你没有待提醒的事。")
        return
    now = time.time()
    lines = ["你在这个群的提醒："]
    for i in items:
        lines.append(f"#{i['id']} 还有 {fmt_left(i['due'] - now)}：{i['text']}")
    lines.append("\n取消：取消提醒 编号")
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "取消提醒",
    aliases=["删提醒"],
    perm="member",
    scope="group",
    usage="取消提醒 3",
    desc="按编号取消提醒",
    category=CAT_MISC,
)
async def cmd_remind_cancel(ctx: Context):
    arg = ctx.args.strip().lstrip("#")
    if not arg.isdigit():
        await ctx.reply("用法：取消提醒 3（编号看「提醒列表」）")
        return
    ok = ctx.bot.reminders.cancel(ctx.group_id, ctx.user_id, int(arg))
    await ctx.reply(f"已取消 #{arg}。" if ok else "没找到这条提醒（编号不对，或者不是你的）。")
