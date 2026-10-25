#!/usr/bin/env python3
"""重新生成应用图标 Assets/logo.ico（供 csproj 的 <ApplicationIcon> 使用）。

为什么需要这个脚本：
    原先的 logo.ico 是坏的 —— ICONDIR 声明有 6 张图，但第 2~6 个目录项全是 0
    （宽高 0、长度 0、偏移 0），而文件里真实存在的 32/48/... 图又没有目录项指向它们。
    Windows 解析这种图标会失败，任务栏 / 资源管理器里就退回成默认占位图标
    （用户反馈的「logo 没了」）。

    用这个脚本从 logo.png 重新生成一份目录自洽的多尺寸 ico：
      - 先按 alpha 裁掉四周透明边（原图 45% 是空白，不裁的话图标里的小 K 特别小）
      - 再留少量内边距，避免贴边被裁
      - 尺寸覆盖 Windows 常用档位 16/24/32/48/64/128/256

用法：
    python tools/make-app-icon.py            # 直接覆盖 Assets/logo.ico
    python tools/make-app-icon.py --check    # 只校验现有 ico 是否结构自洽，不改文件
"""
import argparse
import os
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'src', 'KunClient.App', 'Assets')
SRC_PNG = os.path.join(ASSETS, 'logo.png')
OUT_ICO = os.path.join(ASSETS, 'logo.ico')

SIZES = [16, 24, 32, 48, 64, 128, 256]
PAD_RATIO = 0.06  # 内容四周留白比例（相对内容较短边）


def inspect_ico(path):
    """解析 ico 目录，返回 (count, entries)。entries 里每项是 dict。"""
    with open(path, 'rb') as f:
        data = f.read()
    reserved, typ, count = struct.unpack_from('<HHH', data, 0)
    entries = []
    for i in range(count):
        off = 6 + i * 16
        w, h, cc, rsv, planes, bpp, size, imgoff = struct.unpack_from('<BBBBHHII', data, off)
        entries.append({
            'index': i, 'w': w or 256, 'h': h or 256, 'bpp': bpp,
            'size': size, 'offset': imgoff,
            'png': data[imgoff:imgoff + 8] == b'\x89PNG\r\n\x1a\n',
        })
    return count, entries, len(data)


def check(path):
    """返回 (ok, message_lines)。ok=False 表示图标结构不可用。"""
    lines = []
    if not os.path.exists(path):
        return False, [f'文件不存在: {path}']
    count, entries, total = inspect_ico(path)
    lines.append(f'文件: {path}')
    lines.append(f'ICONDIR count={count} 文件大小={total}')

    ok = True
    usable = 0
    for e in entries:
        valid = e['size'] > 0 and e['offset'] > 0 and e['offset'] + e['size'] <= total
        usable += 1 if valid else 0
        lines.append(
            f"  #{e['index']}: {e['w']}x{e['h']} bpp={e['bpp']} bytes={e['size']} "
            f"off={e['offset']} {'PNG' if e['png'] else 'BMP'} {'OK' if valid else '← 损坏（空目录项）'}"
        )
        if not valid:
            ok = False

    if usable == 0:
        ok = False
    # 16px 档必须存在，否则任务栏小图标会糊
    if not any(e['w'] == 16 and e['size'] > 0 for e in entries):
        ok = False
        lines.append('  缺少可用的 16x16 档位')
    lines.append(f'结论: {"结构自洽" if ok else "结构损坏 —— Windows 会退回默认占位图标"}')
    return ok, lines


def _bmp_frame(img):
    """把一张 RGBA 图编码成 ICO 内嵌的 BMP/DIB 帧（32bpp + 全零 AND 掩码，行自下而上）。"""
    w, h = img.size
    px = img.load()
    xor = bytearray()
    for y in range(h - 1, -1, -1):          # DIB 是自下而上存储
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))      # BGRA
    mask_row = ((w + 31) // 32) * 4         # AND 掩码每行按 4 字节对齐
    and_mask = bytes(mask_row * h)          # 32bpp 走 alpha，掩码全 0 即可

    header = struct.pack(
        '<IiiHHIIiiII',
        40, w, h * 2, 1, 32, 0, len(xor) + len(and_mask), 0, 0, 0, 0
    )
    return header + bytes(xor) + and_mask


def _png_frame(img):
    import io
    buf = io.BytesIO()
    img.save(buf, format='PNG', optimize=True)
    return buf.getvalue()


def write_ico(img, path):
    """手写 ICO：<=128 用 BMP/DIB（兼容性最好），256 用 PNG（否则单帧就 256KB）。

    不直接用 Pillow 的 ICO 写出，是因为它对每个尺寸都写成 PNG 帧，
    而部分 Windows 外壳路径对「非 256 的 PNG 帧」支持不佳 —— 应用图标宁可保守。
    """
    frames = []
    for s in SIZES:
        frame_img = img.resize((s, s), Image.LANCZOS)
        frames.append((s, _png_frame(frame_img) if s >= 256 else _bmp_frame(frame_img)))

    n = len(frames)
    offset = 6 + n * 16
    dir_entries = bytearray()
    blob = bytearray()
    for s, data in frames:
        dir_entries += struct.pack(
            '<BBBBHHII',
            s if s < 256 else 0, s if s < 256 else 0, 0, 0, 1, 32, len(data), offset
        )
        blob += data
        offset += len(data)

    with open(path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, n) + bytes(dir_entries) + bytes(blob))
    return [(s, len(d)) for s, d in frames]


def build():
    if not os.path.exists(SRC_PNG):
        sys.exit(f'找不到源图: {SRC_PNG}')

    src = Image.open(SRC_PNG).convert('RGBA')
    bbox = src.split()[3].getbbox()
    if bbox:
        src = src.crop(bbox)

    # 加内边距，做成正方形画布，避免非等比缩放变形
    side = max(src.size)
    pad = int(side * PAD_RATIO)
    canvas_side = side + pad * 2
    canvas = Image.new('RGBA', (canvas_side, canvas_side), (0, 0, 0, 0))
    canvas.paste(src, ((canvas_side - src.size[0]) // 2, (canvas_side - src.size[1]) // 2), src)

    layout = write_ico(canvas, OUT_ICO)
    print(f'已生成 {OUT_ICO}')
    print(f'  源内容尺寸 {src.size} → 画布 {canvas.size}（内边距 {pad}px）')
    print('  帧: ' + ', '.join(f'{s}px/{n}B' for s, n in layout))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true', help='只校验现有 ico，不重新生成')
    args = ap.parse_args()

    if args.check:
        ok, lines = check(OUT_ICO)
        print('\n'.join(lines))
        sys.exit(0 if ok else 1)

    build()
    ok, lines = check(OUT_ICO)
    print('\n'.join(lines))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
