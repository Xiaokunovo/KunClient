"""客户端发版自动播报。

KunClient 服务端 `/api/app/version` 里的 `version` 就是「当前发版号」。本模块定时
轮询它，一旦发现版本号变化，就把更新内容（changelog）**主动**推送到已登记的目标群。

为什么要做：发版是高频动作（改一行 UI 也可能重出包），此前每次都要人工在群里
敲一遍更新内容，容易漏、容易抄错；版本号由服务端 `app_settings` 唯一决定，拿来
当触发条件最可靠——客户端检查更新读的就是它，不存在「群里说的版本 ≠ 实际发的包」。

平台约束（qq 官方机器人）：
- **主动消息**（不带 msg_id）需要平台开通群发/提醒能力；未开通时接口返回 403，
  这里捕获后写进 `last_result` 并在人工「kun发版播报」时暴露出来，不静默重试风暴；
- 首次运行只**记录基线版本**、不播报（否则每次重启都会把当前版本当成「新版本」群发）；
- 开关 / 目标群 / 上次版本全部落 data.json 的 `release` 段，重启不丢。

安全默认：目标群为空 = **不播报**。必须是管理员显式把群加进目标列表才会发出去，
避免一次误配置把发版消息撒到所有群。
"""
import asyncio
import logging
import re
import time

log = logging.getLogger("qq.release")

DEFAULT_INTERVAL = 600          # 轮询间隔（秒），config.kunclient.release_broadcast.interval_sec 可覆盖
CHANGELOG_MAX_LINES = 8         # 单条播报最多列几行更新内容

# 服务端 changelog 的入库约定是「一行一条」，行首可能带「·」「-」「1.」「1、」等。
# 这里与服务端 server.js 的邮件渲染保持**同一套剥离规则**（同上还有「本次更新…」这类
# 说明行），否则群里会出现「· 1. 全新 Fluent UI 界面」这种双重编号。
_BULLET_RE = re.compile(r"^[·•\-*]\s*")
_NUMBER_RE = re.compile(r"^\d+[.、)]\s*")


def clean_changelog(text: str) -> list[str]:
    """把服务端 changelog 文本切成干净条目列表（剥符号/编号，丢说明行）。"""
    out = []
    for raw in str(text or "").splitlines():
        line = _NUMBER_RE.sub("", _BULLET_RE.sub("", raw.strip())).strip()
        if line and not line.startswith("本次更新"):
            out.append(line)
    return out


