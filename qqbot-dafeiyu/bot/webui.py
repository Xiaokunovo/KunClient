"""Web 控制台：跟机器人跑在同一个进程里，直接读内存里的实时状态。

    http://127.0.0.1:8088

默认只绑 127.0.0.1 —— 这台机器上还跑着别的东西，控制台能改配置、能踢人，
绝不能裸奔到公网。要在自己电脑上访问就走 SSH 隧道：

    ssh -L 8088:127.0.0.1:8088 root@<服务器>

鉴权：首次启动自动生成一个口令写进 config.json 的 webui.token，登录后存 cookie。
"""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any

from aiohttp import web

from . import event as ev_mod

ROOT = Path(__file__).resolve().parent.parent
COOKIE = "kc_webui"

# 允许从控制台改的字段（白名单，别让人把 onebot.ws_url 改成奇怪的东西）
EDITABLE = [
    "bot.name",
    "bot.nicknames",
    "bot.reply_with_at",
    "bot.nickname_trigger",
    "bot.group_trigger",
    "deepseek.api_key",
    "deepseek.model",
    "deepseek.reasoner_model",
    "deepseek.temperature",
    "deepseek.max_tokens",
    "chat.system_prompt",
    "chat.context_rounds",
    "chat.context_ttl",
    "chat.group_cooldown",
    "chat.group_reply_probability",
    "chat.group_speak_cooldown",
    "chat.smart_reply_probability",
    "chat.reasoning_trigger",
    "group.welcome",
    "group.welcome_text",
    "group.anti_spam.enabled",
    "group.anti_spam.window",
    "group.anti_spam.limit",
    "group.anti_spam.mute",
    "group.warn_mute_threshold",
    "group.warn_mute_seconds",
    "permission.superusers",
    "permission.admins",
    "guard.enabled",
    "guard.min_length",
    "guard.sample_rate",
    "guard.ban_level",
    "guard.mute_level",
    "guard.mute_seconds",
    "guard.recall_level",
    "guard.min_confidence",
    "guard.max_per_minute",
    "guard.prompt",
]


