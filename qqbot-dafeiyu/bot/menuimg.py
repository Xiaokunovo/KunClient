"""把命令菜单画成一张图。

为什么不用纯文本：QQ 里 32 条命令排下来是一大坨，手机上要滑三屏，还容易被折行折烂。
图片能按分类排好、字距统一，扫一眼就看完。

字体：Windows 用微软雅黑，Linux 用文泉驿/Noto，macOS 用苹方。
**一个都找不到就返回 None**，调用方退回纯文本菜单 —— 宁可难看，不能变成一堆方块。
"""

from __future__ import annotations

import io
import platform
from pathlib import Path

FONT_CANDIDATES = [
    # Windows
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    # Linux
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    # macOS
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
]

# 粗体候选。表情包/大字报用粗体才有味道，常规体看着像文档。
BOLD_CANDIDATES = [
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Bold.ttc",
    "/System/Library/Fonts/PingFang.ttc",
]

# 配色：浅色卡片 + 深蓝强调，跟控制台同一套
BG = (247, 249, 252)
CARD = (255, 255, 255)
INK = (27, 36, 50)
SUB = (124, 138, 160)
LINE = (232, 238, 247)
ACCENT = (47, 111, 237)
ACCENT_SOFT = (233, 240, 255)

CAT_COLORS = {
    "AI 对话": (47, 111, 237),
    "群管理": (230, 90, 90),
    "群设置": (235, 150, 60),
    "娱乐": (219, 96, 160),
    "系统": (120, 110, 200),
    "其它": (80, 150, 140),
}


def find_font() -> str | None:
    for path in FONT_CANDIDATES:
        if Path(path).exists():
            return path
    # Linux 上再兜底问一下 fontconfig
    if platform.system() == "Linux":
        import subprocess

        try:
            out = subprocess.run(
                ["fc-match", "-f", "%{file}", "sans-serif:lang=zh"],
                capture_output=True, timeout=5,
            ).stdout.decode("utf-8", "replace").strip()
            if out and Path(out).exists():
                return out
        except Exception:  # noqa: BLE001
            pass
    return None


def find_bold_font() -> str | None:
    """优先粗体；没有就退回常规体（总比没字好）。"""
    for path in BOLD_CANDIDATES:
        if Path(path).exists():
            return path
    return find_font()


def _ellipsis(draw, text: str, font, max_w: float) -> str:
    if draw.textlength(text, font=font) <= max_w:
        return text
    while text and draw.textlength(text + "…", font=font) > max_w:
        text = text[:-1]
    return text + "…"


def render_menu(commands: list, cfg, level_name: str = "") -> bytes | None:
    """commands 是 router.unique() 过滤后的列表。返回 PNG 字节，失败返回 None。"""
    font_path = find_font()
    if not font_path:
        return None
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None

    W = 780
    PAD = 30
    inner = W - PAD * 2

    f_title = ImageFont.truetype(font_path, 30)
    f_sub = ImageFont.truetype(font_path, 15)
    f_cat = ImageFont.truetype(font_path, 18)
    f_use = ImageFont.truetype(font_path, 17)
    f_desc = ImageFont.truetype(font_path, 14)

    # ---- 先算高度 ----
    groups: dict[str, list] = {}
    order = ["AI 对话", "群管理", "群设置", "娱乐", "系统", "其它"]
    for cmd in commands:
        groups.setdefault(cmd.category, []).append(cmd)
    cats = sorted(groups, key=lambda c: (order.index(c) if c in order else 99, c))

    # 命令一多（>30）就切紧凑模式：三列、只排用法不排说明、行高收到 36。
    # 否则 54 条命令排两列要 1760px，手机上得滑三屏。
    compact = len(commands) > 30
    cols = 3 if compact else 2
    COL_GAP = 20 if compact else 22
    col_w = (inner - COL_GAP * (cols - 1)) // cols
    ROW_H = 36 if compact else 46
    CAT_H = 40 if compact else 44

    height = PAD + 46 + 26  # 标题区
    for cat in cats:
        n = len(groups[cat])
        rows = (n + cols - 1) // cols
        height += CAT_H + rows * ROW_H + 10
    height += 46  # 底部提示

    img = Image.new("RGB", (W, height), BG)
    d = ImageDraw.Draw(img)

    # 顶部强调条
    d.rounded_rectangle([PAD, PAD - 8, PAD + 54, PAD - 3], radius=3, fill=ACCENT)

    y = PAD + 6
    d.text((PAD, y), str(cfg.get("bot.name", "机器人")), font=f_title, fill=INK)
    name_w = d.textlength(str(cfg.get("bot.name", "机器人")), font=f_title)
    d.text((PAD + name_w + 12, y + 12), "命令菜单", font=f_sub, fill=SUB)
    y += 46

    total = len(commands)
    d.text((PAD, y), f"共 {total} 个命令 · 你的权限：{level_name}", font=f_sub, fill=SUB)
    y += 34

    for cat in cats:
        color = CAT_COLORS.get(cat, ACCENT)
        # 分类标题
        d.rounded_rectangle([PAD, y, PAD + 4, y + 20], radius=2, fill=color)
        d.text((PAD + 14, y - 1), cat, font=f_cat, fill=INK)
        cw = d.textlength(cat, font=f_cat)
        d.text((PAD + 14 + cw + 8, y + 4), str(len(groups[cat])), font=f_desc, fill=SUB)
        y += CAT_H

        items = groups[cat]
        for i, cmd in enumerate(items):
            col = i % cols
            x = PAD + col * (col_w + COL_GAP)
            ry = y + (i // cols) * ROW_H

            usage = cmd.usage or cmd.name
            d.text((x, ry), _ellipsis(d, usage, f_use, col_w - 6), font=f_use, fill=INK)
            if not compact:
                desc = cmd.desc or ""
                if desc:
                    d.text((x, ry + 22), _ellipsis(d, desc, f_desc, col_w - 8), font=f_desc, fill=SUB)

        rows = (len(items) + cols - 1) // cols
        y += rows * ROW_H + 10
        # 分隔线
        if cat != cats[-1]:
            d.line([PAD, y - 4, W - PAD, y - 4], fill=LINE, width=1)

    tip = ("发「/帮助 分类名」看详细说明 · 命令前缀 / 或 #" if compact
           else "发「/帮助 分类名」只看某一类 · 命令前缀 / 或 #")
    d.text((PAD, height - 40), tip, font=f_desc, fill=SUB)

    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def level_name(level: int) -> str:
    if level >= 100:
        return "主人"
    if level >= 60:
        return "管理员"
    if level >= 50:
        return "群主"
    if level >= 40:
        return "群管"
    return "成员"
