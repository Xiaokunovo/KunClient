"""表情包生成。

两种玩法：
1. 纯文字表情包 —— 白底黑字那种经典款，几种配色可选；
2. 给图配字 —— 回复一张图，把大字压在图上（这才是群里真正会用的）。

字号是「从大到小试到放得下」算出来的，不是写死的 —— 短句字巨大，长句自动缩小，
比固定字号好看得多。
"""

from __future__ import annotations

import io

from .menuimg import find_bold_font, find_font

# 配色：(背景, 文字, 描边/边框)
STYLES = {
    "white":  ((255, 255, 255), (26, 26, 26), (26, 26, 26)),
    "black":  ((24, 26, 31), (245, 246, 248), (24, 26, 31)),
    "yellow": ((255, 217, 59), (26, 26, 26), (26, 26, 26)),
    "red":    ((198, 40, 40), (255, 235, 59), (120, 16, 16)),
    "paper":  ((245, 240, 225), (62, 39, 35), (198, 186, 158)),
    "blue":   ((47, 111, 237), (255, 255, 255), (30, 74, 168)),
}

STYLE_ALIAS = {
    "白": "white", "白底": "white", "white": "white", "默认": "white",
    "黑": "black", "黑底": "black", "black": "black",
    "黄": "yellow", "黄底": "yellow", "暴漫": "yellow", "yellow": "yellow",
    "红": "red", "红底": "red", "red": "red",
    "报纸": "paper", "复古": "paper", "paper": "paper",
    "蓝": "blue", "蓝底": "blue", "blue": "blue",
}


def _is_bold_path(path: str) -> bool:
    p = (path or "").lower()
    return any(k in p for k in ("bd.", "bold", "simhei", "zenhei", "pingfang"))


def _tokens(text: str) -> list[str]:
    """切成「一个汉字」或「一个英文单词」，换行时按这个粒度断。"""
    out: list[str] = []
    buf = ""
    for ch in text:
        if ch.isascii() and (ch.isalnum() or ch in "'-"):
            buf += ch
        else:
            if buf:
                out.append(buf)
                buf = ""
            out.append(ch)
    if buf:
        out.append(buf)
    return out


def _wrap(draw, text: str, font, max_w: float) -> list[str]:
    lines: list[str] = []
    for raw_line in text.split("\n"):
        cur = ""
        for tok in _tokens(raw_line):
            trial = cur + tok
            if cur and draw.textlength(trial, font=font) > max_w:
                lines.append(cur)
                cur = tok.lstrip()
            else:
                cur = trial
        lines.append(cur)
    return lines or [""]


def _fit(draw, text: str, font_path: str, max_w: float, max_h: float, hi: int = 110):
    """从大到小试字号，返回 (font, lines, line_height)。"""
    from PIL import ImageFont

    size = hi
    while size >= 16:
        font = ImageFont.truetype(font_path, size)
        lines = _wrap(draw, text, font, max_w)
        line_h = int(size * 1.28)
        if line_h * len(lines) <= max_h:
            return font, lines, line_h
        size -= 4
    font = ImageFont.truetype(font_path, 16)
    return font, _wrap(draw, text, font, max_w)[: int(max_h // 21) or 1], 21


def _draw_centered(draw, lines, font, line_h, box, fill, stroke=None, stroke_w=0):
    x0, y0, x1, y1 = box
    total = line_h * len(lines)
    y = y0 + (y1 - y0 - total) / 2
    for line in lines:
        w = draw.textlength(line, font=font)
        draw.text(
            (x0 + (x1 - x0 - w) / 2, y), line, font=font, fill=fill,
            stroke_width=stroke_w, stroke_fill=stroke,
        )
        y += line_h


def render_meme(text: str, style: str = "white", size: int = 560) -> bytes | None:
    """纯文字表情包。"""
    font_path = find_bold_font()
    if not font_path or not text.strip():
        return None
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return None

    bg, fg, edge = STYLES.get(style, STYLES["white"])
    img = Image.new("RGB", (size, size), bg)
    d = ImageDraw.Draw(img)

    pad = 44
    border = 6
    if style != "white":
        d.rectangle([0, 0, size - 1, size - 1], outline=edge, width=border)

    font, lines, line_h = _fit(d, text.strip(), font_path, size - pad * 2, size - pad * 2, hi=150)
    # 同色描边 = 跨平台的「伪加粗」。已经是粗体的字体再加描边会糊成一团，所以要判一下。
    embolden = 0 if _is_bold_path(font_path) else max(1, font.size // 56)
    _draw_centered(d, lines, font, line_h, (pad, pad, size - pad, size - pad),
                   fg, stroke=fg, stroke_w=embolden)

    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def render_on_image(raw: bytes, text: str, position: str = "bottom") -> bytes | None:
    """在用户给的图上压大字。图片会先缩到长边 640，免得输出过大。"""
    font_path = find_bold_font()
    if not font_path:
        return None
    try:
        from PIL import Image, ImageDraw, ImageOps
    except ImportError:
        return None

    try:
        src = Image.open(io.BytesIO(raw))
        src = ImageOps.exif_transpose(src).convert("RGB")
    except Exception:  # noqa: BLE001
        return None

    # 太大就缩，太小就放大到能看清字
    long_edge = max(src.size)
    if long_edge > 720:
        ratio = 720 / long_edge
        src = src.resize((max(1, int(src.width * ratio)), max(1, int(src.height * ratio))), Image.LANCZOS)
    elif long_edge < 320:
        ratio = 320 / long_edge
        src = src.resize((int(src.width * ratio), int(src.height * ratio)), Image.LANCZOS)

    W, H = src.size
    d = ImageDraw.Draw(src)
    pad = int(W * 0.06)
    max_w = W - pad * 2

    if position == "top":
        box_h = int(H * 0.45)
        box = (pad, pad // 2, W - pad, pad // 2 + box_h)
    elif position == "middle":
        box_h = int(H * 0.5)
        box = (pad, (H - box_h) // 2, W - pad, (H + box_h) // 2)
    else:
        box_h = int(H * 0.45)
        box = (pad, H - box_h - pad // 2, W - pad, H - pad // 2)

    font, lines, line_h = _fit(d, text.strip(), font_path, max_w, box_h, hi=max(28, int(W * 0.22)))
    _draw_centered(d, lines, font, line_h, box, (255, 255, 255), (0, 0, 0), max(2, font.size // 14))

    buf = io.BytesIO()
    src.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def parse_style(args: str) -> tuple[str, str]:
    """把「黑 你好」拆成 (style, 正文)；第一个词不是风格就整串都算正文。"""
    parts = args.strip().split(None, 1)
    if len(parts) == 2 and parts[0] in STYLE_ALIAS:
        return STYLE_ALIAS[parts[0]], parts[1].strip()
    return "white", args.strip()