class WebUI:
    def __init__(self, bot):
        self.bot = bot
        self.cfg = bot.cfg
        self.token = self.cfg.ensure_webui_token()
        self.host = str(self.cfg.get("webui.host", "127.0.0.1"))
        self.port = int(self.cfg.get("webui.port", 8088))
        self._runner: web.AppRunner | None = None
        self._started_at = time.time()
        self._names: dict[int, tuple[dict[int, str], float]] = {}

    # ---------------- 生命周期 ----------------
    async def start(self) -> None:
        app = web.Application(middlewares=[self._auth_mw])
        app.router.add_get("/", self.page)
        app.router.add_post("/api/login", self.login)
        app.router.add_post("/api/logout", self.logout)
        app.router.add_get("/api/overview", self.overview)
        app.router.add_get("/api/config", self.get_config)
        app.router.add_post("/api/config", self.set_config)
        app.router.add_get("/api/groups", self.groups)
        app.router.add_post("/api/group", self.set_group)
        app.router.add_get("/api/logs", self.logs)
        app.router.add_get("/api/sessions", self.sessions)
        app.router.add_post("/api/session/clear", self.session_clear)
        app.router.add_get("/api/reminders", self.reminders)
        app.router.add_post("/api/reminder/delete", self.reminder_delete)
        app.router.add_get("/api/stats", self.stats)
        app.router.add_post("/api/group/action", self.group_action)
        app.router.add_get("/api/guard", self.guard_status)
        app.router.add_post("/api/guard/test", self.guard_test)
        app.router.add_post("/api/action", self.action)

        self._runner = web.AppRunner(app, access_log=None)
        await self._runner.setup()
        site = web.TCPSite(self._runner, self.host, self.port)
        await site.start()

    async def stop(self) -> None:
        if self._runner:
            await self._runner.cleanup()

    # ---------------- 鉴权 ----------------
    @web.middleware
    async def _auth_mw(self, request: web.Request, handler):
        path = request.path
        if path in ("/", "/api/login"):
            return await handler(request)
        if request.cookies.get(COOKIE) == self.token:
            return await handler(request)
        if path.startswith("/api/"):
            return web.json_response({"ok": False, "error": "未登录"}, status=401)
        raise web.HTTPFound("/")

    async def login(self, request: web.Request) -> web.Response:
        data = await request.json()
        if str(data.get("token") or "") != self.token:
            await asyncio.sleep(0.6)  # 拖一下暴力试口令
            return web.json_response({"ok": False, "error": "口令不对"}, status=403)
        resp = web.json_response({"ok": True})
        resp.set_cookie(COOKIE, self.token, max_age=30 * 86400, httponly=True, samesite="Lax")
        return resp

    async def logout(self, request: web.Request) -> web.Response:
        resp = web.json_response({"ok": True})
        resp.del_cookie(COOKIE)
        return resp

    # ---------------- 页面 ----------------
    async def page(self, request: web.Request) -> web.Response:
        authed = request.cookies.get(COOKIE) == self.token
        html = PAGE_DASH if authed else PAGE_LOGIN
        return web.Response(text=html, content_type="text/html", charset="utf-8")

    # ---------------- API ----------------
    async def overview(self, request: web.Request) -> web.Response:
        bot = self.bot
        groups: list[dict] = []
        try:
            groups = await bot.api.get_group_list()
        except Exception:  # noqa: BLE001
            pass
        data = {
            "name": self.cfg.get("bot.name"),
            "online": bot.api.connected,
            "self_id": bot.api.self_id,
            "uptime": bot.uptime(),
            "stats": bot.stats,
            "deepseek": {
                "ready": bot.ds.ready,
                "model": bot.ds.model,
                "calls": bot.ds.stats["calls"],
                "tokens": bot.ds.stats["tokens"],
            },
            "sessions": len(getattr(bot.memory, "_data", {})),
            "groups": len(groups),
            "group_list": [
                {"group_id": g.get("group_id"), "group_name": g.get("group_name"),
                 "member_count": g.get("member_count")}
                for g in groups[:200]
            ],
            "superusers": self.cfg.get("permission.superusers") or [],
            "ws_url": self.cfg.get("onebot.ws_url"),
        }
        return web.json_response({"ok": True, "data": data})

    async def get_config(self, request: web.Request) -> web.Response:
        out: dict[str, Any] = {}
        for key in EDITABLE:
            val = self.cfg.get(key)
            if key == "deepseek.api_key":
                val = mask_key(val)
            out[key] = val
        return web.json_response({"ok": True, "data": out})

    async def set_config(self, request: web.Request) -> web.Response:
        patch = await request.json()
        changed = []
        for key, val in patch.items():
            if key not in EDITABLE:
                continue
            if key == "deepseek.api_key":
                # 前端拿到的是打码值，原样提交回来就别覆盖
                if not val or str(val).startswith("sk-****"):
                    continue
            self.cfg.set(key, coerce(self.cfg.get(key), val), save=False)
            changed.append(key)
        if not changed:
            return web.json_response({"ok": True, "data": {"changed": []}})
        self.cfg.save()
        self.bot.perms.reload()
        self.bot.ds.reload(self.cfg)
        return web.json_response({"ok": True, "data": {"changed": changed}})

    async def guard_status(self, request: web.Request) -> web.Response:
        g = self.bot.guard
        return web.json_response({"ok": True, "data": {
            "status": g.status(),
            "records": g.recent(80),
        }})

    async def guard_test(self, request: web.Request) -> web.Response:
        """试判一段文本：只判定、不执行。调提示词的时候全靠它。"""
        body = await request.json()
        text = str(body.get("text") or "").strip()
        if not text:
            return web.json_response({"ok": False, "error": "文本为空"}, status=400)
        g = self.bot.guard
        v = await g.analyze(text, hit="控制台测试")
        return web.json_response({"ok": True, "data": {
            "level": v.level, "category": v.category, "reason": v.reason,
            "confidence": round(v.confidence, 2), "error": v.error,
            "action": g.decide(v),
        }})

    async def groups(self, request: web.Request) -> web.Response:
        try:
            raw = await self.bot.api.get_group_list()
        except Exception as exc:  # noqa: BLE001
            return web.json_response({"ok": False, "error": f"取群列表失败：{exc}"}, status=502)
        out = []
        for g in raw:
            gid = int(g.get("group_id") or 0)
            if not gid:
                continue
            out.append({
                "group_id": gid,
                "group_name": g.get("group_name") or str(gid),
                "member_count": g.get("member_count"),
                "welcome": self.bot.state.welcome_on(gid, self.cfg.get("group.welcome", True)),
                "welcome_text": self.bot.state.welcome_text(gid, self.cfg.get("group.welcome_text", "")),
                "words": self.bot.state.words(gid),
                "keywords": self.bot.state.keywords(gid),
                "stats": self.bot.state.stats(gid),
            })
        out.sort(key=lambda x: -(x["stats"].get("messages") or 0))
        return web.json_response({"ok": True, "data": out})

    async def set_group(self, request: web.Request) -> web.Response:
        body = await request.json()
        gid = int(body.get("group_id") or 0)
        if not gid:
            return web.json_response({"ok": False, "error": "缺少 group_id"}, status=400)
        st = self.bot.state
        if "welcome" in body:
            st.set(gid, "welcome", bool(body["welcome"]))
        if "welcome_text" in body:
            st.set(gid, "welcome_text", str(body["welcome_text"])[:300])
        if "words" in body:
            words = [w.strip() for w in str(body["words"]).splitlines() if w.strip()]
            st.set(gid, "words", words[:200])
        if "keywords" in body:
            kws: dict[str, str] = {}
            for line in str(body["keywords"]).splitlines():
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if k.strip() and v.strip():
                    kws[k.strip()[:40]] = v.strip()[:300]
                if len(kws) >= 50:
                    break
            st.set(gid, "keywords", kws)
        st.flush()
        return web.json_response({"ok": True})

    async def _member_names(self, group_id: int) -> dict[int, str]:
        """群成员名缓存 5 分钟 —— 统计页要按群显示人名，别每次全量拉。"""
        now = time.time()
        cached = self._names.get(group_id)
        if cached and now - cached[1] < 300:
            return cached[0]
        names: dict[int, str] = {}
        try:
            for m in await self.bot.api.get_group_member_list(group_id):
                uid = int(m.get("user_id") or 0)
                if uid:
                    names[uid] = str(m.get("card") or m.get("nickname") or uid)
        except Exception:  # noqa: BLE001
            pass
        if names:
            self._names[group_id] = (names, now)
        return names

    async def sessions(self, request: web.Request) -> web.Response:
        data = getattr(self.bot.memory, "_data", {}) or {}
        now = time.time()
        rows = []
        for key, item in data.items():
            msgs = item.get("messages") or []
            last_user = next((m.get("content", "") for m in reversed(msgs) if m.get("role") == "user"), "")
            rows.append({
                "key": key,
                "turns": len(msgs) // 2,
                "age": int(now - float(item.get("ts", now))),
                "preview": str(last_user)[:80],
            })
        rows.sort(key=lambda r: r["age"])
        return web.json_response({"ok": True, "data": rows})

    async def session_clear(self, request: web.Request) -> web.Response:
        body = await request.json()
        key = str(body.get("key") or "")
        if not key:
            n = self.bot.memory.clear_prefix("group:") + self.bot.memory.clear_prefix("private:")
            return web.json_response({"ok": True, "msg": f"清掉 {n} 个会话"})
        n = self.bot.memory.clear(key)
        return web.json_response({"ok": True, "msg": f"清掉 {n // 2} 轮"})

    async def reminders(self, request: web.Request) -> web.Response:
        items = self.bot.reminders.store.get("items", []) or []
        now = time.time()
        rows = [{
            "id": i.get("id"),
            "gid": i.get("gid"),
            "uid": i.get("uid"),
            "text": i.get("text"),
            "left": int(float(i.get("due", now)) - now),
        } for i in items]
        rows.sort(key=lambda r: r["left"])
        return web.json_response({"ok": True, "data": rows})

    async def reminder_delete(self, request: web.Request) -> web.Response:
        body = await request.json()
        rid = int(body.get("id") or 0)
        gid = int(body.get("gid") or 0)
        items = [i for i in (self.bot.reminders.store.get("items", []) or [])
                 if not (int(i.get("id") or 0) == rid and int(i.get("gid") or 0) == gid)]
        self.bot.reminders.store.update({"items": items})
        return web.json_response({"ok": True, "msg": "已删除"})

    async def stats(self, request: web.Request) -> web.Response:
        bot = self.bot
        trend = bot.metrics.series(7)
        cmd_top = sorted(bot.cmd_counts.items(), key=lambda kv: -kv[1])[:15]
        points = []
        for gid in list(getattr(bot.state.store, "all", lambda: {})().keys())[:30]:
            try:
                g = int(gid)
            except (TypeError, ValueError):
                continue
            top = bot.state.points_top(g, 5)
            if not top:
                continue
            names = await self._member_names(g)
            points.append({
                "group_id": g,
                "top": [{"uid": u, "name": names.get(u, str(u)), "points": p} for u, p in top],
            })
        return web.json_response({"ok": True, "data": {
            "trend": trend,
            "cmd_top": [{"name": n, "count": c} for n, c in cmd_top],
            "points": points[:12],
            "today": bot.metrics.today(),
            "total": bot.stats["messages"],
            "sessions": len(getattr(bot.memory, "_data", {}) or {}),
            "reminders": bot.reminders.count(),
        }})

    async def group_action(self, request: web.Request) -> web.Response:
        body = await request.json()
        gid = int(body.get("group_id") or 0)
        uid = int(body.get("user_id") or 0)
        act = str(body.get("action") or "")
        if not gid or not uid:
            return web.json_response({"ok": False, "error": "缺少 group_id / user_id"}, status=400)
        try:
            if act == "mute":
                await self.bot.api.set_group_ban(gid, uid, int(body.get("seconds") or 600))
                return web.json_response({"ok": True, "msg": "已禁言"})
            if act == "unmute":
                await self.bot.api.set_group_ban(gid, uid, 0)
                return web.json_response({"ok": True, "msg": "已解禁"})
            if act == "kick":
                await self.bot.api.set_group_kick(gid, uid, reject_add_request=bool(body.get("black")))
                return web.json_response({"ok": True, "msg": "已移出群聊"})
        except Exception as exc:  # noqa: BLE001
            return web.json_response({"ok": False, "error": str(exc)}, status=502)
        return web.json_response({"ok": False, "error": "未知动作"}, status=400)

    async def logs(self, request: web.Request) -> web.Response:
        n = min(int(request.query.get("lines", 200)), 2000)
        path = ROOT / "data" / "logs" / "bot.log"
        if not path.exists():
            return web.json_response({"ok": True, "data": ""})
        try:
            text = await asyncio.to_thread(_tail, path, n)
        except OSError as exc:
            return web.json_response({"ok": False, "error": str(exc)}, status=500)
        return web.json_response({"ok": True, "data": text})

    async def action(self, request: web.Request) -> web.Response:
        body = await request.json()
        act = body.get("action")
        if act == "reload":
            new = type(self.cfg).load()
            self.cfg._data = new._data
            self.bot.perms.reload()
            self.bot.ds.reload(self.cfg)
            return web.json_response({"ok": True, "msg": "配置已重载"})
        if act == "clear_context":
            key = body.get("key")
            if key:
                n = self.bot.memory.clear(str(key))
                return web.json_response({"ok": True, "msg": f"清掉 {n // 2} 轮"})
            n = self.bot.memory.clear_prefix("group:") + self.bot.memory.clear_prefix("private:")
            return web.json_response({"ok": True, "msg": f"清掉 {n} 个会话"})
        if act == "test_send":
            gid = int(body.get("group_id") or 0)
            text = str(body.get("text") or "小小坤ovo 上线测试")[:200]
            if not gid:
                return web.json_response({"ok": False, "error": "缺少 group_id"}, status=400)
            try:
                await self.bot.api.send_group(gid, [ev_mod.text(text)])
            except Exception as exc:  # noqa: BLE001
                return web.json_response({"ok": False, "error": str(exc)}, status=502)
            return web.json_response({"ok": True, "msg": "已发送"})
        return web.json_response({"ok": False, "error": "未知动作"}, status=400)


