from PIL import Image, ImageDraw
from pathlib import Path

# 2026-10-16：原脚本写死了旧开发机路径（D:\桌面\SWT-C++\resources），换机即失效；
# 这里改为相对脚本自身定位，并把产物同时写入引擎资源目录与外壳资源目录。
# 勾选态配色同步 iOS 27 系统蓝（#0A84FF），与 light.qss / dark.qss 的强调色一致。
HERE = Path(__file__).resolve()
REPO = HERE.parents[3]                       # third_party/swt-edl/tools → 仓库根
OUT_DIRS = [
    HERE.parents[1] / 'resources',           # third_party/swt-edl/resources
    REPO / 'src' / 'cpp' / 'resources',      # 外壳资源目录
]
S = 4

ACCENT = '#0A84FF'        # iOS 27 系统蓝
OFF_LIGHT_FILL = '#ffffff'
OFF_LIGHT_STROKE = '#c3cfde'
OFF_DARK_FILL = '#20252b'
OFF_DARK_STROKE = '#4a5663'


def checkbox(path, fill, stroke, checked=False):
    im = Image.new('RGBA', (16 * S, 16 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((1 * S, 1 * S, 15 * S, 15 * S), radius=3 * S,
                        fill=fill, outline=stroke, width=S)
    if checked:
        pts = [(4.2 * S, 8.2 * S), (6.8 * S, 10.7 * S), (11.9 * S, 5.4 * S)]
        d.line(pts, fill='#ffffff', width=2 * S, joint='curve')
    im.resize((16, 16), Image.Resampling.LANCZOS).save(path, optimize=True)


def logo(path, size=128):
    n = size * S
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    # 与 SVG 端点近似的对角线性渐变（iOS 蓝 → 深一档）
    pix = Image.new('RGBA', (n, n))
    p = pix.load()
    c0 = (90, 176, 255)      # #5AB0FF
    c1 = (10, 132, 255)      # #0A84FF
    for y in range(n):
        for x in range(n):
            t = max(0, min(1, ((x - 16 * S) + (y - 12 * S)) / (202 * S)))
            p[x, y] = tuple(round(c0[i] * (1 - t) + c1[i] * t) for i in range(3)) + (255,)
    mask = Image.new('L', (n, n), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((8 * S, 8 * S, 120 * S, 120 * S), radius=30 * S, fill=255)
    im.alpha_composite(Image.composite(pix, Image.new('RGBA', (n, n), (0, 0, 0, 0)), mask))
    d = ImageDraw.Draw(im)
    d.ellipse((31 * S, 30 * S, 93 * S, 92 * S), outline='white', width=14 * S)
    d.line(((73 * S, 76 * S), (98 * S, 101 * S)), fill='white', width=14 * S)
    d.ellipse(((73 * S - 7 * S), (76 * S - 7 * S), (73 * S + 7 * S), (76 * S + 7 * S)), fill='white')
    d.ellipse(((98 * S - 7 * S), (101 * S - 7 * S), (98 * S + 7 * S), (101 * S + 7 * S)), fill='white')
    im.resize((size, size), Image.Resampling.LANCZOS).save(path, optimize=True)


# 各目录 logo 目标尺寸：外壳用 256，引擎独立运行用 128（勿降级既有资产）
LOGO_SIZE = {0: 128, 1: 256}

for idx, root in enumerate(OUT_DIRS):
    root.mkdir(parents=True, exist_ok=True)
    checkbox(root / 'check-off-dark.png', OFF_DARK_FILL, OFF_DARK_STROKE)
    checkbox(root / 'check-off-light.png', OFF_LIGHT_FILL, OFF_LIGHT_STROKE)
    checkbox(root / 'check-on.png', ACCENT, ACCENT, True)
    logo(root / 'logo.png', LOGO_SIZE.get(idx, 128))
    for f in ('logo.png', 'check-on.png', 'check-off-dark.png', 'check-off-light.png'):
        p = root / f
        print(p, p.stat().st_size)

