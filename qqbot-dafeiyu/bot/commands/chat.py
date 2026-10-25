"""AI 对话相关命令（自动聊天在 app.py，这里放显式调用的那几个）。"""

from __future__ import annotations

from ..deepseek import DeepSeekError
from ..router import Context, router

CAT = "AI 对话"


async def _ask(ctx: Context, question: str, *, reasoner: bool = False) -> None:
    if not question:
        await ctx.reply("问啥？")
        return
    if not ctx.bot.ds.ready:
        await ctx.reply("DeepSeek API Key 还没配，去 config.json 里填 deepseek.api_key。")
        return

    model = ctx.cfg.get("deepseek.reasoner_model") if reasoner else ctx.cfg.get("deepseek.model")
    messages = ctx.bot.memory.build_messages(
        ctx.ev.session_key, ctx.bot._system_prompt(ctx.ev, ctx.actor), question
    )
    try:
        async with ctx.bot._chat_sem:
            reply = await ctx.bot.ds.chat(messages, model=model)
    except DeepSeekError as exc:
        await ctx.reply(f"脑子卡住了：{exc}")
        return

    content = reply.content or "（没想出来）"
    ctx.bot.memory.append(ctx.ev.session_key, question, content)
    ctx.bot.stats["ai"] += 1
    await ctx.reply(content)


@router.command(
    "问",
    aliases=["ai", "ask"],
    perm="member",
    usage="问 你的问题",
    desc="直接问一句（不进上下文）",
    category=CAT,
)
async def cmd_ask(ctx: Context):
    await _ask(ctx, ctx.args.strip())


@router.command(
    "深度",
    aliases=["思考", "think"],
    perm="member",
    usage="深度 你的问题",
    desc="用 reasoner 模型认真想",
    category=CAT,
)
async def cmd_think(ctx: Context):
    await _ask(ctx, ctx.args.strip(), reasoner=True)


@router.command(
    "总结",
    aliases=["summary", "划重点"],
    perm="group.admin",
    scope="group",
    usage="总结 [条数]",
    desc="总结最近 N 条群消息",
    category=CAT,
)
async def cmd_summary(ctx: Context):
    if not ctx.bot.ds.ready:
        await ctx.reply("DeepSeek API Key 还没配。")
        return
    n = 30
    if ctx.args.strip().isdigit():
        n = max(5, min(int(ctx.args.strip()), 100))
    items = ctx.bot.ledger.recent(ctx.group_id, n)
    if len(items) < 5:
        await ctx.reply("最近消息太少，没啥好总结的。")
        return

    lines = []
    for it in items:
        if it["id"] == ctx.ev.message_id:
            continue
        lines.append(f"{it['uid']}: {it['text']}")
    transcript = "\n".join(lines)[-6000:]
    prompt = (
        "下面是 QQ 群的聊天记录（格式「QQ号: 内容」）。请用 3~6 条要点总结大家在聊什么，"
        "有结论/待办就单独点出来。不要复述原话，不要客套。\n\n" + transcript
    )
    try:
        async with ctx.bot._chat_sem:
            reply = await ctx.bot.ds.chat(
                [{"role": "system", "content": "你是一个擅长抓重点的群聊记录整理助手。"},
                 {"role": "user", "content": prompt}],
                max_tokens=600,
            )
    except DeepSeekError as exc:
        await ctx.reply(f"总结失败：{exc}")
        return
    await ctx.reply("【最近群聊小结】\n" + (reply.content or "（没总结出来）"), at=False)


@router.command(
    "人设",
    aliases=["system", "人格"],
    perm="super",
    usage="人设 [设置 提示词]",
    desc="查看/修改 AI 人设",
    category=CAT,
)
async def cmd_persona(ctx: Context):
    parts = ctx.args.split(None, 1)
    if not parts or parts[0] in ("查看", "show"):
        await ctx.reply("当前人设：\n" + str(ctx.cfg.get("chat.system_prompt")), at=False)
        return
    if parts[0] in ("设置", "set", "改"):
        if len(parts) < 2 or not parts[1].strip():
            await ctx.reply("用法：人设 设置 你是一只……")
            return
        ctx.cfg.set("chat.system_prompt", parts[1].strip(), save=True)
        await ctx.reply("人设已更新（立即生效，不用重启）。")
        return
    await ctx.reply("用法：人设 / 人设 设置 新的系统提示词")
