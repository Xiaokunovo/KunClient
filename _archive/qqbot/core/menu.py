# -*- coding: utf-8 -*-
"""自定义菜单 / 指令面板的离线校验（对照官方 api-v2 文档的硬限制）。

文档出处（2026-10-17 抓取）：
  PUT /v2/menu      https://bot.q.qq.com/wiki/develop/api-v2/autogen/api/v2_menu.put.html
  POST /v2/panels   https://bot.q.qq.com/wiki/develop/api-v2/autogen/api/v2_panels.post.html

为什么要把限制写成代码：超限时平台只回一个 `40030013 超出数量限制`，
**不告诉你是哪一项超了**。等上线才发现「菜单没生效」再回头数汉字，太亏。
所以推送前先在本地全量校验一遍，错在哪一项、超了几个字符，日志里直接说清楚。

两条长度口径（文档原文）：
- 菜单 name「最多 10 个字符，一个中文汉字算 2 个字符」→ 即 5 个汉字；
- 子菜单 / 面板元素 name「最多 14 个字符，约 7 个中文汉字」；
- 面板元素 desc「最多 30 个字符，约 15 个中文汉字」。
所以计长一律用 `width()`（非 ASCII 记 2），不要用 `len()`。
"""

MENU_MAX_ITEMS = 10
MENU_NAME_MAX = 10
SUB_MAX_ITEMS = 5
SUB_NAME_MAX = 14

PANEL_MAX_ITEMS = 20
PANEL_NAME_MAX = 14
PANEL_DESC_MAX = 30
PANEL_REMARK_MAX = 255
PANEL_MAX_COUNT = 20          # 一个机器人最多 20 个面板

MENU_TYPES = ("switch", "send_message", "link", "menu")
SUB_TYPES = ("send_message", "link")     # 二级菜单不支持 menu
PANEL_ITEM_TYPES = ("command", "link")


def width(text) -> int:
    """按文档口径计长：ASCII 记 1，其余（汉字/全角标点）记 2。"""
    return sum(1 if ord(c) < 128 else 2 for c in str(text or ""))


def _check_url(where: str, url, errors: list):
    if not isinstance(url, str) or not url.startswith("https://"):
        errors.append(f"{where} 的 link 必须以 https:// 开头（现在是 {url!r}），"
                      "否则平台回 40030008")


def validate_menu(menu) -> list[str]:
    """校验 `{"items": [...]}`，返回错误列表（空 = 通过）。"""
    errors: list[str] = []
    if not isinstance(menu, dict):
        return ["menu 必须是对象（形如 {\"items\": [...]}）"]
    items = menu.get("items")
    if not isinstance(items, list) or not items:
        return ["menu.items 必须是非空数组"]
    if len(items) > MENU_MAX_ITEMS:
        errors.append(f"顶层菜单 {len(items)} 项，超过上限 {MENU_MAX_ITEMS} 项")

    for i, it in enumerate(items, 1):
        tag = f"第 {i} 项（{it.get('name') if isinstance(it, dict) else it!r}）"
        if not isinstance(it, dict):
            errors.append(f"{tag} 必须是对象")
            continue
        name = it.get("name") or ""
        if not name:
            errors.append(f"{tag} 缺 name")
        elif width(name) > MENU_NAME_MAX:
            errors.append(f"{tag} 的 name「{name}」宽 {width(name)}，"
                          f"超过 {MENU_NAME_MAX}（≈5 个汉字）")
        typ = it.get("type") or ""
        if typ not in MENU_TYPES:
            errors.append(f"{tag} 的 type「{typ}」不合法，只能是 {'/'.join(MENU_TYPES)}")
        elif typ == "send_message" and not (it.get("send_message") or "").strip():
            errors.append(f"{tag} 是 send_message 类型但没写 send_message 文本")
        elif typ == "link":
            _check_url(tag, it.get("link"), errors)
        elif typ == "switch":
            sw = it.get("switch") or {}
            if not (sw.get("switch_id") or "").strip():
                errors.append(f"{tag} 是 switch 类型但缺 switch.switch_id")
        elif typ == "menu":
            subs = it.get("sub_menu_items")
            if not isinstance(subs, list) or not subs:
                errors.append(f"{tag} 是 menu 类型但没有 sub_menu_items")
                continue
            if len(subs) > SUB_MAX_ITEMS:
                errors.append(f"{tag} 有 {len(subs)} 个子项，超过上限 {SUB_MAX_ITEMS}")
            for j, sub in enumerate(subs, 1):
                stag = f"{tag} 的子项 {j}（{sub.get('name') if isinstance(sub, dict) else sub!r}）"
                if not isinstance(sub, dict):
                    errors.append(f"{stag} 必须是对象")
                    continue
                sname = sub.get("name") or ""
                if not sname:
                    errors.append(f"{stag} 缺 name")
                elif width(sname) > SUB_NAME_MAX:
                    errors.append(f"{stag} 的 name「{sname}」宽 {width(sname)}，"
                                  f"超过 {SUB_NAME_MAX}（≈7 个汉字）")
                styp = sub.get("type") or ""
                if styp not in SUB_TYPES:
                    errors.append(f"{stag} 的 type「{styp}」不合法，二级菜单只能是 "
                                  f"{'/'.join(SUB_TYPES)}（不支持再嵌套 menu）")
                elif styp == "send_message" and not (sub.get("send_message") or "").strip():
                    errors.append(f"{stag} 是 send_message 类型但没写 send_message 文本")
                elif styp == "link":
                    _check_url(stag, sub.get("link"), errors)
    return errors