class ReleaseWatcher:
    """发版轮询 + 主动播报。由 Bot 在 run() 里 create_task 常驻。"""

    def __init__(self, bot):
        self.bot = bot
        self._task: asyncio.Task | None = None

    # ---------- 状态 ----------

    def state(self) -> dict:
        """data.json 的 release 段（挂在 data 上的 dict，可安全就地改写）。"""
        return self.bot.storage.data.setdefault("release", {})

    @property
    def enabled(self) -> bool:
        return bool(self.state().get("enabled", True))

    def targets(self) -> list[str]:
        return [str(t) for t in (self.state().get("targets") or []) if str(t).strip()]

    @property
    def interval(self) -> int:
        cfg = (self.bot.config.get("kunclient") or {}).get("release_broadcast") or {}
        try:
            n = int(cfg.get("interval_sec", DEFAULT_INTERVAL))
        except (TypeError, ValueError):
            n = DEFAULT_INTERVAL
        return max(60, n)

    # ---------- 构文 ----------

    def compose(self, v: dict) -> str:
        """把服务端版本信息拼成一条群公告式播报。"""
        ver = v.get("version") or "?"
        lines = [f"📦 KunClient 客户端已更新：{ver}"]
        if v.get("buildDate"):
            lines.append(f"📅 构建日期：{v['buildDate']}")
        if v.get("forceUpdate"):
            lines.append("⚡ 本次为强制更新，旧版本将无法继续使用")
        changelog = clean_changelog(v.get("changelog"))
        if changelog:
            lines.append("")
            lines.append("📝 更新内容：")
            shown = changelog[:CHANGELOG_MAX_LINES]
            lines += [f"· {ln}" for ln in shown]
            if len(changelog) > len(shown):
                lines.append(f"（共 {len(changelog)} 条，详见客户端内更新说明）")
        dl = v.get("downloadUrl") or ""
        if dl.startswith("/"):
            dl = self.bot.kun.base + dl
        lines.append("")
        if dl:
            lines.append(f"⬇️ 下载：{dl}")
        lines.append("（客户端内「检查更新」可一键升级）")
        return "\n".join(lines)

    # ---------- 播报 ----------

    async def broadcast(self, v: dict, *, targets: list[str] | None = None) -> dict:
        """把版本信息主动推送到目标群。返回 {sent, failed, total, detail}。"""
        gids = targets if targets is not None else self.targets()
        text = self.compose(v)
        sent, failed = 0, []
        for gid in gids:
            try:
                # 不传 msg_id ⇒ 主动消息（需平台开通能力）
                await self.bot.api.send_group_message(gid, content=text, msg_seq=1)
                sent += 1
                await asyncio.sleep(0.5)   # 轻微限速，避免触发平台频控
            except Exception as e:
                failed.append(gid)
                log.warning("发版播报发送到群 %s 失败：%s", gid, e)
        detail = f"成功 {sent}/{len(gids)}"
        if failed:
            detail += f"，失败 {len(failed)}"
        return {"sent": sent, "failed": failed, "total": len(gids), "detail": detail}

    async def manual(self, ctx) -> str:
        """管理员手动播报当前版本（二次确认后调用）。返回人话结果。"""
        v = await self.bot.kun.request("GET", "/api/app/version")
        gids = self.targets()
        if not gids:
            return ("⚠️ 还没有登记播报目标群。先在目标群里发「kun发版播报 本群」把它加进来，"
                    "或发「kun发版播报 全部」把机器人已知的群全部加入。")
        res = await self.broadcast(v, targets=gids)
        # 手动播报：全部失败时同样不推进基线（留给下一轮自动补发）
        self._record(v.get("version") or "", res["detail"], failed=res["failed"],
                     advance=res["sent"] > 0)
        await self.bot.storage.save()
        line = f"📣 已播报版本 {v.get('version')}（{res['detail']}）"
        if res["failed"]:
            line += "\n（失败多为未开通主动消息权限：403 无权限，需在 QQ 开放平台申请群发能力）"
        return line

    def _record(self, version: str, detail: str, *, failed: list | None = None,
                advance: bool = True):
        """写播报状态。advance=False 时**不推进** last_version（保留重试机会）。"""
        st = self.state()
        if advance and version:
            st["last_version"] = version
        st["last_try_version"] = version or st.get("last_try_version", "")
        st["last_ts"] = time.time()
        st["last_time"] = time.strftime("%Y-%m-%d %H:%M", time.localtime())
        st["last_result"] = detail
        st["last_failed"] = list(failed or [])

    # ---------- 轮询 ----------

    async def check_once(self) -> dict | None:
        """轮询一次。版本号变化且开关打开则播报。

        返回 None 表示「无动作」；否则返回本次结果 dict（供测试断言）。
        """
        st = self.state()
        v = await self.bot.kun.request("GET", "/api/app/version")
        ver = str(v.get("version") or "")
        if not ver:
            return None
        last = str(st.get("last_version") or "")
        if not last:
            # 首次运行：只记基线，不播报（否则每次重启都把当前版本当新版本群发）
            st["last_version"] = ver
            st["baseline_time"] = time.strftime("%Y-%m-%d %H:%M")
            await self.bot.storage.save()
            log.info("发版播报已建立基线版本：%s", ver)
            return {"event": "baseline", "version": ver}
        if ver == last:
            return None
        if not self.enabled:
            log.info("检测到新版本 %s（上版 %s），但播报开关已关闭，跳过", ver, last)
            return {"event": "skipped", "version": ver, "reason": "disabled"}
        if not self.targets():
            log.info("检测到新版本 %s，但未登记目标群，跳过（先发「kun发版播报 本群」）", ver)
            return {"event": "skipped", "version": ver, "reason": "no_targets"}
        res = await self.broadcast(v)
        if res["sent"] > 0:
            self._record(ver, res["detail"], failed=res["failed"])
            await self.bot.storage.save()
            log.info("已播报新版本 %s：%s", ver, res["detail"])
            return {"event": "broadcast", "version": ver, "result": res}
        # 一个都没发出去（绝大多数是未开通「主动消息」权限 → 403）。此时**不推进基线**：
        # 一旦把版本标记成「已播报」，权限开通后这条更新就永远不会补发了。
        self._record(ver, f"版本 {ver} 发送失败：{res['detail']}", failed=res["failed"],
                     advance=False)
        await self.bot.storage.save()
        log.warning("新版本 %s 播报全部失败（%s），不推进基线，下轮重试", ver, res["detail"])
        return {"event": "broadcast_failed", "version": ver, "result": res}

    async def loop(self):
        """常驻轮询。任何异常只记日志，绝不退出（发版播报失败不该拖垮机器人）。"""
        log.info("发版播报已启动（间隔 %ss，开关 %s，目标 %d 个群）",
                 self.interval, "开" if self.enabled else "关", len(self.targets()))
        while True:
            await asyncio.sleep(self.interval)
            try:
                await self.check_once()
            except Exception as e:
                log.warning("发版轮询失败（下轮重试）：%s", e)

    def start(self):
        if self._task is None or self._task.done():
            self._task = asyncio.create_task(self.loop())

    def stop(self):
        if self._task and not self._task.done():
            self._task.cancel()
        self._task = None
