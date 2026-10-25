# -*- coding: utf-8 -*-
"""菜单 / 指令面板自测：py main.py --menutest

做三件事（全离线，不联网、不发消息）：
1. 用 core/menu.py 校验 menu.json 与 panels.json —— 字符宽度、数量上限、类型、
   https 前缀，以及「面板里的 command 名必须真的是已注册命令」；
2. 把结构打印成树，标出每项的实际字符宽度（文档口径：汉字算 2），
   方便肉眼确认「5 个汉字就是 10 字符」这类边界；
3. 拿几个**故意写错**的样本反证校验器本身有效（超长 name、超量 items、
   非 https 链接、面板里写了展示名而不是命令）—— 校验器自己失灵比不校验更危险。
"""
import asyncio
import json
import os

from core.menu import (MENU_MAX_ITEMS, MENU_NAME_MAX, SUB_MAX_ITEMS,
                       SUB_NAME_MAX, PANEL_NAME_MAX, PANEL_DESC_MAX,
                       validate_menu, validate_panels, width)

OK = "  OK   "
BAD = "  FAIL "


def _load(name: str):
    with open(name, "r", encoding="utf-8") as f:
        return json.load(f)


def _print_menu(menu):
    items = menu.get("items") or []
    print(f"  顶层 {len(items)}/{MENU_MAX_ITEMS} 项：")
    for i, it in enumerate(items, 1):
        typ = it.get("type")
        print(f"   {i}. {it.get('name')}"
              f"  [{typ}]  name {width(it.get('name'))}/{MENU_NAME_MAX} 字符")
        if typ == "menu":
            subs = it.get("sub_menu_items") or []
            print(f"      └ 子菜单 {len(subs)}/{SUB_MAX_ITEMS} 项")
            for j, sub in enumerate(subs, 1):
                print(f"        {j}) {sub.get('name')}"
                      f"  → {sub.get('send_message') or sub.get('link')}"
                      f"  （name {width(sub.get('name'))}/{SUB_NAME_MAX}）")
        else:
            print(f"      → {it.get('send_message') or it.get('link')}")


def _print_panels(specs):
    for spec in specs:
        panel = spec.get("panel") or {}
        items = panel.get("items") or []
        print(f"  [{spec.get('scope')}/{spec.get('target_type') or 'all'}] "
              f"{panel.get('remark')} —— {len(items)} 项")
        for i, it in enumerate(items, 1):
            flag = "（仅管理员）" if it.get("only_admin") else ""
            print(f"    {i}. {it.get('name')}{flag}"
                  f"  desc={it.get('desc')!r}"
                  f"  （name {width(it.get('name'))}/{PANEL_NAME_MAX}，"
                  f"desc {width(it.get('desc'))}/{PANEL_DESC_MAX}）")


def _expect_reject(label, errors, keyword):
    if not errors:
        print(f"{BAD}{label} —— 校验器没拦住！")
        return False
    if keyword and not any(keyword in e for e in errors):
        print(f"{BAD}{label} —— 拦住了但理由不对：{errors[0]}")
        return False
    print(f"{OK}{label} —— 已拦住：{errors[0]}")
    return True


HELP_CASES = ("帮助", "帮助 更多", "帮助 管理", "帮助 kun")


def _smoke_help(bot):
    """把每个帮助页真的发一遍（mock 掉发送接口），返回发出的文本列表。

    为什么不能只调纯函数：帮助文本是拼出来的，出错点常在 `_show_help` 的闭包里
    （真实踩过：删掉常量 `SEP` 后 `_show_help` 还在引用，纯函数预览完全看不出来，
    线上直接回「命令执行出错：name 'SEP' is not defined」）。
    """
    sent = []

    async def fake_send(target, *, content=None, **kw):
        sent.append(content or "")
        return {"id": f"M{len(sent)}"}

    bot.api.send_group_message = fake_send
    bot.api.send_c2c_message = fake_send
    if not (bot.gateway.bot_user or {}).get("username"):
        bot.gateway.bot_user = {"username": "KunClient Bot"}

    async def _run():
        for i, text in enumerate(HELP_CASES):
            await bot.router.handle_event("GROUP_AT_MESSAGE_CREATE", {
                "id": f"h{i}", "content": " " + text,
                "group_openid": "GHELP", "group_id": "GHELP",
                "author": {"id": "UHELP", "member_openid": "UHELP",
                           "username": "测试", "member_role": "owner"},
                "timestamp": "2026-10-17T19:00:00+08:00",
            })

    asyncio.run(_run())
    return sent


def _cleanup(path):
    for p in (path, path + ".bak"):
        if os.path.exists(p):
            os.remove(p)


