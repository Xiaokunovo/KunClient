"""机器人主体：事件分发、闲聊触发、风控、回复封装。"""

from __future__ import annotations

import asyncio
import logging
import re
import time
from pathlib import Path

from . import event as ev_mod
from .bans import GlobalBans
from .config import Config
from .deepseek import DeepSeek, DeepSeekError
from .event import Event, build
from .guard import Guard
from .memory import ContextStore
from .metrics import Metrics
from .onebot import ApiError, OneBot
from .permission import LEVELS, Actor, Permissions
from .reminders import Reminders
from .router import Context, Router, normalize
from .state import GroupState
from .storage import Counter, MessageLedger

log = logging.getLogger("bot")

ROOT = Path(__file__).resolve().parent.parent

# 「像不像在提问」的粗判据。宁可漏判（少接一句），也别把陈述句当问题抢答。
QUESTION_WORDS = (
    "吗", "呢", "怎么", "为什么", "为啥", "什么", "多少", "哪里", "哪个", "哪位",
    "谁", "是不是", "能不能", "有没有", "会不会", "要不要", "如何", "咋", "几号",
    "请问", "求问", "有人知道", "咋办", "怎么办",
)


def looks_like_question(text: str) -> bool:
    if "?" in text or "？" in text:
        return True
    if len(text) < 3:  # 「嗯」「1」这种不算
        return False
    return any(w in text for w in QUESTION_WORDS)