# ---------------- 小工具 ----------------
def mask_key(key: str) -> str:
    key = str(key or "")
    if len(key) < 10:
        return "sk-****（未配置）" if not key else "sk-****"
    return f"sk-****{key[-4:]}"


def coerce(old: Any, new: Any) -> Any:
    """按原值类型转换，避免表单把 8 变成 "8" 之后所有比较都失败。"""
    if isinstance(old, bool):
        return bool(new) if not isinstance(new, str) else new.strip().lower() in ("1", "true", "on", "yes")
    if isinstance(old, int) and not isinstance(old, bool):
        try:
            return int(float(new))
        except (TypeError, ValueError):
            return old
    if isinstance(old, float):
        try:
            return float(new)
        except (TypeError, ValueError):
            return old
    if isinstance(old, list):
        if isinstance(new, list):
            return new
        return [x.strip() for x in str(new).replace("，", ",").split(",") if x.strip()]
    return new


def _tail(path: Path, n: int) -> str:
    with open(path, "rb") as fh:
        fh.seek(0, 2)
        size = fh.tell()
        block = min(size, max(8192, n * 200))
        fh.seek(size - block)
        data = fh.read().decode("utf-8", "replace")
    return "\n".join(data.splitlines()[-n:])


# ---------------- 前端 ----------------
_COMMON_CSS = """
*{box-sizing:border-box;margin:0;padding:0}
:root{color-scheme:light}
body{font-family:-apple-system,"Segoe UI","Microsoft YaHei",sans-serif;background:#f4f7fb;color:#1b2432;
     font-size:14px;line-height:1.6;-webkit-font-smoothing:antialiased}
a{color:#2f6fed}
.wrap{max-width:1080px;margin:0 auto;padding:24px 20px 60px}
.top{display:flex;align-items:center;gap:12px;margin-bottom:22px}
.logo{width:40px;height:40px;border-radius:12px;background:linear-gradient(140deg,#5b8cff,#2f6fed);
      display:flex;align-items:center;justify-content:center;color:#fff;font-weight:700;font-size:17px;
      box-shadow:0 6px 16px rgba(47,111,237,.28)}
.top h1{font-size:17px;font-weight:650;letter-spacing:.2px}
.top .meta{font-size:12px;color:#7c8aa0}
.spacer{flex:1}
.badge{font-size:12px;padding:3px 10px;border-radius:20px;background:#e8f7ee;color:#1a8f4c;font-weight:600}
.badge.off{background:#fdeaea;color:#c33}
.btn{border:0;border-radius:9px;padding:8px 14px;font-size:13px;cursor:pointer;font-family:inherit;
     background:#eef2f8;color:#33415c;transition:.15s}
.btn:hover{background:#e3e9f3}
.btn.primary{background:#2f6fed;color:#fff}
.btn.primary:hover{background:#255fd6}
.btn.danger{background:#fdeaea;color:#c33}
.btn:disabled{opacity:.5;cursor:not-allowed}
.tabs{display:flex;gap:4px;margin-bottom:16px;border-bottom:1px solid #e4eaf3}
.tab{padding:9px 16px;font-size:14px;color:#6b7a90;cursor:pointer;border-bottom:2px solid transparent;
     margin-bottom:-1px;font-weight:500}
.tab.on{color:#2f6fed;border-bottom-color:#2f6fed}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(168px,1fr));gap:12px;margin-bottom:20px}
.card{background:#fff;border-radius:14px;padding:14px 16px;box-shadow:0 1px 3px rgba(20,40,80,.06)}
.card .k{font-size:12px;color:#8494a9;margin-bottom:5px}
.card .v{font-size:19px;font-weight:650;letter-spacing:.2px}
.card .v.sm{font-size:14px;font-weight:600;word-break:break-all}
.panel{background:#fff;border-radius:14px;padding:18px 20px;box-shadow:0 1px 3px rgba(20,40,80,.06);
       margin-bottom:16px}
.panel h2{font-size:14px;font-weight:650;margin-bottom:14px;color:#2b3950}
label{display:block;font-size:12px;color:#7c8aa0;margin:12px 0 5px}
input[type=text],input[type=password],input[type=number],textarea,select{
  width:100%;border:1px solid #dfe6f0;border-radius:9px;padding:9px 11px;font-size:13px;
  font-family:inherit;background:#fbfcfe;color:#1b2432;outline:none;transition:.15s}
input:focus,textarea:focus{border-color:#2f6fed;background:#fff;box-shadow:0 0 0 3px rgba(47,111,237,.10)}
textarea{resize:vertical;min-height:74px;line-height:1.55}
.row{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.switch{display:flex;align-items:center;gap:8px;margin-top:12px}
.switch input{width:auto}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;font-size:12px;color:#8494a9;font-weight:500;padding:8px 10px;border-bottom:1px solid #eef2f8}
td{padding:9px 10px;border-bottom:1px solid #f4f7fb}
tr:hover td{background:#fafcff}
pre{background:#0f1723;color:#c9d6e5;padding:14px 16px;border-radius:12px;font-size:12px;
    line-height:1.6;overflow:auto;max-height:520px;font-family:ui-monospace,Consolas,monospace}
.toast{position:fixed;left:50%;bottom:28px;transform:translateX(-50%) translateY(20px);background:#1b2432;
       color:#fff;padding:10px 18px;border-radius:10px;font-size:13px;opacity:0;transition:.25s;pointer-events:none}
.toast.on{opacity:1;transform:translateX(-50%) translateY(0)}
.hint{font-size:12px;color:#93a1b5;margin-top:6px}
.login{min-height:100vh;display:flex;align-items:center;justify-content:center}
.login .box{background:#fff;padding:32px 30px;border-radius:18px;box-shadow:0 12px 40px rgba(20,40,80,.10);
            width:340px;text-align:center}
.login h1{font-size:17px;margin-bottom:6px}
.login p{font-size:13px;color:#7c8aa0;margin-bottom:20px}
"""

