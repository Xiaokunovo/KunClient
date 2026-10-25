"""群管理命令：关键词自动回复、黑名单、管理员、欢迎语、平台能力说明。

身份体系：所有目标用户都用**真实 QQ 号**（uid）表示，见 core/identity.py。
- 「绑定QQ」一人一号、锁定不可改绑，防冒领、防绕过拉黑；
- 「全局拉黑」按 QQ 号生效，跨群跨私聊，优先级高于本群黑名单；对方没绑 QQ 号时
  先用 identity_ban（openid 兜底拦截）封住已知身份，绑定/登记后自动升级为按号封；
- 「拉黑」是本群黑名单，同样以 QQ 号为准。

权限体系（v3 重做，见 core/permissions.py）：
- 每条命令在 @router.command 上声明 perm 节点，由 Router 中心化判定，
  命令内部不再自查 is_super / is_admin；
- 四个角色：owner(100) > super(80) > group_admin(40) > user(0)；
- 平台群主/管理员身份**默认不再自动提权**（config.permissions.trust_platform_roles
  可开启），授权一律显式持久化到 data.json roles 表；
- 所有针对人的操作先过 bot.guard_target：不能对主人、不能对权限>=自己的成员执行；
- 授权/封禁/解绑全部落 audit 审计缓冲（「审计日志」查看）。

说明：群禁言 / 踢人 / 入群审批 / 撤回消息这些**群聊管理接口**官方已于 2026-08-10
与 09-03 两批开放，实现集中在 handlers/group_cmds.py；本文件只管黑名单与角色授权。
"""
import logging
import re

from core.identity import is_openid, is_qq
from .router import MENTION_RE

log = logging.getLogger("qq.admin")

DEFAULT_WELCOME = (
    "大家好，我是 KunClient 机器人 🤖\n"
    "我的账号：@我 发送「kun我」\n"
    "全部功能：@我 发送「帮助」"
)


