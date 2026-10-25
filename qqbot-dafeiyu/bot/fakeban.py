"""假封禁通知（fban）。

纯娱乐：生成一张「账号已被封禁」的截图，@ 一下对方，不执行任何真实操作。
真封禁是 `/拉黑`（踢出 + 进黑名单 + 拒绝再入），别搞混。
"""

from __future__ import annotations

import io
import time

from .menuimg import find_bold_font, find_font

W, H = 640, 430


def render_fake_ban(user_id: int, nickname: str, days: int = 3,
                    reason: str = "违反群规", by: str = "") -> bytes | None:
    font_path = find_font()
    bold_path = find_bold_font()
    if not font_path:
        return None
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None

    img = Image.new("RGB", (W, H), (26, 29, 36))
    d = ImageDraw.Draw(img)

    # 顶部红色警示条
    d.rectangle([0, 0, W, 6], fill=(226, 68, 68))

    f_title = ImageFont.truetype(bold_path, 30)
    f_sub = ImageFont.truetype(font_path, 15)
    f_label = ImageFont.truetype(font_path, 16)
    f_value = ImageFont.truetype(bold_path, 19)
    f_foot = ImageFont.truetype(font_path, 13)

    # 警告图标：圆 + 感叹号
    cx, cy, r = 62, 88, 26
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(226, 68, 68), width=3)
    d.rectangle([cx - 2, cy - 13, cx + 2, cy + 4], fill=(226, 68, 68))
    d.ellipse([cx - 2.5, cy + 8, cx + 2.5, cy + 13], fill=(226, 68, 68))

    d.text((cx + r + 18, 62), "账号已被封禁", font=f_title, fill=(240, 242, 246))
    d.text((cx + r + 20, 100), "该账号存在违规行为，已限制登录", font=f_sub, fill=(140, 148, 162))

    # 信息面板
    px0, py0, px1 = 44, 152, W - 44
    d.rounded_rectangle([px0, py0, px1, py0 + 186], radius=14, fill=(33, 37, 46),
                        outline=(48, 54, 66), width=1)

    now = time.time()
    until = time.strftime("%Y-%m-%d %H:%M", time.localtime(now + days * 86400))
    rows = [
        ("账　号", f"{nickname or user_id}（{user_id}）"),
        ("原　因", (reason or "违反群规")[:24]),
        ("时　长", f"{days} 天"),
        ("解封时间", until),
    ]
    y = py0 + 20
    for label, value in rows:
        d.text((px0 + 24, y), label, font=f_label, fill=(126, 134, 148))
        d.text((px0 + 118, y - 2), value, font=f_value, fill=(228, 232, 238))
        y += 40

    # 底部
    d.text((44, H - 66), "如有疑问，请联系群管理员申诉", font=f_sub, fill=(120, 128, 142))
    foot = f"本通知由 {by or '群管理系统'} 生成 · 仅供娱乐" if by else "仅供娱乐 · 非真实封禁"
    fw = d.textlength(foot, font=f_foot)
    d.text((W - 44 - fw, H - 32), foot, font=f_foot, fill=(96, 104, 118))

    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()
