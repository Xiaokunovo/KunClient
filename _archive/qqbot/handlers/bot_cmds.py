"""机器人（KunClient）自管理命令：状态 / 统计 / 日志 / 重连网关 / 菜单同步。

全部需要 bot.manage 权限（超级管理员及以上），由 Router 按 perm 节点中心化判定。
"""
import logging
import os
import time
from collections import deque

from .basic_cmds import VERSION

log = logging.getLogger("qq.bot")


def _fmt_dur(sec: float) -> str:
    sec = int(sec)
    d, sec = divmod(sec, 86400)
    h, sec = divmod(sec, 3600)
    m, s = divmod(sec, 60)
    parts = []
    if d:
        parts.append(f"{d}天")
    if h:
        parts.append(f"{h}小时")
    if m:
        parts.append(f"{m}分")
    parts.append(f"{s}秒")
    return "".join(parts)


def register(bot, router):

    @router.command("bot状态", aliases=("状态", "运行状态", "status"), perm="bot.manage",
                    help="机器人运行状态（超级管理员及以上）")
    async def bot_status(ctx, args):
        s = bot.stats
        gw = bot.gateway
        name = gw.bot_user.get("username") or "KunClient Bot"
        if bot.tokens._expire_at:
            token_text = f"剩余约 {max(0, int(bot.tokens._expire_at - time.time()) // 60)} 分钟"
        else:
            token_text = "未获取"
        lines = [
            f"🤖 {name} 运行状态",
            "━━━━━━━━━━━━",
            f"⏱ 运行时长：{_fmt_dur(time.time() - s['start'])}",
            f"🔌 网关：{'已连接' if gw.connected else '未连接'}（累计重连 {gw.reconnect_count} 次）",
            f"🔑 token：{token_text}",
            f"📦 版本：{VERSION}",
        ]
        await router.reply(ctx, "\n".join(lines))

    @router.command("bot统计", aliases=("统计",), perm="bot.manage", help="消息/命令统计（超级管理员及以上）")
    async def bot_stats(ctx, args):
        s = bot.stats
        lines = [
            "📊 运行统计（自启动以来）",
            "━━━━━━━━━━━━",
            f"收到的消息：{s['msg_recv']}",
            f"执行的命令：{s['cmd']}",
            f"发出的回复：{s['replies']} 条",
            f"拦截黑名单：{s['blocked']} 次（含全局+本群）",
            f"权限拒绝：{s.get('denied', 0)} 次",
            f"命令出错：{s['errors']} 次",
        ]
        gb = bot.identity.global_blacklist()
        if gb:
            lines.append(f"全局黑名单：{len(gb)} 个 QQ 号")
        await router.reply(ctx, "\n".join(lines))

    @router.command("日志", aliases=("最近日志",), perm="bot.manage", help="查看最近几行运行日志（超级管理员及以上）")
    async def bot_log(ctx, args):
        path = bot.config.get("log_file", "bot.log")
        if not os.path.exists(path):
            return await router.reply(ctx, "暂无日志文件。")
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            tail = list(deque(f, maxlen=5))
        text = "📜 最近日志：\n" + "\n".join(line.rstrip() for line in tail)
        await router.reply(ctx, text)

    @router.command("重连网关", aliases=("reconnect",), perm="bot.manage",
                    help="主动重连 WebSocket 网关（超级管理员及以上）")
    async def reconnect_gw(ctx, args):
        await router.reply(ctx, "🔌 已发送重连指令，网关正在重连（通常几秒内完成，会话会自动恢复）。")
        await bot.gateway.force_reconnect()

    @router.command("群列表", perm="bot.manage", help="查看机器人活跃过的群（超级管理员及以上）")
    async def group_list(ctx, args):
        groups = bot.groups
        if not groups:
            return await router.reply(ctx, "暂无群消息记录（机器人被 @ 过的群会出现在这里）。")
        ordered = sorted(groups.items(), key=lambda kv: kv[1].get("last") or 0, reverse=True)
        now = time.time()
        lines = [f"👥 活跃群（{len(ordered)} 个，按最近活跃排序）", "━━━━━━━━━━━━"]
        for i, (gid, info) in enumerate(ordered, 1):
            last = info.get("last") or 0
            ago = _fmt_dur(now - last) + "前" if last else "未知"
            lines.append(f"{i}. {gid[:12]}… ｜ {info['count']} 条 ｜ 最近：{ago}")
        await router.reply(ctx, "\n".join(lines))

    # ---------- 聊天菜单 ----------

    def _sync_line(label: str, r: dict) -> str:
        """把 sync_menu / sync_panels 的结构化结果翻成一句人话。"""
        st = (r or {}).get("status")
        if st == "updated":
            ver = (r or {}).get("version")
            cnt = (r or {}).get("count")
            extra = f"，version={ver}" if ver is not None else ""
            if cnt is not None:
                extra = f"，{cnt} 个"
            return f"✅ {label}已推送{extra}"
        if st == "unchanged":
            return f"✅ {label}与线上一致（version={(r or {}).get('version')}），无需重推"
        if st == "missing":
            return f"ℹ️ {label}：没有对应配置文件"
        if st == "invalid":
            errs = (r or {}).get("errors") or []
            return f"❌ {label}配置不合规（未推送，线上保持原样）：\n  · " + "\n  · ".join(errs[:6])
        if st == "error":
            return f"❌ {label}推送失败：{((r or {}).get('errors') or ['未知错误'])[0]}"
        return f"❌ {label}同步未完成（status={st}）"

    @router.command("菜单同步", perm="bot.manage",
                    help="把 menu.json / panels.json 同步为平台菜单与指令面板（超级管理员及以上）")
    async def menu_sync(ctx, args):
        # 复用 main.Bot 的同步逻辑：它同时兼容新旧两种 menu.json 格式，
        # 并且**先本地校验再推送**（超字符/超数量/命令名写错都当场拦住），
        # 内容与线上一致时直接跳过，不会白涨 version。
        r_menu = await bot.sync_menu()
        r_panels = await bot.sync_panels()
        lines = [_sync_line("私聊自定义菜单", r_menu),
                 _sync_line("指令面板", r_panels)]
        if not (r_menu or {}).get("ok") or not (r_panels or {}).get("ok"):
            lines.append("")
            lines.append("详细原因见 bot.log；改好 JSON 后再发一次「菜单同步」。")
        await router.reply(ctx, "\n".join(lines))

    @router.command("菜单查看", perm="bot.manage",
                    help="查看当前自定义菜单与指令面板（超级管理员及以上）")
    async def menu_view(ctx, args):
        lines = []
        try:
            data = await bot.api.get_menu()
        except Exception as e:
            data = {"error": str(e)}
        if data.get("error"):
            lines.append(f"📋 自定义菜单读取失败：{data['error']}")
        else:
            items = (data.get("menu") or {}).get("items") or []
            lines.append(f"📋 私聊自定义菜单（version={data.get('version')}，"
                         f"{len(items)}/10 项）")
            for i, it in enumerate(items, 1):
                typ = it.get("type")
                if typ == "menu":
                    subs = it.get("sub_menu_items") or []
                    lines.append(f"{i}. {it.get('name')} ▾（{len(subs)} 项）")
                    for j, sub in enumerate(subs, 1):
                        lines.append(f"   {j}) {sub.get('name')} → "
                                     f"{sub.get('send_message') or sub.get('link')}")
                else:
                    lines.append(f"{i}. {it.get('name')} → "
                                 f"{it.get('send_message') or it.get('link')}")
            lines.append("")
            lines.append("注：/v2/menu 只作用于单聊（C2C），群里看到的是下面的指令面板。")
        for scope, label in (("group", "群聊"), ("c2c", "单聊")):
            try:
                pdata = await bot.api.list_panels(scope, limit=5)
                recs = (pdata or {}).get("records") or []
                lines.append("")
                lines.append(f"🧩 {label}指令面板（{len(recs)} 个）：")
                if not recs:
                    lines.append("（无）")
                for r in recs:
                    panel = r.get("panel") or {}
                    items = panel.get("items") or []
                    adm = sum(1 for it in items if it.get("only_admin"))
                    lines.append(f"· {r.get('panel_id')}｜{panel.get('remark') or '—'}"
                                 f"｜{len(items)} 项" + (f"（{adm} 项仅管理员）" if adm else ""))
                    for it in items:
                        lines.append(f"    {it.get('name')}｜{it.get('desc') or ''}"
                                     + ("｜仅管理员" if it.get("only_admin") else ""))
            except Exception as e:
                lines.append("")
                lines.append(f"🧩 {label}指令面板读取失败：{e}")
        await router.reply(ctx, "\n".join(lines))
