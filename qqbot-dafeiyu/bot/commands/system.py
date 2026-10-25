"""系统命令：帮助 / 状态 / 重载 / 绑定主人 / 关于 / 上下文。"""

from __future__ import annotations

import platform
import sys

from ..router import Context, router
from ..targets import resolve_target

CAT = "系统"


@router.command(
    "帮助",
    aliases=["help", "菜单", "menu", "?"],
    perm="member",
    usage="帮助 [分类]",
    desc="看看我会什么（不带参数发图片菜单）",
    category=CAT,
)
async def cmd_help(ctx: Context):
    import base64

    from ..menuimg import level_name, render_menu

    raw = ctx.args.strip()
    cats = ctx.bot.router.categories(ctx.actor.level)
    cat = _match_category(raw, cats) if raw else None

    if raw and not cat:
        await ctx.reply(f"没有「{raw}」这一类。可选：{'、'.join(cats)}")
        return

    if not cat:
        # 默认发图片菜单；字体/依赖缺失时自动退回文本
        cmds = [c for c in ctx.bot.router.unique() if not c.hidden and c.level <= ctx.actor.level]
        png = render_menu(cmds, ctx.cfg, level_name(ctx.actor.level))
        if png:
            b64 = "base64://" + base64.b64encode(png).decode()
            await ctx.bot.reply(ctx.ev, "", at=False, image=b64)
            return

    text = ctx.bot.router.help_text(ctx.actor.level, cat)
    header = f"{ctx.cfg.get('bot.name', '小小坤ovo')} · {cat or '全部命令'}\n"
    body = header + text
    if ctx.is_group:
        mode = str(ctx.cfg.get("bot.group_trigger", "smart")).lower()
        tip = {
            "at": "群里 @我 或叫我名字才会接话；",
            "prob": "群里我会随机接话，@我一定回；",
            "all": "群里随便说，我都会接；",
        }.get(mode, "群里 @我一定回，问问题我也会接；")
        body += f"\n\n（{tip}命令前缀 / 或 #）"
    await ctx.reply(body, at=False)


def _match_category(raw: str, cats: list[str]) -> str | None:
    """「群管」也能匹配到「群管理」，别让人记全名。"""
    raw = raw.strip()
    if not raw:
        return None
    if raw in cats:
        return raw
    for c in cats:
        if raw in c or c in raw:
            return c
    return None


@router.command(
    "状态",
    aliases=["status", "运行状态"],
    perm="admin",
    usage="状态",
    desc="运行状态、调用统计",
    category=CAT,
)
async def cmd_status(ctx: Context):
    bot = ctx.bot
    ds = bot.ds
    mem = bot.memory
    sessions = len(getattr(mem, "_data", {}))
    lines = [
        f"运行时长：{bot.uptime()}",
        f"连接：{'在线' if bot.api.connected else '离线'}  {ctx.cfg.get('onebot.ws_url')}",
        f"账号：{bot.api.self_id or '未获取'}",
        f"消息：{bot.stats['messages']} 条｜命令 {bot.stats['commands']} 次｜AI {bot.stats['ai']} 次｜异常 {bot.stats['errors']} 次",
        f"DeepSeek：{ds.model}｜调用 {ds.stats['calls']} 次｜消耗 {ds.stats['tokens']} tokens",
        f"上下文会话：{sessions} 个（保留 {ctx.cfg.get('chat.context_rounds')} 轮 / {ctx.cfg.get('chat.context_ttl')}s）",
        f"Python {platform.python_version()}｜{sys.platform}",
    ]
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "重载",
    aliases=["reload"],
    perm="super",
    usage="重载",
    desc="重新读取 config.json（改完配置不用重启）",
    category=CAT,
)
async def cmd_reload(ctx: Context):
    try:
        new_cfg = type(ctx.cfg).load()
    except SystemExit as exc:
        await ctx.reply(f"重载失败：{exc}")
        return
    ctx.bot.cfg._data = new_cfg._data
    ctx.bot.perms.reload()
    ctx.bot.ds.__init__(ctx.bot.cfg)
    await ctx.reply("配置已重载。")


@router.command(
    "绑定主人",
    aliases=["绑定"],
    perm="member",
    scope="private",
    usage="绑定主人",
    desc="第一次使用时把自己设成主人（仅在还没配置主人时可用）",
    category=CAT,
)
async def cmd_bind(ctx: Context):
    supers = ctx.cfg.get("permission.superusers") or []
    if supers and ctx.user_id not in [int(x) for x in supers if str(x).isdigit()]:
        await ctx.reply("已经有主人了，别抢。")
        return
    if ctx.user_id in [int(x) for x in supers if str(x).isdigit()]:
        await ctx.reply("你已经是主人了。")
        return
    supers.append(ctx.user_id)
    ctx.cfg.set("permission.superusers", supers, save=True)
    ctx.bot.perms.reload()
    await ctx.reply(f"好了，{ctx.user_id} 现在是主人。发「/帮助」看看能干啥。")


@router.command(
    "关于",
    aliases=["about", "版本"],
    perm="member",
    usage="关于",
    desc="关于我",
    category=CAT,
)
async def cmd_about(ctx: Context):
    text = (
        f"{ctx.cfg.get('bot.name', '大肥鱼')} v1.0\n"
        f"一只接在 DeepSeek 上的 QQ 小鲸鱼，兼职群管。\n"
        f"协议端：OneBot 11（NapCat）｜模型：{ctx.cfg.get('deepseek.model')}\n"
        f"咕噜咕噜~"
    )
    await ctx.reply(text, at=False)