PAGE_LOGIN = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小小坤ovo · 控制台</title><style>""" + _COMMON_CSS + """
.login input{margin-bottom:12px;text-align:center;letter-spacing:1px}
</style></head><body>
<div class="login"><div class="box">
  <div class="logo" style="margin:0 auto 14px">坤</div>
  <h1>小小坤ovo 控制台</h1>
  <p>输入口令进入（在 config.json 的 webui.token）</p>
  <input type="password" id="tok" placeholder="访问口令" autofocus>
  <button class="btn primary" style="width:100%" onclick="go()">进入</button>
  <div class="hint" id="err" style="color:#c33"></div>
</div></div>
<script>
async function go(){
  const t=document.getElementById('tok').value.trim();
  const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({token:t})});
  if(r.ok){location.reload();}else{document.getElementById('err').textContent='口令不对';}
}
document.getElementById('tok').addEventListener('keydown',e=>{if(e.key==='Enter')go()});
</script></body></html>"""

PAGE_DASH = """<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>小小坤ovo · 控制台</title><style>""" + _COMMON_CSS + """
</style></head><body>
<div class="wrap">
  <div class="top">
    <div class="logo">坤</div>
    <div>
      <h1 id="botname">小小坤ovo</h1>
      <div class="meta" id="meta">加载中…</div>
    </div>
    <div class="spacer"></div>
    <span class="badge" id="badge">连接中</span>
    <button class="btn" onclick="logout()">退出</button>
  </div>

  <div id="offline" style="display:none;background:#fff4f4;border:1px solid #f6d5d5;border-radius:14px;
       padding:14px 18px;margin-bottom:16px;font-size:13px;color:#a33;line-height:1.8">
    <b>⚠️ 机器人当前没连上 QQ</b><br>
    QQ 掉登录了（被踢下线 / 扫码过期）。需要重新扫码：<br>
    <code style="background:#fff;padding:2px 6px;border-radius:5px">cd /www/wwwroot/dafeiyu-bot &amp;&amp; ./manage.sh qr</code>
    　然后手机 QQ 扫终端里那张码。
  </div>

  <div class="tabs">
    <div class="tab on" data-t="ov">概览</div>
    <div class="tab" data-t="grp">群管理</div>
    <div class="tab" data-t="ses">会话</div>
    <div class="tab" data-t="rem">提醒</div>
    <div class="tab" data-t="stat">统计</div>
    <div class="tab" data-t="grd">哨兵</div>
    <div class="tab" data-t="cfg">配置</div>
    <div class="tab" data-t="log">日志</div>
  </div>

  <div id="p-ov">
    <div class="cards" id="cards"></div>
    <div class="panel">
      <h2>快捷操作</h2>
      <button class="btn" onclick="act('reload')">重载配置</button>
      <button class="btn danger" onclick="act('clear_context')">清空全部上下文</button>
    </div>
  </div>

  <div id="p-grp" style="display:none">
    <div class="panel">
      <h2>群列表</h2>
      <div class="hint" id="grphint">点一行展开编辑</div>
      <table><thead><tr><th>群号</th><th>群名</th><th>人数</th><th>消息</th><th>欢迎</th><th>违禁词</th><th>关键词</th></tr></thead>
      <tbody id="grpbody"></tbody></table>
    </div>
    <div class="panel" id="grpedit" style="display:none">
      <h2 id="grptitle">编辑群</h2>
      <div class="switch"><input type="checkbox" id="g_welcome"><label for="g_welcome" style="margin:0">开启入群欢迎</label></div>
      <label>欢迎语（支持 {at} {name} {group}）</label>
      <textarea id="g_text"></textarea>
      <label>违禁词（一行一个，命中自动撤回+警告）</label>
      <textarea id="g_words" style="min-height:92px"></textarea>
      <label>关键词自动回复（一行一个，格式：触发词=回复；回复里写 {at} 会 @ 提问的人）</label>
      <textarea id="g_keywords" style="min-height:92px" placeholder="打卡=已记录 {at}"></textarea>
      <div style="margin-top:14px">
        <button class="btn primary" onclick="saveGroup()">保存</button>
        <button class="btn" onclick="testSend()">发条测试消息</button>
      </div>
      <label style="margin-top:20px">成员操作（填 QQ 号）</label>
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <input type="text" id="g_uid" placeholder="QQ号" style="flex:1;min-width:150px">
        <button class="btn" onclick="memberAct('mute')">禁言 10 分钟</button>
        <button class="btn" onclick="memberAct('unmute')">解禁</button>
        <button class="btn danger" onclick="memberAct('kick')">移出群聊</button>
      </div>
    </div>
  </div>

  <div id="p-cfg" style="display:none">
    <div class="panel">
      <h2>DeepSeek</h2>
      <label>API Key</label><input type="password" id="c_api_key" placeholder="sk-...">
      <div class="row">
        <div><label>日常模型</label><input type="text" id="c_model"></div>
        <div><label>深度思考模型</label><input type="text" id="c_reasoner_model"></div>
      </div>
      <div class="row">
        <div><label>温度 temperature</label><input type="number" step="0.1" id="c_temperature"></div>
        <div><label>max_tokens</label><input type="number" id="c_max_tokens"></div>
      </div>
      <label>人设提示词（system prompt）</label>
      <textarea id="c_system_prompt" style="min-height:110px"></textarea>
    </div>
    <div class="panel">
      <h2>对话</h2>
      <label>群里什么时候接话</label>
      <select id="c_group_trigger">
        <option value="smart">看场合（默认）：@ 必回，像提问的才回</option>
        <option value="at">只在 @ 我 或叫我名字时才回</option>
        <option value="prob">按概率随机接，@ 必回</option>
        <option value="all">所有消息都接（很吵，慎用）</option>
      </select>
      <div class="row">
        <div><label>记忆轮数</label><input type="number" id="c_context_rounds"></div>
        <div><label>闲置过期（秒）</label><input type="number" id="c_context_ttl"></div>
      </div>
      <div class="row">
        <div><label>同人冷却（秒）</label><input type="number" id="c_group_cooldown"></div>
        <div><label>接话概率（prob 模式）</label><input type="number" step="0.05" id="c_prob"></div>
      </div>
      <div class="row">
        <div><label>说完后安静多久（秒，@ 不受限）</label><input type="number" id="c_speak_cd"></div>
        <div><label>非提问的接话概率（smart 模式）</label><input type="number" step="0.01" id="c_smart_p"></div>
      </div>
      <div class="row">
        <div><label>深度触发词</label><input type="text" id="c_reasoning_trigger"></div>
        <div><label>机器人名字</label><input type="text" id="c_bot_name"></div>
      </div>
      <label>昵称触发词（逗号分隔，at 模式下生效）</label><input type="text" id="c_nicknames">
    </div>
    <div class="panel">
      <h2>风控</h2>
      <div class="switch"><input type="checkbox" id="c_anti"><label for="c_anti" style="margin:0">开启反刷屏</label></div>
      <div class="row">
        <div><label>窗口（秒）</label><input type="number" id="c_window"></div>
        <div><label>条数阈值</label><input type="number" id="c_limit"></div>
      </div>
      <div class="row">
        <div><label>禁言时长（秒）</label><input type="number" id="c_mute"></div>
        <div><label>警告几次自动禁言</label><input type="number" id="c_warn_n"></div>
      </div>
      <div class="row">
        <div><label>自动禁言时长（秒）</label><input type="number" id="c_warn_s"></div>
        <div><label>主人 QQ（逗号分隔）</label><input type="text" id="c_supers"></div>
      </div>
      <div style="margin-top:16px"><button class="btn primary" onclick="saveCfg()">保存配置</button></div>
    </div>
  </div>

  <div id="p-log" style="display:none">
    <div class="panel">
      <h2>运行日志 <span class="hint" id="logtip"></span></h2>
      <button class="btn" onclick="loadLogs()">刷新</button>
      <div class="switch" style="display:inline-flex;margin-left:10px">
        <input type="checkbox" id="autolog" checked><label for="autolog" style="margin:0">自动刷新</label>
      </div>
      <pre id="logs" style="margin-top:12px">加载中…</pre>
    </div>
  </div>

  <div id="p-ses" style="display:none">
    <div class="panel">
      <h2>对话上下文 <span class="hint" id="seshint"></span></h2>
      <button class="btn" onclick="loadSessions()">刷新</button>
      <button class="btn danger" onclick="clearAllSessions()">全部清空</button>
      <table style="margin-top:12px">
        <thead><tr><th>会话</th><th>轮数</th><th>闲置</th><th>最后一句</th><th></th></tr></thead>
        <tbody id="sesbody"></tbody>
      </table>
    </div>
  </div>

  <div id="p-rem" style="display:none">
    <div class="panel">
      <h2>待发提醒 <span class="hint" id="remhint"></span></h2>
      <button class="btn" onclick="loadReminders()">刷新</button>
      <table style="margin-top:12px">
        <thead><tr><th>#</th><th>群号</th><th>内容</th><th>还有</th><th></th></tr></thead>
        <tbody id="rembody"></tbody>
      </table>
    </div>
  </div>

  <div id="p-grd" style="display:none">
    <div class="cards" id="grdcards"></div>
    <div class="panel">
      <h2>试判一段文本 <span class="hint">只判定，不会真的处置 —— 调提示词时用这个</span></h2>
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <input type="text" id="grdtest" placeholder="粘贴一句想试试的话，比如「你这个废物东西」"
               style="flex:1;min-width:220px">
        <button class="btn primary" onclick="guardTest()">判定</button>
      </div>
      <div id="grdresult" class="hint" style="margin-top:10px;line-height:1.9"></div>
    </div>
    <div class="panel">
      <h2>处置阈值 <span class="hint">判到几级做什么</span></h2>
      <div class="row">
        <div><label>封禁（拉黑 + 踢出）</label><input type="number" min="1" max="4" id="grd_ban"></div>
        <div><label>禁言</label><input type="number" min="1" max="4" id="grd_mute"></div>
        <div><label>撤回 + 警告</label><input type="number" min="1" max="4" id="grd_recall"></div>
        <div><label>把握门槛（低于此值只记录）</label><input type="number" step="0.05" min="0" max="1" id="grd_conf"></div>
        <div><label>随机抽查（0 = 只审可疑的）</label><input type="number" step="0.01" min="0" max="1" id="grd_sample"></div>
        <div><label>AI 禁言时长（秒）</label><input type="number" id="grd_msec"></div>
      </div>
      <div style="margin-top:12px">
        <button class="btn primary" onclick="guardSave()">保存阈值</button>
        <button class="btn" onclick="guardToggle()" id="grd_toggle">开关</button>
      </div>
      <div class="hint" style="margin-top:10px">
        L0 正常 · L1 轻微 · L2 不当 · L3 严重 · L4 极端。
        阈值设成 4 表示「只有极端内容才封」，设成 2 会非常激进，别这么干。
      </div>
    </div>
    <div class="panel">
      <h2>判定记录 <span class="hint" id="grdhint"></span></h2>
      <button class="btn" onclick="loadGuard()">刷新</button>
      <table style="margin-top:12px">
        <thead><tr><th>时间</th><th>谁</th><th>等级</th><th>动作</th><th>内容 / 理由</th></tr></thead>
        <tbody id="grdbody"></tbody>
      </table>
    </div>
  </div>

  <div id="p-stat" style="display:none">
    <div class="cards" id="statcards"></div>
    <div class="panel">
      <h2>最近 7 天消息量</h2>
      <div id="chart"></div>
    </div>
    <div class="panel">
      <h2>命令使用排行</h2>
      <table><thead><tr><th>命令</th><th>次数</th></tr></thead><tbody id="cmdtbody"></tbody></table>
    </div>
    <div class="panel">
      <h2>各群积分榜</h2>
      <div id="pointsbox" class="hint">加载中…</div>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
