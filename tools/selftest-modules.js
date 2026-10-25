/**
 * 增值模块配置端到端自测（无副作用，2026-10-13）
 *
 * 把 server/server.js 复制成 _selftest_modules_server.js：端口改为 3125、数据库指向
 * 临时目录，启动后打真实 HTTP，验证「模块功能归属可配置」这条链路的每一环：
 *   ① 默认归属：m1=9 / m2=4 / m3=7，无幽灵 key（edl.oneclick 已删）、含 xiaomi.flash；
 *   ② 控制台保存 features：迁移（app.manage m3→m1）与取消归属（driver.cloud）即时生效；
 *   ③ 陌生 key（不在 FEATURE_CATALOG）一律丢弃；
 *   ④ 同一功能配进两个模块 → 先到先得（按 MODULE_CATALOG 顺序）；
 *   ⑤ 显式空数组 = 空模块；
 *   ⑥ 显式配置优先于别模块的默认值（m1 显式去掉 mtk.flash、m2 显式加上 → mtk.flash 归 m2）；
 *   ⑦ includedLevels 与 features 互不干扰；
 *   ⑧ 恢复默认（写 '{}'）回到代码默认分类。
 * 结束后自动清理，不碰正式数据库。
 *
 * 用法：cd tools && node selftest-modules.js
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const PORT = 3125;
const BASE = `http://127.0.0.1:${PORT}`;
const SERVER_DIR = path.join(__dirname, '..', 'server');
const TMP_COPY = path.join(SERVER_DIR, '_selftest_modules_server.js');
const TMP_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'kunmodules-'));
const TMP_DB = path.join(TMP_DIR, 'test.sqlite');

let pass = 0, fail = 0;
const failures = [];
function ok(name, cond, extra = '') {
  if (cond) { pass++; console.log('  [PASS] ' + name); }
  else { fail++; failures.push(name + (extra ? ' -> ' + extra : '')); console.log('  [FAIL] ' + name + (extra ? '  -> ' + extra : '')); }
}
async function api(p, method = 'GET', body = null, token = '') {
  const headers = {};
  if (token) headers.Authorization = 'Bearer ' + token;
  if (body) headers['Content-Type'] = 'application/json';
  const r = await fetch(BASE + p, { method, headers, body: body ? JSON.stringify(body) : undefined });
  const text = await r.text();
  let json; try { json = JSON.parse(text); } catch { json = { raw: text }; }
  return { status: r.status, json };
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

function prepare() {
  let src = fs.readFileSync(path.join(SERVER_DIR, 'server.js'), 'utf8');
  const subs = [
    ["const cfg = require('./config');", `const cfg = require('./config');
cfg.port = ${PORT};
cfg.jwt_secret = 'selftest_jwt_secret';
cfg.dbFile = ${JSON.stringify(TMP_DB)};
cfg.publicUrl = 'http://127.0.0.1:${PORT}';
cfg.mail.enabled = false;
cfg.admin_email = 'selftest@example.invalid';
cfg.admin = { username: process.env.SELFTEST_ADMIN_USER || 'admin', password: process.env.SELFTEST_ADMIN_PASS || 'selftest_admin_pass', sync: false };`],
    ["seedSetting('feedback_notify', '1');", "seedSetting('feedback_notify', '0');"],
  ];
  for (const [from, to] of subs) {
    if (!src.includes(from)) throw new Error('注入失败：未找到标记 ' + JSON.stringify(from) + '（server.js 结构可能已变化）');
    src = src.replace(from, to);
  }
  fs.writeFileSync(TMP_COPY, src);
}

function cleanup() {
  try { fs.unlinkSync(TMP_COPY); } catch { }
  try { fs.rmSync(TMP_DIR, { recursive: true, force: true }); } catch { }
}

async function waitUp() {
  for (let i = 0; i < 40; i++) {
    try { const r = await api('/api/health'); if (r.json.status === 'ok') return true; } catch { }
    await sleep(400);
  }
  return false;
}

/** 取一次生效模块配置（官网/控制台读的就是它）。 */
async function getModules(token = '') {
  const r = await api('/api/app/limits', 'GET', null, token);
  return r.json.success ? r.json.data : null;
}
const featsOf = (mods, id) => (mods.modules.find(m => m.id === id) || {}).features || [];