@router.command(
    "清空上下文",
    aliases=["重置", "reset", "忘掉"],
    perm="member",
    usage="清空上下文",
    desc="忘掉当前会话的聊天记录",
    category="AI 对话",
)
async def cmd_reset(ctx: Context):
    n = ctx.bot.memory.clear(ctx.ev.session_key)
    await ctx.reply(f"忘了 {n // 2} 轮对话。重新开始吧。" if n else "本来就没记着什么。")


@router.command(
    "上下文",
    aliases=["记忆"],
    perm="member",
    usage="上下文",
    desc="看看当前会话记了几轮",
    category="AI 对话",
)
async def cmd_context(ctx: Context):
    key = ctx.ev.session_key
    turns = ctx.bot.memory.turns(key)
    limit = ctx.cfg.get("chat.context_rounds")
    await ctx.reply(f"当前会话记着 {turns}/{limit} 轮。")


@router.command(
    "模型",
    aliases=["model"],
    perm="admin",
    usage="模型 [chat|reasoner]",
    desc="查看或切换对话模型",
    category="AI 对话",
)
async def cmd_model(ctx: Context):
    arg = ctx.args.strip().lower()
    cfg = ctx.cfg
    if not arg:
        await ctx.reply(
            f"当前模型：{cfg.get('deepseek.model')}\n"
            f"深度思考模型：{cfg.get('deepseek.reasoner_model')}\n"
            f"发「模型 chat」或「模型 reasoner」切换。"
        )
        return
    if arg in ("chat", "普通", "快速"):
        cfg.set("deepseek.model", "deepseek-chat", save=True)
        ctx.bot.ds.model = "deepseek-chat"
        await ctx.reply("切回 deepseek-chat。")
    elif arg in ("reasoner", "深度", "思考"):
        cfg.set("deepseek.model", "deepseek-reasoner", save=True)
        ctx.bot.ds.model = "deepseek-reasoner"
        await ctx.reply("切到 deepseek-reasoner（会慢一些，但更会想）。")
    else:
        await ctx.reply("用法：模型 chat / 模型 reasoner")


@router.command(
    "设置",
    aliases=["配置", "settings"],
    perm="admin",
    usage="设置",
    desc="看看当前关键配置",
    category=CAT,
)
async def cmd_settings(ctx: Context):
    cfg = ctx.cfg
    trigger = {
        "at": "@ 或昵称才回",
        "all": "所有消息都回",
        "prob": f"按 {cfg.get('chat.group_reply_probability')} 概率回，@ 必回",
    }.get(str(cfg.get("bot.group_trigger", "smart")).lower(),
          f"@ 必回，提问也回，其余 {cfg.get('chat.smart_reply_probability')} 概率")
    lines = [
        f"名字：{cfg.get('bot.name')}｜前缀：{' '.join(cfg.get('bot.prefixes', []))}",
        f"群里接话：{trigger}｜说完安静 {cfg.get('chat.group_speak_cooldown')}s｜回复带 @：{'开' if cfg.get('bot.reply_with_at') else '关'}",
        f"上下文：{cfg.get('chat.context_rounds')} 轮 / {cfg.get('chat.context_ttl')}s",
        f"温度：{cfg.get('deepseek.temperature')}｜max_tokens：{cfg.get('deepseek.max_tokens')}",
        f"反刷屏：{'开' if cfg.get('group.anti_spam.enabled') else '关'}"
        f"（{cfg.get('group.anti_spam.window')}s 内 {cfg.get('group.anti_spam.limit')} 条）",
        f"警告阈值：{cfg.get('group.warn_mute_threshold')} 次 → 禁言 {cfg.get('group.warn_mute_seconds')}s",
    ]
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "echo",
    aliases=["复读"],
    perm="super",
    usage="echo 内容",
    desc="原样发回来（测连接用）",
    category=CAT,
    hidden=True,
)
async def cmd_echo(ctx: Context):
    await ctx.reply(ctx.args or "（空）", at=False)


@router.command(
    "戳",
    aliases=["戳一戳", "poke"],
    perm="member",
    scope="group",
    usage="戳 @某人",
    desc="戳一戳对方",
    category="其它",
)
async def cmd_poke(ctx: Context):
    target, _ = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("戳谁？@ 一下或者回复他。")
        return
    try:
        await ctx.api.call("group_poke", group_id=ctx.group_id, user_id=target)
    except Exception:  # noqa: BLE001
        await ctx.reply("这个协议端不支持戳一戳。")


@router.command(
    "赞",
    aliases=["点赞"],
    perm="member",
    scope="group",
    usage="赞 @某人",
    desc="给 TA 的资料卡点 10 个赞",
    category="其它",
)
async def cmd_like(ctx: Context):
    target, _ = await resolve_target(ctx, ctx.args)
    if not target:
        await ctx.reply("给谁点赞？@ 一下或者回复他。")
        return
    try:
        await ctx.api.send_like(target, 10)
        await ctx.reply("赞了 10 下。")
    except Exception:  # noqa: BLE001
        await ctx.reply("点赞失败，可能今天次数用完了。")
