/**
 * 本地 API 自测脚本（无副作用）
 *
 * 把 server/server.js 复制成 _selftest_server.js：端口改为 3123、数据库指向临时目录，
 * 启动后验证「用户密钥自助管理 / 问题反馈 / 用户 ID 修改」等接口，最后自动清理。
 *
 * 用法：cd tools && node selftest-api.js
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const PORT = 3123;
const BASE = `http://127.0.0.1:${PORT}`;
const SERVER_DIR = path.join(__dirname, '..', 'server');
const TMP_COPY = path.join(SERVER_DIR, '_selftest_server.js');
const TMP_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'kunapi-'));
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
    // 软配置注入：端口/数据库/邮件走 config，通过环境变量覆盖最干净
    ["const cfg = require('./config');", `const cfg = require('./config');
cfg.port = ${PORT};
cfg.jwt_secret = 'selftest_jwt_secret';
cfg.dbFile = ${JSON.stringify(TMP_DB)};
cfg.publicUrl = 'http://127.0.0.1:${PORT}';
cfg.mail.enabled = false;
cfg.admin_email = 'selftest@example.invalid';
cfg.admin = { username: process.env.SELFTEST_ADMIN_USER || 'admin', password: process.env.SELFTEST_ADMIN_PASS || 'selftest_admin_pass', sync: false };`],
    // 自测环境关闭「反馈邮件通知」，避免对外真实发信
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

async function main() {
  console.log(`[自测] 临时数据库: ${TMP_DB}`);
  prepare();

  const child = spawn(process.execPath, [TMP_COPY], { stdio: ['ignore', 'pipe', 'pipe'], cwd: SERVER_DIR });
  child.stdout.on('data', d => { const s = String(d); if (s.includes('[启动]')) process.stdout.write('  [server] ' + s); });
  child.stderr.on('data', d => process.stdout.write('  [server:err] ' + String(d)));
  let exited = null;
  child.on('exit', (code, sig) => { exited = code ?? sig; });

  try {
    if (!await waitUp()) throw new Error(`服务器未就绪 (exit=${exited})`);
    console.log(`  \u2713 服务器已就绪 (端口 ${PORT})`);

    console.log('\n[1] 注册 / 登录');
    let r = await api('/api/register', 'POST', { username: 'tester_a', password: 'pass123456' });
    ok('注册用户 A', r.json.success && !!r.json.data?.token, r.json.message || JSON.stringify(r.json));
    let tokenA = r.json.data?.token;
    r = await api('/api/profile', 'GET', null, tokenA);
    ok('获取个人资料', r.json.success && r.json.data?.username === 'tester_a' && Number.isFinite(r.json.data?.id));
    const idA = r.json.data?.id;
    r = await api('/api/register', 'POST', { username: 'tester_b', password: 'pass123456' });
    const tokenB = r.json.data?.token;
    ok('注册用户 B', r.json.success && !!tokenB, r.json.message || '');
    r = await api('/api/login', 'POST', { username: process.env.SELFTEST_ADMIN_USER || 'admin', password: process.env.SELFTEST_ADMIN_PASS || 'selftest_admin_pass' });
    const tokenAdmin = r.json.data?.token;
    ok('管理员登录', r.json.success && r.json.data?.role === 'admin', r.json.message || '');

    console.log('\n[2] 用户自助密钥管理');
    r = await api('/api/user/generate-key', 'POST', {}, tokenA);
    ok('用户生成密钥', r.json.success && /^KUN-[A-Z0-9]{4}-[A-Z0-9]{4}-[A-Z0-9]{4}$/.test(r.json.data?.key || ''), JSON.stringify(r.json));
    const ownKey1 = r.json.data?.key;
    r = await api('/api/user/generate-key', 'POST', {}, tokenA);
    ok('再次生成密钥', r.json.success, r.json.message || '');
    const ownKey2 = r.json.data?.key;
    r = await api('/api/user/keys', 'GET', null, tokenA);
    const myKeys = r.json.data || [];
    ok('我的密钥列表', r.json.success && myKeys.length === 2, JSON.stringify(myKeys));
    const k1 = myKeys.find(k => k.key_code === ownKey1), k2 = myKeys.find(k => k.key_code === ownKey2);
    r = await api(`/api/user/keys/${k1.id}`, 'PUT', { status: 'disabled' }, tokenA);
    ok('停用密钥', r.json.success, r.json.message || '');
    r = await api(`/api/user/keys/${k1.id}`, 'PUT', { status: 'active' }, tokenA);
    ok('启用密钥', r.json.success, r.json.message || '');
    r = await api(`/api/user/keys/${k1.id}`, 'PUT', { status: 'bogus' }, tokenA);
    ok('非法状态被拒绝(400)', r.status === 400);
    r = await api(`/api/user/keys/${k1.id}`, 'PUT', { status: 'disabled' }, tokenB);
    ok('不能操作他人密钥(404)', r.status === 404);
    r = await api(`/api/user/keys/${k1.id}`, 'DELETE', null, tokenB);
    ok('不能删除他人密钥(404)', r.status === 404);
    r = await api(`/api/user/keys/${k2.id}`, 'DELETE', null, tokenA);
    ok('删除密钥', r.json.success, r.json.message || '');
    r = await api('/api/user/keys/batch-delete', 'POST', { ids: [k1.id] }, tokenA);
    ok('批量删除密钥', r.json.success && /1 个密钥/.test(r.json.message || ''), r.json.message || '');
    r = await api('/api/user/keys', 'GET', null, tokenA);
    ok('删除后列表为空', r.json.success && (r.json.data || []).length === 0, JSON.stringify(r.json.data));
    console.log('\n[3] 密钥登录（一次性）');
    // 2026-10-02 收权：/api/auth/generate-key 由「完全公开」改为「管理员及以上」。
    // 原来任何人 POST 一下就能铸卡并 saveDb()（整库重写）= 无限铸币机 + 写放大入口。
    // 所以这里三条一起锁：匿名必须被拒、普通用户必须被拒、管理员仍能铸卡（供下面的
    // 一次性登录链路使用）。别把它改回匿名可用 —— 那是本次安全审计关掉的口子。
    r = await api('/api/auth/generate-key', 'POST', {});
    ok('匿名铸卡被拒(401)', r.status === 401, `实际 ${r.status} ${JSON.stringify(r.json)}`);
    r = await api('/api/auth/generate-key', 'POST', {}, tokenA);
    ok('普通用户铸卡被拒(403)', r.status === 403, `实际 ${r.status}`);
    r = await api('/api/auth/generate-key', 'POST', {}, tokenAdmin);
    const pubKey = r.json.data?.key;
    ok('管理员铸卡成功', r.json.success && /^KUN-/.test(pubKey || ''), JSON.stringify(r.json));
    r = await api('/api/auth/key-login', 'POST', { key: pubKey });
    const keyToken = r.json.data?.token;
    ok('密钥登录成功', r.json.success && !!keyToken, r.json.message || '');
    r = await api('/api/user/keys', 'GET', null, keyToken);
    const afterUse = r.json.data || [];
    ok('登录后密钥标记为已消费(used)', r.json.success && afterUse.length === 1 && afterUse[0].status === 'used',
      JSON.stringify(afterUse));
    r = await api('/api/auth/key-login', 'POST', { key: pubKey });
    ok('密钥不可复用(401)', r.status === 401, `实际 ${r.status}`);

    console.log('\n[4] 用户 ID 只读（普通用户不可改，管理员可改）');
    r = await api(`/api/users/${idA}/change-id`, 'PUT', { newId: 99901 }, tokenA);
    ok('普通用户改 ID 被拒绝(403)', r.status === 403, `实际 ${r.status}`);
    r = await api(`/api/users/${idA}/change-id`, 'PUT', { newId: 99901 }, tokenAdmin);
    ok('管理员改 ID 成功', r.json.success, r.json.message || JSON.stringify(r.json));
    r = await api('/api/users', 'GET', null, tokenAdmin);
    const moved = (r.json.data || []).find(u => u.id === 99901);
    ok('新 ID 下用户保持不变', r.json.success && moved?.username === 'tester_a', JSON.stringify(moved || null));

    // 改主键会连带让旧 token 失效：鉴权中间件每次请求都按 token 里的 userId 回库取账号，
    // 旧 id 已不存在 → 401。这是有意的——否则「改 ID」就成了绕过角色校验的后门，
    // 而且被盗号的 token 只要改一次 id 就能继续用。所以这里要断言旧 token 失效，
    // 然后用新 ID 重新登录继续后面的用例。
    r = await api('/api/feedback/mine', 'GET', null, tokenA);
    ok('改 ID 后旧 token 立即失效(401)', r.status === 401, `实际 ${r.status}`);
    r = await api('/api/login', 'POST', { username: 'tester_a', password: 'pass123456' });
    tokenA = r.json.data?.token;
    ok('用新 ID 重新登录成功', r.json.success && !!tokenA, r.json.message || '');

    console.log('\n[5] 问题反馈');
    r = await api('/api/feedback', 'POST', { title: '', content: '内容' }, tokenA);
    ok('空标题被拒绝(400)', r.status === 400);
    r = await api('/api/feedback', 'POST', { category: 'bug', title: '自测反馈', content: '这是一条来自 API 自测的反馈内容', contact: 'tester@example.com' }, tokenA);
    const fbId = r.json.data?.id;
    ok('提交反馈', r.json.success && Number.isFinite(fbId), r.json.message || JSON.stringify(r.json));
    r = await api('/api/feedback', 'POST', { category: 'bug', title: '自测反馈2', content: '马上再提交一条，应当触发限流' }, tokenA);
    ok('60 秒内重复提交被限流(429)', r.status === 429, `实际 ${r.status}`);
    r = await api('/api/feedback/mine', 'GET', null, tokenA);
    ok('我的反馈列表', r.json.success && (r.json.data || []).some(f => f.id === fbId), JSON.stringify(r.json.data));
    r = await api('/api/feedback', 'GET', null, tokenAdmin);
    ok('管理员可见反馈', r.json.success && (r.json.data || []).some(f => f.id === fbId), JSON.stringify((r.json.data || []).slice(0, 2)));
    r = await api(`/api/feedback/${fbId}`, 'PUT', { status: 'resolved', reply: '已处理' }, tokenAdmin);
    ok('管理员处理反馈', r.json.success, r.json.message || '');
  } finally {
    try { child.kill(); } catch { }
    cleanup();
  }

  console.log(`\n===== 自测结果: ${pass} 通过, ${fail} 失败 =====`);
  if (failures.length) {
    console.log('失败项:');
    for (const f of failures) console.log('  * ' + f);
  }
  process.exitCode = fail ? 1 : 0;
}

main().catch(e => {
  console.error('[自测] 异常退出:', e.message);
  cleanup();
  process.exit(1);
});
