// 服务端静态自检：语法 + 调用了未定义的全局函数 + 未定义的 API 路由处理器
// 用法：node check-server.js
const fs = require('fs');
const path = require('path');

// 2026-10-22 起 server/ 归档到 _archive/，这里原来硬写 ../server/server.js，
// 一跑就是 ENOENT（而且是在读文件那一步炸，看起来像「脚本坏了」）。
// 与 deploy-config.js 同一套回落顺序：先新位置，再归档位置。
function resolveServerFile() {
  const candidates = [
    path.join(__dirname, '..', 'server', 'server.js'),
    path.join(__dirname, '..', '_archive', 'server', 'server.js'),
  ];
  for (const f of candidates) if (fs.existsSync(f)) return f;
  console.error('找不到 server.js（已试过：\n  ' + candidates.join('\n  ') + '）');
  process.exit(2);
}

const file = resolveServerFile();
const src = fs.readFileSync(file, 'utf8');
console.log(`检查目标：${path.relative(path.join(__dirname, '..'), file)}`);
let fail = 0;
const ok = (m) => console.log('  OK   ' + m);
const bad = (m) => { fail++; console.log('  FAIL ' + m); };

// ---- 1. 语法 ----
try { new Function(src); ok('server.js 语法通过'); }
catch (e) { bad('语法错误: ' + e.message); }

// ---- 2. 调用了未定义的全局函数（曾出现 applyTrialDays 被调用但未定义）----
// 先把字符串/模板串/注释剥掉，避免 SQL 关键字（VALUES、datetime…）被当成函数调用。
function stripLiterals(code) {
  let out = '', i = 0;
  while (i < code.length) {
    const ch = code[i];
    if (ch === '/' && code[i + 1] === '/') { while (i < code.length && code[i] !== '\n') i++; continue; }
    if (ch === '/' && code[i + 1] === '*') { i += 2; while (i < code.length && !(code[i] === '*' && code[i + 1] === '/')) i++; i += 2; continue; }
    if (ch === '"' || ch === "'") {
      const q = ch; i++;
      while (i < code.length && code[i] !== q) { if (code[i] === '\\') i++; i++; }
      i++; out += ' 0 '; continue;
    }
    if (ch === '`') {
      i++; out += '';
      while (i < code.length && code[i] !== '`') {
        if (code[i] === '\\') { i += 2; continue; }
        if (code[i] === '$' && code[i + 1] === '{') {
          let depth = 1; i += 2; const start = i;
          while (i < code.length && depth > 0) { if (code[i] === '{') depth++; else if (code[i] === '}') depth--; i++; }
          out += ' ' + code.slice(start, i - 1) + ' ';
          continue;
        }
        i++;
      }
      i++; continue;
    }
    out += ch; i++;
  }
  return out;
}
const code = stripLiterals(src);

