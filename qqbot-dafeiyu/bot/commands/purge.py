"""群清理：全体禁言快捷入口 + 清理长期不说话的成员。

⚠️ 踢人是不可逆的，而且一次踢几十个人很容易误伤（潜水的不一定是死人，
可能只是不爱说话）。所以这里的规矩是：

1. **先预览，再确认**：`/清理死人` 只列名单不踢；带「确认」才真动手；
2. **单次上限 20 人**，踢不完再发一次 —— 宁可多点几次，也别一键清群；
3. **绝不碰**群主、管理员、机器人自己；
4. 入群不满 N 天的不算死人（新人还没来得及说话）；
5. 每次踢之间 sleep 0.6s，避免触发平台频控。
"""

from __future__ import annotations

import asyncio
import re
import time

from ..onebot import ApiError
from ..router import Context, router

CAT = "群管理"

MAX_PER_RUN = 20
KICK_GAP = 0.6


# ==================== 全体禁言 ====================
@router.command(
    "全体禁言",
    aliases=["全员禁言", "全禁"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="全体禁言 [时长]",
    desc="一键全员禁言，到点自动解除",
    category=CAT,
)
async def cmd_whole_ban(ctx: Context):
    from ..targets import fmt_duration, parse_duration

    secs = parse_duration(ctx.args) if ctx.args.strip() else 600
    secs = secs or 600
    await ctx.api.set_group_whole_ban(ctx.group_id, True)
    await ctx.reply(f"全员禁言已开启，{fmt_duration(secs)}后自动解除（也可以发「全员解禁」提前解）。")
    ctx.bot._spawn(_auto_unmute(ctx, secs))


async def _auto_unmute(ctx: Context, secs: int):
    await asyncio.sleep(max(1, secs))
    try:
        await ctx.api.set_group_whole_ban(ctx.group_id, False)
        await ctx.api.send_group(ctx.group_id, [{"type": "text", "data": {"text": "全员禁言时间到，已自动解除。"}}])
    except ApiError:
        pass


@router.command(
    "全员解禁",
    aliases=["全体解禁"],
    perm="group.admin",
    scope="group",
    need_admin_bot=True,
    usage="全员解禁",
    desc="关闭全员禁言",
    category=CAT,
)
async def cmd_whole_unban(ctx: Context):
    await ctx.api.set_group_whole_ban(ctx.group_id, False)
    await ctx.reply("全员禁言已关闭，大家可以说话了。")


# ==================== 清理死人 ====================
@router.command(
    "清理死人",
    aliases=["清理潜水", "清死人", "清理死人群"],
    perm="group.owner",
    scope="group",
    need_admin_bot=True,
    usage="清理死人 [天数]",
    desc="列出 N 天没说话的成员；加「确认」才真的踢",
    category=CAT,
)
async def cmd_purge(ctx: Context):
    args = ctx.args.strip()
    do_kick = ("确认" in args) or ("确定" in args)
    m = re.search(r"(\d+)", args)
    days = max(1, min(int(m.group(1)) if m else 30, 365))

    try:
        members = await ctx.api.get_group_member_list(ctx.group_id)
    except ApiError as exc:
        await ctx.reply(f"拿不到成员列表：{exc}")
        return
    if not members:
        await ctx.reply("成员列表是空的（接口可能没开）。")
        return

    now = time.time()
    span = days * 86400
    dead: list[tuple[int, str, int]] = []  # (uid, name, 沉默天数)

    for mem in members:
        uid = int(mem.get("user_id") or 0)
        role = str(mem.get("role") or "member")
        if not uid or uid == ctx.api.self_id or role in ("owner", "admin"):
            continue
        name = str(mem.get("card") or mem.get("nickname") or uid)
        last = int(mem.get("last_sent_time") or 0)
        joined = int(mem.get("join_time") or 0)

        if last > 0:
            idle = now - last
        elif joined > 0:
            idle = now - joined   # 从没发过言，就按入群时间算
        else:
            continue              # 两个时间都没有，不敢判

        if idle >= span:
            dead.append((uid, name, int(idle // 86400)))

    if not dead:
        await ctx.reply(f"最近 {days} 天没有需要清理的成员，群里挺活跃。")
        return

    dead.sort(key=lambda x: -x[2])
    total = len(dead)
    shown = dead[:MAX_PER_RUN]

    if not do_kick:
        lines = [f"最近 {days} 天没说话：{total} 人（按沉默时长排序，只显示前 {len(shown)} 个）"]
        for uid, name, idle in shown:
            lines.append(f"· {name}（{uid}）沉默 {idle} 天")
        if total > len(shown):
            lines.append(f"… 还有 {total - len(shown)} 人")
        lines.append(f"\n确认要踢的话，发：清理死人 {days} 确认")
        lines.append(f"一次最多踢 {MAX_PER_RUN} 人，踢不完再发一次。群主/管理员不会被清。")
        await ctx.reply("\n".join(lines), at=False)
        return

    ok, fail = 0, 0
    for uid, _name, _idle in shown:
        try:
            await ctx.api.set_group_kick(ctx.group_id, uid, reject_add_request=False)
            ok += 1
        except ApiError:
            fail += 1
        await asyncio.sleep(KICK_GAP)

    left = total - ok
    msg = f"已清理 {ok} 人"
    if fail:
        msg += f"（{fail} 个失败）"
    if left > 0:
        msg += f"，还剩 {left} 人，再发「清理死人 {days} 确认」继续。"
    else:
        msg += "，清完了。"
    await ctx.reply(msg)
