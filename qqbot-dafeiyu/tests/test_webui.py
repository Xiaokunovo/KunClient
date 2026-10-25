"""Web 控制台离线测试：假 OneBot + 真 WebUI。

    python tests/test_webui.py
"""

from __future__ import annotations

import asyncio
import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from bot import commands  # noqa: F401,E402
from bot.app import Bot  # noqa: E402
from bot.config import Config  # noqa: E402
from bot.router import router  # noqa: E402
from fake_onebot import SELF_ID, FakeOneBot  # noqa: E402

PORT = 30099
WEB = 18088
PASS, FAIL = [], []


def check(name: str, cond: bool, detail: str = "") -> None:
    (PASS if cond else FAIL).append(name if cond else f"{name} {detail}")
    print(f"  {'✓' if cond else '✗'} {name}  {'' if cond else detail}")


async def http(path: str, method: str = "GET", body: dict | None = None, cookie: str = ""):
    import aiohttp

    url = f"http://127.0.0.1:{WEB}{path}"
    headers = {"Content-Type": "application/json"}
    if cookie:
        headers["Cookie"] = cookie
    async with aiohttp.ClientSession() as s:
        async with s.request(method, url, json=body, headers=headers, timeout=aiohttp.ClientTimeout(total=15)) as r:
            text = await r.text()
            return r.status, text, r.headers


