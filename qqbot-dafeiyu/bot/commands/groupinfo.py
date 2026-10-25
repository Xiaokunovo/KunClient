"""群信息类命令：公告 / 名片 / 头衔 / 精华 / 成员 / 群资料 / 欢迎语。"""

from __future__ import annotations

import time

from .. import event as ev_mod
from ..onebot import ApiError
from ..router import Context, router
from ..targets import display_name, resolve_target

CAT = "群设置"
CAT_MEMBER = "群管理"


# ==================== 公告 ====================
@router.command(
    "公告",
    aliases=["群公告", "announce"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="公告 内容",
    desc="发一条群公告",
    category=CAT,
)
async def cmd_notice(ctx: Context):
    content = ctx.args.strip()
    if not content:
        await ctx.reply("用法：公告 今天八点开会")
        return
    if len(content) > 500:
        content = content[:500]
    await ctx.api.send_group_notice(ctx.group_id, content)
    await ctx.reply("公告已发布。")


# ==================== 名片 / 头衔 ====================
@router.command(
    "名片",
    aliases=["改名片", "card"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="名片 @某人 新名片",
    desc="修改群名片，留空则清除",
    category=CAT,
)
async def cmd_card(ctx: Context):
    target, rest = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("没找到人。@ 一下、回复消息，或者给 QQ 号。")
        return
    card = rest.strip()[:30]
    await ctx.api.set_group_card(ctx.group_id, target, card)
    name = await display_name(ctx, target)
    await ctx.reply(f"已把 {name} 的群名片改成「{card}」。" if card else f"已清除 {name} 的群名片。")


@router.command(
    "头衔",
    aliases=["称号", "title"],
    perm="group.owner",
    scope="group",
    need_admin_bot=True,
    usage="头衔 @某人 头衔名",
    desc="设置群头衔（群主）",
    category=CAT,
)
async def cmd_title(ctx: Context):
    target, rest = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("没找到人。@ 一下、回复消息，或者给 QQ 号。")
        return
    title = rest.strip()[:18]
    await ctx.api.set_group_special_title(ctx.group_id, target, title)
    await ctx.reply(f"已设置头衔「{title}」。" if title else "已清除头衔。")


# ==================== 精华 ====================
@router.command(
    "精华",
    aliases=["设精", "essence"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="精华（回复一条消息）",
    desc="把回复的消息设为精华",
    category=CAT,
)
async def cmd_essence(ctx: Context):
    mid = ctx.ev.reply_id
    if not mid:
        await ctx.reply("先回复要设精的那条消息，再发「精华」。")
        return
    await ctx.api.set_essence_msg(mid)
    await ctx.reply("已设为群精华。")


# ==================== 成员 / 群资料 ====================
@router.command(
    "群成员",
    aliases=["成员列表", "members"],
    perm="group.admin",
    scope="group",
    usage="群成员 [页]",
    desc="列出群成员",
    category=CAT,
)
async def cmd_members(ctx: Context):
    page = 1
    if ctx.args.strip().isdigit():
        page = max(1, int(ctx.args.strip()))
    try:
        members = await ctx.api.get_group_member_list(ctx.group_id)
    except ApiError as exc:
        await ctx.reply(f"拿不到成员列表：{exc}")
        return
    if not members:
        await ctx.reply("成员列表是空的（可能是接口没开）。")
        return
    members.sort(key=lambda m: ({"owner": 0, "admin": 1}.get(str(m.get("role")), 2), -int(m.get("last_sent_time") or 0)))
    per = 20
    total_page = (len(members) + per - 1) // per
    page = min(page, total_page)
    chunk = members[(page - 1) * per: page * per]
    lines = [f"群成员 {len(members)} 人（第 {page}/{total_page} 页）"]
    for m in chunk:
        role = {"owner": "群主", "admin": "管理"}.get(str(m.get("role")), "　　")
        name = m.get("card") or m.get("nickname") or ""
        lines.append(f"{role} {m.get('user_id')} {name}")
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "群信息",
    aliases=["群资料", "ginfo"],
    perm="member",
    scope="group",
    usage="群信息",
    desc="看看本群的基本资料",
    category=CAT,
)
async def cmd_ginfo(ctx: Context):
    try:
        info = await ctx.api.get_group_info(ctx.group_id)
    except ApiError as exc:
        await ctx.reply(f"查不到群信息：{exc}")
        return
    stats = ctx.bot.state.stats(ctx.group_id)
    lines = [
        f"群名：{info.get('group_name', '未知')}",
        f"群号：{ctx.group_id}",
        f"人数：{info.get('member_count', '?')} / 上限 {info.get('max_member_count', '?')}",
        f"我收到的消息：{stats.get('messages', 0)} 条",
    ]
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "改群名",
    aliases=["群名"],
    perm="group.owner",
    scope="group",
    need_admin_bot=True,
    usage="改群名 新群名",
    desc="修改群名称（群主）",
    category=CAT,
)
async def cmd_rename(ctx: Context):
    name = ctx.args.strip()
    if not name:
        await ctx.reply("用法：改群名 新群名")
        return
    await ctx.api.set_group_name(ctx.group_id, name[:30])
    await ctx.reply(f"群名已改为「{name[:30]}」。")


# ==================== 欢迎语 ====================
@router.command(
    "欢迎语",
    aliases=["入群欢迎"],
    perm="group.admin",
    scope="group",
    usage="欢迎语 设置|开关|查看",
    desc="入群欢迎语（支持占位符）",
    category=CAT,
)
async def cmd_welcome(ctx: Context):
    parts = ctx.arg_list()
    default = ctx.cfg.get("group.welcome_text", "")
    if not parts or parts[0] in ("查看", "show"):
        on = ctx.bot.state.welcome_on(ctx.group_id, ctx.cfg.get("group.welcome", True))
        text = ctx.bot.state.welcome_text(ctx.group_id, default)
        await ctx.reply(f"入群欢迎：{'开' if on else '关'}\n文案：{text}", at=False)
        return

    action = parts[0]
    if action in ("开", "开启", "on"):
        ctx.bot.state.set(ctx.group_id, "welcome", True)
        await ctx.reply("入群欢迎已开启。")
    elif action in ("关", "关闭", "off"):
        ctx.bot.state.set(ctx.group_id, "welcome", False)
        await ctx.reply("入群欢迎已关闭。")
    elif action in ("设置", "set", "改"):
        text = ctx.args.split(None, 1)[1].strip() if len(parts) > 1 else ""
        if not text:
            await ctx.reply("用法：欢迎语 设置 欢迎 {at} 加入本群~")
            return
        ctx.bot.state.set(ctx.group_id, "welcome_text", text[:300])
        await ctx.reply("欢迎语已更新。可用占位符：{at} {name} {group}")
    else:
        await ctx.reply("用法：欢迎语 设置 文案 / 开关 / 查看")


# ==================== @全体 ====================
@router.command(
    "@全体",
    aliases=["全体成员", "atall"],
    perm="group.admin",
    scope="group",
    usage="@全体 [内容]",
    desc="@ 全体成员，别乱用",
    category=CAT,
)
async def cmd_atall(ctx: Context):
    content = ctx.args.strip()
    segments = [{"type": "at", "data": {"qq": "all"}}]
    if content:
        segments.append(ev_mod.text(" " + content))
    await ctx.api.send_group(ctx.group_id, segments)