def run():
    print("=" * 60)
    print("菜单 / 指令面板自测（对照官方 api-v2 文档的限制）")
    print("=" * 60)

    # 已注册命令名（含别名）——面板里 type=command 的 name 必须命中其中之一
    from main import Bot, load_config
    cfg = load_config()
    cfg["data_file"] = "data.menutest.json"
    bot = Bot(cfg)
    commands = set(bot.router.commands)
    _cleanup(cfg["data_file"])

    failed = 0

    # ---------- ① 真实配置 ----------
    print("\n[1/3] 校验 menu.json")
    if not os.path.exists("menu.json"):
        print(f"{BAD}menu.json 不存在")
        failed += 1
    else:
        menu_cfg = _load("menu.json")
        menu_cfg.pop("_说明", None)
        errs = validate_menu(menu_cfg.get("menu"))
        _print_menu(menu_cfg.get("menu") or {})
        if errs:
            failed += 1
            for e in errs:
                print(f"{BAD}{e}")
        else:
            print(f"{OK}menu.json 通过")

    print("\n[2/3] 校验 panels.json")
    if not os.path.exists("panels.json"):
        print(f"{BAD}panels.json 不存在")
        failed += 1
    else:
        panel_cfg = _load("panels.json")
        specs = panel_cfg.get("panels") or []
        errs = validate_panels(specs, commands=commands)
        _print_panels(specs)
        if errs:
            failed += 1
            for e in errs:
                print(f"{BAD}{e}")
        else:
            print(f"{OK}panels.json 通过（command 名全部是已注册命令）")

    print("\n[3/4] 帮助页渲染冒烟（真的走一遍命令路径）")
    try:
        sent = _smoke_help(bot)
    except Exception as e:
        sent = []
        print(f"{BAD}帮助命令抛异常：{e}")
        failed += 1
    if len(sent) < len(HELP_CASES):
        if sent or not failed:
            print(f"{BAD}只发出 {len(sent)}/{len(HELP_CASES)} 条回复")
            failed += 1
    else:
        for text, out in zip(HELP_CASES, sent):
            first = (out.splitlines() or [""])[0]
            bad = (not out.strip() or "{" in out or "SEP" in out
                   or "命令执行出错" in out)
            print(f"{BAD if bad else OK}{text} → {first[:34]}")
            failed += 1 if bad else 0

    # ---------- ③ 反证：故意写错必须被拦住 ----------
    print("\n[4/4] 反证校验器本身有效")
    cases = [
        ("顶层 name 超过 10 字符（6 个汉字）",
         {"items": [{"name": "六个汉字的菜单", "type": "send_message",
                     "send_message": "帮助"}]}, "超过 10"),
        ("items 超过 10 项",
         {"items": [{"name": "帮", "type": "send_message", "send_message": "帮助"}] * 11},
         "超过上限"),
        ("子菜单超过 5 项",
         {"items": [{"name": "更多", "type": "menu", "sub_menu_items":
                     [{"name": "项", "type": "send_message", "send_message": "帮助"}] * 6}]},
         "超过上限"),
        ("link 不是 https",
         {"items": [{"name": "官网", "type": "link", "link": "http://x.com"}]}, "https://"),
        ("type 不合法",
         {"items": [{"name": "怪", "type": "button", "send_message": "帮助"}]}, "不合法"),
        ("send_message 类型缺文本",
         {"items": [{"name": "空", "type": "send_message"}]}, "没写 send_message"),
        ("二级菜单里嵌 menu",
         {"items": [{"name": "更多", "type": "menu", "sub_menu_items":
                     [{"name": "再嵌", "type": "menu", "sub_menu_items": []}]}]},
         "不支持再嵌套"),
    ]
    for label, menu, kw in cases:
        failed += 0 if _expect_reject(label, validate_menu(menu), kw) else 1

    panel_cases = [
        ("面板 command 名不是已注册命令（写了展示名）",
         [{"scope": "group", "panel": {"remark": "x", "items": [
             {"type": "command", "name": "一键签到", "desc": "x"}]}}], "不是已注册命令"),
        ("面板项 desc 超过 30 字符",
         [{"scope": "group", "panel": {"remark": "x", "items": [
             {"type": "command", "name": "帮助",
              "desc": "一二三四五六七八九十一二三四五六"}]}}], "超过 30"),
        ("面板 scope 不合法",
         [{"scope": "qq", "panel": {"remark": "x", "items": [
             {"type": "command", "name": "帮助", "desc": "x"}]}}], "不合法"),
        ("面板元素超过 20 项",
         [{"scope": "group", "panel": {"remark": "x", "items": [
             {"type": "command", "name": "帮助", "desc": "x"}] * 21}}], "超过上限"),
    ]
    for label, specs, kw in panel_cases:
        failed += 0 if _expect_reject(label, validate_panels(specs, commands=commands), kw) else 1

    print("\n" + "=" * 60)
    if failed:
        print(f"菜单自测失败 ❌（{failed} 项）")
        raise SystemExit(1)
    print("菜单自测全部通过 ✅（结构合规 + 校验器有效；未推送任何内容到平台）")
