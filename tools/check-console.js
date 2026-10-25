// 控制台自检工具（v6 多文件结构）
// 用法：node check-console.js
//
// v6 起控制台拆成 index.html + css.v2/console.css + js.v2/**.js（ES 模块，无打包）。
// ⚠️ 目录名带 .v2 是**故意的**：文件名不带内容哈希，靠改名让浏览器丢掉旧缓存。
// 检查项相应改为「跨文件」：
//   1. 每个 JS 文件语法（按 ESM 解析）
//   2. import 的具名导出在目标文件里真的存在
//   3. $('id') 引用的元素必须在 index.html 或某个 JS 模板串里出现过
//   4. data-act / data-change / data-input / data-submit 的值必须有 registerActions 注册
//   5. emoji 残留、旧色残留、关键功能标记
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawnSync } = require('child_process');

// 2026-10-22 起 server/ 归档到 _archive/：与 check-server.js 同一套回落，
// 否则脚本在 readFileSync 那一步就 ENOENT（看起来像「脚本坏了」）。
function resolveConsoleDir() {
  const candidates = [
    path.join(__dirname, '..', 'server', 'console'),
    path.join(__dirname, '..', '_archive', 'server', 'console'),
  ];
  for (const d of candidates) if (fs.existsSync(path.join(d, 'index.html'))) return d;
  console.error('找不到控制台目录（已试过：\n  ' + candidates.join('\n  ') + '）');
  process.exit(2);
}

const ROOT = resolveConsoleDir();
const HTML = path.join(ROOT, 'index.html');
const CSS = path.join(ROOT, 'css.v2', 'console.css');
const JS_DIR = path.join(ROOT, 'js.v2');   // 2026-10-20 改名破缓存，见 index.html 注释

let fail = 0;
const ok = (m) => console.log('  OK   ' + m);
const bad = (m) => { fail++; console.log('  FAIL ' + m); };

function walk(dir, out) {
  for (const f of fs.readdirSync(dir)) {
    const p = path.join(dir, f);
    const st = fs.statSync(p);
    if (st.isDirectory()) walk(p, out);
    else if (f.endsWith('.js')) out.push(p);
  }
  return out;
}

const html = fs.readFileSync(HTML, 'utf8');
const css = fs.readFileSync(CSS, 'utf8');
const files = walk(JS_DIR, []);
const srcOf = {};
files.forEach((f) => { srcOf[f] = fs.readFileSync(f, 'utf8'); });
const allJs = Object.values(srcOf).join('\n');
const all = html + '\n' + allJs;

// ---- 1. 语法（ESM）----
{
  const tmp = path.join(os.tmpdir(), 'kc-check-' + process.pid);
  fs.mkdirSync(tmp, { recursive: true });
  let n = 0;
  for (const f of files) {
    const mjs = path.join(tmp, path.basename(f, '.js') + '-' + n++ + '.mjs');
    fs.writeFileSync(mjs, srcOf[f]);
    const r = spawnSync(process.execPath, ['--check', mjs], { encoding: 'utf8' });
    if (r.status !== 0) bad('语法错误 ' + path.relative(ROOT, f) + ': ' + String(r.stderr || '').split('\n')[0]);
  }
  fs.rmSync(tmp, { recursive: true, force: true });
  if (!fail) ok(`JS 语法通过（${files.length} 个模块）`);
}

// ---- 2. import / export 对账 ----
{
  const exp = {};
  for (const f of files) {
    const s = srcOf[f];
    const set = new Set();
    for (const m of s.matchAll(/export\s+(?:async\s+)?function\s+([A-Za-z0-9_$]+)/g)) set.add(m[1]);
    for (const m of s.matchAll(/export\s+(?:const|let|var)\s+([A-Za-z0-9_$]+)/g)) set.add(m[1]);
    for (const m of s.matchAll(/export\s+class\s+([A-Za-z0-9_$]+)/g)) set.add(m[1]);
    if (/export\s+default/.test(s)) set.add('default');
    exp[path.resolve(f)] = set;
  }
  let missing = 0;
  for (const f of files) {
    const s = srcOf[f];
    for (const m of s.matchAll(/import\s+([\s\S]*?)\s+from\s+'([^']+)'/g)) {
      const clause = m[1].trim();
      const spec = m[2];
      if (!spec.startsWith('.')) continue;
      const target = path.resolve(path.dirname(f), spec);
      if (!exp[target]) {
        bad(`${path.relative(ROOT, f)} 引用了不存在的文件 ${spec}`);
        missing++;
        continue;
      }
      const named = m[2] && clause.match(/\{([\s\S]*?)\}/);
      if (named) {
        for (const raw of named[1].split(',')) {
          const name = raw.trim().split(/\s+as\s+/)[0].trim();
          if (name && !exp[target].has(name)) {
            bad(`${path.relative(ROOT, f)} 从 ${spec} 导入了不存在的 ${name}`);
            missing++;
          }
        }
      }
      const hasDefault = /^[A-Za-z0-9_$]+\s*(,|$)/.test(clause) && !clause.startsWith('{');
      if (hasDefault && !exp[target].has('default')) {
        bad(`${path.relative(ROOT, f)} 从 ${spec} 默认导入，但对方没有 export default`);
        missing++;
      }
    }
  }
  if (!missing) ok('import / export 全部对得上');
}

