"""今日运势卡。

同一个 QQ 号在同一天永远抽到同一张（用 uid+日期 当随机种子），
不然群友反复刷就能刷出「大吉」，这玩意儿就不值钱了。

版式：深色渐变头部（分数 + 等级）+ 白色内容区（宜忌 / 幸运 / 点评）。
🔴 上一版把分数圆环画在「渐变已经淡成白色」的区域，白圆配白底 = 看不见。
这版改成**实色头部块**，对比度靠分块解决，不靠描边硬撑。
"""

from __future__ import annotations

import io
import random
import time

from .menuimg import find_bold_font, find_font

GOOD = [
    "摸鱼", "写代码", "早睡", "喝奶茶", "表白", "开会", "刷题", "撸猫", "散步", "请假",
    "投资", "打扫", "复盘", "剪头发", "换头像", "请客", "夸人", "装死", "学习", "吃火锅",
    "迟到", "发朋友圈", "买彩票", "清内存", "改签名", "看番", "爬山", "早起", "删好友", "搬家",
]
BAD = [
    "熬夜", "对线", "立flag", "借钱", "改需求", "裸奔", "吹牛", "抢红包", "开黑", "发誓",
    "看前任", "喝咖啡", "打车", "双排", "删库", "装懂", "画饼", "摸鱼被抓", "通宵", "发誓减肥",
    "接私活", "给老板提意见", "吃自助", "周末早起", "相信明天", "裸辞", "拼手速", "信运气",
]
COLORS = [
    ("鲸蓝", (77, 107, 254)), ("薄荷绿", (46, 189, 133)), ("落日橙", (240, 137, 68)),
    ("樱花粉", (236, 122, 158)), ("雾紫", (139, 122, 224)), ("奶白", (240, 240, 235)),
    ("墨黑", (45, 52, 66)), ("鹅黄", (238, 199, 84)), ("海盐青", (90, 180, 190)),
]
TIERS = [
    (90, "大吉", (232, 150, 40), ["今天你说啥都对，别浪费。", "运气好到犯规，赶紧干点正事。", "天时地利人和，冲。"]),
    (75, "吉", (40, 158, 106), ["顺，按计划走就行。", "小有斩获，稳住。", "该出手时别犹豫。"]),
    (60, "中吉", (56, 118, 216), ["平平淡淡才是真。", "不功不过，适合干杂活。", "按部就班，别折腾。"]),
    (40, "小吉", (134, 112, 206), ["有点小磕碰，问题不大。", "低调点，别出头。", "该怂就怂。"]),
    (0, "凶", (110, 118, 132), ["今天适合躺平。", "少说话，多喝水。", "什么都别干，就是最好的干。"]),
]

W = 640
HEAD_H = 322
BODY_H = 410
H = HEAD_H + BODY_H


def _tier(score: int):
    for threshold, name, color, comments in TIERS:
        if score >= threshold:
            return name, color, comments
    return TIERS[-1][1], TIERS[-1][2], TIERS[-1][3]


def _shade(rgb, k: float):
    """k<1 变暗，k>1 变亮（夹到 0~255）。"""
    return tuple(max(0, min(255, int(c * k))) for c in rgb)


def _header_image(width: int, height: int, color, radius: int):
    """深色渐变头部，底部两角圆角（用遮罩抠，别指望 rectangle 自带圆角）。"""
    from PIL import Image, ImageDraw

    hdr = Image.new("RGB", (width, height), color)
    d = ImageDraw.Draw(hdr)
    top = _shade(color, 0.72)
    for y in range(height):
        t = y / max(1, height - 1)
        row = tuple(int(top[i] + (color[i] - top[i]) * (t ** 0.9)) for i in range(3))
        d.line([(0, y), (width, y)], fill=row)

    # 右上角两枚半透明装饰圆，给纯色块一点层次
    deco = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    dd = ImageDraw.Draw(deco)
    dd.ellipse([width - 190, -110, width + 60, 140], fill=(255, 255, 255, 18))
    dd.ellipse([width - 120, 60, width + 30, 210], fill=(255, 255, 255, 14))
    hdr = Image.alpha_composite(hdr.convert("RGBA"), deco).convert("RGB")

    mask = Image.new("L", (width, height), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, width, height + radius], radius=radius, fill=255)
    return hdr, mask