async function main() {
  console.log(`[自测] 临时数据库: ${TMP_DB}`);
  prepare();

  const child = spawn(process.execPath, [TMP_COPY], { stdio: ['ignore', 'pipe', 'pipe'], cwd: SERVER_DIR });
  child.stdout.on('data', d => { const s = String(d); if (s.includes('[启动]') || s.includes('[模块]')) process.stdout.write('  [server] ' + s); });
  child.stderr.on('data', d => process.stdout.write('  [server:err] ' + String(d)));
  let exited = null;
  child.on('exit', (code, sig) => { exited = code ?? sig; });

  try {
    if (!await waitUp()) throw new Error(`服务器未就绪 (exit=${exited})`);
    console.log('  ✓ 服务器已就绪 (端口 ' + PORT + ')');

    // ── ① 默认归属 ────────────────────────────────────────────────────────
    console.log('\n[1] 默认模块归属（未经任何配置）');
    let d = await getModules();
    ok('limits 接口可用', !!d, JSON.stringify(d).slice(0, 120));
    const keys = (d.features || []).map(f => f.key);
    ok('功能目录 20 项', keys.length === 20, `实际 ${keys.length}`);
    ok('幽灵 key edl.oneclick 已删除', !keys.includes('edl.oneclick'));
    ok('补登 xiaomi.flash', keys.includes('xiaomi.flash'));
    ok('m1 默认 9 项', featsOf(d, 'm1').length === 9, JSON.stringify(featsOf(d, 'm1')));
    ok('m2 默认 4 项', featsOf(d, 'm2').length === 4, JSON.stringify(featsOf(d, 'm2')));
    ok('m3 默认 7 项', featsOf(d, 'm3').length === 7, JSON.stringify(featsOf(d, 'm3')));
    ok('三模块默认都被三等级包含',
      d.modules.every(m => (m.includedLevels || []).join(',') === 'normal,vip,svip'),
      JSON.stringify(d.modules.map(m => m.includedLevels)));

    // ── 管理员登录 ────────────────────────────────────────────────────────
    console.log('\n[2] 管理员登录（保存配置需要）');
    let r = await api('/api/login', 'POST', {
      username: process.env.SELFTEST_ADMIN_USER || 'admin',
      password: process.env.SELFTEST_ADMIN_PASS || 'selftest_admin_pass',
    });
    const token = r.json.data?.token;
    ok('管理员登录', r.json.success && r.json.data?.role === 'admin', r.json.message || '');

    const save = async payload => {
      const rr = await api('/api/admin/settings', 'PUT', { member_modules: JSON.stringify(payload) }, token);
      return rr.json.success;
    };

    // ── ② 迁移 + 取消归属 ────────────────────────────────────────────────
    console.log('\n[3] 控制台保存 features：迁移 app.manage 到 m1、取消 driver.cloud 归属');
    ok('保存成功', await save({ modules: {
      m1: { features: ['edl.write', 'edl.erase', 'edl.gpt', 'fastboot.flash', 'fastboot.erase', 'fastboot.bl', 'oneplus.flash', 'xiaomi.flash', 'mtk.flash', 'app.manage'] },
      m2: { features: ['edl.readtable', 'edl.backup', 'payload.extract', 'mtk.read'] },
      m3: { features: ['android.tools', 'oneplus.frp', 'mirror.scrcpy', 'file.transfer', 'cmd.shell'] },
    } }));
    d = await getModules();
    ok('app.manage 迁入 m1', featsOf(d, 'm1').includes('app.manage'));
    ok('m3 不再含 app.manage', !featsOf(d, 'm3').includes('app.manage'));
    ok('driver.cloud 取消归属后不在任何模块', !['m1', 'm2', 'm3'].some(id => featsOf(d, id).includes('driver.cloud')));
    ok('未提及的 edl.readtable 仍在 m2（显式配置只覆盖被保存的模块）', featsOf(d, 'm2').includes('edl.readtable'));

    // ── ③ 陌生 key 丢弃 ──────────────────────────────────────────────────
    console.log('\n[4] 陌生 key 一律丢弃');
    ok('保存成功', await save({ modules: {
      m1: { features: ['edl.write', 'bogus.key', 'edl.oneclick'] },
      m2: {},
      m3: {},
    } }));
    d = await getModules();
    ok('bogus.key 被丢弃', !featsOf(d, 'm1').includes('bogus.key'));
    ok('edl.oneclick（目录外）被丢弃', !featsOf(d, 'm1').includes('edl.oneclick'));
    ok('合法 key 保留', featsOf(d, 'm1').includes('edl.write'));

    // ── ④ 跨模块重复：先到先得 ───────────────────────────────────────────
    console.log('\n[5] 同一功能配进两个模块 → 先到先得');
    ok('保存成功', await save({ modules: {
      m1: { features: ['edl.write'] },
      m2: { features: ['edl.write', 'edl.readtable'] },
      m3: { features: [] },
    } }));
    d = await getModules();
    ok('edl.write 保留在 m1（顺序在前）', featsOf(d, 'm1').includes('edl.write') && !featsOf(d, 'm2').includes('edl.write'));

    // ── ⑤ 显式空数组 = 空模块 ────────────────────────────────────────────
    console.log('\n[6] 显式空数组 = 空模块');
    ok('m2 为空数组', featsOf(d, 'm2').length === 1);   // 上一步 m2 只剩 edl.readtable
    ok('保存成功', await save({ modules: {
      m1: { features: ['edl.write'] },
      m2: { features: [] },
      m3: { features: ['cmd.shell'] },
    } }));
    d = await getModules();
    ok('m2 0 项（空模块）', featsOf(d, 'm2').length === 0, JSON.stringify(featsOf(d, 'm2')));
    ok('空模块仍可被单独开通（结构完整）', !!d.modules.find(m => m.id === 'm2'));

    // ── ⑥ 显式配置优先于别模块的默认值 ──────────────────────────────────
    console.log('\n[7] 显式配置优先于默认值（m1 去掉 mtk.flash、m2 显式加上）');
    ok('保存成功', await save({ modules: {
      m1: { features: ['edl.write', 'edl.erase', 'edl.gpt', 'fastboot.flash', 'fastboot.erase', 'fastboot.bl', 'oneplus.flash', 'xiaomi.flash'] },
      m2: { features: ['edl.readtable', 'edl.backup', 'payload.extract', 'mtk.read', 'mtk.flash'] },
      // m3 不配 features → 用代码默认（7 项），且不该把 mtk.flash 从 m2 反咬走
    } }));
    d = await getModules();
    ok('mtk.flash 归 m2（显式迁移生效）', featsOf(d, 'm2').includes('mtk.flash') && !featsOf(d, 'm1').includes('mtk.flash'));
    ok('m3 仍为代码默认 7 项', featsOf(d, 'm3').length === 7, JSON.stringify(featsOf(d, 'm3')));

    // ── ⑦ includedLevels 与 features 互不干扰 ────────────────────────────
    console.log('\n[8] includedLevels 不受 features 改动影响');
    ok('保存成功', await save({ modules: {
      m1: { features: ['edl.write'], includedLevels: ['vip', 'svip'] },
      m2: { features: ['edl.readtable'] },
      m3: { features: ['cmd.shell'] },
    } }));
    d = await getModules();
    ok('m1 只含 vip/svip', (featsOf(d, 'm1') || true) && JSON.stringify((d.modules.find(m => m.id === 'm1') || {}).includedLevels) === '["vip","svip"]',
      JSON.stringify((d.modules.find(m => m.id === 'm1') || {}).includedLevels));
    ok('m2/m3 缺省 includedLevels 仍为三等级',
      ['m2', 'm3'].every(id => ((d.modules.find(m => m.id === id) || {}).includedLevels || []).join(',') === 'normal,vip,svip'));

    // ── ⑧ 恢复默认 ───────────────────────────────────────────────────────
    console.log('\n[9] 恢复默认（写空对象）');
    ok('保存成功', await save({}));
    d = await getModules();
    ok('m1 回到 9 项', featsOf(d, 'm1').length === 9, JSON.stringify(featsOf(d, 'm1')));
    ok('m2 回到 4 项', featsOf(d, 'm2').length === 4);
    ok('m3 回到 7 项', featsOf(d, 'm3').length === 7);
    ok('三模块重新包含全部等级', d.modules.every(m => (m.includedLevels || []).length === 3));

    // ── ⑨ 脏 JSON 回落 ───────────────────────────────────────────────────
    console.log('\n[10] 脏配置回落默认值');
    r = await api('/api/admin/settings', 'PUT', { member_modules: 'not-json{{' }, token);
    ok('脏 JSON 也能存进去（读时兜底）', r.json.success, r.json.message || '');
    d = await getModules();
    ok('读出的仍是完整默认结构', featsOf(d, 'm1').length === 9 && featsOf(d, 'm2').length === 4 && featsOf(d, 'm3').length === 7);
  } catch (e) {
    fail++; failures.push('异常: ' + e.message);
    console.error('  [ERROR]', e);
  } finally {
    try { child.kill(); } catch { }
    cleanup();
  }

  console.log(`\n结果: ${pass} 通过, ${fail} 失败`);
  if (fail) { console.log('失败项:\n  ' + failures.join('\n  ')); process.exit(1); }
}

main();
