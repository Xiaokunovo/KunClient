"""目标解析：把「@某人 / 回复 / QQ号 / 群名片 / 昵称」统一解析成 user_id。

优先级（也是线上最好用的顺序）：
  1. 消息里 @ 了谁（最准，OneBot 的 at 段带 QQ 号）
  2. 回复的那条消息的发送者
  3. 参数里的纯数字（QQ 号）
  4. 群名片 / 昵称精确匹配 → 前缀匹配（要拉一次成员列表，稍慢但只有兜底才走）
"""

from __future__ import annotations

import re

from .router import Context

DURATION_RE = re.compile(r"(\d+)\s*(秒|s|sec|分钟|分|m|min|小时|时|h|天|d)?", re.I)

UNIT_SECONDS = {
    "": 60, "秒": 1, "s": 1, "sec": 1,
    "分钟": 60, "分": 60, "m": 60, "min": 60,
    "小时": 3600, "时": 3600, "h": 3600,
    "天": 86400, "d": 86400,
}

MAX_BAN = 30 * 86400  # QQ 上限 30 天


def parse_duration(text: str, default_unit: str = "分钟") -> int | None:
    """'10分' → 600；'1小时30分' → 5400；纯数字按 default_unit。"""
    text = text.strip().replace(" ", "")
    if not text:
        return None
    total = 0
    matched = False
    for m in DURATION_RE.finditer(text):
        num, unit = m.group(1), (m.group(2) or "")
        if not num:
            continue
        unit = unit.lower()
        secs = UNIT_SECONDS.get(unit)
        if secs is None:
            secs = UNIT_SECONDS[default_unit]
        total += int(num) * secs
        matched = True
    if not matched:
        return None
    return max(0, min(total, MAX_BAN))


def fmt_duration(seconds: int) -> str:
    if seconds <= 0:
        return "0秒"
    if seconds % 86400 == 0:
        return f"{seconds // 86400}天"
    if seconds % 3600 == 0:
        return f"{seconds // 3600}小时"
    if seconds % 60 == 0:
        return f"{seconds // 60}分钟"
    return f"{seconds}秒"


async def _from_reply(ctx: Context) -> int | None:
    if not ctx.ev.reply_id:
        return None
    try:
        msg = await ctx.api.get_msg(ctx.ev.reply_id)
    except Exception:  # noqa: BLE001
        return None
    return int((msg.get("sender") or {}).get("user_id") or 0) or None


async def _from_members(ctx: Context, name: str) -> int | None:
    if not ctx.is_group or not name:
        return None
    try:
        members = await ctx.api.get_group_member_list(ctx.group_id)
    except Exception:  # noqa: BLE001
        return None
    exact, fuzzy = [], []
    for m in members:
        card = str(m.get("card") or "")
        nick = str(m.get("nickname") or "")
        uid = int(m.get("user_id") or 0)
        if not uid:
            continue
        if name in (card, nick):
            exact.append(uid)
        elif name and (name in card or name in nick):
            fuzzy.append(uid)
    pool = exact or fuzzy
    return pool[0] if len(pool) == 1 else None


async def resolve_target(ctx: Context, args: str) -> tuple[int | None, str]:
    """返回 (user_id, 去掉目标后的剩余参数)。"""
    rest = args

    # 1) @
    ats = [uid for uid in ctx.ev.at_ids if uid != ctx.ev.self_id]
    if ats:
        return ats[0], rest

    # 2) 回复
    uid = await _from_reply(ctx)
    if uid:
        return uid, rest

    # 3) 纯数字 QQ 号（5~12 位）
    m = re.search(r"(?<!\d)(\d{5,12})(?!\d)", rest)
    if m:
        rest = rest.replace(m.group(1), " ", 1)
        return int(m.group(1)), rest.strip()

    # 4) 群名片/昵称（取第一个非时长、非关键字的词）
    tokens = [t for t in re.split(r"\s+", rest.strip()) if t]
    for tok in tokens:
        if parse_duration(tok) is not None:
            continue
        uid = await _from_members(ctx, tok)
        if uid:
            return uid, rest.replace(tok, " ", 1).strip()

    return None, rest


async def display_name(ctx: Context, user_id: int) -> str:
    try:
        info = await ctx.api.get_group_member_info(ctx.group_id, user_id)
        return str(info.get("card") or info.get("nickname") or user_id)
    except Exception:  # noqa: BLE001
        return str(user_id)
