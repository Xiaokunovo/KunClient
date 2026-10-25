/**
 * 模块分类防漂移校验（2026-10-13）
 *
 * 背景：「能力全景」三卡在官网优先按 /api/app/limits 实时渲染，data.ts 里的
 * MODULES 只是接口不可达时的静态回落。两份内容一旦漂移，就会出现
 * 「控制台改了分类、官网回落版还是旧的」这种只有接口挂掉时才暴露的问题。
 *
 * 本工具把三处逐项对齐：
 *   ① server/server.js  FEATURE_CATALOG（功能目录，唯一真相）
 *   ② server/server.js  MODULE_CATALOG（默认模块归属）
 *   ③ website/src/lib/data.ts  FEATURE_HINTS（每个功能的卡片文案）+ MODULES（回落卡片）
 *
 * 校验规则：
 *   · 目录里每个功能都必须有 FEATURE_HINTS 文案（否则实时卡片只能显示干 label）；
 *   · FEATURE_HINTS 不允许残留目录外的 key（删了功能忘了删文案）；
 *   · 回落 MODULES 的 moduleId 集合 == MODULE_CATALOG 的 id 集合；
 *   · 每个模块：回落卡片 items == 默认 features 经 FEATURE_HINTS 映射后的文案（按序）；
 *   · title / desc 与 MODULE_CATALOG 一致（desc 忽略末尾句号）。
 *
 * 用法：cd tools && node check-modules-drift.js   （退出码非 0 = 有漂移）
 */
const fs = require('fs');
const path = require('path');

const SERVER = path.resolve(__dirname, '..', 'server', 'server.js');
const DATA = path.resolve(__dirname, '..', 'website', 'src', 'lib', 'data.ts');

const server = fs.readFileSync(SERVER, 'utf8');
const data = fs.readFileSync(DATA, 'utf8');

let fail = 0;
const bad = msg => { fail++; console.log('  [DRIFT] ' + msg); };
const good = msg => console.log('  OK   ' + msg);

// ── ① 服务端功能目录 ──────────────────────────────────────────────────────
const fcBlock = server.match(/const FEATURE_CATALOG = \[([\s\S]*?)\n\];/);
if (!fcBlock) { console.error('找不到 server.js 的 FEATURE_CATALOG'); process.exit(2); }
const catalog = [...fcBlock[1].matchAll(/key: '([^']+)',\s*label: '([^']+)'/g)]
  .map(m => ({ key: m[1], label: m[2] }));

// ── ② 服务端默认模块归属 ──────────────────────────────────────────────────
const mcBlock = server.match(/const MODULE_CATALOG = \[([\s\S]*?)\n\];/);
if (!mcBlock) { console.error('找不到 server.js 的 MODULE_CATALOG'); process.exit(2); }
const defaultModules = [];
mcBlock[1].split(/\n  \{/).forEach(chunk => {
  const id = chunk.match(/id: '([^']+)'/);
  if (!id) return;
  const title = (chunk.match(/title: '([^']+)'/) || [])[1] || '';
  const desc = (chunk.match(/desc: '([^']+)'/) || [])[1] || '';
  const fBlock = (chunk.match(/features: \[([\s\S]*?)\]/) || [])[1] || '';
  const features = [...fBlock.matchAll(/'([^']+)'/g)].map(m => m[1]);
  defaultModules.push({ id: id[1], title, desc, features });
});