let OV = {}, GROUPS = [], CURG = 0;

function toast(m){const t=document.getElementById('toast');t.textContent=m;t.classList.add('on');
  clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('on'),1900);}

async function api(path, opt){
  const r = await fetch(path, Object.assign({headers:{'Content-Type':'application/json'}}, opt||{}));
  if(r.status===401){location.reload();throw new Error('401');}
  return r.json();
}

document.querySelectorAll('.tab').forEach(el=>el.onclick=()=>{
  document.querySelectorAll('.tab').forEach(x=>x.classList.remove('on'));
  el.classList.add('on');
  ['ov','grp','ses','rem','stat','grd','cfg','log'].forEach(k=>{
    document.getElementById('p-'+k).style.display = (k===el.dataset.t) ? '' : 'none';
  });
  if(el.dataset.t==='grp') loadGroups();
  if(el.dataset.t==='ses') loadSessions();
  if(el.dataset.t==='rem') loadReminders();
  if(el.dataset.t==='stat') loadStats();
  if(el.dataset.t==='grd') loadGuard();
  if(el.dataset.t==='cfg') loadCfg();
  if(el.dataset.t==='log') loadLogs();
});

async function loadOV(){
  const j = await api('/api/overview'); if(!j.ok) return;
  OV = j.data;
  document.getElementById('botname').textContent = OV.name;
  document.getElementById('meta').textContent =
    (OV.self_id ? '账号 '+OV.self_id : '未登录') + ' · 运行 ' + OV.uptime + ' · ' + OV.ws_url;
  const b = document.getElementById('badge');
  b.textContent = OV.online ? '在线' : '离线';
  b.className = 'badge' + (OV.online ? '' : ' off');
  document.getElementById('offline').style.display = OV.online ? 'none' : '';
  const c = [
    ['消息总数', OV.stats.messages],
    ['命令调用', OV.stats.commands],
    ['AI 对话', OV.stats.ai],
    ['异常', OV.stats.errors],
    ['DeepSeek', OV.deepseek.ready ? OV.deepseek.model : '未配置 Key'],
    ['Token 消耗', OV.deepseek.tokens],
    ['会话上下文', OV.sessions],
    ['所在群', OV.groups],
  ];
  document.getElementById('cards').innerHTML = c.map(([k,v])=>
    `<div class="card"><div class="k">${k}</div><div class="v ${String(v).length>10?'sm':''}">${v}</div></div>`).join('');
}