const NODE_GLOBALS = new Set([
  'require','module','exports','process','console','Buffer','setTimeout','setInterval','clearTimeout','clearInterval',
  'setImmediate','clearImmediate','Promise','Object','Array','String','Number','Boolean','Date','Math','JSON','RegExp',
  'Error','TypeError','RangeError','Map','Set','WeakMap','WeakSet','Symbol','Proxy','Reflect','parseInt','parseFloat',
  'isNaN','isFinite','encodeURIComponent','decodeURIComponent','escape','unescape','fetch','URL','URLSearchParams',
  'TextEncoder','TextDecoder','AbortController','structuredClone','queueMicrotask','BigInt','Function','eval','if',
  'for','while','switch','catch','return','typeof','new','await','async','do','else','try','var','let','const',
  'Intl','BigInt64Array','Uint8Array','Int8Array','Uint16Array','Int16Array','Uint32Array','Int32Array','Float32Array',
  'Float64Array','ArrayBuffer','DataView','SharedArrayBuffer','Atomics','WebAssembly','global','globalThis','navigator'
]);
const defined = new Set([
  ...[...code.matchAll(/function\s+([A-Za-z_$][\w$]*)/g)].map(m => m[1]),
  ...[...code.matchAll(/(?:const|let|var)\s+([A-Za-z_$][\w$]*)/g)].map(m => m[1]),
  ...[...code.matchAll(/([A-Za-z_$][\w$]*)\s*[:=]\s*(?:async\s*)?(?:function|\()/g)].map(m => m[1]),
  // 解构导入：`const { buildFirehoseIndex } = require('./lib/firehoseIndex')`。
  // ⚠️ 上面那条 `(?:const|let|var)\s+([A-Za-z_$][\w$]*)` 匹配不到它 —— `const` 后面跟的是 `{`。
  //    漏掉的后果：server.js 里唯一的解构导入 buildFirehoseIndex 会被报成「未定义的全局函数」，
  //    于是 check-server 恒 FAIL。恒 FAIL 的自检比没有自检更糟：真正的失败会被这条噪声淹没。
  //    这是 2026-10-09 实测到的既有盲区（与当次改动无关，只是顺手补上）。
  ...[...code.matchAll(/(?:const|let|var)\s*\{([^}]*)\}\s*=/g)].flatMap(m =>
    m[1].split(',').map(s => s.trim().split(':').pop().trim()).filter(s => /^[A-Za-z_$][\w$]*$/.test(s))),
  // for (const x of ...) / 解构 / 形参：用宽松匹配兜住常见写法
  ...[...code.matchAll(/\(\s*([^()]*)\)\s*(?:=>|\{)/g)].flatMap(m =>
    m[1].split(',').map(s => s.trim().replace(/[={}\s].*$/, '')).filter(s => /^[A-Za-z_$][\w$]*$/.test(s))),
]);
const calls = new Set([...code.matchAll(/(?<![.\w$])([a-zA-Z_$][\w$]*)\s*\(/g)].map(m => m[1]));
const miss = [...calls].filter(c => !defined.has(c) && !NODE_GLOBALS.has(c) &&
  !/^(create|get|set|has|delete|push|pop|shift|unshift|slice|splice|split|join|map|filter|find|some|every|reduce|forEach|includes|indexOf|replace|match|test|exec|trim|toLowerCase|toUpperCase|toString|startsWith|endsWith|padStart|padEnd|repeat|concat|sort|reverse|fill|keys|values|entries|then|catch|finally|apply|call|bind|of|from|isArray|now|parse|stringify|max|min|abs|ceil|floor|round|random|sign|pow|sqrt|log|toISOString|getTime|getFullYear|getMonth|getDate|getHours|setDate|setTime|toFixed|toLocaleString|localeCompare|assign|freeze|create|defineProperty|getOwnPropertyNames|charCodeAt|codePointAt|fromCharCode|at|flat|flatMap|findIndex|lastIndexOf|toUpperCase|normalize|format|resolve|all|race|allSettled|any|IsFinite|isFinite|isNaN|IsNaN|next|send|json|status|redirect|use|get|post|put|delete|patch|all|listen|on|emit|once|removeListener|kill|end|write|read|close|destroy|resume|pause|pipe|unpipe|ref|unref|hashSync|compareSync|sign|verify|decode|encode|run|exec|prepare|close|all|each|get|serialize|deserialize|dirname|basename|extname|resolve|join|normalize|existsSync|readFileSync|writeFileSync|mkdirSync|rmSync|unlinkSync|statSync|createReadStream|createWriteStream|randomBytes|randomUUID|createHash|createCipheriv|createDecipheriv|pbkdf2Sync|scryptSync|timingSafeEqual|createTransport|sendMail|verify|setHeader|writeHead|End|append|appendFileSync|copyFileSync|renameSync|readdirSync|chmodSync|utimesSync|toISOString|toJSON|getUTCFullYear|getUTCString|toUTCString|stringify|parse|concat|isInteger|isSafeInteger|toPrecision|toExponential|valueOf|hasOwnProperty|isPrototypeOf|propertyIsEnumerable|toLocaleDateString|toLocaleTimeString|toDateString|toTimeString)/.test(c));
miss.length ? bad('调用了未定义的全局函数: ' + miss.join(', ')) : ok('所有全局函数调用都有定义');

// ---- 3. 路由处理器里用到的辅助函数（抽样检查关键登录/注册路径）----
const risky = ['applyTrialDays', 'addVipDays', 'isVipActive', 'daysLeftOf', 'isVipForever', 'entitlementOf', 'sendEmail', 'saveDb'];
const undef = risky.filter(fn => src.includes(fn + '(') && !new RegExp(`function\\s+${fn}\\s*\\(`).test(src) &&
  !new RegExp(`(?:const|let|var)\\s+${fn}\\s*=`).test(src));
undef.length ? bad('关键辅助函数未定义: ' + undef.join(', ')) : ok('关键辅助函数（会员/邮件/存储）均已定义');

// ---- 4. 静态文件目录是否都存在（否则 express.static 会静默失效）----
const dirs = [...src.matchAll(/express\.static\(\s*path\.join\(__dirname,\s*'([^']+)'\s*\)/g)].map(m => m[1]);
// 相对 server.js 自己所在目录判断（归档后 server.js 在 _archive/server/，
// 写死 '../server' 会把整组目录都判成不存在）。
const serverDir = path.dirname(file);
const missDirs = dirs.filter(d => !fs.existsSync(path.join(serverDir, d)));
missDirs.length ? bad('express.static 指向的目录不存在: ' + missDirs.join(', ')) : ok(`静态目录均存在（${dirs.join(', ')}）`);

// ---- 5. 【回归护栏】ISO 时间列不能直接和 datetime('now') 做字符串比较 ----
// verification_codes.expires_at / license_keys.expires_at 存的是 ISO（带 'T'），
// 而 datetime('now') 返回 'YYYY-MM-DD HH:MM:SS'。两者比较时，从第 11 个字符起
// 'T'(0x54) > ' '(0x20) 恒成立 —— 于是**当天已过期**的记录一律被判为未过期：
// 10 分钟有效的验证码实际能用一整天（登录/绑定/换绑邮箱），过期卡密当天仍算有效。
// 必须写成 datetime(列名) > datetime('now')，让 SQLite 先把两种格式归一化。
// 这条护栏的由来：上一轮修好了 2 处（reset-password / code-register），
// 却漏改另外 4 处，且没有任何检查会发现 —— 所以固化成断言。
const isoCols = ['expires_at'];
const rawCompare = [];
isoCols.forEach(col => {
  const re = new RegExp(`(?<!datetime\\()\\b${col}\\s*[<>]=?\\s*datetime\\(`, 'g');
  for (const m of src.matchAll(re)) rawCompare.push(m[0].trim());
});
rawCompare.length
  ? bad(`ISO 时间列与 datetime('now') 裸比较（须写成 datetime(列名)）: ${[...new Set(rawCompare)].join(' | ')}`)
  : ok('ISO 时间列均经 datetime() 归一化后再比较');

console.log(fail ? `\n${fail} 项检查失败` : '\n全部检查通过');
process.exit(fail ? 1 : 0);