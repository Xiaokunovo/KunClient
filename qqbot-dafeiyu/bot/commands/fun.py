"""娱乐向命令：今日运势、签到积分、骰子、猜拳、群友印象。"""

from __future__ import annotations

import base64
import random

from ..fortunecard import draw_fortune
from ..onebot import ApiError
from ..router import Context, router
from ..targets import display_name, resolve_target

CAT = "娱乐"

RPS = {"石头": "剪刀", "剪刀": "布", "布": "石头"}
RPS_EMOJI = {"石头": "✊", "剪刀": "✌️", "布": "✋"}
RPS_ALIAS = {"1": "石头", "2": "剪刀", "3": "布", "r": "石头", "s": "剪刀", "p": "布",
             "rock": "石头", "scissors": "剪刀", "paper": "布"}

TAGS = [
    "群里的定海神针", "摸鱼冠军", "深夜出没", "表情包批发商", "话题终结者", "冷场王",
    "嘴替", "人间清醒", "乐子人", "潜水艇", "复读机", "键盘侠（褒义）", "气氛组组长",
    "工具人", "卷王", "养生大师", "抽象艺术家", "暴躁老哥", "温柔一刀", "熬夜冠军",
    "代码打印机", "报点机器", "气氛破坏者", "人间蒸发", "热心市民", "赛博菩萨",
]
TAG_COMMENTS = [
    "总之挺有存在感的。", "群里没你不行。", "存在即合理。", "不多说，懂的都懂。",
    "建议保持。", "有点东西。", "大家都这么觉得。", "你开心就好。",
]


# ==================== 今日运势 ====================
@router.command(
    "运势",
    aliases=["今日运势", "fortune"],
    perm="member",
    usage="运势 [@某人]",
    desc="抽一张今日运势卡（每天固定）",
    category=CAT,
)
async def cmd_fortune(ctx: Context):
    target = ctx.user_id
    if ctx.ev.at_ids or ctx.ev.reply_id or ctx.args.strip():
        uid, _ = await resolve_target(ctx, ctx.args)
        if uid:
            target = uid

    name = await display_name(ctx, target) if target != ctx.user_id else ctx.ev.nickname
    png = draw_fortune(target, name, str(ctx.cfg.get("bot.name", "")))
    if not png:
        await ctx.reply("画不出来（服务器缺中文字体或 Pillow），先凑合看看：今天宜摸鱼，忌熬夜。")
        return
    b64 = "base64://" + base64.b64encode(png).decode()
    await ctx.bot.reply(ctx.ev, "", at=False, image=b64)


# ==================== 签到 / 积分 ====================
@router.command(
    "签到",
    aliases=["打卡", "checkin"],
    perm="member",
    scope="group",
    usage="签到",
    desc="每天一次，连续签到加成",
    category=CAT,
)
async def cmd_checkin(ctx: Context):
    r = ctx.bot.state.checkin(ctx.group_id, ctx.user_id)
    if r["already"]:
        await ctx.reply(f"今天已经签过了，积分 {r['points']}，连续 {r['streak']} 天。明天再来。")
        return
    extra = f"，连续 {r['streak']} 天" if r["streak"] > 1 else ""
    await ctx.reply(f"签到成功 +{r['gained']} 分{extra}，当前 {r['points']} 分。")


@router.command(
    "积分",
    aliases=["我的积分", "points"],
    perm="member",
    scope="group",
    usage="积分 [@某人]",
    desc="看看积分和连续签到天数",
    category=CAT,
)
async def cmd_points(ctx: Context):
    target = ctx.user_id
    if ctx.ev.at_ids or ctx.ev.reply_id:
        uid, _ = await resolve_target(ctx, ctx.args)
        if uid:
            target = uid
    pts = ctx.bot.state.points_of(ctx.group_id, target)
    streak = ctx.bot.state.streak_of(ctx.group_id, target)
    name = await display_name(ctx, target) if target != ctx.user_id else ctx.ev.nickname
    await ctx.reply(f"{name}：{pts} 分" + (f"，连续签到 {streak} 天。" if streak else "。"), at=False)


@router.command(
    "积分榜",
    aliases=["积分排行", "富婆榜"],
    perm="member",
    scope="group",
    usage="积分榜",
    desc="本群积分前 10",
    category=CAT,
)
async def cmd_points_top(ctx: Context):
    rows = ctx.bot.state.points_top(ctx.group_id, 10)
    if not rows:
        await ctx.reply("还没人签到，第一名虚位以待。")
        return
    names: dict[int, str] = {}
    try:
        for m in await ctx.api.get_group_member_list(ctx.group_id):
            uid = int(m.get("user_id") or 0)
            if uid:
                names[uid] = str(m.get("card") or m.get("nickname") or uid)
    except ApiError:
        pass
    medal = ["🥇", "🥈", "🥉"]
    lines = ["本群积分榜"]
    for i, (uid, pts) in enumerate(rows):
        mark = medal[i] if i < 3 else f"{i + 1}."
        lines.append(f"{mark} {names.get(uid, uid)}  {pts} 分")
    await ctx.reply("\n".join(lines), at=False)