async function act(a){
  const j = await api('/api/action',{method:'POST',body:JSON.stringify({action:a})});
  toast(j.ok ? (j.msg||'完成') : ('失败：'+j.error));
  if(a==='reload') loadCfg();
}

async function loadGroups(){
  const j = await api('/api/groups');
  if(!j.ok){document.getElementById('grphint').textContent='取群列表失败：'+j.error;return;}
  GROUPS = j.data;
  document.getElementById('grphint').textContent = GROUPS.length ? '共 '+GROUPS.length+' 个群，点一行展开编辑' : '机器人还没加入任何群';
  document.getElementById('grpbody').innerHTML = GROUPS.map(g=>`
    <tr onclick="editGroup(${g.group_id})" style="cursor:pointer">
      <td>${g.group_id}</td><td>${esc(g.group_name)}</td>
      <td>${g.member_count??'-'}</td><td>${g.stats.messages||0}</td>
      <td>${g.welcome?'开':'关'}</td><td>${g.words.length}</td>
      <td>${Object.keys(g.keywords||{}).length}</td>
    </tr>`).join('');
}

function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}

function editGroup(gid){
  const g = GROUPS.find(x=>x.group_id===gid); if(!g) return;
  CURG = gid;
  document.getElementById('grpedit').style.display='';
  document.getElementById('grptitle').textContent = '编辑群 ' + g.group_name + '（' + gid + '）';
  document.getElementById('g_welcome').checked = g.welcome;
  document.getElementById('g_text').value = g.welcome_text || '';
  document.getElementById('g_words').value = (g.words||[]).join('\\n');
  document.getElementById('g_keywords').value = Object.entries(g.keywords||{})
    .map(([k,v])=>k+'='+v).join('\\n');
  document.getElementById('grpedit').scrollIntoView({behavior:'smooth',block:'nearest'});
}

async function saveGroup(){
  const j = await api('/api/group',{method:'POST',body:JSON.stringify({
    group_id: CURG,
    welcome: document.getElementById('g_welcome').checked,
    welcome_text: document.getElementById('g_text').value,
    words: document.getElementById('g_words').value,
    keywords: document.getElementById('g_keywords').value,
  })});
  toast(j.ok?'已保存':'失败：'+j.error);
  loadGroups();
}

async function testSend(){
  const j = await api('/api/action',{method:'POST',body:JSON.stringify({
    action:'test_send', group_id:CURG, text:'小小坤ovo 上线测试，收到请忽略~'})});
  toast(j.ok?'已发送到群里':'发送失败：'+j.error);
}

async function loadCfg(){
  const j = await api('/api/config'); if(!j.ok) return;
  const d = j.data;
  const set=(id,v)=>{const el=document.getElementById(id); if(el) el.value = v===null||v===undefined?'':v;};
  set('c_api_key', d['deepseek.api_key']);
  set('c_model', d['deepseek.model']);
  set('c_reasoner_model', d['deepseek.reasoner_model']);
  set('c_temperature', d['deepseek.temperature']);
  set('c_max_tokens', d['deepseek.max_tokens']);
  set('c_system_prompt', d['chat.system_prompt']);
  set('c_context_rounds', d['chat.context_rounds']);
  set('c_context_ttl', d['chat.context_ttl']);
  set('c_group_cooldown', d['chat.group_cooldown']);
  set('c_prob', d['chat.group_reply_probability']);
  set('c_speak_cd', d['chat.group_speak_cooldown']);
  set('c_smart_p', d['chat.smart_reply_probability']);
  document.getElementById('c_group_trigger').value = d['bot.group_trigger'] || 'all';
  set('c_reasoning_trigger', d['chat.reasoning_trigger']);
  set('c_bot_name', d['bot.name']);
  set('c_nicknames', (d['bot.nicknames']||[]).join(', '));
  document.getElementById('c_anti').checked = !!d['group.anti_spam.enabled'];
  set('c_window', d['group.anti_spam.window']);
  set('c_limit', d['group.anti_spam.limit']);
  set('c_mute', d['group.anti_spam.mute']);
  set('c_warn_n', d['group.warn_mute_threshold']);
  set('c_warn_s', d['group.warn_mute_seconds']);
  set('c_supers', (d['permission.superusers']||[]).join(', '));
}

