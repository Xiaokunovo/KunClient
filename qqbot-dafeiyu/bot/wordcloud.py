"""群聊词云。

数据源是内存里的消息台账（每群最近 200 条，重启会清空），所以是「最近的词云」，
不是「历史全量」。够用 —— 群友想看的就是最近大家在聊什么。

分词优先用 jieba；装不上就退化成「双字统计」，效果糙一点但不会挂。
布局是手写的阿基米德螺旋 + 矩形碰撞检测，不依赖 wordcloud 那种重库。
"""

from __future__ import annotations

import io
import math
import re
from collections import Counter

from .menuimg import find_font

STOPWORDS = set("""
的 了 是 我 你 他 她 它 们 在 有 就 不 也 都 这 那 个 上 下 里 和 与 或 但 而 被 把 给 让
从 到 对 为 以 及 等 着 过 地 得 很 太 更 最 会 能 要 想 说 去 来 好 还 又 再 只 才 没 别
啊 吧 呢 吗 哦 嗯 哈 呀 呗 啦 嘛 咯 喔 诶 呵 嘿 唉 咦 哇 唉 么 啥 咋 俺 咱 您
一个 我们 你们 他们 什么 这个 那个 就是 可以 没有 不是 还是 这样 那样 现在 时候
自己 知道 觉得 应该 已经 因为 所以 但是 如果 然后 而且 或者 只是 还有 一下 一样
""".split())

PALETTE = [
    (47, 111, 237), (232, 90, 90), (46, 160, 110), (235, 150, 60),
    (139, 122, 224), (219, 96, 160), (58, 140, 200), (200, 120, 60),
]


def _tokenize(texts: list[str]) -> Counter:
    joined = "\n".join(texts)
    # 去掉 QQ 号、链接、标点
    joined = re.sub(r"https?://\S+", " ", joined)
    joined = re.sub(r"\d{5,}", " ", joined)
    joined = re.sub(r"\[[^\]]{1,6}\]", " ", joined)  # [图片] [表情]
    joined = re.sub(r"[^\w\u4e00-\u9fff]+", " ", joined)

    counter: Counter = Counter()
    try:
        import jieba  # type: ignore

        for word in jieba.cut(joined):
            word = word.strip()
            if len(word) >= 2 and word not in STOPWORDS and not word.isdigit():
                counter[word] += 1
    except ImportError:
        # 退化：连续汉字切成 2 字窗口
        for chunk in re.findall(r"[\u4e00-\u9fff]{2,}", joined):
            for i in range(len(chunk) - 1):
                bg = chunk[i:i + 2]
                if bg not in STOPWORDS:
                    counter[bg] += 1
    return counter


def build(texts: list[str], limit: int = 60) -> list[tuple[str, int]]:
    counter = _tokenize(texts)
    # 只留出现 2 次以上的，否则全是噪音
    items = [(w, c) for w, c in counter.most_common(limit * 3) if c >= 2]
    return items[:limit]


def render(texts: list[str], title: str = "") -> bytes | None:
    font_path = find_font()
    if not font_path:
        return None
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None

    words = build(texts)
    if not words:
        return None

    W, H = 760, 520
    img = Image.new("RGB", (W, H), (250, 251, 253))
    d = ImageDraw.Draw(img)

    # 标题
    if title:
        f_title = ImageFont.truetype(font_path, 20)
        d.text((34, 26), title, font=f_title, fill=(120, 130, 148))

    placed: list[tuple[float, float, float, float]] = []
    top_margin = 74 if title else 24   # 有标题就把上面那块让出来，别糊在一起
    cx, cy = W / 2, (H + top_margin) / 2 + 8
    top = words[0][1] or 1
    max_size, min_size = 62, 15

    for idx, (word, count) in enumerate(words):
        ratio = count / top
        size = int(min_size + (max_size - min_size) * (ratio ** 0.62))
        try:
            font = ImageFont.truetype(font_path, size)
        except OSError:
            continue
        tw = d.textlength(word, font=font)
        th = size * 1.15
        if tw > W - 80:
            continue

        spot = _find_spot(placed, tw, th, cx, cy, W, H, idx == 0, top_margin)
        if not spot:
            continue
        x, y = spot
        color = PALETTE[idx % len(PALETTE)]
        # 词频越高颜色越深一点
        d.text((x, y), word, font=font, fill=color)
        placed.append((x, y, x + tw, y + th))

    if not placed:
        return None

    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def _find_spot(placed, tw, th, cx, cy, W, H, first=False, top=24):
    """阿基米德螺旋找一个不撞车的位置。"""
    pad = 5

    def ok(x, y):
        if x < 20 or y < top or x + tw > W - 20 or y + th > H - 18:
            return False
        for (x0, y0, x1, y1) in placed:
            if not (x + tw + pad < x0 or x > x1 + pad or y + th + pad < y0 or y > y1 + pad):
                return False
        return True

    if first:
        x, y = cx - tw / 2, cy - th / 2
        if ok(x, y):
            return x, y

    angle = 0.0
    radius = 0.0
    for _ in range(2600):
        angle += 0.28
        radius += 1.15
        x = cx + radius * math.cos(angle) - tw / 2
        y = cy + radius * math.sin(angle) * 0.62 - th / 2  # 压扁一点，横向更饱满
        if ok(x, y):
            return x, y
    return None