# ==================== 骰子 ====================
@router.command(
    "骰子",
    aliases=["roll", "dice"],
    perm="member",
    scope="group",
    usage="骰子 [面数|@某人]",
    desc="掷骰子，@ 人则对战比大小",
    category=CAT,
)
async def cmd_dice(ctx: Context):
    arg = ctx.args.strip()
    faces = 6
    if arg.isdigit():
        faces = max(2, min(int(arg), 100))

    # 对战模式
    if ctx.ev.at_ids or ctx.ev.reply_id:
        uid, _ = await resolve_target(ctx, arg)
        if uid and uid != ctx.user_id:
            a, b = random.randint(1, faces), random.randint(1, faces)
            me = ctx.ev.nickname
            you = await display_name(ctx, uid)
            if a > b:
                res = f"我赢了，{me} 运气不错。"
            elif a < b:
                res = f"{you} 赢了，服气。"
            else:
                res = "平局，再来一局？"
            await ctx.reply(f"{me} 掷出 {a}，{you} 掷出 {b}。{res}", at=False)
            return

    await ctx.reply(f"🎲 {random.randint(1, faces)}（1~{faces}）")


# ==================== 猜拳 ====================
@router.command(
    "猜拳",
    aliases=["剪刀石头布", "rps"],
    perm="member",
    scope="group",
    usage="猜拳 石头",
    desc="跟我猜拳：石头 / 剪刀 / 布",
    category=CAT,
)
async def cmd_rps(ctx: Context):
    raw = ctx.args.strip().lower()
    mine = RPS_ALIAS.get(raw, raw if raw in RPS else "")
    if not mine:
        await ctx.reply("出什么？石头 / 剪刀 / 布")
        return
    bot_choice = random.choice(list(RPS))
    if mine == bot_choice:
        result = "平局。"
    elif RPS[mine] == bot_choice:
        result = "你赢了……"
    else:
        result = "我赢了，咕。"
    await ctx.reply(
        f"你 {RPS_EMOJI[mine]} {mine}，我 {RPS_EMOJI[bot_choice]} {bot_choice}。{result}", at=False
    )


# ==================== 群友印象 ====================
@router.command(
    "印象",
    aliases=["评价", "打分"],
    perm="member",
    scope="group",
    usage="印象 [@某人]",
    desc="随机给 TA 贴几个标签（纯属瞎编）",
    category=CAT,
)
async def cmd_impression(ctx: Context):
    target = ctx.user_id
    if ctx.ev.at_ids or ctx.ev.reply_id or ctx.args.strip():
        uid, _ = await resolve_target(ctx, ctx.args)
        if uid:
            target = uid
    name = await display_name(ctx, target) if target != ctx.user_id else ctx.ev.nickname
    tags = random.sample(TAGS, 2)
    await ctx.reply(f"{name}：{tags[0]} / {tags[1]}。{random.choice(TAG_COMMENTS)}", at=False)


# ==================== 假封禁（娱乐） ====================
@router.command(
    "fban",
    aliases=["假封禁", "fakeban"],
    perm="member",
    scope="group",
    usage="fban [@某人] [天数] [原因]",
    desc="生成一张「账号已被封禁」的假截图（纯娱乐，不会真封）",
    category=CAT,
)
async def cmd_fake_ban(ctx: Context):
    import re as _re

    from ..fakeban import render_fake_ban

    target = ctx.user_id
    if ctx.ev.at_ids or ctx.ev.reply_id:
        uid, _ = await resolve_target(ctx, ctx.args)
        if uid:
            target = uid

    args = ctx.args
    m = _re.search(r"(\d+)\s*天?", args)
    days = max(1, min(int(m.group(1)) if m else 3, 3650))
    reason = args
    for chunk in (str(target), f"{days}天", f"{days} 天", str(days)):
        reason = reason.replace(chunk, " ")
    reason = " ".join(reason.split()) or "违反群规"

    name = await display_name(ctx, target) if target != ctx.user_id else ctx.ev.nickname
    png = render_fake_ban(target, name, days, reason, str(ctx.cfg.get("bot.name", "")))
    if not png:
        await ctx.reply("画不出来（服务器缺中文字体或 Pillow）。就当封了吧，{} 天。".format(days))
        return

    b64 = "base64://" + base64.b64encode(png).decode()
    # 先 @ 一下当事人，再发图
    await ctx.bot.reply(ctx.ev, f"{name} 已被封禁 {days} 天，请文明发言。", at=False, image=b64)