async function saveCfg(){
  const body = {
    'deepseek.api_key': document.getElementById('c_api_key').value,
    'deepseek.model': document.getElementById('c_model').value,
    'deepseek.reasoner_model': document.getElementById('c_reasoner_model').value,
    'deepseek.temperature': document.getElementById('c_temperature').value,
    'deepseek.max_tokens': document.getElementById('c_max_tokens').value,
    'chat.system_prompt': document.getElementById('c_system_prompt').value,
    'chat.context_rounds': document.getElementById('c_context_rounds').value,
    'chat.context_ttl': document.getElementById('c_context_ttl').value,
    'chat.group_cooldown': document.getElementById('c_group_cooldown').value,
    'chat.group_reply_probability': document.getElementById('c_prob').value,
    'chat.group_speak_cooldown': document.getElementById('c_speak_cd').value,
    'chat.smart_reply_probability': document.getElementById('c_smart_p').value,
    'bot.group_trigger': document.getElementById('c_group_trigger').value,
    'chat.reasoning_trigger': document.getElementById('c_reasoning_trigger').value,
    'bot.name': document.getElementById('c_bot_name').value,
    'bot.nicknames': document.getElementById('c_nicknames').value,
    'group.anti_spam.enabled': document.getElementById('c_anti').checked,
    'group.anti_spam.window': document.getElementById('c_window').value,
    'group.anti_spam.limit': document.getElementById('c_limit').value,
    'group.anti_spam.mute': document.getElementById('c_mute').value,
    'group.warn_mute_threshold': document.getElementById('c_warn_n').value,
    'group.warn_mute_seconds': document.getElementById('c_warn_s').value,
    'permission.superusers': document.getElementById('c_supers').value,
  };
  const j = await api('/api/config',{method:'POST',body:JSON.stringify(body)});
  if(j.ok){toast('已保存 '+(j.data.changed.length)+' 项，立即生效');loadOV();}
  else toast('失败：'+j.error);
}

async function loadLogs(){
  const j = await api('/api/logs?lines=300');
  const box = document.getElementById('logs');
  if(!j.ok){box.textContent='读日志失败：'+j.error;return;}
  box.textContent = j.data || '(还没有日志)';
  box.scrollTop = box.scrollHeight;
  document.getElementById('logtip').textContent = '· ' + new Date().toLocaleTimeString('zh-CN');
}

function fmtAge(sec){
  if(sec < 60) return sec + ' 秒';
  if(sec < 3600) return Math.floor(sec/60) + ' 分钟';
  if(sec < 86400) return Math.floor(sec/3600) + ' 小时';
  return Math.floor(sec/86400) + ' 天';
}

async function loadSessions(){
  const j = await api('/api/sessions');
  const body = document.getElementById('sesbody');
  if(!j.ok){ document.getElementById('seshint').textContent = '取会话失败'; return; }
  const rows = j.data;
  document.getElementById('seshint').textContent = '· ' + rows.length + ' 个会话';
  if(!rows.length){ body.innerHTML = '<tr><td colspan="5" style="color:#93a1b5">还没有上下文</td></tr>'; return; }
  body.innerHTML = rows.map(r=>`<tr>
    <td>${esc(r.key)}</td><td>${r.turns}</td><td>${fmtAge(r.age)}</td>
    <td style="color:#6b7a90">${esc(r.preview||'—')}</td>
    <td><button class="btn" onclick="clearSession('${esc(r.key)}')">清空</button></td>
  </tr>`).join('');
}

async function clearSession(key){
  const j = await api('/api/session/clear',{method:'POST',body:JSON.stringify({key})});
  toast(j.ok ? j.msg : ('失败：'+j.error));
  loadSessions();
}

async function clearAllSessions(){
  const j = await api('/api/session/clear',{method:'POST',body:JSON.stringify({})});
  toast(j.ok ? j.msg : ('失败：'+j.error));
  loadSessions();
}

async function loadReminders(){
  const j = await api('/api/reminders');
  const body = document.getElementById('rembody');
  if(!j.ok){ document.getElementById('remhint').textContent = '取提醒失败'; return; }
  const rows = j.data;
  document.getElementById('remhint').textContent = '· ' + rows.length + ' 条待发';
  if(!rows.length){ body.innerHTML = '<tr><td colspan="5" style="color:#93a1b5">没有待发的提醒</td></tr>'; return; }
  body.innerHTML = rows.map(r=>`<tr>
    <td>#${r.id}</td><td>${r.gid}</td><td>${esc(r.text)}</td>
    <td>${r.left > 0 ? fmtAge(r.left) : '已到期'}</td>
    <td><button class="btn" onclick="delReminder(${r.id},${r.gid})">删除</button></td>
  </tr>`).join('');
}

async function delReminder(id,gid){
  const j = await api('/api/reminder/delete',{method:'POST',body:JSON.stringify({id,gid})});
  toast(j.ok ? '已删除' : ('失败：'+j.error));
  loadReminders();
}

function barChart(rows){
  const max = Math.max(1, ...rows.map(r=>r.count));
  return '<div style="display:flex;align-items:flex-end;gap:10px;height:168px;padding-top:10px">' +
    rows.map(r=>`<div style="flex:1;display:flex;flex-direction:column;align-items:center;gap:6px">
      <div style="font-size:11px;color:#8494a9">${r.count}</div>
      <div style="width:100%;max-width:52px;height:${Math.max(3, r.count/max*112)}px;
                  background:linear-gradient(180deg,#6f9bff,#2f6fed);border-radius:6px 6px 3px 3px"></div>
      <div style="font-size:11px;color:#93a1b5">${r.date.slice(5)}</div>
    </div>`).join('') + '</div>';
}