class Bot:
    def __init__(self, cfg: Config, router: Router):
        self.cfg = cfg
        self.router = router
        self.api = OneBot(
            ws_url=cfg.get("onebot.ws_url"),
            token=cfg.get("onebot.token", ""),
            reconnect_min=float(cfg.get("onebot.reconnect_min", 2)),
            reconnect_max=float(cfg.get("onebot.reconnect_max", 30)),
            call_timeout=float(cfg.get("onebot.call_timeout", 15)),
        )
        self.perms = Permissions(cfg)
        self.state = GroupState(ROOT / "data" / "groups.json")
        self.ledger = MessageLedger()
        self.ds = DeepSeek(cfg)
        self.memory = ContextStore(
            ROOT / "data" / "context.json",
            rounds=int(cfg.get("chat.context_rounds", 8)),
            ttl=float(cfg.get("chat.context_ttl", 1800)),
        )
        self.reminders = Reminders(ROOT / "data" / "reminders.json")
        self.metrics = Metrics(ROOT / "data" / "metrics.json")
        self.global_bans = GlobalBans(ROOT / "data" / "bans.json")
        self.guard = Guard(self, ROOT / "data" / "guard.jsonl")
        self.cmd_counts: dict[str, int] = {}
        self._reminder_tick = 10.0  # 秒；测试里可以调小，不用等 10 秒
        self._reminder_wake = asyncio.Event()
        self._last_speak: dict[int, float] = {}  # 群 -> 上次开口时间，用于「别连着说」
        self.started_at = time.time()
        self.stats = {"messages": 0, "commands": 0, "ai": 0, "errors": 0}

        self._chat_sem = asyncio.Semaphore(int(cfg.get("chat.max_concurrent", 4)))
        self._cooldown = Counter(window=float(cfg.get("chat.group_cooldown", 3)), limit=1)
        self._spam = Counter(
            window=float(cfg.get("group.anti_spam.window", 8)),
            limit=int(cfg.get("group.anti_spam.limit", 6)),
        )
        self._bg: set[asyncio.Task] = set()

        self.api.on_event(self.on_event)

        self.webui = None
        if cfg.get("webui.enabled", True):
            from .webui import WebUI

            self.webui = WebUI(self)

    # ================= 生命周期 =================
    async def start(self) -> None:
        if self.webui:
            try:
                await self.webui.start()
                log.info("Web 控制台：http://%s:%s", self.webui.host, self.webui.port)
            except OSError as exc:
                log.error("Web 控制台启动失败（端口被占？）：%s", exc)
        self._spawn(self._housekeeping())
        self._spawn(self._reminder_loop())
        await self.api.run()

    async def shutdown(self) -> None:
        self.memory.flush()
        self.state.flush()
        self.metrics.flush()
        if self.webui:
            await self.webui.stop()
        await self.ds.close()
        await self.api.close()

    def _spawn(self, coro) -> None:
        task = asyncio.create_task(coro)
        self._bg.add(task)
        task.add_done_callback(self._bg.discard)

    async def _housekeeping(self) -> None:
        """每 60s 清理过期上下文 / 计数器，每 5 分钟落盘一次。"""
        tick = 0
        while True:
            await asyncio.sleep(60)
            tick += 1
            try:
                self.memory.sweep()
                self._spam.sweep()
                if tick % 5 == 0:
                    self.memory.flush()
                    self.state.flush()
                    self.metrics.flush()
            except Exception:  # noqa: BLE001
                log.exception("后台维护异常")

    def wake_reminders(self) -> None:
        """新加了提醒就叫醒循环，别让「1 分钟后提醒我」硬等下一个 tick。"""
        self._reminder_wake.set()

    async def _reminder_loop(self) -> None:
        """扫到期提醒。落盘存储，重启不丢。

        🔴 用 Event + wait_for 而不是裸 sleep：裸 sleep 的话，刚加完提醒
        还要等满一个 tick（10s）才被看见，「1秒后提醒」会变成「11秒后提醒」。
        """
        while True:
            try:
                await asyncio.wait_for(self._reminder_wake.wait(), timeout=self._reminder_tick)
            except asyncio.TimeoutError:
                pass
            self._reminder_wake.clear()
            try:
                for item in self.reminders.pop_due():
                    gid = int(item.get("gid") or 0)
                    uid = int(item.get("uid") or 0)
                    if not gid:
                        continue
                    segs = [ev_mod.at(uid), ev_mod.text(f" 提醒你：{item.get('text', '')}")]
                    try:
                        await self.api.send_group(gid, segs)
                    except ApiError as exc:
                        log.warning("提醒发送失败 gid=%s：%s", gid, exc)
            except Exception:  # noqa: BLE001
                log.exception("提醒循环异常")

    # ================= 事件入口 =================
    async def on_event(self, raw: dict) -> None:
        post_type = raw.get("post_type")
        try:
            if post_type == "message":
                await self._on_message(ev_mod.wrap(raw))
            elif post_type == "notice":
                await self._on_notice(ev_mod.wrap(raw))
            elif post_type == "request":
                await self._on_request(ev_mod.wrap(raw))
        except ApiError as exc:
            self.stats["errors"] += 1
            log.warning("调用平台接口失败：%s", exc)
        except Exception:  # noqa: BLE001
            self.stats["errors"] += 1
            log.exception("处理事件异常：%s", post_type)

    # ---- 消息 -------------------------------------------------------
    async def _on_message(self, ev: Event) -> None:
        if ev.user_id == ev.self_id and self.cfg.get("bot.ignore_self", True):
            return
        if not ev.segments:
            return

        self.stats["messages"] += 1
        self.metrics.bump()
        if ev.is_group:
            self.ledger.add(ev.group_id, ev.message_id, ev.user_id, ev.text)
            self.state.bump_message(ev.group_id, ev.user_id)

        if ev.is_group and await self._moderate(ev):
            return

        # AI 内容哨兵：异步跑，绝不阻塞正常消息处理。
        # 判定要等模型 1~3 秒，同步的话整条消息链都会被拖住。
        #
        # 🔴 命令消息必须跳过：群管发「/违禁词 添加 死全家」时，
        # 那条命令本身就带着敏感词，不排除的话会把自己的管理员封掉。
        if ev.is_group and self.guard.enabled:
            prefixes = self.cfg.get("bot.prefixes", ["/", "#"])
            if not self.router.is_command_like(ev, prefixes):
                self._spawn(self.guard.check(ev, ev.text))

        await self._dispatch(ev)

    async def _dispatch(self, ev: Event) -> None:
        prefixes = self.cfg.get("bot.prefixes", ["/", "#"])
        parsed = self.router.parse(ev, prefixes)
        actor = self.perms.resolve(ev)

        if parsed:
            cmd, args = parsed
            await self._run_command(ev, cmd, args, actor)
            return

        # 带了前缀但没匹配到命令 → 提示（私聊不提示，避免干扰聊天）
        body = normalize(ev.text)
        if ev.is_group and any(body.startswith(p) for p in prefixes) and len(body) > 1:
            await self.reply(ev, "没有这个命令。发「/帮助」看看我会啥。", at=False)
            return

        # 关键词自动回复：优先于 AI 对话（是群管显式配的规则，比模型瞎聊靠谱）
        if ev.is_group:
            hit = self.state.match_keyword(ev.group_id, body)
            if hit:
                await self._send_keyword_reply(ev, *hit)
                return

        if self._should_chat(ev):
            # 非 @ 的主动接话要过群冷却：刚说过就先安静
            if ev.is_group and not ev.at_me and not self._can_speak(ev.group_id):
                log.debug("群 %s 冷却中，跳过主动接话", ev.group_id)
                return
            await self._chat(ev)

    async def _send_keyword_reply(self, ev: Event, word: str, template: str) -> None:
        segments: list[dict] = []
        text = template
        if "{at}" in text:
            segments.append(ev_mod.at(ev.user_id))
            text = text.replace("{at}", "")
        if text.strip():
            segments.append(ev_mod.text(text.strip()))
        if not segments:
            return
        try:
            await self.api.send_group(ev.group_id, segments)
        except ApiError as exc:
            log.warning("关键词回复发送失败：%s", exc)

    def _should_chat(self, ev: Event) -> bool:
        if ev.has_media and not ev.text.strip():
            return False
        text = normalize(ev.text)
        if not text:
            return False
        if ev.is_private:
            return self.cfg.get("bot.command_in_private", True)

        # 群里接话的策略（bot.group_trigger）
        #   at    —— 只有 @机器人 或提到昵称才接（最省 token、最不打扰）
        #   smart —— 默认。@ 必回；像提问的才回；其余按很低概率
        #   prob  —— 按 chat.group_reply_probability 随机，@ 必回
        #   all   —— 所有消息都接（很吵，一般不要用）
        mode = str(self.cfg.get("bot.group_trigger", "smart")).lower()
        if ev.at_me:
            return True
        if mode == "all":
            return True
        if mode == "at":
            return self._nickname_hit(text)
        if mode == "prob":
            import random

            return random.random() < float(self.cfg.get("chat.group_reply_probability", 0.35))
        # smart
        if self._nickname_hit(text):
            return True
        if looks_like_question(text):
            return True
        import random

        return random.random() < float(self.cfg.get("chat.smart_reply_probability", 0.08))

    def _nickname_hit(self, text: str) -> bool:
        if not self.cfg.get("bot.nickname_trigger", True):
            return False
        return any(n and n in text for n in self.cfg.get("bot.nicknames", []))

    def _can_speak(self, group_id: int) -> bool:
        """刚说过话就安静一会儿，别在群里连着刷。@ 不走这个限制。"""
        gap = float(self.cfg.get("chat.group_speak_cooldown", 20))
        if gap <= 0:
            return True
        return time.time() - self._last_speak.get(group_id, 0.0) >= gap

    async def _run_command(self, ev: Event, cmd, args: str, actor: Actor) -> None:
        # 作用域
        if cmd.scope == "group" and not ev.is_group:
            await self.reply(ev, "这个命令只能在群里用。")
            return
        if cmd.scope == "private" and not ev.is_private:
            await self.reply(ev, "这个命令请私聊我。")
            return
        # 权限
        if not self.perms.require(actor, cmd.perm):
            need = {
                "group.admin": "群管理",
                "group.owner": "群主",
                "admin": "机器人管理员",
                "super": "主人",
            }.get(cmd.perm, "更高权限")
            await self.reply(ev, f"权限不够，这个要{need}才能用。", at=True)
            return
        # 机器人自身是否具备群管理身份
        if cmd.need_admin_bot and ev.is_group:
            ok, why = await self.perms.can_manage(self.api, ev.group_id)
            if not ok:
                await self.reply(ev, why, at=False)
                return

        self.stats["commands"] += 1
        self.cmd_counts[cmd.name] = self.cmd_counts.get(cmd.name, 0) + 1
        ctx = Context(self, ev, cmd, args, actor)
        log.info("[cmd] %s by %s(%s) in %s", cmd.name, ev.nickname, ev.user_id, ev.group_id or "private")
        await cmd.func(ctx)

    # ================= 风控 =================
    async def _moderate(self, ev: Event) -> bool:
        """返回 True 表示这条消息已被处理，不再往下走。"""
        if ev.role in ("owner", "admin") and self.cfg.get("group.anti_spam.exempt_admin", True):
            return False
        text = normalize(ev.text)

        # 违禁词：撤回 + 警告
        words = list(self.cfg.get("group.auto_recall_words") or []) + self.state.words(ev.group_id)
        hit = next((w for w in words if w and w in text), None)
        if hit:
            await self._recall_and_warn(ev, f"命中违禁词「{hit}」")
            return True

        # 刷屏
        if self.cfg.get("group.anti_spam.enabled", True):
            key = f"{ev.group_id}:{ev.user_id}"
            if self._spam.exceeded(key):
                self._spam.reset(key)
                seconds = int(self.cfg.get("group.anti_spam.mute", 60))
                ok, _ = await self.perms.can_manage(self.api, ev.group_id)
                if ok:
                    try:
                        await self.api.set_group_ban(ev.group_id, ev.user_id, seconds)
                        await self.reply(ev, f"刷屏了，先安静 {seconds} 秒。", at=True)
                        return True
                    except ApiError as exc:
                        log.warning("刷屏禁言失败：%s", exc)
        return False

    async def _recall_and_warn(self, ev: Event, reason: str) -> None:
        try:
            await self.api.delete_msg(ev.message_id)
        except ApiError as exc:
            log.warning("撤回失败：%s", exc)
        n = self.state.warn(ev.group_id, ev.user_id)
        threshold = int(self.cfg.get("group.warn_mute_threshold", 3))
        tip = f"{ev.nickname} {reason}，第 {n} 次警告"
        if n >= threshold:
            ok, _ = await self.perms.can_manage(self.api, ev.group_id)
            if ok:
                secs = int(self.cfg.get("group.warn_mute_seconds", 600))
                try:
                    await self.api.set_group_ban(ev.group_id, ev.user_id, secs)
                    self.state.clear_warn(ev.group_id, ev.user_id)
                    tip += f"，已禁言 {secs // 60} 分钟"
                except ApiError:
                    pass
        await self.reply(ev, tip + "。", at=False)

    # ================= 通知 =================
    async def _on_notice(self, ev: Event) -> None:
        nt = ev.notice_type
        if nt == "group_increase" and self.state.welcome_on(
            ev.group_id, self.cfg.get("group.welcome", True)
        ):
            await self._send_welcome(ev)
        elif nt == "group_decrease" and self.state.farewell_on(
            ev.group_id, self.cfg.get("group.farewell", False)
        ):
            tpl = self.state.farewell_text(ev.group_id, self.cfg.get("group.farewell_text", ""))
            name = ev.raw.get("target_id") or ev.user_id
            try:
                await self.api.send_group(ev.group_id, build(tpl.format(name=name, group=ev.group_id)))
            except ApiError:
                pass
        elif nt == "group_admin":
            self.perms.invalidate_bot_role(ev.group_id)
        elif nt == "group_recall":
            log.debug("群 %s 撤回了一条消息", ev.group_id)

    async def _send_welcome(self, ev: Event) -> None:
        tpl = self.state.welcome_text(ev.group_id, self.cfg.get("group.welcome_text", "欢迎 {at}"))
        group_name = ""
        try:
            info = await self.api.get_group_info(ev.group_id)
            group_name = str(info.get("group_name") or "")
        except ApiError:
            pass
        nickname = ""
        try:
            member = await self.api.get_group_member_info(ev.group_id, ev.user_id)
            nickname = str(member.get("card") or member.get("nickname") or "")
        except ApiError:
            pass

        # 模板里用 {at} 占位，替换成真正的 at 段
        rendered = tpl.format(at="\u0000AT\u0000", group=group_name or ev.group_id, name=nickname or ev.user_id)
        parts = rendered.split("\u0000AT\u0000")
        segments: list[dict] = []
        for i, part in enumerate(parts):
            if part:
                segments.append(ev_mod.text(part))
            if i < len(parts) - 1:
                segments.append(ev_mod.at(ev.user_id))
        if len(parts) == 1:  # 模板没写 {at}，那也得 @ 一下
            segments.insert(0, ev_mod.at(ev.user_id))
        try:
            await self.api.send_group(ev.group_id, segments)
        except ApiError as exc:
            log.warning("欢迎语发送失败：%s", exc)

    async def _on_request(self, ev: Event) -> None:
        if ev.request_type != "group":
            return
        gid = ev.group_id
        flag = str(ev.raw.get("flag") or "")
        comment = str(ev.raw.get("comment") or "")
        verify = self.state.get(gid, "verify") or {}

        # 黑名单优先级最高：全局 > 本群，命中直接拒绝，不进验证流程
        if flag:
            gban = self.global_bans.get(ev.user_id)
            lban = self.state.is_banned(gid, ev.user_id)
            if gban or lban:
                why = (gban or lban).get("reason") or "在黑名单里"
                try:
                    await self.api.set_group_add_request(flag, False, f"已被拒绝：{why}")
                    log.info("拒绝黑名单入群：群 %s 用户 %s", gid, ev.user_id)
                    return
                except ApiError as exc:
                    log.warning("拒绝黑名单申请失败：%s", exc)

        # 配了入群验证：附言里带答案就自动放行，答错不拒绝，只通知主人
        if verify.get("on") and verify.get("answer") and flag:
            answer = str(verify["answer"])
            if answer.lower() in comment.lower():
                try:
                    await self.api.set_group_add_request(flag, True)
                    log.info("入群验证通过：群 %s 用户 %s", gid, ev.user_id)
                    return
                except ApiError as exc:
                    log.warning("自动通过入群申请失败：%s", exc)

        hint = ""
        if verify.get("on") and verify.get("question"):
            hint = f"\n（本群入群问题：{verify['question']}）"
        note = (
            f"入群申请：群 {gid}，申请人 {ev.user_id}"
            f"，附言：{comment or '无'}{hint}\n"
            f"同意：/同意 {flag}\n拒绝：/拒绝 {flag}"
        )
        for uid in self.perms.superusers:
            try:
                await self.api.send_private(uid, note)
            except ApiError:
                pass

    # ================= AI 对话 =================
    async def _chat(self, ev: Event) -> None:
        if not self.ds.ready:
            await self.reply(ev, "我还没接上大脑（DeepSeek API Key 没配），先去 config.json 里填一下。")
            return

        text = normalize(ev.text)
        if not text:
            return
        max_chars = int(self.cfg.get("chat.max_input_chars", 1500))
        if len(text) > max_chars:
            text = text[:max_chars] + "…"

        # 冷却：同群同人 3 秒一次，避免连点刷爆
        cd_key = f"{ev.session_key}:{ev.user_id}"
        if self._cooldown.hit(cd_key) > 1:
            log.debug("对话冷却中，忽略 %s", cd_key)
            return

        model = self.cfg.get("deepseek.model")
        trigger = self.cfg.get("chat.reasoning_trigger", "深度")
        if trigger and trigger in text:
            model = self.cfg.get("deepseek.reasoner_model")
            text = text.replace(trigger, "").strip() or text

        system_prompt = self._system_prompt(ev, self.perms.resolve(ev))
        messages = self.memory.build_messages(ev.session_key, system_prompt, text)

        async with self._chat_sem:
            try:
                typing = asyncio.create_task(self._typing(ev))
                try:
                    reply = await self.ds.chat(messages, model=model)
                finally:
                    typing.cancel()
            except DeepSeekError as exc:
                await self.reply(ev, f"脑子卡住了：{exc}")
                return

        content = reply.content or "（想不出来，换个说法？）"
        self.memory.append(ev.session_key, text, content)
        self.stats["ai"] += 1
        log.info("[ai] %s <- %r -> %r (%s tokens)", ev.session_key, text[:40], content[:40], reply.total_tokens)
        await self.reply(ev, content)
        if ev.is_group:
            self._last_speak[ev.group_id] = time.time()

    async def _typing(self, ev: Event) -> None:
        """尝试发「正在输入」状态，失败静默（不同 OneBot 实现支持度不一）。"""
        try:
            if ev.is_group:
                await self.api.call("set_group_typing", group_id=ev.group_id, status="typing")
        except Exception:  # noqa: BLE001
            pass

    def _system_prompt(self, ev: Event, actor: Actor | None = None) -> str:
        """人设 + 现场信息。

        关键是让模型知道**现在是谁在说话**：主人 / 群管 / 普通群友，
        它才不会对主人也用客服腔，也不会把群管的指令当普通闲聊。
        """
        base = str(self.cfg.get("chat.system_prompt") or "")
        name = self.cfg.get("bot.name", "机器人")
        extra: list[str] = []

        if ev.is_group:
            extra.append(f"当前是 QQ 群聊（群号 {ev.group_id}），你在群里叫「{name}」。")
        else:
            extra.append(f"当前是 QQ 私聊，你在 QQ 上叫「{name}」。")

        speaker = ev.nickname or str(ev.user_id)
        role, tip = self._speaker_role(ev, actor)
        extra.append(f"正在说话的人：{speaker}（QQ {ev.user_id}），身份是「{role}」。")
        if tip:
            extra.append(tip)

        extra.append(f"现在时间：{time.strftime('%Y-%m-%d %H:%M')}。")
        return base + "\n" + "\n".join(extra)

    def _speaker_role(self, ev: Event, actor: Actor | None) -> tuple[str, str]:
        level = actor.level if actor else LEVELS["member"]
        if level >= LEVELS["super"]:
            return "主人", (
                "主人是你唯一的所有者：对他不用客套，可以更随意亲近，称呼「主人」；"
                "他让你做的事优先照办，做不到就直说原因。"
            )
        if level >= LEVELS["admin"]:
            return "机器人管理员", "他是主人授权的管理员，可以指挥你做群务，但主人优先级更高。"
        if level >= LEVELS["group.owner"]:
            return "群主", "他是本群群主，在本群里说了算，配合他管理群聊。"
        if level >= LEVELS["group.admin"]:
            return "群管", "他是本群管理员，涉及群务的要求可以配合。"
        return "普通群友", ""

    # ================= 发送 =================
    async def reply(
        self,
        ev: Event,
        content: str,
        *,
        at: bool = True,
        quote: bool = False,
        image: str | None = None,
    ) -> int:
        segments: list[dict] = []
        if quote and ev.message_id:
            segments.append(ev_mod.reply(ev.message_id))
        if at and ev.is_group and self.cfg.get("bot.reply_with_at", True):
            segments.append(ev_mod.at(ev.user_id))
            segments.append(ev_mod.text(" "))
        if content:
            segments.append(ev_mod.text(content))
        if image:
            segments.append(ev_mod.image(image))
        if not segments:
            return 0

        try:
            if ev.is_group:
                return await self.api.send_group(ev.group_id, segments)
            return await self.api.send_private(ev.user_id, segments)
        except ApiError as exc:
            log.warning("发送失败：%s", exc)
            # 群发失败（比如被禁言）时，如果是主人命令，私聊兜底告知
            if ev.is_group and ev.user_id in self.perms.superusers:
                try:
                    await self.api.send_private(ev.user_id, f"[群 {ev.group_id} 发送失败] {content}\n原因：{exc}")
                except ApiError:
                    pass
            return 0

    # ================= 工具 =================
    def uptime(self) -> str:
        secs = int(time.time() - self.started_at)
        d, rem = divmod(secs, 86400)
        h, rem = divmod(rem, 3600)
        m, s = divmod(rem, 60)
        parts = []
        if d:
            parts.append(f"{d}天")
        if h or d:
            parts.append(f"{h}小时")
        parts.append(f"{m}分{s}秒")
        return "".join(parts)
