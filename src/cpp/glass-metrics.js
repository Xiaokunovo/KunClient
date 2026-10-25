// 量化评估「液态玻璃」效果的客观指标。
//
// 用法：node glass-metrics.js <before.png> <after.png>
//
// 指标含义：
//   局部对比度（local contrast） —— 玻璃区域内的像素标准差 / 梯度能量。
//     真玻璃因为磨砂保留了背板的低频结构，边缘还有折射造成的形变，
//     所以这个值明显高于"纯色板"。白板的局部方差接近 0。
//   边缘梯度 — 沿玻璃边界一圈的梯度能量。折射会让边缘附近出现"内容被挤压"
//     的高梯度带。
//
// 只依赖 Node 内置模块 + 手写 PNG 解码（不引第三方包）。

const fs = require('fs');
const zlib = require('zlib');

// ── 最小 PNG 解码器（只支持 8-bit RGB/RGBA，非隔行）──────────────
function decodePNG(buf) {
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error('不是 PNG');
  let pos = 8;
  let width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat = [];

  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);

    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      if (bitDepth !== 8) throw new Error('只支持 8-bit，实际 ' + bitDepth);
      if (colorType !== 2 && colorType !== 6) throw new Error('只支持 RGB/RGBA，实际 colorType=' + colorType);
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
    pos += 12 + len;
  }

  const raw = zlib.inflateSync(Buffer.concat(idat));
  const channels = colorType === 6 ? 4 : 3;
  const stride = width * channels;
  const out = Buffer.alloc(width * height * 3);

  let prev = Buffer.alloc(stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));

    const cur = Buffer.alloc(stride);
    for (let i = 0; i < stride; i++) {
      const a = i >= channels ? cur[i - channels] : 0;
      const b = prev[i];
      const c = i >= channels ? prev[i - channels] : 0;
      let v = line[i];
      switch (filter) {
        case 0: break;
        case 1: v = (v + a) & 255; break;
        case 2: v = (v + b) & 255; break;
        case 3: v = (v + ((a + b) >> 1)) & 255; break;
        case 4: {
          const p = a + b - c;
          const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
          const pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          v = (v + pr) & 255;
          break;
        }
        default: throw new Error('未知 filter ' + filter);
      }
      cur[i] = v;
    }
    for (let x = 0; x < width; x++) {
      out[(y * width + x) * 3 + 0] = cur[x * channels + 0];
      out[(y * width + x) * 3 + 1] = cur[x * channels + 1];
      out[(y * width + x) * 3 + 2] = cur[x * channels + 2];
    }
    prev = cur;
  }
  return { width, height, data: out };
}

const gray = (img, x, y) => {
  const i = (y * img.width + x) * 3;
  return 0.299 * img.data[i] + 0.587 * img.data[i + 1] + 0.114 * img.data[i + 2];
};

// 区域统计：std（标准差）+ 梯度能量
function regionStats(img, x0, y0, x1, y1) {
  x0 = Math.max(0, x0); y0 = Math.max(0, y0);
  x1 = Math.min(img.width - 1, x1); y1 = Math.min(img.height - 1, y1);

  let n = 0, sum = 0, sumSq = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const v = gray(img, x, y);
      sum += v; sumSq += v * v; n++;
    }
  }
  const mean = n ? sum / n : 0;
  const variance = n ? (sumSq / n - mean * mean) : 0;
  const std = Math.sqrt(Math.max(0, variance));

  // 梯度能量：Sobel 简化为相邻差
  let gradSum = 0, gradN = 0;
  for (let y = y0 + 1; y < y1 - 1; y++) {
    for (let x = x0 + 1; x < x1 - 1; x++) {
      const gx = gray(img, x + 1, y) - gray(img, x - 1, y);
      const gy = gray(img, x, y + 1) - gray(img, x, y - 1);
      gradSum += Math.sqrt(gx * gx + gy * gy);
      gradN++;
    }
  }
  const gradient = gradN ? gradSum / gradN : 0;

  return { mean, std, gradient, pixels: n };
}

function load(path) {
  return decodePNG(fs.readFileSync(path));
}

function main() {
  const [beforePath, afterPath] = process.argv.slice(2);
  if (!beforePath || !afterPath) {
    console.error('用法: node glass-metrics.js <before.png> <after.png>');
    process.exit(2);
  }

  const before = load(beforePath);
  const after = load(afterPath);

  console.log(`before: ${before.width}x${before.height}`);
  console.log(`after : ${after.width}x${after.height}`);
  if (before.width !== after.width || before.height !== after.height) {
    console.error('⚠ 尺寸不一致，无法逐像素对比');
  }

  // 探测区域（按 1280x820 基准；其他尺寸按比例缩放）
  const sx = before.width / 1280, sy = before.height / 820;
  const R = (a, b, c, d) => [Math.round(a * sx), Math.round(b * sy), Math.round(c * sx), Math.round(d * sy)];

  const regions = [
    // Dock 玻璃条整体（含图标）—— 看整体"有没有内容"。
    // 关键不是绝对值，而是**改动前后的相对变化**。
    { name: 'Dock 整条',        box: R(468, 665, 812, 742), kind: 'glass' },
    // Dock 内部的水平切片（避开图标：图标只在中部纵向区，取最上和最下两条边缘带）
    { name: 'Dock 上边缘带',    box: R(480, 666, 800, 676), kind: 'glass' },
    { name: 'Dock 下边缘带',    box: R(480, 730, 800, 740), kind: 'glass' },
    // 左上品牌胶囊整体 + 纯玻璃区
    { name: '顶栏-左胶囊',      box: R(10, 10, 192, 46),   kind: 'glass' },
    // 右上窗口按钮胶囊
    { name: '顶栏-右胶囊',      box: R(1082, 10, 1262, 46), kind: 'glass' },
    // 对照组：纯背景（无玻璃）
    { name: '[对照] 空白背景',  box: R(300, 400, 420, 500), kind: 'bg' },
  ];

  console.log('');
  console.log('区域                  std(前 → 后)          梯度(前 → 后)         变化');
  console.log('─'.repeat(92));

  let improved = 0;
  for (const r of regions) {
    const b = regionStats(before, ...r.box);
    const a = regionStats(after, ...r.box);
    const dStd = a.std - b.std;
    const dGrad = a.gradient - b.gradient;
    const pct = b.std > 0.01 ? (dStd / b.std * 100) : 0;

    const flag = r.kind === 'glass'
      ? (pct > 15 ? '✓ 改善' : (Math.abs(pct) <= 15 ? '≈ 持平' : '✗ 变差'))
      : '（对照，应基本不变）';

    if (r.kind === 'glass' && pct > 15) improved++;

    console.log(
      r.name.padEnd(18) +
      `${b.std.toFixed(2)} → ${a.std.toFixed(2)}`.padEnd(22) +
      `${b.gradient.toFixed(2)} → ${a.gradient.toFixed(2)}`.padEnd(22) +
      flag
    );
  }

  console.log('');
  const glassCount = regions.filter(r => r.kind === 'glass').length;
  if (improved === glassCount) {
    console.log(`✓ ${improved}/${glassCount} 个玻璃区域显著改善（std 提升 >15%）`);
    process.exit(0);
  } else if (improved > 0) {
    console.log(`◐ ${improved}/${glassCount} 个玻璃区域改善，其余不明显`);
    process.exit(0);
  } else {
    console.log(`✗ 没有明显改善 —— 改动可能没生效`);
    process.exit(1);
  }
}

main();
