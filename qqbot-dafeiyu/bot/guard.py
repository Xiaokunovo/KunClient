"""AI 内容哨兵：本地粗筛 → DeepSeek 判定 → 分级处置。

## 为什么是两层，不是「每条消息都问 AI」

群里一天几千条消息，每条都过一次 DeepSeek：
- token 成本直接失控（一条 200 token，3000 条 = 60 万 token/天）
- 每条都等 1~3 秒，消息处理会被拖成一锅粥
- 正常聊天被反复送审，模型也更容易「为了显得有用」而瞎判

所以本地先粗筛（零成本），只有**可疑**的才送 AI。粗筛写宽一点没关系
（多花点 token 而已），真正的判定交给模型。

## 三条红线（都是「不这么做就会出事」的）

1. **AI 不可靠时绝不封人**：key 失效 / 超时 / 返回不是合法 JSON，
   一律只记录不执行。宁可漏封，也不能因为接口抖动把人误封了。
2. **一切判定可回溯**：写 `data/guard.jsonl`，控制台能查。
   误封必须能翻案，否则这功能就是个定时炸弹。
3. **等级 + 置信度双门槛**：level 够高还不够，模型自己得有把握。
   不确定的判定一律降级处理。
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path

from .config import DEFAULT_GUARD_PROMPT
from .deepseek import DeepSeekError

log = logging.getLogger("guard")

# ==================== 本地粗筛特征 ====================
# 只用来决定「要不要问 AI」，**不直接判定违规**。
# 宁可多问（浪费点 token），也别漏 —— 真正的判断在模型那边。
SUSPECT_PATTERNS: list[tuple[str, str]] = [
    # 广告 / 引流
    (r"(加|进|扫)\s*(微信|vx|v信|薇信|群|我)\s*[:：]?\s*[a-zA-Z0-9_\-]{4,}", "广告引流"),
    (r"(代练|刷单|返利|日赚|躺赚|一手货源|低价出|私聊我|加我好友)", "广告引流"),
    (r"(https?://|www\.)\S{6,}", "外链"),
    # 违禁品 / 暴恐
    (r"(炸弹|雷管|自制枪|买枪|卖枪|制毒|冰毒|海洛因|大麻)", "违禁品"),
    (r"(暴动|游行|示威|颠覆国家|推翻政府)", "涉政"),
    # 色情
    (r"(约炮|裸聊|一夜情|黄片|成人视频|色情服务|福利姬)", "色情"),
    # 辱骂（写具体点，别用单字「傻」这种，会误伤「傻子才用这个」这类玩笑）
    (r"(死全家|全家死光|nmsl|cnm|草泥马|傻逼|煞笔|脑残|废物东西|你妈死了)", "辱骂"),
    # 诈骗
    (r"(转账给我|扫码付|充值返|验证码发我|银行卡号发|垫付|刷流水)", "诈骗"),
]

# 结构特征：不需要关键词也能看出不对劲
STRUCT_PATTERNS: list[tuple[str, str]] = [
    (r"([^\s。，,.!?！？])\1{9,}", "重复刷屏"),   # 排除标点，否则「。。。。。。」会被判
    (r"(?:\[CQ:at,qq=\d+\]\s*){6,}", "批量艾特"),
]

DEFAULT_PROMPT = DEFAULT_GUARD_PROMPT


@dataclass
class Verdict:
    level: int = 0
    category: str = "正常"
    reason: str = ""
    confidence: float = 0.0
    hit: str = ""          # 命中的本地嫌疑特征
    error: str = ""        # 非空 = AI 判定失败，此时 level 一律按 0 处理

    @property
    def ok(self) -> bool:
        return not self.error

    def to_dict(self) -> dict:
        return {
            "level": self.level,
            "category": self.category,
            "reason": self.reason,
            "confidence": round(self.confidence, 2),
            "hit": self.hit,
            "error": self.error,
        }


@dataclass
class Record:
    """一条判定记录（审计用）。"""

    ts: float
    group_id: int
    user_id: int
    name: str
    text: str
    verdict: Verdict
    action: str = "none"

    def to_dict(self) -> dict:
        return {
            "ts": int(self.ts),
            "time": time.strftime("%m-%d %H:%M:%S", time.localtime(self.ts)),
            "gid": self.group_id,
            "uid": self.user_id,
            "name": self.name,
            "text": self.text[:300],
            "action": self.action,
            **self.verdict.to_dict(),
        }


class Guard:
    """内容哨兵。挂在 Bot 上，由 `Bot._guard_check()` 驱动。"""

    def __init__(self, bot, path: Path):
        self.bot = bot
        self.path = path
        self.records: deque[Record] = deque(maxlen=300)
        self._recent_ids: deque[str] = deque(maxlen=200)
        self._stamps: deque[float] = deque(maxlen=120)
        self._sem = asyncio.Semaphore(2)
        self.stats = {"checked": 0, "flagged": 0, "banned": 0, "errors": 0, "skipped": 0}
        self._load_recent()

    # ---------------- 配置快捷取值 ----------------
    def cfg(self, key: str, default=None):
        return self.bot.cfg.get(f"guard.{key}", default)

    @property
    def enabled(self) -> bool:
        return bool(self.cfg("enabled", True))

    # ---------------- 审计 ----------------
    def _load_recent(self) -> None:
        """启动时回读最近 100 条，控制台不用等新数据。"""
        if not self.path.exists():
            return
        try:
            lines = self.path.read_text(encoding="utf-8").splitlines()[-100:]
        except OSError:
            return
        for line in lines:
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            v = Verdict(
                level=int(d.get("level") or 0),
                category=str(d.get("category") or "正常"),
                reason=str(d.get("reason") or ""),
                confidence=float(d.get("confidence") or 0),
                hit=str(d.get("hit") or ""),
            )
            self.records.append(Record(
                ts=float(d.get("ts") or 0), group_id=int(d.get("gid") or 0),
                user_id=int(d.get("uid") or 0), name=str(d.get("name") or ""),
                text=str(d.get("text") or ""), verdict=v, action=str(d.get("action") or "none"),
            ))

    def _write(self, rec: Record) -> None:
        self.records.append(rec)
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(rec.to_dict(), ensure_ascii=False) + "\n")
        except OSError as exc:
            log.warning("审计日志写入失败：%s", exc)

    def recent(self, limit: int = 60) -> list[dict]:
        return [r.to_dict() for r in list(self.records)[-limit:][::-1]]

    # ---------------- 第一层：本地粗筛 ----------------
    def screen(self, text: str, ev) -> tuple[bool, str]:
        """返回 (要不要送 AI, 命中的特征名)。

        白名单、太短、纯媒体都在这里挡掉 —— 挡掉的是 99% 的正常消息。
        """
        if not self.enabled:
            return False, ""

        # 豁免：主人 / 机器人管理员 / 群主 / 群管
        if self.cfg("exempt_admin", True):
            if ev.role in ("owner", "admin"):
                return False, ""
            level = self.bot.perms.resolve(ev).level
            if level >= self.bot.perms.threshold("group.owner"):
                return False, ""

        body = text.strip()
        if len(body) < int(self.cfg("min_length", 4)):
            return False, ""

        # 纯图片/表情包没有文字可审
        if ev.has_media and len(body) < 2:
            return False, ""

        for pattern, name in SUSPECT_PATTERNS:
            if re.search(pattern, body, re.I):
                return True, name

        raw = "".join(
            f"[CQ:at,qq={s.get('data', {}).get('qq', '')}]" if s.get("type") == "at" else ""
            for s in (ev.segments or [])
        )
        for pattern, name in STRUCT_PATTERNS:
            if re.search(pattern, body) or (raw and re.search(pattern, raw)):
                return True, name

        # 随机抽查：覆盖那些「没命中关键词但确实有问题」的情况
        rate = float(self.cfg("sample_rate", 0.0))
        if rate > 0:
            import random

            if random.random() < rate:
                return True, "随机抽查"

        return False, ""

    # ---------------- 第二层：AI 判定 ----------------
    async def analyze(self, text: str, *, group_name: str = "", hit: str = "") -> Verdict:
        ds = self.bot.ds
        if not ds.ready:
            return Verdict(hit=hit, error="DeepSeek 未配置")

        prompt = str(self.cfg("prompt") or DEFAULT_PROMPT)
        payload = [
            {"role": "system", "content": prompt},
            {"role": "user", "content": f"群名：{group_name or '未知'}\n消息：\n{text[:800]}"},
        ]
        try:
            async with self._sem:
                reply = await ds.chat(
                    payload, temperature=0.0, max_tokens=200, json_mode=True
                )
        except DeepSeekError as exc:
            self.stats["errors"] += 1
            return Verdict(hit=hit, error=str(exc))
        except Exception as exc:  # noqa: BLE001
            self.stats["errors"] += 1
            return Verdict(hit=hit, error=f"调用异常：{exc}")

        return self._parse(reply.content, hit)

    def _parse(self, content: str, hit: str) -> Verdict:
        """容错解析。模型偶尔会包 ```json、加前后缀、或者少字段。"""
        raw = (content or "").strip()
        if not raw:
            return Verdict(hit=hit, error="模型返回空内容")

        # 去掉 markdown 包裹
        raw = re.sub(r"^```(?:json)?\s*|\s*```$", "", raw, flags=re.S).strip()
        # 抠第一个 { 到最后一个 }
        start, end = raw.find("{"), raw.rfind("}")
        if start < 0 or end <= start:
            return Verdict(hit=hit, error=f"返回不是 JSON：{raw[:80]}")
        try:
            data = json.loads(raw[start : end + 1])
        except json.JSONDecodeError as exc:
            return Verdict(hit=hit, error=f"JSON 解析失败：{exc}")

        if not isinstance(data, dict):
            return Verdict(hit=hit, error="返回不是对象")

        try:
            level = int(data.get("level", 0))
        except (TypeError, ValueError):
            level = 0
        try:
            conf = float(data.get("confidence", 0.0))
        except (TypeError, ValueError):
            conf = 0.0

        return Verdict(
            level=max(0, min(4, level)),
            category=str(data.get("category") or "其他")[:12],
            reason=str(data.get("reason") or "")[:120],
            confidence=max(0.0, min(1.0, conf)),
            hit=hit,
        )

    # ---------------- 第三层：分级处置 ----------------
    def decide(self, v: Verdict) -> str:
        """把判定翻译成动作。AI 失败 / 把握不足一律降级。"""
        if not v.ok:
            return "none"
        min_conf = float(self.cfg("min_confidence", 0.7))
        if v.confidence < min_conf:
            # 模型自己都没把握，就别动手了。记下来供人工看。
            return "review"

        if v.level >= int(self.cfg("ban_level", 4)):
            return "ban"
        if v.level >= int(self.cfg("mute_level", 3)):
            return "mute"
        if v.level >= int(self.cfg("recall_level", 2)):
            return "recall"
        return "none"

    async def enforce(self, ev, v: Verdict, action: str) -> str:
        """执行处置。返回实际做了什么的描述（写进审计）。"""
        api = self.bot.api
        note = v.reason or v.category

        if action == "ban":
            # 拉黑（写本群黑名单）+ 踢出。顺序：先写名单再踢，
            # 万一踢失败，名单也已经在，下次申请照样拦得住。
            try:
                self.bot.state.add_ban(ev.group_id, ev.user_id, f"[AI] {note}", ev.self_id)
            except Exception:  # noqa: BLE001
                log.exception("写入黑名单失败")
            try:
                await api.delete_msg(ev.message_id)
            except Exception:  # noqa: BLE001
                pass
            try:
                await api.set_group_kick(ev.group_id, ev.user_id, reject_add_request=True)
            except Exception as exc:  # noqa: BLE001
                log.warning("AI 哨兵踢人失败：%s", exc)
                return "ban-failed"
            self.stats["banned"] += 1
            log.warning(
                "[guard] 封禁 %s(%s) in %s：%s / %s（把握 %.2f）",
                ev.nickname, ev.user_id, ev.group_id, v.category, note, v.confidence,
            )
            return "ban"

        if action in ("mute", "recall"):
            try:
                await api.delete_msg(ev.message_id)
            except Exception:  # noqa: BLE001
                pass
            if action == "mute":
                seconds = int(self.cfg("mute_seconds", 600))
                try:
                    await api.set_group_ban(ev.group_id, ev.user_id, seconds)
                except Exception as exc:  # noqa: BLE001
                    log.warning("AI 哨兵禁言失败：%s", exc)
            try:
                tip = {
                    "mute": f"这条内容不合适（{v.category}），先安静 {int(self.cfg('mute_seconds', 600)) // 60} 分钟。",
                    "recall": f"这条内容不合适（{v.category}），已撤回。",
                }[action]
                await self.bot.reply(ev, tip, at=True)
            except Exception:  # noqa: BLE001
                pass
            log.info("[guard] %s %s：%s", action, ev.user_id, note)
            return action

        return "review" if action == "review" else "none"

    # ---------------- 入口 ----------------
    async def check(self, ev, text: str) -> None:
        """完整流程。由 Bot 用 _spawn 异步调用，不阻塞正常消息处理。"""
        if not ev.is_group or not text.strip():
            return
        if self.bot.cfg.get("bot.ignore_self", True) and ev.user_id == ev.self_id:
            return

        # 同一条消息别重复审（重连补发、事件重投都可能造成）
        key = f"{ev.group_id}:{ev.message_id}"
        if ev.message_id and key in self._recent_ids:
            return
        if ev.message_id:
            self._recent_ids.append(key)

        ok, hit = self.screen(text, ev)
        if not ok:
            self.stats["skipped"] += 1
            return

        # 限流：刷屏时别把 DeepSeek 打爆
        now = time.time()
        while self._stamps and now - self._stamps[0] > 60:
            self._stamps.popleft()
        if len(self._stamps) >= int(self.cfg("max_per_minute", 10)):
            self.stats["skipped"] += 1
            log.debug("[guard] 每分钟判定数超限，跳过")
            return
        self._stamps.append(now)

        self.stats["checked"] += 1
        # 不主动查群名：多一次 API 调用换来的判定提升很有限，
        # 而群信息查询在刷屏场景下会变成新的瓶颈。
        v = await self.analyze(text, group_name="", hit=hit)
        if v.level > 0 or v.error:
            self.stats["flagged"] += 1
        action = self.decide(v)
        done = await self.enforce(ev, v, action) if action not in ("none", "review") else action
        self._write(Record(
            ts=time.time(), group_id=ev.group_id, user_id=ev.user_id,
            name=ev.nickname or str(ev.user_id), text=text, verdict=v, action=done,
        ))

    def status(self) -> dict:
        return {
            "enabled": self.enabled,
            "sample_rate": float(self.cfg("sample_rate", 0.0)),
            "min_length": int(self.cfg("min_length", 4)),
            "ban_level": int(self.cfg("ban_level", 4)),
            "mute_level": int(self.cfg("mute_level", 3)),
            "recall_level": int(self.cfg("recall_level", 2)),
            "min_confidence": float(self.cfg("min_confidence", 0.7)),
            "max_per_minute": int(self.cfg("max_per_minute", 10)),
            "ready": self.bot.ds.ready,
            "stats": dict(self.stats),
        }