def validate_panels(specs, commands=None) -> list[str]:
    """校验 panels.json 里的 `panels` 数组，返回错误列表（空 = 通过）。

    commands：已注册命令名集合（含别名）。文档规定 `type=command` 的元素
    **点一下就把 name 原样填进输入框**，所以 name 必须本身是一条合法命令 ——
    写成「每日签到」这种好看的展示名，用户点完发出去就是「没有这个命令」。
    这是最容易踩且最难自查的坑，所以顺手在本地挡掉。
    """
    errors: list[str] = []
    if not isinstance(specs, list) or not specs:
        return ["panels 必须是非空数组"]
    if len(specs) > PANEL_MAX_COUNT:
        errors.append(f"面板 {len(specs)} 个，超过单机器人上限 {PANEL_MAX_COUNT} 个")
    known = None
    if commands:
        known = {str(c) for c in commands} | {str(c).lower() for c in commands}

    for i, spec in enumerate(specs, 1):
        tag = f"第 {i} 个面板"
        if not isinstance(spec, dict):
            errors.append(f"{tag} 必须是对象")
            continue
        scope = spec.get("scope")
        if scope not in ("c2c", "group", "channel", "dm"):
            errors.append(f"{tag} 的 scope「{scope}」不合法，只能是 c2c/group/channel/dm")
        target = spec.get("target_type") or "all"
        if target not in ("all", "specific"):
            errors.append(f"{tag} 的 target_type「{target}」不合法，只能是 all/specific")
        elif target == "specific" and scope in ("channel", "dm"):
            errors.append(f"{tag}：channel/dm 场景只支持 target_type=all")
        panel = spec.get("panel")
        if not isinstance(panel, dict):
            errors.append(f"{tag} 缺 panel 对象")
            continue
        remark = panel.get("remark") or ""
        if width(remark) > PANEL_REMARK_MAX:
            errors.append(f"{tag} 的 remark 宽 {width(remark)}，超过 {PANEL_REMARK_MAX}")
        items = panel.get("items")
        if not isinstance(items, list) or not items:
            errors.append(f"{tag} 的 panel.items 必须是非空数组")
            continue
        if len(items) > PANEL_MAX_ITEMS:
            errors.append(f"{tag} 有 {len(items)} 个元素，超过上限 {PANEL_MAX_ITEMS}")
        for j, it in enumerate(items, 1):
            itag = f"{tag} 元素 {j}（{it.get('name') if isinstance(it, dict) else it!r}）"
            if not isinstance(it, dict):
                errors.append(f"{itag} 必须是对象")
                continue
            name = it.get("name") or ""
            if not name:
                errors.append(f"{itag} 缺 name")
            elif width(name) > PANEL_NAME_MAX:
                errors.append(f"{itag} 的 name「{name}」宽 {width(name)}，"
                              f"超过 {PANEL_NAME_MAX}（≈7 个汉字）")
            desc = it.get("desc") or ""
            if width(desc) > PANEL_DESC_MAX:
                errors.append(f"{itag} 的 desc 宽 {width(desc)}，"
                              f"超过 {PANEL_DESC_MAX}（≈15 个汉字）")
            typ = it.get("type") or ""
            if typ not in PANEL_ITEM_TYPES:
                errors.append(f"{itag} 的 type「{typ}」不合法，只能是 command/link")
            elif typ == "link":
                _check_url(itag, it.get("link"), errors)
            elif typ == "command" and known is not None and name not in known:
                errors.append(f"{itag} 的 name「{name}」不是已注册命令（含别名）—— "
                              "type=command 时用户点一下就把 name 原样发出去，"
                              "所以这里必须写命令本身，不能写展示名")
    return errors


# ---------- 变更比对（避免每次重启都白推一遍） ----------

def _strip(obj):
    """剥掉平台自动回填、我们不维护的字段（icon 等），便于深比较。"""
    if isinstance(obj, dict):
        return {k: _strip(v) for k, v in obj.items()
                if k not in ("icon", "version", "created_at", "updated_at")}
    if isinstance(obj, list):
        return [_strip(v) for v in obj]
    return obj


def menu_changed(remote, local) -> bool:
    """平台上的菜单与本地配置是否不同。remote 为 None（没查过）时一律当「不同」。"""
    if not isinstance(remote, dict):
        return True
    return _strip(remote) != _strip(local)
