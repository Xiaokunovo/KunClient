"""群互动：每日签到、积分、积分商店（兑换 KunClient 会员，数据存本地 data.json）。

积分/排行以**真实 QQ 号**（uid）为键，同一个人换群也认得出同一份积分；
未绑定QQ 时先用占位键，绑定后自动认领（见 core/identity.py）。
"""
import logging
import random
import time

from core.identity import is_qq

log = logging.getLogger("qq.fun")


def _today() -> str:
    """按北京时间取当天日期（服务器多为 UTC）。"""
    return time.strftime("%Y-%m-%d", time.gmtime(time.time() + 8 * 3600))


def _yesterday() -> str:
    return time.strftime("%Y-%m-%d", time.gmtime(time.time() + 8 * 3600 - 86400))


def register(bot, router):

    @router.command("签到", aliases=("打卡",), help="每日签到得积分（连续 7 天额外 +5）")
    async def sign_in(ctx, args):
        if not ctx.is_group:
            return await router.reply(ctx, "签到请在群里使用（积分按群统计）。")
        today = _today()
        rec = bot.storage.data.setdefault("sign", {}).setdefault(ctx.chat_key, {}).setdefault(
            ctx.uid, {"total": 0, "streak": 0, "last": "", "count": 0})
        if rec["last"] == today:
            return await router.reply(
                ctx, f"今天已经签过到啦～\n连续签到 {rec['streak']} 天 ｜ 总积分 {rec['total']}")
        gain = random.randint(1, 10)
        rec["streak"] = rec["streak"] + 1 if rec["last"] == _yesterday() else 1
        bonus = 5 if rec["streak"] >= 7 else 0
        rec["total"] += gain + bonus
        rec["count"] += 1
        rec["last"] = today
        await bot.storage.save()
        text = (f"✅ 签到成功 +{gain} 分"
                + (f"（连续 {rec['streak']} 天，额外 +{bonus}）" if bonus else "")
                + f"\n连续签到 {rec['streak']} 天 ｜ 总积分 {rec['total']}")
        await router.reply(ctx, text)

    @router.command("积分", aliases=("我的积分", "查询积分"), help="查看自己的积分与排名")
    async def my_points(ctx, args):
        table = bot.storage.data.get("sign", {}).get(ctx.chat_key or "", {})
        rec = table.get(ctx.uid)
        if not rec or not rec.get("total"):
            return await router.reply(ctx, "还没有积分，发「签到」开始攒分。")
        ranked = sorted(table.items(), key=lambda kv: kv[1].get("total", 0), reverse=True)
        rank = next(i for i, (o, _) in enumerate(ranked, 1) if o == ctx.uid)
        who = bot.identity.display(ctx.uid, ctx.chat_key or "")
        await router.reply(
            ctx, f"🏅 总积分：{rec['total']}（{who} ｜ 本群第 {rank}/{len(ranked)}）\n"
                 f"连续签到：{rec['streak']} 天 ｜ 累计签到：{rec['count']} 天")

    @router.command("积分排行", aliases=("排行", "排行榜"), help="本群积分 Top10")
    async def points_rank(ctx, args):
        table = bot.storage.data.get("sign", {}).get(ctx.chat_key or "", {})
        ranked = sorted(table.items(), key=lambda kv: kv[1].get("total", 0), reverse=True)
        ranked = [(o, r) for o, r in ranked if r.get("total")]
        if not ranked:
            return await router.reply(ctx, "本群还没有人签到，发「签到」抢占第一！")
        medals = ["🥇", "🥈", "🥉"]

        lines = ["🏆 本群积分排行 Top10"]
        for i, (o, r) in enumerate(ranked[:10], 1):
            icon = medals[i - 1] if i <= 3 else f"{i}."
            mark = "（就是你）" if o == ctx.uid else ""
            lines.append(f"{icon} {bot.identity.display(o, ctx.chat_key or '')} —— "
                         f"{r['total']} 分（连续{r['streak']}天）{mark}")
        await router.reply(ctx, "\n".join(lines))

    # ---------- 积分商店：积分兑换 KunClient 会员 ----------

    def _points_rec(ctx):
        table = bot.storage.data.setdefault("sign", {}).setdefault(ctx.chat_key or "", {})
        return table.setdefault(ctx.uid, {"total": 0, "streak": 0, "last": "", "count": 0})

    def _resolve_member(ctx, word: str) -> str | None:
        """按 QQ 号或群昵称解析目标 uid（未绑定也可用 QQ 号）。"""
        if not word:
            return None
        ident = bot.identity
        cand = word.lstrip("@").strip()
        if is_qq(cand):
            return ident.uid_of_qq(cand)
        roster = bot.storage.data.get("roster", {}).get(ctx.chat_key or "", {})
        exact = [oid for oid, info in roster.items() if info.get("username") == cand]
        if exact:
            return ident.uid_of_openid(exact[0])
        sub = [oid for oid, info in roster.items() if cand and cand in (info.get("username") or "")]
        return ident.uid_of_openid(sub[0]) if len(sub) == 1 else None

    @router.command("积分商店", help="查看积分兑换规则（管理员可设置/关闭）")
    async def points_shop(ctx, args):
        rule = bot.storage.data.get("point_shop") or {}
        parts = args.split()
        if parts and parts[0] in ("设置", "开启"):
            if not bot.perms.allows(ctx, "points.manage"):
                return await router.reply(ctx, bot.perms.deny_text(ctx, "points.manage"))
            if len(parts) < 3 or not parts[1].isdigit() or not parts[2].isdigit():
                return await router.reply(
                    ctx, "用法：积分商店 设置 <所需积分> <会员天数> [svip]\n例：积分商店 设置 100 30")
            level = parts[3].lower() if len(parts) > 3 else "vip"
            if level not in ("vip", "svip"):
                level = "vip"
            bot.storage.data["point_shop"] = {"cost": int(parts[1]), "days": int(parts[2]),
                                              "level": level, "off": False}
            bot.perms._audit("point_shop.set", ctx, detail=f"{parts[1]} 分 = {parts[2]} 天 {level}")
            await bot.storage.save()
            return await router.reply(ctx, f"✅ 积分商店已开启：{parts[1]} 积分 = {parts[2]} 天 {level.upper()} 会员。")
        if parts and parts[0] == "关闭":
            if not bot.perms.allows(ctx, "points.manage"):
                return await router.reply(ctx, bot.perms.deny_text(ctx, "points.manage"))
            rule = bot.storage.data.setdefault("point_shop", {})
            rule["off"] = True
            bot.perms._audit("point_shop.off", ctx, detail="关闭积分商店")
            await bot.storage.save()
            return await router.reply(ctx, "✅ 积分商店已关闭。")
        if not rule or rule.get("off") or not rule.get("cost"):
            return await router.reply(ctx, "积分商店暂未开启（管理员：积分商店 设置 <积分> <天数> [svip]）。")
        await router.reply(
            ctx, f"🛒 积分商店：{rule['cost']} 积分 = {rule['days']} 天 {rule.get('level', 'vip').upper()} 会员\n"
                 f"发送「积分兑换」即可兑换到绑定的 KunClient 账号（积分不足会提示）。")

    @router.command("积分兑换", help="用积分兑换会员，自动到账绑定的 KunClient 账号")
    async def points_redeem(ctx, args):
        rule = bot.storage.data.get("point_shop") or {}
        if rule.get("off") or not rule.get("cost"):
            return await router.reply(ctx, "积分商店暂未开启。")
        rec_u = bot.storage.data.get("kun_users", {}).get(ctx.uid)
        if not rec_u:
            return await router.reply(ctx, "兑换需要先绑定 KunClient 账号：kun绑定邮箱 <邮箱>。")
        rec = _points_rec(ctx)
        cost, days = rule["cost"], rule.get("days", 30)
        if rec["total"] < cost:
            return await router.reply(
                ctx, f"积分不足：需要 {cost} 分，你当前 {rec['total']} 分（还差 {cost - rec['total']} 分）。\n"
                     "发「签到」攒分～")
        # 先生成卡密并兑换到账，全部成功才扣积分；任一步失败不扣分
        keys = await bot.kun.request("POST", "/api/keys",
                                     json_body={"count": 1, "days": days, "level": rule.get("level", "vip")})
        key_list = keys if isinstance(keys, list) else (keys or {}).get("keys") or []
        if not key_list:
            return await router.reply(ctx, "⚠️ 卡密生成失败，积分未扣除，请稍后再试。")
        await bot.kun.user_request(ctx.uid, rec_u, "POST", "/api/auth/redeem",
                                   json_body={"key": key_list[0].strip().upper()})
        rec["total"] -= cost
        await bot.storage.save()
        await router.reply(
            ctx, f"🎉 兑换成功！{days} 天 {rule.get('level', 'vip').upper()} 会员已到账你的账号"
                 f"「{rec_u.get('username')}」（-{cost} 积分，剩余 {rec['total']} 分）。")

    @router.command("积分调整", perm="points.manage", usage="积分调整 <QQ号|群昵称> <±分数>",
                    help="给成员加/扣积分")
    async def points_adjust(ctx, args):
        parts = args.split()
        if len(parts) < 2 or not parts[-1].lstrip("-").isdigit():
            return await router.reply(ctx, "用法：积分调整 <QQ号|群昵称> <±分数>\n例：积分调整 2089449815 +50")
        target = _resolve_member(ctx, parts[0])
        delta = int(parts[-1])
        if target is None:
            return await router.reply(
                ctx, f"没找到「{parts[0]}」（填 QQ 号，或用对方在本群的昵称——需对方 @ 过机器人）。")
        guard = bot.guard_target(ctx, target, "调整积分")
        if guard:
            return await router.reply(ctx, guard)
        table = bot.storage.data.setdefault("sign", {}).setdefault(ctx.chat_key or "", {})
        trec = table.setdefault(target, {"total": 0, "streak": 0, "last": "", "count": 0})
        trec["total"] = max(0, trec["total"] + delta)
        bot.perms._audit("points.adjust", ctx, target=target, detail=f"{delta:+d} 分")
        await bot.storage.save()
        await router.reply(ctx, f"✅ 已{'加' if delta >= 0 else '扣'} {abs(delta)} 分给 "
                                f"{bot.identity.display(target, ctx.chat_key or '')}，"
                                f"当前 {trec['total']} 分。")
