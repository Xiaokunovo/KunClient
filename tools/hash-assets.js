#!/usr/bin/env node
/**
 * 把 next 静态导出产物里的 chunk 改成「内容寻址」文件名。
 *
 * ── 为什么必须做（2026-10-10 实测的真故障）──
 * Turbopack 产出的 chunk 文件名（`0cz1d0mv5g_q7.js`）**跨构建是稳定的**，不是内容哈希。
 * 而线上走 Cloudflare，CF 对 `.js/.css` 默认按扩展名缓存，并把 Browser Cache TTL（4h）
 * 写进 `Cache-Control`。于是每次部署后会出现：
 *
 *     新 index.html  →  引用 0cz1d0mv5g_q7.js
 *     CF 边缘        →  仍然回旧的那一份 0cz1d0mv5g_q7.js
 *
 * 新旧 bundle 的内部模块 id 对不上 → 客户端入口抛错、React 根本没 hydrate →
 * 页面看着「在」，但所有 JS 交互全停（首屏标题不揭示、圆环不转、数字不滚）。
 * 实测：绕开 CF（nginx 回环）拿到的是正确内容，经 CF 拿到的是旧内容；
 * 带 `?cb=xxx` 的请求绕开了缓存键，拿到的也是正确内容 —— 三条一起才定性成「边缘缓存旧副本」。
 *
 * ── 修法 ──
 * 文件名带上内容哈希：内容变 → 名字变 → **新 URL**，CF 不可能命中旧副本；
 * 内容不变 → 名字不变 → 缓存继续有效。既修了故障，又不用去清 CF 缓存。
 *
 * 幂等：对同一份产物重复跑不会二次加哈希。
 */
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const OUT = path.resolve(__dirname, "..", "website", "out");
const CHUNKS = path.join(OUT, "_next", "static", "chunks");
const HASHED = /\.[0-9a-f]{8}\.(js|css)$/;
const TEXT = new Set([".html", ".txt", ".js", ".css", ".json", ".xml", ".svg"]);

if (!fs.existsSync(CHUNKS)) {
  console.error("找不到 " + CHUNKS + " —— 先跑 next build");
  process.exit(1);
}

const names = fs.readdirSync(CHUNKS).filter((n) => n.endsWith(".js") || n.endsWith(".css"));
if (names.some((n) => HASHED.test(n))) {
  console.log("chunk 名已带内容哈希，跳过（幂等）");
  process.exit(0);
}

// 1) 算出内容哈希 → 新文件名
const map = new Map();
for (const n of names) {
  const buf = fs.readFileSync(path.join(CHUNKS, n));
  const h = crypto.createHash("sha256").update(buf).digest("hex").slice(0, 8);
  const ext = path.extname(n);
  map.set(n, n.slice(0, -ext.length) + "." + h + ext);
}

// 2) 先重命名
for (const [a, b] of map) fs.renameSync(path.join(CHUNKS, a), path.join(CHUNKS, b));

// 3) 单遍替换全部文本引用。
//    ⚠️ 必须用「一次性正则替换」而不是循环 replace：新名字**包含旧名字作为前缀**
//    （0cz1d0mv5g_q7.js → 0cz1d0mv5g_q7.a1b2c3d4.js），多趟替换会把新名字再"加工"一次。
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const re = new RegExp("(" + [...map.keys()].sort((a, b) => b.length - a.length).map(esc).join("|") + ")", "g");

let touched = 0;
(function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { walk(p); continue; }
    if (!TEXT.has(path.extname(e.name))) continue;
    const s = fs.readFileSync(p, "utf8");
    const o = s.replace(re, (m) => map.get(m) || m);
    if (o !== s) { fs.writeFileSync(p, o); touched++; }
  }
})(OUT);

console.log(`内容寻址完成：改名 ${map.size} 个 chunk，改写 ${touched} 个引用文件`);
for (const [a, b] of map) console.log("  " + a + "  ->  " + b);