async def run() -> None:
    srv = FakeOneBot(PORT)
    await srv.start()
    tmp = Path(tempfile.mkdtemp(prefix="webui-test-"))

    cfg = Config({
        "bot": {"name": "小小坤ovo", "nicknames": ["小小坤ovo"]},
        "onebot": {"ws_url": f"ws://127.0.0.1:{PORT}"},
        "deepseek": {"api_key": "sk-test-1234567890abcd"},
        "webui": {"enabled": True, "host": "127.0.0.1", "port": WEB, "token": "testtoken123"},
        "group": {"anti_spam": {"enabled": False}},
        "log": {"level": "WARNING"},
    }, tmp / "config.json")

    bot = Bot(cfg, router)

    from bot.deepseek import Reply

    async def guard_stub(messages, *, model=None, temperature=None, max_tokens=None, json_mode=False):
        return Reply(content='{"level": 3, "category": "辱骂", "reason": "测试判定", "confidence": 0.9}')

    bot.ds.chat = guard_stub  # type: ignore[method-assign]

    task = asyncio.create_task(bot.start())

    import time

    end = time.time() + 6
    while time.time() < end and bot.api.self_id != SELF_ID:
        await asyncio.sleep(0.05)
    await asyncio.sleep(0.4)

    print("\n[1] 页面与鉴权")
    st, html, _ = await http("/")
    check("未登录返回登录页", st == 200 and "访问口令" in html, f"status={st}")
    st, _, _ = await http("/api/overview")
    check("未登录访问 API 被拒", st == 401, f"status={st}")
    st, body, _ = await http("/api/login", "POST", {"token": "wrong"})
    check("错误口令被拒", st == 403, f"status={st}")
    st, body, hdr = await http("/api/login", "POST", {"token": "testtoken123"})
    check("正确口令登录成功", st == 200 and '"ok": true' in body.replace("\n", ""), body[:80])
    cookie = ""
    for k, v in hdr.items():
        if k.lower() == "set-cookie":
            cookie = v.split(";")[0]
    check("下发了 cookie", cookie.startswith("kc_webui="), cookie)

    st, html, _ = await http("/", cookie=cookie)
    check("登录后是控制台", st == 200 and "概览" in html and "群管理" in html, f"status={st}")

    print("\n[2] 概览")
    st, body, _ = await http("/api/overview", cookie=cookie)
    import json

    data = json.loads(body)["data"]
    check("概览可读", st == 200 and data["online"] is True, f"status={st}")
    check("账号正确", data["self_id"] == SELF_ID, str(data["self_id"]))
    check("名字已改为 小小坤ovo", data["name"] == "小小坤ovo", data["name"])
    check("DeepSeek 状态可读", data["deepseek"]["ready"] is True, str(data["deepseek"]))

    print("\n[3] 配置读写")
    st, body, _ = await http("/api/config", cookie=cookie)
    conf = json.loads(body)["data"]
    check("API Key 被打码", conf["deepseek.api_key"].startswith("sk-****"), conf["deepseek.api_key"])
    st, body, _ = await http("/api/config", "POST",
                             {"chat.context_rounds": 12, "group.anti_spam.limit": 9,
                              "deepseek.api_key": conf["deepseek.api_key"]}, cookie)
    check("保存配置成功", st == 200 and json.loads(body)["ok"], body[:120])
    check("轮数已写入", cfg.get("chat.context_rounds") == 12, str(cfg.get("chat.context_rounds")))
    check("打码的 key 没覆盖真 key", cfg.get("deepseek.api_key") == "sk-test-1234567890abcd",
          cfg.get("deepseek.api_key"))
    st, body, _ = await http("/api/config", cookie=cookie)
    check("回读一致", json.loads(body)["data"]["chat.context_rounds"] == 12, body[:120])

    print("\n[4] 群管理")
    st, body, _ = await http("/api/groups", cookie=cookie)
    groups = json.loads(body)["data"]
    check("拿到群列表", st == 200 and len(groups) == 2, f"status={st} n={len(groups) if st==200 else '?'}")
    if groups:
        gid = groups[0]["group_id"]
        st, body, _ = await http("/api/group", "POST",
                                 {"group_id": gid, "welcome": False, "welcome_text": "欢迎 {at}",
                                  "words": "广告\n刷屏",
                                  "keywords": "打卡=已记录 {at}\n价格=私聊"}, cookie)
        check("保存群设置", st == 200 and json.loads(body)["ok"], body[:100])
        check("违禁词已落库", bot.state.words(gid) == ["广告", "刷屏"], str(bot.state.words(gid)))
        check("关键词已落库", bot.state.keywords(gid) == {"打卡": "已记录 {at}", "价格": "私聊"},
              str(bot.state.keywords(gid)))
        st, body, _ = await http("/api/groups", cookie=cookie)
        g = [x for x in json.loads(body)["data"] if x["group_id"] == gid][0]
        check("回读群设置", g["welcome"] is False and g["words"] == ["广告", "刷屏"], str(g)[:140])
        check("回读关键词", len(g.get("keywords") or {}) == 2, str(g.get("keywords")))


    print("\n[4b] 会话 / 提醒 / 统计 / 群操作")
    bot.memory.append("group:555000111", "你好", "在的")
    bot.memory.append("private:20003", "私聊测试", "收到")
    bot.metrics.bump(5)
    bot.cmd_counts["帮助"] = 3
    bot.cmd_counts["签到"] = 7
    bot.reminders.add(555000111, 20003, "开会", 600)

    st, body, _ = await http("/api/sessions", cookie=cookie)
    sess = json.loads(body)["data"]
    check("会话列表有两条", st == 200 and len(sess) == 2, f"status={st} n={len(sess)}")
    check("会话带轮数", any(x["turns"] == 1 for x in sess), str(sess)[:140])

    st, body, _ = await http("/api/session/clear", "POST", {"key": "private:20003"}, cookie)
    check("单条会话可清", st == 200 and json.loads(body)["ok"], body[:100])
    st, body, _ = await http("/api/sessions", cookie=cookie)
    check("清完只剩一条", len(json.loads(body)["data"]) == 1, body[:140])

    st, body, _ = await http("/api/reminders", cookie=cookie)
    rems = json.loads(body)["data"]
    check("提醒列表", st == 200 and len(rems) == 1, body[:140])
    if rems:
        st, body, _ = await http("/api/reminder/delete", "POST",
                                 {"id": rems[0]["id"], "gid": rems[0]["gid"]}, cookie)
        check("提醒可删除", st == 200 and json.loads(body)["ok"], body[:100])
        check("删完为空", bot.reminders.count() == 0, str(bot.reminders.count()))

    st, body, _ = await http("/api/stats", cookie=cookie)
    st_data = json.loads(body)["data"]
    check("统计有 7 天趋势", st == 200 and len(st_data["trend"]) == 7, str(len(st_data.get("trend", []))))
    check("今日消息计数", st_data["today"] >= 5, str(st_data["today"]))
    check("命令排行取到", st_data["cmd_top"] and st_data["cmd_top"][0]["name"] == "签到",
          str(st_data["cmd_top"])[:100])

    st, body, _ = await http("/api/group/action", "POST",
                             {"group_id": 555000111, "user_id": 20004, "action": "mute", "seconds": 600}, cookie)
    check("后台禁言", st == 200 and json.loads(body)["ok"], body[:120])
    check("真的调了禁言", any(a == "set_group_ban" for a, _ in srv.calls), str(srv.calls)[-3:])
    st, body, _ = await http("/api/group/action", "POST",
                             {"group_id": 555000111, "user_id": 20004, "action": "unmute"}, cookie)
    check("后台解禁", st == 200 and json.loads(body)["ok"], body[:120])

    print("\n[4c] 哨兵接口")
    st, body, _ = await http("/api/guard", cookie=cookie)
    gd = json.loads(body)["data"]
    check("取到哨兵状态", st == 200 and gd["status"]["enabled"] is True, body[:140])
    check("状态含阈值", gd["status"]["ban_level"] == 4 and gd["status"]["mute_level"] == 3,
          str(gd["status"])[:160])
    check("初始无记录", isinstance(gd["records"], list), str(gd.get("records"))[:80])

    bans0 = len(srv.find("set_group_ban"))   # 前面 [4b] 已经调过一次禁言，得比增量
    kicks0 = len(srv.find("set_group_kick"))
    st, body, _ = await http("/api/guard/test", "POST", {"text": "你这个废物东西"}, cookie)
    d = json.loads(body)["data"]
    check("试判返回等级", st == 200 and d["level"] == 3, body[:160])
    check("试判给出动作", d["action"] == "mute", str(d)[:160])
    check("试判不执行", len(srv.find("set_group_ban")) == bans0
          and len(srv.find("set_group_kick")) == kicks0, str(srv.calls)[-3:])

    st, body, _ = await http("/api/guard/test", "POST", {"text": ""}, cookie)
    check("空文本被拒", st == 400, f"status={st}")

    st, body, _ = await http("/api/config", "POST", {"guard.enabled": False,
                                                     "guard.ban_level": 3}, cookie)
    check("能改哨兵配置", st == 200 and cfg.get("guard.enabled") is False, body[:120])
    check("阈值也落了", cfg.get("guard.ban_level") == 3, str(cfg.get("guard.ban_level")))
    st, body, _ = await http("/api/config", "POST", {"guard.enabled": True, "guard.ban_level": 4}, cookie)

    print("\n[5] 日志与动作")
    st, body, _ = await http("/api/logs?lines=50", cookie=cookie)
    check("能读日志", st == 200 and json.loads(body)["ok"], f"status={st}")
    st, body, _ = await http("/api/action", "POST", {"action": "clear_context"}, cookie)
    check("清空上下文", st == 200 and json.loads(body)["ok"], body[:100])
    st, body, _ = await http("/api/action", "POST", {"action": "reload"}, cookie)
    check("重载配置", st == 200 and json.loads(body)["ok"], body[:100])
    st, body, _ = await http("/api/action", "POST", {"action": "test_send", "group_id": 555000111}, cookie)
    check("测试发消息", st == 200 and json.loads(body)["ok"], body[:120])
    check("真的发出去了", any(s.get("group_id") == 555000111 for s in srv.sent), str(srv.sent)[:120])

    print("\n[6] 退出")
    st, body, hdr = await http("/api/logout", "POST", cookie=cookie)
    check("退出成功", st == 200, f"status={st}")

    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    await bot.shutdown()
    await srv.stop()

    print("\n" + "=" * 46)
    print(f"通过 {len(PASS)} 项，失败 {len(FAIL)} 项")
    if FAIL:
        for f in FAIL:
            print("  ✗", f)
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        asyncio.run(run())
    except SystemExit:
        raise
    except Exception:  # noqa: BLE001
        traceback.print_exc()
        raise SystemExit(2)