// ── ③ 官网静态回落 + 功能文案表 ───────────────────────────────────────────
const hintsBlock = data.match(/export const FEATURE_HINTS[^{]*\{([\s\S]*?)\n\};/);
if (!hintsBlock) { console.error('找不到 data.ts 的 FEATURE_HINTS'); process.exit(2); }
const hints = {};
[...hintsBlock[1].matchAll(/'([^']+)':\s*'((?:[^'\\]|\\.)*)'/g)].forEach(m => { hints[m[1]] = m[2]; });

const modBlock = data.match(/export const MODULES = \[([\s\S]*?)\n\];/);
if (!modBlock) { console.error('找不到 data.ts 的 MODULES'); process.exit(2); }
const staticModules = [];
modBlock[1].split(/\n  \{/).forEach(chunk => {
  const id = chunk.match(/moduleId:\s*['"]([^'"]+)['"]/);
  if (!id) return;
  const title = (chunk.match(/title:\s*['"]([^'"]*)['"]/) || [])[1] || '';
  const desc = (chunk.match(/desc:\s*['"]([^'"]*)['"]/) || [])[1] || '';
  const iBlock = (chunk.match(/items: \[([\s\S]*?)\]/) || [])[1] || '';
  const items = [...iBlock.matchAll(/['"]((?:[^'"\\]|\\.)*)['"]/g)].map(m => m[1]);
  staticModules.push({ id: id[1], title, desc, items });
});

// ── 校验 ──────────────────────────────────────────────────────────────────
console.log('模块分类防漂移校验（server.js ⇄ website/data.ts）');
console.log(`  功能目录 ${catalog.length} 项；默认模块 ${defaultModules.length} 个；回落卡片 ${staticModules.length} 张`);

// 每个功能都要有文案
const noHint = catalog.filter(f => !hints[f.key]).map(f => f.key);
if (noHint.length) bad(`以下功能缺 FEATURE_HINTS 文案：${noHint.join(', ')}`);
else good('每个功能都有卡片文案');

// 不允许残留目录外 key
const stale = Object.keys(hints).filter(k => !catalog.some(f => f.key === k));
if (stale.length) bad(`FEATURE_HINTS 残留目录外 key：${stale.join(', ')}`);
else good('无残留文案 key');

// 模块 id 集合一致
const defIds = defaultModules.map(m => m.id).join(',');
const staIds = staticModules.map(m => m.id).join(',');
if (defIds !== staIds) bad(`模块 id 不一致：服务端 [${defIds}] vs 回落 [${staIds}]`);
else good(`模块 id 一致（${defIds}）`);

// 逐模块比对
defaultModules.forEach(def => {
  const sta = staticModules.find(m => m.id === def.id);
  if (!sta) return;
  const expect = def.features.map(k => hints[k]).filter(Boolean);
  if (expect.join('\n') !== sta.items.join('\n')) {
    bad(`${def.id} items 与默认归属不符：\n        期望: ${expect.join(' / ')}\n        实际: ${sta.items.join(' / ')}`);
  } else good(`${def.id} items 与默认归属一致（${expect.length} 项）`);
  if (sta.title !== def.title) bad(`${def.id} title 不一致：${def.title} vs ${sta.title}`);
  const strip = s => String(s || '').replace(/[。．.\s]+$/, '');
  if (strip(sta.desc) !== strip(def.desc)) bad(`${def.id} desc 不一致：\n        服务端: ${def.desc}\n        回落:   ${sta.desc}`);
});

// 目录自身一致性（重复归属 / 幽灵 key）
const owner = {};
defaultModules.forEach(m => m.features.forEach(k => {
  if (owner[k]) bad(`功能 ${k} 同时属于 ${owner[k]} 与 ${m.id}`);
  owner[k] = m.id;
}));
const orphan = catalog.filter(f => !owner[f.key]).map(f => f.key);
const ghost = Object.keys(owner).filter(k => !catalog.some(f => f.key === k));
if (orphan.length) bad(`未归入任何模块的功能：${orphan.join(', ')}`);
if (ghost.length) bad(`模块内 key 不在功能目录：${ghost.join(', ')}`);
if (!orphan.length && !ghost.length) good('默认归属无孤儿 / 无幽灵 key');

if (fail) {
  console.log(`\n共 ${fail} 处漂移 —— 改 server.js 的目录/归属后，请同步 website/src/lib/data.ts。`);
  process.exit(1);
}
console.log('\n全部一致，无漂移。');
