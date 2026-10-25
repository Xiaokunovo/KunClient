"""AI 内容哨兵的控制命令。"""

from __future__ import annotations

from ..router import Context, router

CAT = "系统"

LEVEL_NAME = {
    0: "正常",
    1: "轻微",
    2: "不当",
    3: "严重",
    4: "极端",
}

ACTION_NAME = {
    "ban": "封禁",
    "mute": "禁言",
    "recall": "撤回",
    "review": "仅记录",
    "none": "放行",
    "ban-failed": "封禁失败",
}


def _status_text(ctx: Context) -> str:
    g = ctx.bot.guard
    s = g.status()
    on = "开启" if s["enabled"] else "关闭"
    ready = "就绪" if s["ready"] else "未配置 Key（只会记录，不会判定）"
    st = s["stats"]
    lines = [
        f"AI 哨兵：{on}｜模型 {ready}",
        f"处置阈值：L{s['ban_level']} 封禁 · L{s['mute_level']} 禁言 · L{s['recall_level']} 撤回",
        f"把握门槛 {s['min_confidence']:.0%}｜抽查 {s['sample_rate']:.0%}｜限流 {s['max_per_minute']} 条/分",
        f"累计：送审 {st['checked']}｜命中 {st['flagged']}｜封禁 {st['banned']}｜跳过 {st['skipped']}｜出错 {st['errors']}",
    ]
    if s["sample_rate"] <= 0:
        lines.append("（当前只审「命中本地嫌疑特征」的消息，不抽样）")
    lines.append("用法：哨兵 开|关|阈值 N|抽查 X|记录|测试 文本")
    return "\n".join(lines)


async def _show_records(ctx: Context, limit: int = 10) -> None:
    g = ctx.bot.guard
    rows = g.recent(limit)
    if not rows:
        await ctx.reply("还没有判定记录。", at=False)
        return
    lines = [f"最近 {len(rows)} 条判定（共 {len(g.records)} 条）："]
    for r in rows:
        act = ACTION_NAME.get(r["action"], r["action"])
        flag = "⚠️" if r.get("error") else ""
        lines.append(
            f"· {r['time'][6:]} {r['name']}({r['uid']}) L{r['level']}"
            f" {r['category']} → {act}{flag}"
        )
        body = str(r.get("text") or "").replace("\n", " ")[:36]
        reason = str(r.get("reason") or r.get("error") or "")[:40]
        lines.append(f"  「{body}」{reason}")
    if len(lines) > 40:  # QQ 单条消息别太长
        lines = lines[:40] + ["…（更多请在控制台看）"]
    await ctx.reply("\n".join(lines), at=False)


@router.command(
    "哨兵",
    aliases=["guard", "审核"],
    perm="admin",
    usage="哨兵 [开|关|阈值|抽查|记录|测试]",
    desc="AI 内容审核，识别过分内容自动处置",
    category=CAT,
)
async def cmd_guard(ctx: Context):
    g = ctx.bot.guard
    cfg = ctx.cfg
    args = ctx.args.strip()

    if not args:
        await ctx.reply(_status_text(ctx), at=False)
        return

    head, _, rest = args.partition(" ")
    head = head.strip()
    rest = rest.strip()

    # ---- 开关 ----
    if head in ("开", "on", "启用"):
        cfg.set("guard.enabled", True, save=True)
        await ctx.reply("AI 哨兵已开启。命中嫌疑特征的消息会交给 DeepSeek 判定。")
        return
    if head in ("关", "off", "关闭"):
        cfg.set("guard.enabled", False, save=True)
        await ctx.reply("AI 哨兵已关闭，不再送审。")
        return

    # ---- 改配置（要主人）----
    if head in ("阈值", "等级", "level", "抽查", "抽样", "rate"):
        if not ctx.actor.is_super:
            await ctx.reply("这个只能主人改。")
            return
        if not rest:
            await ctx.reply(_status_text(ctx), at=False)
            return

        if head in ("阈值", "等级", "level"):
            try:
                n = int(rest)
            except ValueError:
                await ctx.reply("阈值要写数字：哨兵 阈值 4")
                return
            n = max(1, min(4, n))
            cfg.set("guard.ban_level", n, save=True)
            await ctx.reply(
                f"封禁阈值已设为 L{n}（{LEVEL_NAME.get(n, '')}）。"
                f"判到 L{n} 及以上会直接拉黑+踢出。"
            )
            return

        try:
            rate = float(rest.rstrip("%")) / (100 if rest.endswith("%") else 1)
        except ValueError:
            await ctx.reply("抽查率要写小数：哨兵 抽查 0.05（= 5%）")
            return
        rate = max(0.0, min(1.0, rate))
        cfg.set("guard.sample_rate", rate, save=True)
        await ctx.reply(
            f"抽查率已设为 {rate:.0%}。" + ("（0 = 只审命中嫌疑特征的消息）" if rate <= 0 else "")
        )
        return

    # ---- 记录 ----
    if head in ("记录", "日志", "log"):
        await _show_records(ctx)
        return

    # ---- 手动测试：只判不动手 ----
    if head in ("测试", "test", "试"):
        if not rest:
            await ctx.reply("用法：哨兵 测试 这句话有没有问题")
            return
        if not ctx.bot.ds.ready:
            await ctx.reply("还没配 DeepSeek Key，测不了。")
            return
        await ctx.reply("送去判定了，等一下…", at=False)
        v = await g.analyze(rest, hit="手动测试")
        if not v.ok:
            await ctx.reply(f"判定失败：{v.error}")
            return
        action = g.decide(v)
        await ctx.reply(
            f"判定结果：L{v.level} {LEVEL_NAME.get(v.level, '')} · {v.category}\n"
            f"理由：{v.reason}\n"
            f"把握：{v.confidence:.0%}（门槛 {g.cfg('min_confidence', 0.7):.0%}）\n"
            f"如果这是群消息，会执行：{ACTION_NAME.get(action, action)}",
            at=False,
        )
        return

    # ---- 撤销（人工翻案）----
    await ctx.reply(_status_text(ctx), at=False)


@router.command(
    "撤销",
    aliases=["翻案", "误封"],
    perm="group.admin",
    scope="group",
    usage="撤销 [@某人]",
    desc="撤销 AI 的误判：解黑 + 解禁",
    category=CAT,
)
async def cmd_revoke(ctx: Context):
    from ..targets import display_name, resolve_target

    uid, _ = await resolve_target(ctx, ctx.args)
    if not uid:
        await ctx.reply("要撤销谁？@ 一下、回复消息，或者给 QQ 号。")
        return

    name = await display_name(ctx, uid)
    did: list[str] = []

    if ctx.bot.state.is_banned(ctx.group_id, uid):
        ctx.bot.state.del_ban(ctx.group_id, uid)
        did.append("移出本群黑名单")
    if ctx.bot.global_bans.get(uid):
        ctx.bot.global_bans.remove(uid)
        did.append("移出全局黑名单")
    try:
        await ctx.api.set_group_ban(ctx.group_id, uid, 0)
        did.append("解除禁言")
    except Exception:  # noqa: BLE001
        pass

    if not did:
        await ctx.reply(f"{name} 没有在黑名单里，也没被禁言。")
        return
    await ctx.reply(f"已撤销 {name} 的处置：{'、'.join(did)}。\n他没被踢的话直接就能说话。")
