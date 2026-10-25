"""端到端离线测试：假 OneBot 服务端 + 真机器人。

跑法：
    python tests/test_e2e.py

不需要 QQ、不需要 DeepSeek Key（模型调用被替换成固定回复）。
"""

from __future__ import annotations

import asyncio
import sys
import time
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from bot import commands  # noqa: F401,E402  注册命令
from bot.app import Bot  # noqa: E402
from bot.config import DEFAULTS, Config  # noqa: E402
from bot.deepseek import Reply  # noqa: E402
from bot.router import router  # noqa: E402
from fake_onebot import SELF_ID, FakeOneBot  # noqa: E402

PORT = 30099
GROUP = 555000111
OWNER = 20001
ADMIN = 20002
MEMBER = 20003
BAD = 20004

PASS, FAIL = [], []


def check(name: str, cond: bool, detail: str = "") -> None:
    if cond:
        PASS.append(name)
        print(f"  ✓ {name}")
    else:
        FAIL.append(f"{name} {detail}")
        print(f"  ✗ {name}  {detail}")


async def step(srv) -> None:
    """清空记录并等一拍，避免上一条用例的迟到回包串到下一轮。"""
    srv.reset()
    await asyncio.sleep(0.25)


async def wait_until(pred, timeout: float = 3.0) -> bool:
    import time

    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        await asyncio.sleep(0.03)
    return False


# 哨兵判定用的「模型回答」，测试里随时改它来模拟不同判定
GUARD = {"content": '{"level": 0, "category": "正常", "reason": "测试放行", "confidence": 0.95}'}


async def stub_chat(messages, *, model=None, temperature=None, max_tokens=None, json_mode=False):
    """替身：把最后一条用户消息回显，方便断言。

    json_mode=True 时是内容哨兵在问「这条违规吗」，返回 GUARD["content"]，
    和闲聊回复分开 —— 否则聊天回复会变成一串 JSON，把断言全带偏。
    """
    last = messages[-1]["content"] if messages else ""
    await asyncio.sleep(0.02)
    if json_mode:
        return Reply(content=GUARD["content"], model=model or "deepseek-chat")
    return Reply(content=f"咕噜：{last}", model=model or "deepseek-chat", prompt_tokens=10, completion_tokens=5)


def build_config(tmp: Path) -> Config:
    data = {
        "bot": {"name": "小小坤ovo", "nickname_trigger": True, "group_trigger": "all"},
        "onebot": {"ws_url": f"ws://127.0.0.1:{PORT}", "token": ""},
        "deepseek": {"api_key": "sk-test", "model": "deepseek-chat"},
        "chat": {"group_cooldown": 0, "group_speak_cooldown": 0, "smart_reply_probability": 0},
        "permission": {"superusers": [], "admins": []},
        "group": {
            "welcome": True,
            "anti_spam": {"enabled": False},
            "warn_mute_threshold": 3,
            "warn_mute_seconds": 600,
        },
        "webui": {"enabled": False},
        "log": {"level": "WARNING"},
    }
    cfg = Config(data, tmp / "config.json")
    return cfg