def register(bot, router):

    # ---------- 关键词自动回复 ----------

    @router.command("关键词", aliases=("kw",), perm="group.manage",
                    usage="关键词 添加 触发词 = 回复 ｜ 删除 触发词 ｜ 列表 ｜ 清空")
    async def keyword_cmd(ctx, args):
        if not ctx.group_openid:
            return await router.reply(ctx, "关键词回复请在群里设置。")
        parts = args.split(None, 1)
        sub = parts[0] if parts else "列表"
        rest = parts[1].strip() if len(parts) > 1 else ""
        kws = bot.storage.group_section("keywords", ctx.group_openid)

        if sub in ("添加", "新增", "add"):
            sep = "=>" if "=>" in rest else ("=" if "=" in rest else None)
            if sep is None:
                return await router.reply(ctx, "格式：关键词 添加 触发词 = 回复内容\n例：关键词 添加 签到 = 签到成功！")
            trigger, reply_text = rest.split(sep, 1)
            trigger, reply_text = trigger.strip(), reply_text.strip()
            if not trigger or not reply_text:
                return await router.reply(ctx, "触发词和回复内容都不能为空。")
            kws[:] = [k for k in kws if k.get("t") != trigger]  # 同名覆盖
            kws.append({"t": trigger, "r": reply_text})
            await bot.storage.save()
            await router.reply(ctx, f"✅ 已添加关键词「{trigger}」，本群共 {len(kws)} 条。")
        elif sub in ("删除", "del", "remove"):
            trigger = rest.strip()
            remain = [k for k in kws if k.get("t") != trigger]
            if len(remain) == len(kws):
                return await router.reply(ctx, f"没有找到关键词「{trigger}」。")
            kws[:] = remain
            await bot.storage.save()
            await router.reply(ctx, f"✅ 已删除「{trigger}」，剩余 {len(remain)} 条。")
        elif sub == "清空":
            kws.clear()
            await bot.storage.save()
            await router.reply(ctx, "✅ 已清空本群关键词。")
        elif sub in ("列表", "list", ""):
            if not kws:
                return await router.reply(ctx, "本群还没有关键词，添加：关键词 添加 触发词 => 回复内容")
            lines = [f"📋 本群关键词（{len(kws)} 条）："]
            lines += [f"{i}. {k['t']} => {k['r']}" for i, k in enumerate(kws, 1)]
            await router.reply(ctx, "\n".join(lines))
        else:
            await router.reply(ctx, f"用法：{router.commands['关键词'].usage}")

    # ---------- 黑名单（本群 + 全局） ----------

    NO_TARGET_TIP = ("没找到这个人。三种写法都行：\n"
                     "① 拉黑 @某人 —— 直接在群里 @ 他\n"
                     "② 拉黑 昵称 —— 他 @ 过机器人就有记录\n"
                     "③ 拉黑 QQ号 —— 你知道号就直接填")

    # ---------- 目标解析：管理员不需要知道 QQ 号 ----------

    def _nickname_candidates(ctx, args):
        """群名册里昵称匹配的候选人，返回 [(uid, 昵称)]。"""
        ident = bot.identity
        roster = bot.storage.data.get("roster", {}).get(ctx.group_openid or "", {})
        cand = re.sub(r"^\s*@\s*", "", args.strip())
        if not cand:
            return []
        exact = [(ident.uid_of_openid(oid), info.get("username") or cand)
                 for oid, info in roster.items() if info.get("username") == cand]
        if exact:
            return exact
        return [(ident.uid_of_openid(oid), info.get("username") or "")
                for oid, info in roster.items() if cand in (info.get("username") or "")]

    def _targets(ctx, args):
        """把命令参数解析成用户主键列表 uid。

        依次尝试：事件 mentions → 文本 <@id> → 群昵称（精确/唯一子串）
        → QQ 号 → 原始 openid。QQ 号直接当 uid 存，对方日后绑定即自动命中。
        """
        ident = bot.identity
        targets: list[str] = []

        def add(uid):
            if uid and uid not in targets:
                targets.append(uid)

        for t in list(ctx.mentions) + MENTION_RE.findall(args):
            add(ident.uid_of_openid(t))
        if not targets:
            cand = re.sub(r"^\s*@\s*", "", args.strip())
            if cand:
                roster = bot.storage.data.get("roster", {}).get(ctx.group_openid or "", {})
                exact = [oid for oid, info in roster.items() if info.get("username") == cand]
                if exact:
                    add(ident.uid_of_openid(exact[0]))
                else:
                    sub = [oid for oid, info in roster.items() if cand in (info.get("username") or "")]
                    if len(sub) == 1:
                        add(ident.uid_of_openid(sub[0]))  # 唯一子串命中，如「U1」→「测试U1」
        if not targets:
            cand = args.strip()
            if is_qq(cand):
                add(ident.uid_of_qq(cand))
            elif is_openid(cand):
                add(ident.uid_of_openid(cand))
        log.debug("目标解析: mentions=%s args=%r targets=%s",
                  ctx.mentions, args, targets)
        return targets

    async def _resolve_or_tip(ctx, args):
        """解析目标；返回 (uids, 提示文案)，提示非 None 表示要直接回复用户。"""
        targets = _targets(ctx, args)
        if targets:
            return targets, None
        cands = _nickname_candidates(ctx, args)
        if len(cands) > 1:
            lines = ["匹配到多个人，用更完整的昵称或直接 @ 他："]
            lines += [f"· {n}" for _, n in cands[:10]]
            return [], "\n".join(lines)
        return [], NO_TARGET_TIP

    def _names(uids, ctx) -> str:
        return "、".join(bot.identity.display(u, ctx.group_openid) for u in uids)

    def _qqs_of(uids) -> list[str]:
        """uid -> 已登记的 QQ 号（没绑定过的拿不到）。"""
        out = []
        for u in uids:
            qq = None if bot.identity.is_openid_uid(u) else u
            if qq and qq not in out:
                out.append(qq)
        return out

    def _split_scope(args):
        """剥掉开头的 全/全局 标记，返回 (是否全局, 剩余参数)。"""
        text = (args or "").strip()
        for pfx in ("全局", "全"):
            if text == pfx:
                return True, ""
            if text.startswith(pfx + " ") or text.startswith(pfx + "　"):
                return True, text[len(pfx):].strip()
        return False, text

    async def _do_global_ban(ctx, args, reason=""):
        """全局封禁的核心逻辑：目标随便怎么写，QQ 号由机器人自己查。

        查到 QQ 号 → 按号封，跨群跨私聊立刻生效（绑定/登记后仍按号生效）；
        查不到（对方没发过 绑定QQ）→ 退而先把他的 openid 加入全局兜底拦截
        （identity_ban，所有已知身份场景生效）+ 封本群，并明确告诉管理员下一步：
        对方一旦绑定 QQ 号，拦截自动升级为按号永久生效。
        """
        targets, tip = await _resolve_or_tip(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        for t in targets:
            guard = bot.guard_target(ctx, t, "封禁")
            if guard:
                return await router.reply(ctx, guard)
        qqs = _qqs_of(targets)
        if not qqs:
            # 认得出是谁，但拿不到 QQ 号 → 平台给的是每群一个的加密 ID，
            # 拿它做不了按号封禁。先用 openid 兜底拦截 + 本群拉黑，并说清怎么补齐。
            lines = []
            bl = bot.storage.group_section("blacklist", ctx.group_openid or "")
            for t in targets:
                added_group = t not in bl
                if added_group:
                    bl.append(t)
                oid = t[len("openid:"):] if bot.identity.is_openid_uid(t) else t
                ok, msg = bot.identity.identity_block(oid, ctx.uid, reason)
                if ok:
                    bot.perms._audit("ban.identity.add", ctx, target=t,
                                     detail=f"openid 兜底全局拦截：{reason or '无原因'}")
                lines.append(("🌐 " if ok else "ℹ️ ") + msg +
                             ("；同时已拉黑本群" if added_group else ""))
            await bot.storage.save()
            return await router.reply(
                ctx, "\n".join(lines) + "\n\n"
                     "想让封禁按 QQ 号永久生效（两条路都不用你手翻聊天记录）：\n"
                     "① 你知道他的 QQ 号 → 设QQ号 <QQ号> @某人，再执行一次本命令\n"
                     "② 不知道 → 让他发一次「绑定QQ」，拦截会自动升级")
        lines = []
        for qq in qqs:
            ok, msg = bot.identity.global_block(qq, ctx.uid, reason)
            if ok:
                bot.perms._audit("ban.global.add", ctx, target=qq,
                                 detail=f"全局封禁：{reason or '无原因'}")
            cover = len(bot.identity.openids_of(qq))
            tail = f"（含 {cover} 个身份）" if cover else "（他在其他群的身份若没登记过，需「关联身份」并号）"
            lines.append(("✅ " if ok else "ℹ️ ") + msg + tail)
        await bot.storage.save()
        await router.reply(ctx, "\n".join(lines))

    async def _do_global_unban(ctx, args):
        targets, tip = await _resolve_or_tip(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        gb = bot.identity.global_blacklist()
        # 也允许直接按「显示名」解：全局黑名单里存的是号，管理员可能只记得昵称
        qqs = _qqs_of(targets) or [q for q in gb
                                   if bot.identity.display(q, ctx.group_openid) == args.strip()]
        # 也允许直接解 openid 兜底拦截（设QQ号 之前封的那批）
        oids = []
        cand = args.strip()
        if not qqs and is_openid(cand):
            oids = [cand]
        if not qqs and not oids:
            hits = [o for o in bot.identity.identity_ban_list()
                    if bot.identity.display(o, ctx.group_openid) == cand]
            oids = hits
        if not qqs and not oids:
            if gb or bot.identity.identity_ban_list():
                await router.reply(ctx, "没找到要解除的号。当前全局黑名单：\n"
                                        + "\n".join(f"· QQ {q}" for q in gb))
            else:
                await router.reply(ctx, "全局黑名单是空的。")
            return
        lines = []
        for qq in qqs:
            ok, msg = bot.identity.global_unblock(qq)
            if ok:
                bot.perms._audit("ban.global.del", ctx, target=qq, detail="解除全局封禁")
            lines.append(("✅ " + msg) if ok else ("❌ " + msg))
        for oid in oids:
            ok, msg = bot.identity.identity_unblock(oid)
            if ok:
                bot.perms._audit("ban.identity.del", ctx, target=oid, detail="解除 openid 兜底拦截")
            lines.append(("✅ " + msg) if ok else ("❌ " + msg))
        await bot.storage.save()
        await router.reply(ctx, "\n".join(lines))

    # ---------- 本群黑名单 ----------

    @router.command("拉黑", perm="group.ban", usage="拉黑 [全] @某人|昵称|QQ号",
                    help="本群拉黑；加「全」= 所有群+私聊（需全局封号权限）")
    async def blacklist_add(ctx, args):
        is_global, rest = _split_scope(args)
        if is_global:
            # 「全」= 全局封号：命令级 perm 只有 group.ban，这里按 global.ban 节点升级校验。
            # ⚠️ 子命令走更高权限路径时必须自己校验——Router 的中心化判定只覆盖命令级节点，
            # 少了这一条，群管理员就能借「拉黑 全」的入口执行全局封禁（实测踩过）。
            if not bot.perms.allows(ctx, "global.ban"):
                return await router.reply(ctx, bot.perms.deny_text(ctx, "global.ban"))
            return await _do_global_ban(ctx, rest)
        if not rest:
            return await router.reply(ctx, "用法：拉黑 @某人 ｜ 拉黑 昵称 ｜ 拉黑 QQ号\n"
                                          "（跨所有群：拉黑 全 @某人）")
        targets, tip = await _resolve_or_tip(ctx, rest)
        if tip:
            return await router.reply(ctx, tip)
        for t in targets:
            guard = bot.guard_target(ctx, t, "拉黑")
            if guard:
                return await router.reply(ctx, guard)
        bl = bot.storage.group_section("blacklist", ctx.group_openid or "")
        added = [t for t in targets if t not in bl]
        bl.extend(added)
        for t in added:
            bot.perms._audit("ban.group.add", ctx, target=t, detail="本群拉黑")
        await bot.storage.save()
        await router.reply(
            ctx, f"✅ 已在本群拉黑：{_names(added, ctx)}\n"
                 f"（要连其他群和私聊一起封：拉黑 全 @某人）")

    @router.command("取消拉黑", aliases=("解除拉黑", "unban"), perm="group.ban",
                    usage="取消拉黑 [全] @某人|昵称|QQ号", help="解除本群/全局拉黑")
    async def blacklist_del(ctx, args):
        is_global, rest = _split_scope(args)
        if is_global:
            # 同「拉黑 全」：解除全局封禁需 global.ban 节点，子命令自己校验
            if not bot.perms.allows(ctx, "global.ban"):
                return await router.reply(ctx, bot.perms.deny_text(ctx, "global.ban"))
            return await _do_global_unban(ctx, rest)
        if not rest:
            return await router.reply(ctx, "用法：取消拉黑 @某人 ｜ 取消拉黑 昵称")
        targets, tip = await _resolve_or_tip(ctx, rest)
        if tip:
            return await router.reply(ctx, tip)
        bl = bot.storage.group_section("blacklist", ctx.group_openid or "")
        removed = [t for t in targets if t in bl]
        bl[:] = [t for t in bl if t not in targets]
        for t in removed:
            bot.perms._audit("ban.group.del", ctx, target=t, detail="解除本群拉黑")
        await bot.storage.save()
        await router.reply(ctx, f"✅ 已解除本群拉黑 {len(removed)} 人：{_names(removed, ctx)}")

    @router.command("拉黑列表", perm="group.ban", help="查看本群黑名单")
    async def blacklist_list(ctx, args):
        bl = bot.storage.group_section("blacklist", ctx.group_openid or "")
        lines = []
        if bl:
            lines += [f"🚫 本群黑名单（{len(bl)} 人）："]
            lines += [f"{i}. {bot.identity.display(u, ctx.group_openid)}" for i, u in enumerate(bl, 1)]
        else:
            lines.append("本群黑名单为空。")
        gb = bot.identity.global_blacklist()
        if gb:
            lines.append("")
            lines.append(f"🌐 全局黑名单（{len(gb)} 人，所有群+私聊生效）：")
            for i, qq in enumerate(gb, 1):
                rec = gb[qq] or {}
                who = bot.identity.display(qq, ctx.group_openid)
                bits = [who] if who != f"QQ:{qq}" else [f"QQ {qq}"]
                if rec.get("reason"):
                    bits.append(rec["reason"])
                bits.append(rec.get("at", "?"))
                lines.append(f"{i}. " + "｜".join(bits))
        await router.reply(ctx, "\n".join(lines))

    # ---------- 全局黑名单短名入口（需 global.ban 权限：super 及以上） ----------

    @router.command("全局拉黑", aliases=("封号", "gban", "全局黑"), perm="global.ban",
                    usage="全局拉黑 @某人|昵称|QQ号 [原因]",
                    help="跨所有群+私聊封号（短命令：封号）")
    async def gban_add(ctx, args):
        if not args.strip():
            return await router.reply(ctx, "用法：封号 @某人 ｜ 封号 昵称 ｜ 封号 QQ号 原因")
        parts = args.split(None, 1)
        is_global, rest = _split_scope(parts[0])
        reason = parts[1].strip() if len(parts) > 1 else ""
        await _do_global_ban(ctx, rest, reason)

    @router.command("全局解黑", aliases=("解封", "gunban", "全局取消拉黑", "全局解除拉黑"),
                    perm="global.ban",
                    usage="全局解黑 <QQ号>", help="解除全局拉黑（短命令：解封）")
    async def gban_del(ctx, args):
        await _do_global_unban(ctx, args)

    @router.command("全局黑名单", aliases=("封号列表", "gblacklist"), perm="global.ban",
                    help="查看全局黑名单（短命令：封号列表）")
    async def gban_list(ctx, args):
        gb = bot.identity.global_blacklist()
        ib = bot.identity.identity_ban_list()
        if not gb and not ib:
            return await router.reply(ctx, "全局黑名单是空的。")
        lines = []
        if gb:
            lines.append(f"🌐 全局黑名单（{len(gb)} 人，所有群+私聊生效）：")
            for i, qq in enumerate(gb, 1):
                rec = gb[qq] or {}
                who = bot.identity.display(qq, ctx.group_openid)
                bits = [who if who != f"QQ:{qq}" else f"QQ {qq}"]
                if rec.get("reason"):
                    bits.append(rec["reason"])
                bits.append(rec.get("at", "?"))
                lines.append(f"{i}. " + "｜".join(bits))
        if ib:
            lines.append("")
            lines.append(f"🧷 openid 兜底拦截（{len(ib)} 个未绑定身份，绑定 QQ 后自动升级）：")
            for i, oid in enumerate(ib, 1):
                rec = ib[oid] or {}
                who = bot.identity.display(oid, ctx.group_openid)
                bits = [who]
                if rec.get("reason"):
                    bits.append(rec["reason"])
                bits.append(rec.get("at", "?"))
                lines.append(f"{i}. " + "｜".join(bits))
        await router.reply(ctx, "\n".join(lines))

    # ---------- 管理员（授权走 roles 显式角色表，全部落审计） ----------

    @router.command("设置管理", aliases=("addadmin", "群管添加", "授权管理"), perm="group.grant",
                    usage="设置管理 @某人|昵称|QQ号（超级管理员及以上）")
    async def set_admin(ctx, args):
        targets, tip = await _resolve_or_tip(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        results = [await bot.perms.grant_group_admin(ctx, t)
                   for t in targets]
        await router.reply(ctx, "\n".join(m for _, m in results))

    @router.command("移除管理", aliases=("deladmin", "群管移除", "移除群管"), perm="group.grant",
                    usage="移除管理 @某人|昵称|QQ号（超级管理员及以上）")
    async def del_admin(ctx, args):
        targets, tip = await _resolve_or_tip(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        results = [await bot.perms.revoke_group_admin(ctx, t)
                   for t in targets]
        await router.reply(ctx, "\n".join(m for _, m in results))

    @router.command("管理列表", aliases=("管理员列表",), help="查看本群管理员（公开）")
    async def admin_list(ctx, args):
        roles = (bot.storage.data.get("roles") or {}).get(ctx.group_openid or "", {})
        demoted = [u for u, r in roles.items() if r == "member"]

        # 三层身份按「人」去重：同一个人（QQ 号与 openid 是同一人）只出现在权限最高那一层。
        # 否则 config 的 admins 存 openid、owner_qq 存 QQ 号时，主人会同时出现在
        # 「主人」与「超级管理员」两段（线上实测：小坤ovo 被列了两遍，一段是 QQ:号 一段是 openid）。
        owner_list = bot.perms.persons(bot.perms.owners(), ctx.group_openid)
        taken = {k for _, k in owner_list}

        cfg_admins = [str(a) for a in (bot.config.get("admins") or [])]
        global_supers = [u for u, r in (bot.perms.roles_global()).items() if r == "super"]
        super_list = [p for p in bot.perms.persons(cfg_admins + global_supers, ctx.group_openid)
                      if p[1] not in taken]
        taken |= {k for _, k in super_list}

        group_list = [p for p in bot.perms.persons(
            [u for u, r in roles.items() if r == "group_admin"], ctx.group_openid)
            if p[1] not in taken]

        lines = ["👥 管理员"]
        if owner_list:
            lines.append(f"\n【主人】{len(owner_list)} 人")
            lines += [f"  · {name}" for name, _ in owner_list]
        if super_list:
            lines.append(f"\n【超级管理员】{len(super_list)} 人")
            lines += [f"  · {name}" for name, _ in super_list]
        if group_list:
            lines.append(f"\n【本群管理员】{len(group_list)} 人")
            lines += [f"  {i}. {name}" for i, (name, _) in enumerate(group_list, 1)]
        else:
            lines.append("\n（本群暂无独立群管理员；授权：设置管理 @某人，超级管理员及以上可用）")
        if demoted:
            lines.append(f"\n已显式降级 {len(demoted)} 人（压过平台群主身份）")
        await router.reply(ctx, "\n".join(lines))

    # ---------- 超级管理员授权（仅主人可达：role.super 节点 = 100） ----------

    @router.command("超管添加", aliases=("addsuper", "授权超管"), perm="role.super",
                    usage="超管添加 @某人|昵称|QQ号（仅主人）")
    async def super_add(ctx, args):
        targets, tip = await _resolve_or_tip(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        results = [await bot.perms.grant_super(ctx, t) for t in targets]
        await router.reply(ctx, "\n".join(m for _, m in results))

    @router.command("超管移除", aliases=("delsuper", "撤销超管"), perm="role.super",
                    usage="超管移除 @某人|昵称|QQ号（仅主人；config.json 来源的管不了）")
    async def super_del(ctx, args):
        targets, tip = await _resolve_or_tip(ctx, args)
        if tip:
            return await router.reply(ctx, tip)
        results = [await bot.perms.revoke_super(ctx, t) for t in targets]
        await router.reply(ctx, "\n".join(m for _, m in results))

    # ---------- 角色自查看（公开） ----------

    @router.command("角色", aliases=("我的角色", "权限"), help="查看自己的角色与权限范围")
    async def role_cmd(ctx, args):
        await router.reply(ctx, bot.perms.role_card(ctx))

    # ---------- 审计日志（bot.manage） ----------

    @router.command("审计日志", aliases=("audit",), perm="bot.manage",
                    usage="审计日志 [条数]", help="查看最近的管理操作记录（授权/封禁/解绑）")
    async def audit_cmd(ctx, args):
        n = 10
        if args.strip().isdigit():
            n = int(args.strip())
        recs = bot.perms.audit_records(n)
        if not recs:
            return await router.reply(ctx, "暂无管理操作记录。")
        op_label = {
            "role.super.grant": "授予超管", "role.super.revoke": "撤销超管",
            "role.group.grant": "授予群管", "role.group.revoke": "撤销群管",
            "ban.group.add": "本群拉黑", "ban.group.del": "解除本群拉黑",
            "ban.global.add": "全局封禁", "ban.global.del": "解除全局封禁",
            "ban.identity.add": "openid 拦截", "ban.identity.del": "解除 openid 拦截",
            "identity.bind": "绑定QQ", "identity.unbind": "解绑QQ",
            "identity.link": "登记QQ", "config.reload": "重载配置",
            # 群管理平台接口
            "group.recall": "撤回消息", "group.mute.set": "禁言",
            "group.mute.del": "解除禁言", "group.member.remove": "移出群聊",
            "group.join.approve": "通过入群", "group.join.decline": "拒绝入群",
            "group.join.strategy.exec": "执行入群策略",
        }
        lines = [f"🧾 最近 {len(recs)} 条管理操作："]
        for r in recs:
            who = bot.identity.display(r.get("by", ""), r.get("group", ""))
            tgt = bot.identity.display(r.get("target", ""), r.get("group", "")) if r.get("target") else "—"
            detail = f"｜{r['detail']}" if r.get("detail") else ""
            lines.append(f"[{r.get('ts', '?')}] {op_label.get(r.get('op'), r.get('op'))} "
                         f"{who} → {tgt}{detail}")
        await router.reply(ctx, "\n".join(lines))

    # ---------- 欢迎语 ----------

    @router.command("欢迎语", perm="group.manage",
                    usage="欢迎语 设置 <文本> ｜ 欢迎语 开启|关闭|查看")
    async def welcome_cmd(ctx, args):
        parts = args.split(None, 1)
        sub = parts[0] if parts else "查看"
        rest = parts[1].strip() if len(parts) > 1 else ""
        cfg = bot.storage.group_section("welcome", ctx.group_openid or "")
        if sub in ("设置", "set"):
            if not rest:
                return await router.reply(ctx, "用法：欢迎语 设置 <文本内容>")
            cfg["on"] = cfg.get("on", True)
            cfg["text"] = rest
            await bot.storage.save()
            await router.reply(ctx, "✅ 进群欢迎语已更新。注意：机器人进群推送（GROUP_ADD_ROBOT）发送的是主动消息，需在开放平台开通主动消息权限后才会实际发出。")
        elif sub == "开启":
            cfg["on"] = True
            await bot.storage.save()
            await router.reply(ctx, "✅ 欢迎语已开启。")
        elif sub == "关闭":
            cfg["on"] = False
            await bot.storage.save()
            await router.reply(ctx, "✅ 欢迎语已关闭。")
        elif sub == "查看":
            text = cfg.get("text") or DEFAULT_WELCOME
            await router.reply(ctx, f"当前欢迎语（{'开启' if cfg.get('on', True) else '关闭'}）：\n{text}")
        else:
            await router.reply(ctx, f"用法：{router.commands['欢迎语'].usage}")

    # ---------- 平台能力说明 ----------

    @router.command("群管", help="群管能力与角色体系说明")
    async def group_admin_info(ctx, args):
        gb = bot.identity.global_blacklist()
        ib = bot.identity.identity_ban_list()
        await router.reply(
            ctx,
            "🛠 群管能力：\n"
            "· 封人：拉黑 @某人（本群）／ 封号 @某人（所有群+私聊）\n"
            "· 关键词自动回复 · 公告 · 群功能开关 · 欢迎语 · 群管理员\n"
            "· 撤回 ／ 禁言 @某人 10分 ／ 解除禁言 ／ 禁言列表\n"
            "· 移除成员 @某人 [拉黑] · 入群申请 ／ 入群同意 ／ 入群拒绝\n\n"
            f"当前：全局黑名单 {len(gb)} 人 ｜ openid 兜底拦截 {len(ib)} 个\n\n"
            "👥 角色体系（发「角色」查看自己的）：\n"
            "· 主人 owner —— config.json owner_qq（可填多个 QQ 号），最高权限\n"
            "· 超级管理员 super —— 全局封号/身份维护/机器人管理/入群策略（超管添加）\n"
            "· 群管理员 group_admin —— 本群关键词/拉黑/积分/撤回/禁言/入群审批（设置管理 授权）\n"
            "· 普通用户 user —— 签到/账号绑定\n"
            "授权全部显式记录、可审计（审计日志）；不能对权限相同或更高的成员操作。\n\n"
            "⚠️ 撤回/禁言/入群审批都要求**机器人在群内是管理员**——不是你是管理员，"
            "是它。发「群信息」可自检它当前身份。\n"
            "⚠️ 平台不下发真实 QQ 号，只有每群一个的加密 openid——"
            "所以群管理接口按 openid 操作；封号仍走机器人的 QQ 号身份层。",
        )

    # ---------- 群功能开关与公告 ----------

    def _register_switch(cmd_name: str, key: str, label: str):
        @router.command(cmd_name, perm="group.manage", usage=f"{cmd_name} 开启|关闭|查看")
        async def switch_cmd(ctx, args, key=key, label=label):
            if not ctx.group_openid:
                return await router.reply(ctx, f"{label}开关请在群里使用。")
            feats = bot.storage.data.setdefault("features", {}).setdefault(ctx.group_openid, {})
            sub = args.strip()
            if sub in ("开", "开启", "on"):
                feats[key] = True
                await bot.storage.save()
                await router.reply(ctx, f"✅ 本群「{label}」已开启。")
            elif sub in ("关", "关闭", "off"):
                feats[key] = False
                await bot.storage.save()
                await router.reply(ctx, f"✅ 本群「{label}」已关闭。")
            else:
                state = "开启" if feats.get(key, True) else "关闭"
                await router.reply(ctx, f"本群「{label}」当前：{state}\n用法：{cmd_name} 开启 / 关闭")
        return switch_cmd

    _register_switch("关键词开关", "keywords", "关键词回复")

    @router.command("公告", perm="group.manage", usage="公告 设置 <文本> ｜ 删除 ｜ 查看")
    async def notice_cmd(ctx, args):
        if not ctx.group_openid:
            return await router.reply(ctx, "公告请在群里设置。")
        notices = bot.storage.data.setdefault("notice", {})
        parts = args.split(None, 1)
        sub = parts[0] if parts else "查看"
        rest = parts[1].strip() if len(parts) > 1 else ""
        if sub in ("设置", "set"):
            if not rest:
                return await router.reply(ctx, "用法：公告 设置 <文本内容>（设置后会显示在本群「帮助」顶部）")
            notices[ctx.group_openid] = {"text": rest}
            await bot.storage.save()
            await router.reply(ctx, "✅ 公告已设置，本群成员发送「帮助」时顶部会显示。")
        elif sub in ("删除", "del"):
            if ctx.group_openid in notices:
                notices.pop(ctx.group_openid)
                await bot.storage.save()
            await router.reply(ctx, "✅ 公告已删除。")
        else:
            n = notices.get(ctx.group_openid)
            if n and n.get("text"):
                await router.reply(ctx, f"📢 当前公告：{n['text']}")
            else:
                await router.reply(ctx, "本群暂无公告，设置：公告 设置 <文本>")

    # ---------- QQ 号绑定（可选；绑定后跨群通用） ----------

    @router.command("绑定QQ", usage="绑定QQ <你的QQ号>",
                    help="登记真实 QQ 号（绑定后资料/积分/账号跨群通用）")
    async def bind_qq(ctx, args):
        code, msg = bot.identity.bind(ctx.sender, args.strip())
        if code in ("ok", "already"):
            # 认领未绑定时用占位键积累的历史数据（积分/收藏/账号）
            bot.storage.claim_user(ctx.uid, args.strip())
            await bot.storage.save()
            log.info("绑定QQ 成功 [%s] ↔ QQ %s", ctx.uid, args.strip())
        await router.reply(ctx, msg)

    @router.command("我的QQ号", aliases=("我的号码", "qq号"), help="查看自己绑定的 QQ 号（可选功能）")
    async def my_qq(ctx, args):
        qq = bot.identity.qq_of(ctx.sender)
        if not qq:
            return await router.reply(
                ctx, "还没绑定 QQ 号。不绑也能正常用机器人。\n"
                     "想绑就发：绑定QQ <你的QQ号> —— 绑了以后积分、收藏、"
                     "KunClient 账号在各个群都通用")
        await router.reply(ctx, f"你的 QQ 号：{qq}"
                             f"（已关联 {len(bot.identity.openids_of(qq))} 个身份）")

    @router.command("绑定表", perm="identity.manage", help="查看已登记的 QQ 号（超级管理员及以上）")
    async def bind_list(ctx, args):
        binds = bot.storage.data.get("qq_bind") or {}
        if not binds:
            return await router.reply(ctx, "还没有人绑定 QQ 号。\n"
                                          "（不影响使用；封人时直接用「封号 @某人」即可）")
        lines = [f"📇 已登记 QQ 号（{len(binds)} 个）："]
        for i, qq in enumerate(sorted(binds, key=lambda k: len(binds[k].get("openids") or []),
                                      reverse=True), 1):
            rec = binds[qq] or {}
            lines.append(f"{i}. QQ {qq}｜{len(rec.get('openids') or [])} 个身份"
                         f"｜{rec.get('at', '?')}")
        await router.reply(ctx, "\n".join(lines[:20]))

    @router.command("解绑QQ", perm="identity.manage", usage="解绑QQ <QQ号>",
                    help="解除某个 QQ 号的绑定（超级管理员及以上）")
    async def unbind_qq(ctx, args):
        qq = args.strip()
        if not is_qq(qq):
            return await router.reply(ctx, "用法：解绑QQ <QQ号>")
        ok, msg = bot.identity.unbind_qq(qq)
        if ok:
            bot.perms._audit("identity.unbind", ctx, target=qq, detail="解除 QQ 绑定")
            await bot.storage.save()
        await router.reply(ctx, ("✅ " + msg) if ok else f"❌ {msg}")

    @router.command("关联身份", aliases=("设QQ号", "登记QQ"), perm="identity.manage",
                    usage="设QQ号 <QQ号> <昵称|openid>",
                    help="直接把某人登记到某 QQ 号上（不必等对方自己绑定；绑定/登记后 openid 拦截自动升级为按号封）")
    async def link_identity(ctx, args):
        parts = args.split(None, 1)
        if len(parts) != 2 or not is_qq(parts[0]):
            return await router.reply(
                ctx, "用法：设QQ号 <QQ号> <昵称|openid>\n"
                     "例：设QQ号 12345678 张三\n"
                     "（你自己知道对方的 QQ 号时用这个，不用等他发「绑定QQ」。"
                     "登记完「封号 @某人」就能跨群封了）")
        qq, who = parts[0].strip(), parts[1].strip()
        oids = [t for t in list(ctx.mentions) + MENTION_RE.findall(who)]
        if not oids:
            who_c = re.sub(r"^\s*@\s*", "", who)
            members = bot.storage.data.get("roster", {}).get(ctx.group_openid or "", {})
            exact = [oid for oid, info in members.items() if info.get("username") == who_c]
            sub = [] if exact else [oid for oid, info in members.items()
                                    if who_c and who_c in (info.get("username") or "")]
            oids = exact or (sub[:1] if len(sub) == 1 else [])
        if not oids:
            return await router.reply(
                ctx, f"没找到「{who}」。\n"
                     "⚠️ 平台群消息事件不提供 @ 对象（没有 mentions 字段），所以「@ 他」用不了；\n"
                     "请写昵称（他 @ 过我一次才进名册）或直接贴 openid。")
        legacy_uid = bot.identity.uid_of_openid(oids[0])
        ok, msg = bot.identity.link_openid(oids[0], qq, create=True)
        if ok:
            bot.storage.claim_user(legacy_uid, qq)
            bot.perms._audit("identity.link", ctx, target=qq, detail=f"登记身份 {oids[0][:8]}…")
            await bot.storage.save()
            log.info("登记 QQ：openid=%s… -> QQ %s", oids[0][:8], qq)
        await router.reply(ctx, ("✅ " + msg) if ok else f"❌ {msg}")

    # ---------- 重载配置 ----------

    @router.command("重载配置", aliases=("reload",), perm="bot.manage",
                    usage="重载配置 [--磁盘]",
                    help="重读 config.json 与数据文件（加 --磁盘 则丢弃内存改动，以磁盘为准）")
    async def reload_cmd(ctx, args):
        # ⚠️ bot.reload() 是协程，必须 await —— 早先这里 await 了一个同步函数，
        # 重载其实已经执行完，却抛 "object NoneType can't be used in 'await' expression"，
        # 被 router 吞成「命令执行出错」，表现为「永远重载不了」。
        #
        # --磁盘：默认会先把内存里的在途改动落盘再重读，这会把**你在服务器上
        # 手工改的 data.json 覆盖掉**（实测把登记好的 QQ 绑定冲掉过）。
        # 手工改过文件就用 --磁盘，让磁盘说了算。
        disk_first = any(a in ("--磁盘", "--disk") for a in args.split())
        try:
            info = await bot.reload(save_first=not disk_first)
        except Exception as e:
            log.warning("重载配置失败: %s", e)
            return await router.reply(ctx, f"❌ 重载失败：{e}")
        bot.perms._audit("config.reload", ctx, detail="磁盘优先" if disk_first else "内存优先")
        bits = ["✅ 已重载 config.json 与 " + info["data_file"]]
        if info["cred_changed"]:
            bits.append("AppID/AppSecret 已变更，机器人已自动重连")
        if disk_first:
            bits.append("已按磁盘内容覆盖内存")
        elif not info["data_changed"]:
            bits.append("（数据文件内容与重载前一致）")
        await router.reply(ctx, "；".join(bits) + "。")