def draw_fortune(user_id: int, nickname: str, bot_name: str = "") -> bytes | None:
    font_path = find_font()
    bold_path = find_bold_font()
    if not font_path:
        return None
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None

    day = time.strftime("%Y-%m-%d")
    rnd = random.Random(f"{user_id}:{day}")

    score = int(max(15, min(99, rnd.gauss(72, 17))))
    tier_name, tier_color, comments = _tier(score)
    good = rnd.sample(GOOD, 2)
    bad = rnd.sample(BAD, 2)
    lucky_num = rnd.randint(1, 9)
    color_name, color_rgb = rnd.choice(COLORS)
    comment = rnd.choice(comments)

    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)

    # ---------------- 头部 ----------------
    hdr, mask = _header_image(W, HEAD_H, tier_color, 34)
    img.paste(hdr, (0, 0), mask)
    d = ImageDraw.Draw(img)

    f_small = ImageFont.truetype(font_path, 15)
    f_label = ImageFont.truetype(font_path, 17)
    f_name = ImageFont.truetype(bold_path, 22)
    f_score = ImageFont.truetype(bold_path, 128)
    f_slash = ImageFont.truetype(font_path, 19)
    f_tier = ImageFont.truetype(bold_path, 21)

    d.text((40, 40), "今日运势", font=f_label, fill=(255, 255, 255))
    dw = d.textlength(day, font=f_small)
    d.text((W - 40 - dw, 43), day, font=f_small, fill=(255, 255, 255))

    # 等级胶囊
    tw = d.textlength(tier_name, font=f_tier)
    pill_w = tw + 34
    d.rounded_rectangle([40, 74, 40 + pill_w, 114], radius=20, fill=(255, 255, 255))
    d.text((40 + 17, 80), tier_name, font=f_tier, fill=tier_color)

    # 昵称
    name = (nickname or str(user_id))[:12]
    d.text((40 + pill_w + 14, 82), name, font=f_name, fill=(255, 255, 255))

    # 大分数：数字和 /100 当成一个整体居中，别把 /100 塞进数字里
    txt = str(score)
    slash = "/100"
    sw = d.textlength(txt, font=f_score)
    lw = d.textlength(slash, font=f_slash)
    total = sw + 8 + lw
    x0 = (W - total) / 2
    base_y = 130
    d.text((x0, base_y), txt, font=f_score, fill=(255, 255, 255))
    d.text((x0 + sw + 8, base_y + 96), slash, font=f_slash, fill=(255, 255, 255))

    d.text((40, HEAD_H - 44), "仅供娱乐 · 别当真", font=f_small, fill=(255, 255, 255))

    # ---------------- 内容区 ----------------
    y = HEAD_H + 34
    f_sec = ImageFont.truetype(bold_path, 19)
    f_tag = ImageFont.truetype(font_path, 19)
    f_quote = ImageFont.truetype(font_path, 19)
    f_num = ImageFont.truetype(bold_path, 34)

    def section(label, items, color, y0):
        tint = tuple(int(c + (255 - c) * 0.88) for c in color)
        d.rounded_rectangle([40, y0, 80, y0 + 40], radius=12, fill=color)
        lw = d.textlength(label, font=f_sec)
        d.text((60 - lw / 2, y0 + 9), label, font=f_sec, fill=(255, 255, 255))

        x = 96
        for item in items:
            w = d.textlength(item, font=f_tag)
            pw = w + 30
            d.rounded_rectangle([x, y0, x + pw, y0 + 40], radius=12, fill=tint)
            d.text((x + 15, y0 + 9), item, font=f_tag, fill=_shade(color, 0.7))
            x += pw + 10
        return y0 + 40

    y = section("宜", good, (40, 158, 106), y) + 16
    y = section("忌", bad, (214, 86, 86), y) + 30

    # 幸运数字 / 幸运色
    tile_w = (W - 80 - 16) // 2
    tile_h = 96
    d.rounded_rectangle([40, y, 40 + tile_w, y + tile_h], radius=16,
                        fill=(249, 250, 252), outline=(232, 238, 247), width=1)
    d.text((62, y + 18), "幸运数字", font=f_small, fill=(140, 150, 165))
    d.text((62, y + 40), str(lucky_num), font=f_num, fill=(40, 50, 66))

    x2 = 40 + tile_w + 16
    d.rounded_rectangle([x2, y, x2 + tile_w, y + tile_h], radius=16,
                        fill=(249, 250, 252), outline=(232, 238, 247), width=1)
    d.text((x2 + 22, y + 18), "幸运色", font=f_small, fill=(140, 150, 165))
    d.rounded_rectangle([x2 + 22, y + 48, x2 + 60, y + 78], radius=8, fill=color_rgb,
                        outline=(214, 220, 230), width=1)
    d.text((x2 + 70, y + 50), color_name, font=f_tag, fill=(40, 50, 66))

    y += tile_h + 26

    # 点评
    d.rounded_rectangle([40, y, W - 40, y + 76], radius=16, fill=(247, 249, 253))
    d.rounded_rectangle([40, y + 18, 44, y + 58], radius=2, fill=tier_color)
    d.text((64, y + 26), "「" + comment + "」", font=f_quote, fill=(70, 82, 100))

    if bot_name:
        footer = f"{bot_name} 出品"
        fw = d.textlength(footer, font=f_small)
        d.text(((W - fw) / 2, H - 32), footer, font=f_small, fill=(178, 186, 198))

    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()