async def run() -> None:
    srv = FakeOneBot(PORT)
    await srv.start()

    tmp = Path(tempfile.mkdtemp(prefix="dafeiyu-test-"))
    cfg = build_config(tmp)

    # 把状态文件指到临时目录，别污染真 data/
    import bot.app as app_mod

    app_mod.ROOT = tmp

    bot = Bot(cfg, router)
    bot.ds.chat = stub_chat  # type: ignore[method-assign]

    task = asyncio.create_task(bot.start())
    ok = await wait_until(lambda: bot.api.self_id == SELF_ID, 6)
    check("连接 OneBot 并取到登录信息", ok and bot.api.connected, f"self_id={bot.api.self_id}")
    await asyncio.sleep(0.25)

    print("\n[1] 帮助命令（群主）")
    await step(srv)
    await srv.push_group_msg(OWNER, "/帮助")
    await wait_until(lambda: srv.sent, 8)
    segs = srv.last_segments()
    check("帮助有回复", bool(segs), str(segs)[:80])
    check("默认发图片菜单", any(x.get("type") == "image" for x in segs), str(segs)[:120])
    await step(srv)
    await srv.push_group_msg(OWNER, "/帮助 全部")
    await wait_until(lambda: srv.sent, 6)
    text = srv.last_text()
    check("分类名不对时给提示", "没有" in text and "可选" in text, text[:80])

    print("\n[2] 普通成员越权")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/禁言 20004 10分")
    await wait_until(lambda: srv.sent)
    check("提示权限不够", "权限不够" in srv.last_text(), srv.last_text())
    check("没有真的禁言", not srv.find("set_group_ban"))

    print("\n[3] 管理员禁言 @")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/禁言 ", extra_segments=[{"type": "at", "data": {"qq": "20004"}},
                                                             {"type": "text", "data": {"text": "10分"}}])
    await wait_until(lambda: srv.find("set_group_ban") and srv.sent)
    bans = srv.find("set_group_ban")
    check("调用了 set_group_ban", bool(bans), str(srv.calls))
    if bans:
        check("目标正确", int(bans[0]["user_id"]) == BAD, str(bans[0]))
        check("时长 10 分钟 = 600s", int(bans[0]["duration"]) == 600, str(bans[0]))
    check("群里回复带了 @", any(s.get("type") == "at" for s in srv.last_segments()), str(srv.last_segments())[:120])

    print("\n[4] 群主按 QQ 号禁言 1 小时")
    await step(srv)
    await srv.push_group_msg(OWNER, "/禁言 20004 1小时")
    await wait_until(lambda: srv.find("set_group_ban") and srv.sent)
    bans = srv.find("set_group_ban")
    check("按 QQ 号解析成功", bool(bans) and int(bans[0]["user_id"]) == BAD, str(bans))
    check("时长 1 小时 = 3600s", bool(bans) and int(bans[0]["duration"]) == 3600, str(bans))

    print("\n[5] 禁言群主应被拒绝")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/禁言 20001 10分")
    await wait_until(lambda: srv.sent)
    check("拒绝禁言群主", "群主" in srv.last_text(), srv.last_text())
    check("没有调用禁言", not srv.find("set_group_ban"))

    print("\n[6] 踢人 + 拉黑")
    await step(srv)
    await srv.push_group_msg(OWNER, "/踢人 20004 拉黑")
    await wait_until(lambda: srv.find("set_group_kick") and srv.sent)
    kicks = srv.find("set_group_kick")
    check("调用了 set_group_kick", bool(kicks), str(srv.calls))
    if kicks:
        check("reject_add_request=True", kicks[0].get("reject_add_request") is True, str(kicks[0]))

    print("\n[7] @机器人 触发 AI 对话")
    await step(srv)
    await srv.push_group_msg(MEMBER, "今天天气咋样", at=True)
    await wait_until(lambda: srv.sent, 4)
    check("AI 有回复", "咕噜" in srv.last_text(), srv.last_text())
    check("回复内容带上了问题", "今天天气咋样" in srv.last_text(), srv.last_text())

    print("\n[8] 上下文记忆")
    await step(srv)
    await srv.push_group_msg(MEMBER, "记住我叫小鱼", at=True)
    await wait_until(lambda: srv.sent, 4)
    check("第二轮进入同一会话", bot.memory.turns(f"group:{GROUP}") >= 1, str(bot.memory.turns(f"group:{GROUP}")))

    print("\n[9] 违禁词自动撤回")
    await step(srv)
    bot.state.add_word(GROUP, "违规词")
    await srv.push_group_msg(MEMBER, "这里有违规词哦")
    await wait_until(lambda: srv.find("delete_msg") and srv.sent)
    check("调用了 delete_msg", bool(srv.find("delete_msg")), str(srv.calls))
    check("有警告提示", "警告" in srv.last_text(), srv.last_text())
    bot.state.del_word(GROUP, "违规词")

    print("\n[10] 入群欢迎")
    await step(srv)
    await srv.push_group_increase(20009)
    await wait_until(lambda: srv.sent)
    segs = srv.last_segments()
    check("欢迎语已发送", bool(segs), str(segs)[:120])
    check("欢迎语 @ 了新成员", any(s.get("type") == "at" and s["data"].get("qq") == "20009" for s in segs), str(segs)[:160])
    check("欢迎语带群名", "测试群" in srv.last_text(), srv.last_text())

    print("\n[11] 撤回（回复消息）")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/撤回", extra_segments=[{"type": "reply", "data": {"id": "8888"}}])
    await wait_until(lambda: srv.find("delete_msg") and srv.sent)
    dels = srv.find("delete_msg")
    check("撤回的是被回复的那条", bool(dels) and int(dels[0]["message_id"]) == 8888, str(dels))

    print("\n[12] 私聊绑定主人")
    await step(srv)
    await srv.push_private_msg(30001, "绑定主人")
    await wait_until(lambda: srv.sent)
    check("绑定成功", 30001 in [int(x) for x in cfg.get("permission.superusers")], str(cfg.get("permission.superusers")))
    check("回执正常", "主人" in srv.last_text(), srv.last_text())

    print("\n[13] 私聊闲聊")
    await step(srv)
    await srv.push_private_msg(30002, "在吗")
    await wait_until(lambda: srv.sent, 4)
    check("私聊自动对话", "咕噜" in srv.last_text(), srv.last_text())

    print("\n[14] 未知命令提示")
    await step(srv)
    await srv.push_group_msg(OWNER, "/不存在的命令")
    await wait_until(lambda: srv.sent)
    check("提示没有这个命令", "没有这个命令" in srv.last_text(), srv.last_text())

    print("\n[14b] 没有前缀的日常发言不能被当成命令")
    await step(srv)
    await srv.push_group_msg(MEMBER, "禁言他啊")
    await wait_until(lambda: srv.sent, 4)
    check("普通发言不触发禁言", not srv.find("set_group_ban"), str(srv.calls))
    check("普通发言不报「没有这个命令」", "没有这个命令" not in srv.last_text(), srv.last_text())
    check("普通发言走的是 AI 对话", "咕噜" in srv.last_text(), repr(srv.sent)[:400])

    print("\n[16] 群里不 @ 也会接话（group_trigger=all）")
    await step(srv)
    await srv.push_group_msg(MEMBER, "今天谁在值班")
    await wait_until(lambda: srv.sent, 4)
    check("不 @ 也回了", "咕噜" in srv.last_text(), srv.last_text())

    print("\n[17] 切回 at 模式后不 @ 就不回")
    cfg.set("bot.group_trigger", "at")
    await step(srv)
    await srv.push_group_msg(MEMBER, "随便聊两句")
    await asyncio.sleep(0.5)
    check("at 模式下不接话", not srv.sent, str(srv.sent)[:120])
    await step(srv)
    await srv.push_group_msg(MEMBER, "随便聊两句", at=True)
    await wait_until(lambda: srv.sent, 4)
    check("at 模式下 @ 了才接话", "咕噜" in srv.last_text(), srv.last_text())
    cfg.set("bot.group_trigger", "all")

    print("\n[15] 群管命令在机器人非管理员时的兜底")
    srv.members[SELF_ID]["role"] = "member"
    bot.perms.invalidate_bot_role(GROUP)
    await step(srv)
    await srv.push_group_msg(OWNER, "/公告 测试公告")
    await wait_until(lambda: srv.sent)
    check("提示机器人不是管理员", "不是管理员" in srv.last_text(), srv.last_text())
    check("没有发公告", not srv.find("_send_group_notice"))
    srv.members[SELF_ID]["role"] = "admin"
    bot.perms.invalidate_bot_role(GROUP)


    print("\n[18] 菜单发的是图片")
    await step(srv)
    await srv.push_group_msg(OWNER, "/帮助")
    await wait_until(lambda: srv.sent, 8)
    segs = srv.last_segments()
    has_img = any(s.get("type") == "image" and str(s.get("data", {}).get("file", "")).startswith("base64://")
                  for s in segs)
    check("返回图片段", has_img, str(segs)[:140])

    print("\n[18b] 帮助带分类返回文本")
    await step(srv)
    await srv.push_group_msg(OWNER, "/帮助 群管")
    await wait_until(lambda: srv.sent, 5)
    check("分类模糊匹配", "禁言" in srv.last_text() and "公告" not in srv.last_text(), srv.last_text()[:80])

    print("\n[19] 关键词自动回复")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/关键词 添加 打卡=已记录{at}")
    await wait_until(lambda: srv.sent)
    check("关键词已添加", "已添加" in srv.last_text(), srv.last_text())
    check("落库正确", bot.state.keywords(GROUP).get("打卡") == "已记录{at}", str(bot.state.keywords(GROUP)))

    await step(srv)
    await srv.push_group_msg(MEMBER, "我来打卡了")
    await wait_until(lambda: srv.sent, 4)
    segs = srv.last_segments()
    check("命中关键词", any(s.get("type") == "at" and s["data"].get("qq") == str(MEMBER) for s in segs)
          and "已记录" in srv.last_text(), str(segs)[:140])
    check("关键词优先于 AI", "咕噜" not in srv.last_text(), srv.last_text())

    await step(srv)
    await srv.push_group_msg(ADMIN, "/关键词 删除 打卡")
    await wait_until(lambda: srv.sent)
    check("关键词已删除", not bot.state.keywords(GROUP), str(bot.state.keywords(GROUP)))

    print("\n[20] 群活跃排行")
    await step(srv)
    for _ in range(3):
        await srv.push_group_msg(MEMBER, "水一条")
        await asyncio.sleep(0.12)
    await step(srv)
    await srv.push_group_msg(OWNER, "/排行")
    await wait_until(lambda: srv.sent, 4)
    txt = srv.last_text()
    check("排行有内容", "活跃榜" in txt and "条" in txt, txt[:120])
    check("统计到成员发言", "小鱼干" in txt or str(MEMBER) in txt, txt[:160])

    print("\n[21] 定时提醒")
    await step(srv)
    bot._reminder_tick = 0.2
    await srv.push_group_msg(MEMBER, "/提醒 1秒 喝水")
    await wait_until(lambda: srv.sent)
    check("提醒已登记", "记下了" in srv.last_text(), srv.last_text())
    # 只认「喝水」那条：普通回复也带 at 段，按 at 判断会误命中
    # 认「提醒你：喝水」整串：登记回执里也有「喝水」两个字，只匹配「喝水」会误命中回执
    got = await wait_until(lambda: any("提醒你：喝水" in str(s.get("message")) for s in srv.sent), 8)
    check("到点真的 @ 了", got, str(srv.sent)[:200])
    if got:
        check("提醒内容正确", "喝水" in srv.last_text(), srv.last_text())
    bot._reminder_tick = 10.0

    print("\n[22] 提醒列表 / 取消")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/提醒 10分 开会")
    await wait_until(lambda: srv.sent)
    rid = srv.last_text().split("#")[1].split(" ")[0] if "#" in srv.last_text() else "0"
    await step(srv)
    await srv.push_group_msg(MEMBER, "/提醒列表")
    await wait_until(lambda: srv.sent)
    check("列表能看到", "开会" in srv.last_text(), srv.last_text())
    await step(srv)
    await srv.push_group_msg(MEMBER, f"/取消提醒 {rid}")
    await wait_until(lambda: srv.sent)
    check("取消成功", "已取消" in srv.last_text(), srv.last_text())

    print("\n[23] 今日运势卡")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/运势")
    await wait_until(lambda: srv.sent, 10)
    segs = srv.last_segments()
    check("运势返回图片", any(x.get("type") == "image" and
                              str(x.get("data", {}).get("file", "")).startswith("base64://") for x in segs),
          str(segs)[:120])

    print("\n[24] 签到 / 积分")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/签到")
    await wait_until(lambda: srv.sent)
    check("首次签到成功", "签到成功" in srv.last_text(), srv.last_text())
    pts1 = bot.state.points_of(GROUP, MEMBER)
    check("积分已入账", pts1 > 0, str(pts1))
    await step(srv)
    await srv.push_group_msg(MEMBER, "/签到")
    await wait_until(lambda: srv.sent)
    check("重复签到被拦", "已经签过" in srv.last_text(), srv.last_text())
    check("积分没重复加", bot.state.points_of(GROUP, MEMBER) == pts1, str(bot.state.points_of(GROUP, MEMBER)))

    await step(srv)
    await srv.push_group_msg(MEMBER, "/积分")
    await wait_until(lambda: srv.sent)
    check("查得到积分", str(pts1) in srv.last_text(), srv.last_text())

    await step(srv)
    await srv.push_group_msg(OWNER, "/积分榜")
    await wait_until(lambda: srv.sent)
    check("积分榜有内容", "积分榜" in srv.last_text() and "分" in srv.last_text(), srv.last_text()[:120])

    print("\n[25] 骰子 / 猜拳")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/骰子")
    await wait_until(lambda: srv.sent)
    txt = srv.last_text()
    check("掷出 1~6", "🎲" in txt and any(f" {i}（" in txt for i in range(1, 7)), txt)

    await step(srv)
    await srv.push_group_msg(MEMBER, "/骰子 100")
    await wait_until(lambda: srv.sent)
    check("支持自定义面数", "1~100" in srv.last_text(), srv.last_text())

    await step(srv)
    await srv.push_group_msg(OWNER, "/猜拳 石头")
    await wait_until(lambda: srv.sent)
    txt = srv.last_text()
    check("猜拳有结果", "石头" in txt and ("赢" in txt or "平局" in txt), txt)

    print("\n[26] 群友印象")
    await step(srv)
    await srv.push_group_msg(OWNER, "/印象 ", extra_segments=[{"type": "at", "data": {"qq": str(MEMBER)}}])
    await wait_until(lambda: srv.sent)
    check("印象有标签", "小鱼干" in srv.last_text(), srv.last_text())

    print("\n[27] smart 模式：看场合接话")
    cfg.set("chat.smart_reply_probability", 0)   # 关掉随机，避免测试抖动
    cfg.set("chat.group_speak_cooldown", 0)
    cfg.set("bot.group_trigger", "smart")

    await step(srv)
    await srv.push_group_msg(MEMBER, "今天天气不错啊")
    await asyncio.sleep(0.5)
    check("陈述句不接话", not srv.sent, str(srv.sent)[:120])

    await step(srv)
    await srv.push_group_msg(MEMBER, "这个怎么弄啊")
    await wait_until(lambda: srv.sent, 4)
    check("疑问句会接话", "咕噜" in srv.last_text(), srv.last_text())

    await step(srv)
    await srv.push_group_msg(MEMBER, "在吗？")
    await wait_until(lambda: srv.sent, 4)
    check("问号也算提问", "咕噜" in srv.last_text(), srv.last_text())

    await step(srv)
    await srv.push_group_msg(MEMBER, "随便说一句", at=True)
    await wait_until(lambda: srv.sent, 4)
    check("@ 一定回", "咕噜" in srv.last_text(), srv.last_text())

    print("\n[28] 说完之后安静一会儿")
    cfg.set("chat.group_speak_cooldown", 30)
    bot._last_speak[GROUP] = time.time()   # 假装刚说过
    await step(srv)
    await srv.push_group_msg(MEMBER, "这又是为什么")
    await asyncio.sleep(0.5)
    chat_sent = [m for m in srv.sent if "咕噜" in str(m.get("message"))]
    check("冷却期内不主动接话", not chat_sent, str(chat_sent)[:200])
    await step(srv)
    await srv.push_group_msg(MEMBER, "这又是为什么", at=True)
    await wait_until(lambda: srv.sent, 4)
    check("冷却期内 @ 仍然回", "咕噜" in srv.last_text(), srv.last_text())

    cfg.set("bot.group_trigger", "all")
    cfg.set("chat.group_speak_cooldown", 0)

    print("\n[29] 表情包 / 词云")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/表情包 今天不想上班")
    await wait_until(lambda: srv.sent, 8)
    segs = srv.last_segments()
    check("表情包是图片", any(x.get("type") == "image" and
                              str(x.get("data", {}).get("file", "")).startswith("base64://") for x in segs),
          str(segs)[:120])

    await step(srv)
    await srv.push_group_msg(MEMBER, "/表情包 黑 就这？")
    await wait_until(lambda: srv.sent, 8)
    check("支持指定风格", any(x.get("type") == "image" for x in srv.last_segments()), str(srv.last_segments())[:120])

    await step(srv)
    await srv.push_group_msg(MEMBER, "/风格")
    await wait_until(lambda: srv.sent)
    check("风格列表", "白底黑字" in srv.last_text() and "暴漫" in srv.last_text(), srv.last_text()[:100])

    # 词云要够量的重复词。先把接话关掉，否则这 18 条每条都会触发 AI，把断言淹掉
    cfg.set("bot.group_trigger", "at")
    for _ in range(18):
        await srv.push_group_msg(MEMBER, "刷机 驱动 变砖 救砖 固件 解锁 内核 模块")
        await asyncio.sleep(0.02)
    await step(srv)
    await srv.push_group_msg(ADMIN, "/词云")
    await wait_until(lambda: srv.sent, 10)
    segs = srv.last_segments()
    ok_cloud = any(x.get("type") == "image" for x in segs) or "词" in srv.last_text()
    check("词云有输出", ok_cloud, str(segs)[:140] + srv.last_text()[:80])

    print("\n[29b] 给图配字（单元）")
    from bot.memegen import render_on_image

    base = (Path(__file__).resolve().parent.parent / "assets" / "avatar.png").read_bytes()
    out = render_on_image(base, "这鱼有点大", "bottom")
    check("能在图上压字", bool(out) and out[:4] == b"\x89PNG", str(len(out) if out else 0))

    print("\n[30] 权限查询 / 授权管理")
    cfg.set("permission.superusers", [OWNER])
    bot.perms.reload()
    await step(srv)
    await srv.push_group_msg(OWNER, "/权限")
    await wait_until(lambda: srv.sent)
    check("主人身份显示正确", "主人" in srv.last_text(), srv.last_text()[:100])

    await step(srv)
    await srv.push_group_msg(MEMBER, "/权限")
    await wait_until(lambda: srv.sent)
    check("普通群友身份", "普通群友" in srv.last_text(), srv.last_text()[:100])

    await step(srv)
    await srv.push_group_msg(OWNER, "/设管理 20002")
    await wait_until(lambda: srv.sent)
    check("授权成功", 20002 in [int(x) for x in (cfg.get("permission.admins") or [])],
          str(cfg.get("permission.admins")))

    await step(srv)
    await srv.push_group_msg(OWNER, "/取消管理 20002")
    await wait_until(lambda: srv.sent)
    check("取消授权", 20002 not in [int(x) for x in (cfg.get("permission.admins") or [])],
          str(cfg.get("permission.admins")))

    print("\n[31] 警告列表 / 按人撤回 / 刷屏参数")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/警告 20004 测试")
    await wait_until(lambda: srv.sent)
    await step(srv)
    await srv.push_group_msg(ADMIN, "/警告列表")
    await wait_until(lambda: srv.sent)
    check("警告列表有人", "捣乱的" in srv.last_text() or "20004" in srv.last_text(), srv.last_text()[:120])

    await step(srv)
    for _ in range(3):
        await srv.push_group_msg(MEMBER, "刷一条")
        await asyncio.sleep(0.05)
    await step(srv)
    await srv.push_group_msg(ADMIN, "/撤回 20003 2")
    await wait_until(lambda: srv.find("delete_msg") and srv.sent, 6)
    check("按人撤回调了接口", len(srv.find("delete_msg")) >= 1, str(srv.calls)[-4:])

    await step(srv)
    await srv.push_group_msg(ADMIN, "/刷屏 10秒5条")
    await wait_until(lambda: srv.sent)
    check("刷屏参数已改", cfg.get("group.anti_spam.window") == 10 and cfg.get("group.anti_spam.limit") == 5,
          f"{cfg.get('group.anti_spam.window')}/{cfg.get('group.anti_spam.limit')}")

    print("\n[32] 入群验证")
    await step(srv)
    await srv.push_group_msg(OWNER, "/入群验证 设置 暗号是什么=kunawa")
    await wait_until(lambda: srv.sent)
    check("验证已设置", (bot.state.get(GROUP, "verify") or {}).get("answer") == "kunawa",
          str(bot.state.get(GROUP, "verify")))

    await step(srv)
    await srv.push_group_request(20010, "kunawa")
    await wait_until(lambda: srv.find("set_group_add_request"), 4)
    reqs = srv.find("set_group_add_request")
    check("答对自动通过", bool(reqs) and reqs[0].get("approve") is True, str(reqs))

    await step(srv)
    await srv.push_group_request(20011, "我是来打广告的")
    await asyncio.sleep(0.5)
    check("答错不自动放行", not srv.find("set_group_add_request"), str(srv.find("set_group_add_request")))
    check("答错通知主人", bool(srv.find("send_private_msg")), str(srv.sent)[:160])

    print("\n[33] AI 知道谁在说话")
    from bot.event import wrap as _wrap

    ev_owner = _wrap({"post_type": "message", "message_type": "group", "group_id": GROUP,
                      "user_id": OWNER, "message": [{"type": "text", "data": {"text": "在吗"}}],
                      "sender": {"role": "owner", "nickname": "群主老王"}})
    p_owner = bot._system_prompt(ev_owner, bot.perms.resolve(ev_owner))
    check("提示词写明主人", "主人" in p_owner and str(OWNER) in p_owner, p_owner[-160:])

    ev_member = _wrap({"post_type": "message", "message_type": "group", "group_id": GROUP,
                       "user_id": MEMBER, "message": [{"type": "text", "data": {"text": "在吗"}}],
                       "sender": {"role": "member", "nickname": "小鱼干"}})
    p_member = bot._system_prompt(ev_member, bot.perms.resolve(ev_member))
    check("提示词区分群友", "普通群友" in p_member and "主人" not in p_member, p_member[-160:])

    print("\n[34] 全体禁言 / 全员解禁")
    await step(srv)
    await srv.push_group_msg(OWNER, "/全体禁言 10分")
    await wait_until(lambda: srv.find("set_group_whole_ban") and srv.sent)
    bans = srv.find("set_group_whole_ban")
    check("开了全员禁言", bool(bans) and bans[0].get("enable") is True, str(bans))

    await step(srv)
    await srv.push_group_msg(OWNER, "/全员解禁")
    await wait_until(lambda: srv.find("set_group_whole_ban") and srv.sent)
    bans = srv.find("set_group_whole_ban")
    check("关了全员禁言", bool(bans) and bans[0].get("enable") is False, str(bans))

    print("\n[35] 清理死人（预览 → 确认）")
    await step(srv)
    await srv.push_group_msg(OWNER, "/清理死人 30")
    await wait_until(lambda: srv.sent, 5)
    txt = srv.last_text()
    check("预览列出名单", "没说话" in txt and "潜水王" in txt, txt[:160])
    check("预览不踢人", not srv.find("set_group_kick"), str(srv.calls)[-3:])
    check("提示要确认", "确认" in txt, txt[:160])

    await step(srv)
    await srv.push_group_msg(OWNER, "/清理死人 30 确认")
    await wait_until(lambda: srv.find("set_group_kick"), 12)
    kicks = srv.find("set_group_kick")
    kicked_ids = {int(k.get("user_id") or 0) for k in kicks}
    check("确认后真的踢了", bool(kicks), str(srv.calls)[-4:])
    check("踢的是死人", kicked_ids <= {20005, 20006}, str(kicked_ids))
    check("没踢群主和管理", 20001 not in kicked_ids and 20002 not in kicked_ids, str(kicked_ids))
    check("没踢自己", 10000 not in kicked_ids, str(kicked_ids))

    print("\n[36] 黑名单")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/拉黑 20004 发广告")
    await wait_until(lambda: srv.find("set_group_kick") and srv.sent, 6)
    kicks = srv.find("set_group_kick")
    check("拉黑会踢人", bool(kicks) and int(kicks[0]["user_id"]) == 20004, str(kicks))
    check("踢人时拒绝再入", kicks and kicks[0].get("reject_add_request") is True, str(kicks))
    check("进了本群黑名单", bot.state.is_banned(GROUP, 20004) is not None, str(bot.state.bans(GROUP)))

    await step(srv)
    await srv.push_group_msg(ADMIN, "/黑名单")
    await wait_until(lambda: srv.sent)
    check("黑名单能看到", "发广告" in srv.last_text(), srv.last_text()[:140])

    await step(srv)
    await srv.push_group_request(20004, "让我进去")
    await wait_until(lambda: srv.find("set_group_add_request"), 5)
    reqs = srv.find("set_group_add_request")
    check("黑名单入群被自动拒", bool(reqs) and reqs[0].get("approve") is False, str(reqs))

    await step(srv)
    await srv.push_group_msg(ADMIN, "/解黑 20004")
    # 等状态真的变了，别只等「有回复」——上一步的迟到回复会让断言提前跑
    await wait_until(lambda: bot.state.is_banned(GROUP, 20004) is None, 5)
    check("解黑成功", bot.state.is_banned(GROUP, 20004) is None, str(bot.state.bans(GROUP)))

    print("\n[37] 全局黑名单")
    await step(srv)
    await srv.push_group_msg(OWNER, "/全局拉黑 20006 跨群广告")
    await wait_until(lambda: bot.global_bans.get(20006) is not None, 6)
    check("全局黑名单已记录", bot.global_bans.get(20006) is not None, str(bot.global_bans.items()))

    await step(srv)
    await srv.push_group_request(20006, "我又来了")
    await wait_until(lambda: srv.find("set_group_add_request"), 5)
    reqs = srv.find("set_group_add_request")
    check("全局黑名单也拦得住", bool(reqs) and reqs[0].get("approve") is False, str(reqs))

    await step(srv)
    await srv.push_group_msg(OWNER, "/全局解黑 20006")
    await wait_until(lambda: bot.global_bans.get(20006) is None, 5)
    check("全局解黑", bot.global_bans.get(20006) is None, str(bot.global_bans.items()))

    print("\n[38] fban 假封禁")
    await step(srv)
    await srv.push_group_msg(MEMBER, "/fban 20004 3 发广告")
    # 等图片真的到了：上一条命令的迟到回复也会让 srv.sent 非空
    await wait_until(lambda: any(x.get("type") == "image"
                                 for m in srv.sent for x in (m.get("message") or [])), 12)
    segs = srv.last_segments()
    check("fban 出图", any(x.get("type") == "image" for x in segs), str(segs)[:140])
    check("fban 不真踢人", not srv.find("set_group_kick"), str(srv.calls)[-3:])
    check("fban 不写黑名单", bot.state.is_banned(GROUP, 20004) is None, str(bot.state.bans(GROUP)))

    print("\n[39] AI 哨兵：本地预筛（不烧 token）")
    cfg.set("guard.enabled", True)
    cfg.set("guard.sample_rate", 0.0)
    await asyncio.sleep(0.15)
    c0, s0 = bot.guard.stats["checked"], bot.guard.stats["skipped"]

    await step(srv)
    await srv.push_group_msg(MEMBER, "今天天气不错啊")
    await asyncio.sleep(0.6)
    check("正常消息不送审", bot.guard.stats["checked"] == c0, f"checked={bot.guard.stats['checked']}")
    check("被本地筛掉了", bot.guard.stats["skipped"] > s0, f"skipped={bot.guard.stats['skipped']}")

    await step(srv)
    await srv.push_group_msg(MEMBER, "嗯")
    await asyncio.sleep(0.5)
    check("太短的消息也不送审", bot.guard.stats["checked"] == c0, f"checked={bot.guard.stats['checked']}")

    print("\n[40] 判 L4 → 撤回 + 踢人 + 拉黑")
    GUARD["content"] = '{"level": 4, "category": "辱骂", "reason": "人身威胁", "confidence": 0.95}'
    await step(srv)
    await srv.push_group_msg(20007, "我要弄死你，死全家")
    ok = await wait_until(lambda: srv.find("set_group_kick"), 10)
    kicks = srv.find("set_group_kick")
    check("L4 会踢人", ok and bool(kicks) and int(kicks[0]["user_id"]) == 20007, str(kicks))
    check("踢时拒绝再入群", bool(kicks) and kicks[0].get("reject_add_request") is True, str(kicks))
    check("写进了本群黑名单", bot.state.is_banned(GROUP, 20007) is not None, str(bot.state.bans(GROUP)))
    check("消息也被撤回了", bool(srv.find("delete_msg")), str(srv.calls)[-4:])
    check("审计记成 ban", any(r["action"] == "ban" for r in bot.guard.recent(5)), str(bot.guard.recent(2))[:200])
    bot.state.del_ban(GROUP, 20007)

    print("\n[41] 判 L2 → 只撤回")
    GUARD["content"] = '{"level": 2, "category": "广告引流", "reason": "发引流广告", "confidence": 0.9}'
    await step(srv)
    await srv.push_group_msg(20008, "加微信 abc12345 有福利")
    ok = await wait_until(lambda: srv.find("delete_msg") and srv.sent, 10)
    check("L2 撤回消息", bool(srv.find("delete_msg")), str(srv.calls)[-4:])
    check("L2 不踢人", not srv.find("set_group_kick"), str(srv.calls)[-4:])
    check("L2 不写黑名单", bot.state.is_banned(GROUP, 20008) is None, str(bot.state.bans(GROUP)))

    print("\n[42] AI 判定失败 → 绝不动手")
    GUARD["content"] = "我觉得这条还行吧"
    await step(srv)
    await srv.push_group_msg(20007, "死全家 又是你这废物东西")
    await asyncio.sleep(1.2)
    check("返回不是 JSON 时不踢人", not srv.find("set_group_kick"), str(srv.calls)[-4:])
    check("也不写黑名单", bot.state.is_banned(GROUP, 20007) is None, str(bot.state.bans(GROUP)))
    check("记为出错", any(r.get("error") for r in bot.guard.recent(5)), str(bot.guard.recent(2))[:200])

    print("\n[43] 把握不足 → 只记录不处置")
    GUARD["content"] = '{"level": 4, "category": "辱骂", "reason": "骂人", "confidence": 0.4}'
    await step(srv)
    await srv.push_group_msg(20008, "死全家 你个废物东西")
    await asyncio.sleep(1.2)
    check("低把握不踢人", not srv.find("set_group_kick"), str(srv.calls)[-4:])
    check("记为 review 待人工看", any(r["action"] == "review" for r in bot.guard.recent(5)),
          str(bot.guard.recent(2))[:220])

    print("\n[44] 群主 / 管理豁免")
    GUARD["content"] = '{"level": 4, "category": "辱骂", "reason": "测试", "confidence": 0.99}'
    c1 = bot.guard.stats["checked"]
    await step(srv)
    await srv.push_group_msg(OWNER, "死全家 测试豁免")
    await asyncio.sleep(0.9)
    check("群主发的不过审", bot.guard.stats["checked"] == c1, f"checked={bot.guard.stats['checked']}")
    check("群主没被踢", not srv.find("set_group_kick"), str(srv.calls)[-3:])

    print("\n[45] 命令消息不过审")
    c2 = bot.guard.stats["checked"]
    await step(srv)
    await srv.push_group_msg(ADMIN, "/违禁词 添加 死全家")
    await asyncio.sleep(0.9)
    check("命令不送审（否则群管会被自己封）", bot.guard.stats["checked"] == c2,
          f"checked={bot.guard.stats['checked']}")
    await step(srv)
    await srv.push_group_msg(ADMIN, "/违禁词 删除 死全家")
    await asyncio.sleep(0.4)

    print("\n[46] /哨兵 测试：只判不动手")
    GUARD["content"] = '{"level": 4, "category": "辱骂", "reason": "严重辱骂", "confidence": 0.98}'
    await step(srv)
    await srv.push_group_msg(OWNER, "/哨兵 测试 你这个废物东西")
    ok = await wait_until(lambda: srv.sent and "判定结果" in srv.last_text(), 10)
    txt = srv.last_text()
    check("给出判定结果", ok, txt[:160])
    check("显示了等级", "L4" in txt, txt[:160])
    check("说明了会执行封禁", "封禁" in txt, txt[:200])
    check("测试不真踢人", not srv.find("set_group_kick"), str(srv.calls)[-3:])

    print("\n[47] /哨兵 关 → 不再送审")
    await step(srv)
    await srv.push_group_msg(OWNER, "/哨兵 关")
    await wait_until(lambda: srv.sent, 6)
    c3 = bot.guard.stats["checked"]
    await step(srv)
    await srv.push_group_msg(20008, "死全家 又来了")
    await asyncio.sleep(0.9)
    check("关闭后不送审", bot.guard.stats["checked"] == c3, f"checked={bot.guard.stats['checked']}")
    check("关闭后不踢人", not srv.find("set_group_kick"), str(srv.calls)[-3:])
    await step(srv)
    await srv.push_group_msg(OWNER, "/哨兵 开")
    await wait_until(lambda: srv.sent, 6)

    print("\n[48] /撤销 人工翻案")
    bot.state.add_ban(GROUP, 20008, "[AI] 测试", 0)
    await step(srv)
    await srv.push_group_msg(OWNER, "/撤销 20008")
    # 撤销是「先删黑名单（同步）→ 再解禁（异步）」，只等前者会抢跑
    ok = await wait_until(lambda: bot.state.is_banned(GROUP, 20008) is None
                          and bool(srv.find("set_group_ban")), 8)
    check("撤销解除了黑名单", bot.state.is_banned(GROUP, 20008) is None, str(bot.state.bans(GROUP)))
    check("撤销也解了禁言", bool(srv.find("set_group_ban")), str(srv.calls)[-3:])

    print("\n[49] 审计日志落盘")
    check("guard.jsonl 存在", (tmp / "data" / "guard.jsonl").exists(), str(tmp))
    rows = bot.guard.recent(50)
    check("记录里有封禁/撤回/出错", any(r["action"] == "ban" for r in rows)
          and any(r["action"] == "recall" for r in rows)
          and any(r.get("error") for r in rows), str([r["action"] for r in rows])[:200])
    check("记录带原文和理由", all("text" in r and "reason" in r for r in rows), str(rows[:1])[:200])

    cfg.set("guard.enabled", True)
    GUARD["content"] = '{"level": 0, "category": "正常", "reason": "测试放行", "confidence": 0.95}'

    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    await bot.shutdown()
    await srv.stop()

    print("\n" + "=" * 50)
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
