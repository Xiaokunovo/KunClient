"""生成图：文字表情包 / 给图配字 / 群聊词云。"""

from __future__ import annotations

import base64

import aiohttp

from .. import wordcloud as wc
from ..memegen import parse_style, render_meme, render_on_image
from ..onebot import ApiError
from ..router import Context, router

CAT = "娱乐"


async def _reply_image(ctx: Context) -> bytes | None:
    """取「被回复的那条消息」里的图片原始字节。"""
    if not ctx.ev.reply_id:
        return None
    try:
        msg = await ctx.api.get_msg(ctx.ev.reply_id)
    except ApiError:
        return None
    for seg in (msg.get("message") or []):
        if seg.get("type") != "image":
            continue
        data = seg.get("data") or {}
        url = data.get("url") or ""
        if not url.startswith("http"):
            # 有些实现只给文件名，拼不出可下载地址就放弃
            continue
        try:
            timeout = aiohttp.ClientTimeout(total=15)
            async with aiohttp.ClientSession(timeout=timeout) as s:
                async with s.get(url) as resp:
                    if resp.status != 200:
                        return None
                    if "image" not in (resp.headers.get("Content-Type") or ""):
                        return None
                    raw = await resp.read()
            if len(raw) > 8 * 1024 * 1024:  # 8MB 以上不处理，太重
                return None
            return raw
        except Exception:  # noqa: BLE001
            return None
    return None


async def _send_png(ctx: Context, png: bytes | None, fallback: str) -> None:
    if not png:
        await ctx.reply(fallback)
        return
    b64 = "base64://" + base64.b64encode(png).decode()
    await ctx.bot.reply(ctx.ev, "", at=False, image=b64)


@router.command(
    "表情包",
    aliases=["meme", "做图"],
    perm="member",
    scope="group",
    usage="表情包 [风格] 文字",
    desc="生成文字表情包；回复一张图则把字压在图上",
    category=CAT,
)
async def cmd_meme(ctx: Context):
    args = ctx.args.strip()

    if not args:
        await ctx.reply(
            "用法：表情包 文字　或　表情包 黑 文字\n"
            "风格：white / black / yellow / red / paper / blue（也可写 白 黑 黄 红 报纸 蓝）\n"
            "换行用 | ，比如：表情包 今天|不想上班\n"
            "回复一张图片再用，会把大字压在图上。"
        )
        return

    style, text = parse_style(args)
    if not text:
        await ctx.reply("要说啥？用法：表情包 黑 你好")
        return
    if len(text) > 120:
        text = text[:120] + "…"
    text = text.replace("|", "\n")

    # 回复了图片 → 在图上加字；否则生成纯文字表情包
    raw = await _reply_image(ctx)
    if raw:
        png = render_on_image(raw, text, "bottom")
        await _send_png(ctx, png, "图处理不了（格式不支持或太大了）。")
        return

    png = render_meme(text, style)
    await _send_png(ctx, png, "生成失败（服务器缺中文字体或 Pillow）。")


@router.command(
    "词云",
    aliases=["wordcloud", "高频词"],
    perm="group.admin",
    scope="group",
    usage="词云",
    desc="把最近群聊画成词云（内存里最近 200 条）",
    category=CAT,
)
async def cmd_wordcloud(ctx: Context):
    items = ctx.bot.ledger.recent(ctx.group_id, 200)
    texts = [i.get("text") or "" for i in items]
    texts = [t for t in texts if len(t.strip()) >= 2]
    if len(texts) < 15:
        await ctx.reply("最近消息太少（不到 15 条），攒一攒再来。")
        return

    png = wc.render(texts, title=f"最近 {len(texts)} 条群聊的高频词")
    if not png:
        # 一个词都统计不出来，或者没字体
        words = wc.build(texts, 20)
        if words:
            top = "、".join(f"{w}({c})" for w, c in words[:15])
            await ctx.reply("画不出来，凑合看词频：\n" + top)
        else:
            await ctx.reply("最近没啥可统计的词。")
        return
    await _send_png(ctx, png, "生成失败。")


@router.command(
    "风格",
    aliases=["表情包风格"],
    perm="member",
    scope="group",
    usage="风格",
    desc="看看表情包有哪些配色",
    category=CAT,
)
async def cmd_styles(ctx: Context):
    pairs = {"white": "白底黑字（经典）", "black": "黑底白字", "yellow": "黄底黑字（暴漫）",
             "red": "红底黄字（警告）", "paper": "报纸风", "blue": "蓝底白字"}
    lines = ["表情包风格："]
    for k, v in pairs.items():
        lines.append(f"· {k} / {v}")
    lines.append("\n用法：表情包 黑 你好　｜　换行用 |")
    await ctx.reply("\n".join(lines), at=False)