// ---- 3. $('id') 引用 ----
{
  const ids = new Set([...all.matchAll(/\bid="([A-Za-z0-9_-]+)"/g)].map((m) => m[1]));
  const used = new Set([...allJs.matchAll(/\$\('([A-Za-z0-9_-]+)'\)/g)].map((m) => m[1]));
  const miss = [...used].filter((i) => !ids.has(i));
  miss.length ? bad('引用了不存在的元素 id: ' + miss.join(', '))
    : ok(`$('id') 引用全部存在（${used.size} 个）`);
}

// ---- 4. 动作注册 ----
{
  const registered = new Set([...all.matchAll(/'([a-z][a-zA-Z0-9]*\.[a-zA-Z0-9_]+)'\s*:/g)].map((m) => m[1]));
  const attrs = ['data-act', 'data-change', 'data-input', 'data-submit'];
  const used = new Set();
  for (const a of attrs) {
    for (const m of all.matchAll(new RegExp(a + '="([a-zA-Z0-9_.]+)"', 'g'))) used.add(m[1]);
  }
  const miss = [...used].filter((x) => !registered.has(x));
  miss.length ? bad('未注册的交互动作: ' + miss.join(', '))
    : ok(`交互动作全部已注册（${used.size} 个）`);
}

// ---- 5. emoji ----
{
  const emojiRe = /[\u{1F300}-\u{1FAFF}\u{2600}-\u{27BF}\u{2B00}-\u{2BFF}\u{FE0F}]/gu;
  const hits = [...all.matchAll(emojiRe)].map((m) => m[0]);
  hits.length ? bad('页面仍含 emoji: ' + [...new Set(hits)].join(' '))
    : ok('无 emoji 残留');
}

// ---- 6. 旧色回归护栏 ----
{
  //   v1 蓝 #2D7AF0 / #0067c0 / #2f8bff
  //   v2 墨绿 #0F6B4F / #0C5A45 / #17835F / #2E9E73 / #1E8F63 / #45B183 / #4FB88A / #E6F2EB
  //   v5 淡蓝强调 #7ab8f3（v6 起统一 #0071e3，填充对比度 4.6:1）
  const OLD = ['#2D7AF0', '#0067c0', '#2f8bff',
    '#0F6B4F', '#0C5A45', '#17835F', '#2E9E73', '#1E8F63', '#45B183', '#4FB88A', '#E6F2EB',
    '#7ab8f3', 'rgba(15,107,79,', 'rgba(30,143,99,', 'rgba(79,184,138,'];
  const lower = (html + css + allJs).toLowerCase();
  const res = OLD.filter((c) => lower.includes(c.toLowerCase()));
  res.length ? bad('仍有旧色残留: ' + res.join(' '))
    : ok(`无旧色残留（查了 ${OLD.length} 个）`);
}

// ---- 7. 关键功能标记 ----
{
  const marks = [
    // 2026-10-20：会员 / 卡密体系下线，'permanent: true' 与 'vipExpireText' 不再守
    // 2026-10-20：卡密 / 增值模块下线，'genKeys' 与 'entitlements' 不再守
    'banned-ips',                 // IP 封禁
    'client-logs',                // 客户端日志
    'LEGAL_VERSION',              // 注册法务版本
    '2FA_REQUIRED',               // 登录二次验证
  ];
  const miss = marks.filter((m) => !all.includes(m));
  miss.length ? bad('缺失关键功能标记: ' + miss.join(', '))
    : ok('会员 / 卡密 / 封禁 / 授权 / 日志 / 2FA 标记齐全');
}

console.log(fail ? `\n${fail} 项检查失败` : '\n全部检查通过');
process.exit(fail ? 1 : 0);