async function loadStats(){
  const j = await api('/api/stats');
  if(!j.ok) return;
  const d = j.data;
  const cards = [
    ['今日消息', d.today], ['累计消息', d.total],
    ['上下文会话', d.sessions], ['待发提醒', d.reminders],
  ];
  document.getElementById('statcards').innerHTML = cards.map(([k,v])=>
    `<div class="card"><div class="k">${k}</div><div class="v">${v}</div></div>`).join('');
  document.getElementById('chart').innerHTML = barChart(d.trend);
  const tb = document.getElementById('cmdtbody');
  tb.innerHTML = d.cmd_top.length
    ? d.cmd_top.map(c=>`<tr><td>${esc(c.name)}</td><td>${c.count}</td></tr>`).join('')
    : '<tr><td colspan="2" style="color:#93a1b5">还没有命令调用</td></tr>';
  const pb = document.getElementById('pointsbox');
  pb.innerHTML = d.points.length
    ? d.points.map(g=>`<div style="margin-bottom:14px">
        <div style="font-size:13px;color:#2b3950;margin-bottom:6px">群 ${g.group_id}</div>
        <div style="font-size:13px;color:#6b7a90">` +
        g.top.map((t,i)=>`${['🥇','🥈','🥉'][i]||((i+1)+'.')} ${esc(t.name)} ${t.points} 分`).join('　') +
        '</div></div>').join('')
    : '还没有人签到';
}

async function memberAct(act){
  const uid = document.getElementById('g_uid').value.trim();
  if(!/^\d{5,12}$/.test(uid)){ toast('先填正确的 QQ 号'); return; }
  if(act === 'kick' && !confirm('确定把 ' + uid + ' 移出群聊？')) return;
  const j = await api('/api/group/action',{method:'POST',body:JSON.stringify({
    group_id: CURG, user_id: Number(uid), action: act, seconds: 600
  })});
  toast(j.ok ? (j.msg || '完成') : ('失败：'+j.error));
}

const GRD_ACTION = {'ban':'封禁','mute':'禁言','recall':'撤回','review':'仅记录','none':'放行','ban-failed':'封禁失败'};
let GRDSTAT = {};

async function loadGuard(){
  const j = await api('/api/guard');
  if(!j.ok){ toast('取哨兵状态失败'); return; }
  const s = j.data.status;
  GRDSTAT = s;
  const cards = [
    ['状态', s.enabled ? '开启' : '关闭'],
    ['送审', s.stats.checked],
    ['命中', s.stats.flagged],
    ['封禁', s.stats.banned],
    ['出错', s.stats.errors],
  ];
  document.getElementById('grdcards').innerHTML = cards.map(([k,v])=>
    `<div class="card"><div class="k">${k}</div><div class="v">${v}</div></div>`).join('');
  document.getElementById('grd_ban').value = s.ban_level;
  document.getElementById('grd_mute').value = s.mute_level;
  document.getElementById('grd_recall').value = s.recall_level;
  document.getElementById('grd_conf').value = s.min_confidence;
  document.getElementById('grd_sample').value = s.sample_rate;
  document.getElementById('grd_msec').value = s.mute_seconds || 600;
  document.getElementById('grd_toggle').textContent = s.enabled ? '关掉哨兵' : '开启哨兵';
  const rows = j.data.records || [];
  document.getElementById('grdhint').textContent =
    '· 最近 ' + rows.length + ' 条' + (s.ready ? '' : '（DeepSeek 未配置，只会记录不会判定）');
  const tb = document.getElementById('grdbody');
  if(!rows.length){
    tb.innerHTML = '<tr><td colspan="5" style="color:#93a1b5">还没有判定记录</td></tr>';
    return;
  }
  tb.innerHTML = rows.map(r=>{
    const act = GRD_ACTION[r.action] || r.action;
    const color = r.action === 'ban' ? '#d9534f' : ((r.error || r.action === 'review') ? '#c8860d' : '#6b7a90');
    return `<tr>
      <td style="color:#8494a9;white-space:nowrap">${esc((r.time||'').slice(6))}</td>
      <td>${esc(r.name||'')}<div style="color:#a4b1c4;font-size:11px">${r.uid}</div></td>
      <td>L${r.level} ${esc(r.category||'')}</td>
      <td style="color:${color}">${esc(act)}</td>
      <td style="color:#6b7a90">${esc((r.text||'').slice(0,50))}
        <div style="color:#9aa7b8;font-size:11px">${esc(r.error||r.reason||'')}</div></td>
    </tr>`;
  }).join('');
}

async function guardTest(){
  const text = document.getElementById('grdtest').value.trim();
  if(!text){ toast('先写点什么'); return; }
  const box = document.getElementById('grdresult');
  box.textContent = '判定中…';
  const j = await api('/api/guard/test', {method:'POST', body:JSON.stringify({text})});
  if(!j.ok){ box.textContent = '失败：' + (j.error||''); return; }
  const d = j.data;
  if(d.error){
    box.innerHTML = `<b style="color:#d9534f">判定失败</b>：${esc(d.error)}<br>（这种时候不会处置任何人）`;
    return;
  }
  const color = d.level >= 4 ? '#d9534f' : (d.level >= 2 ? '#c8860d' : '#3a9b6e');
  box.innerHTML = `<b style="color:${color}">L${d.level} · ${esc(d.category)}</b>`
    + `（把握 ${Math.round((d.confidence||0)*100)}%）<br>`
    + `理由：${esc(d.reason||'—')}<br>`
    + `若是群消息，会执行：<b>${esc(GRD_ACTION[d.action]||d.action)}</b>`;
}

async function guardSave(){
  const patch = {
    'guard.ban_level': Number(document.getElementById('grd_ban').value),
    'guard.mute_level': Number(document.getElementById('grd_mute').value),
    'guard.recall_level': Number(document.getElementById('grd_recall').value),
    'guard.min_confidence': Number(document.getElementById('grd_conf').value),
    'guard.sample_rate': Number(document.getElementById('grd_sample').value),
    'guard.mute_seconds': Number(document.getElementById('grd_msec').value),
  };
  const j = await api('/api/config', {method:'POST', body:JSON.stringify(patch)});
  toast(j.ok ? '阈值已保存' : ('失败：'+j.error));
  loadGuard();
}

async function guardToggle(){
  const j = await api('/api/config', {method:'POST',
    body:JSON.stringify({'guard.enabled': !GRDSTAT.enabled})});
  toast(j.ok ? (GRDSTAT.enabled ? '哨兵已关闭' : '哨兵已开启') : ('失败：'+j.error));
  loadGuard();
}

async function logout(){await api('/api/logout',{method:'POST'});location.reload();}

loadOV();
setInterval(loadOV, 5000);
setInterval(()=>{if(document.getElementById('autolog').checked &&
  document.getElementById('p-log').style.display!=='none') loadLogs();}, 5000);
</script></body></html>"""
