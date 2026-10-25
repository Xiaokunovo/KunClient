// 官网对外口径（site_stage）自测 + 已下架功能的回归断言。
// 在临时端口 + 临时数据库上拉起 server.js，跑完自动清理，不影响正式库与线上。
// 用法：node selftest-content.js
//
// 覆盖两类：
//   1) 云端 ROM / 模块功能已整体删除 —— 相关接口必须 404。
//      这条断言能挡住两类回归：有人把接口加回来；或部署时漏传 server.js
//      导致线上还是旧版本（旧版本会返回 200/401，不会 404）。
//   2) site_stage 开关 —— 官网「公测免费 / 正式收费」口径由控制台决定。
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const http = require('http');

const SERVER_DIR = path.resolve(__dirname, '..', 'server');
const PORT = Number(process.env.SELFTEST_PORT) || 3124;
const TMP_DB = path.join(SERVER_DIR, '.runtime', `selftest-content-${process.pid}.sqlite`);

const ADMIN_USER = process.env.SELFTEST_ADMIN_USER || 'admin';
const ADMIN_PASS = process.env.SELFTEST_ADMIN_PASS || 'selftest_admin_pass';

let fail = 0;
const pass = (m) => console.log('  [PASS] ' + m);
const bad = (m) => { fail++; console.log('  [FAIL] ' + m); };
const check = (ok, m) => (ok ? pass(m) : bad(m));

function req(method, urlPath, { body, headers = {}, token } = {}) {
  return new Promise((resolve, reject) => {
    const h = { ...headers };
    if (token) h.Authorization = 'Bearer ' + token;
    if (typeof body === 'string') h['Content-Type'] = h['Content-Type'] || 'application/json';
    const r = http.request({ host: '127.0.0.1', port: PORT, method, path: urlPath, headers: h }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => {
        const raw = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(raw); } catch { }
        resolve({ status: res.statusCode, json, text: raw.slice(0, 200) });
      });
    });
    r.on('error', reject);
    if (body !== undefined) r.write(body);
    r.end();
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function cleanup() {
  try { fs.unlinkSync(TMP_DB); } catch { }
}

(async () => {
  cleanup();
  fs.mkdirSync(path.dirname(TMP_DB), { recursive: true });

  const child = spawn(process.execPath, ['server.js'], {
    cwd: SERVER_DIR,
    env: {
      ...process.env,
      PORT: String(PORT),
      DB_PATH: TMP_DB,
      ADMIN_USER,
      ADMIN_PASS,
      ADMIN_EMAIL: 'selftest@example.invalid',
      MAIL_DISABLE: '1',
      JWT_SECRET: 'selftest-content-secret',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let log = '';
  child.stdout.on('data', (d) => log += d);
  child.stderr.on('data', (d) => log += d);
  const stop = () => { try { child.kill(); } catch { } cleanup(); };

  let up = false;
  for (let i = 0; i < 40; i++) {
    await sleep(300);
    try { if ((await req('GET', '/api/health')).status === 200) { up = true; break; } } catch { }
  }
  if (!up) { console.error('服务未启动:\n' + log.slice(-1500)); stop(); process.exit(1); }
  console.log(`  ✓ 服务器已就绪 (端口 ${PORT})`);

  const login = await req('POST', '/api/login', { body: JSON.stringify({ username: ADMIN_USER, password: ADMIN_PASS }) });
  if (!login.json?.success) { console.error('管理员登录失败: ' + login.text); stop(); process.exit(1); }
  const token = login.json.data.token;

  console.log('\n[1] 已下架的云端 ROM / 模块功能：接口必须 404');
  {
    // 带 token 也不该存在——若返回 200/401 说明路由还在（旧代码）或被加回来了
    const gone = [
      ['GET', '/api/roms'],
      ['GET', '/api/roms/download/anything.ofp'],
      ['GET', '/api/rom-proxy/catalog'],
      ['GET', '/api/rom-proxy/devices/OPPO/PEQM00/versions'],
      ['GET', '/api/modules'],
      ['GET', '/api/modules/download/anything.zip'],
      ['GET', '/api/admin/content'],
      ['DELETE', '/api/admin/content/module/anything.zip'],
    ];
    for (const [method, p] of gone) {
      const r = await req(method, p, { token });
      check(r.status === 404, `${method} ${p} -> 404（实际 ${r.status}）`);
    }

    // 目录也应当不在了（上传接口删了、目录也删了，避免又冒出空列表）
    check(!fs.existsSync(path.join(SERVER_DIR, 'assets', 'roms')), 'assets/roms 已删除');
    check(!fs.existsSync(path.join(SERVER_DIR, 'assets', 'modules')), 'assets/modules 已删除');

    // 反面对照：同时期的其他功能必须还活着，避免"一刀切删过头"
    const alive = ['/api/app/version', '/api/loaders/firehose/index', '/api/packages/tools'];
    for (const p of alive) {
      const r = await req('GET', p);
      check(r.status === 200, `${p} 仍然可用（${r.status}）`);
    }
  }

  console.log('\n[2] 官网对外口径开关（site_stage）');
  {
    const anon = await req('GET', '/api/site/info');
    check(anon.status === 200 && anon.json?.success, '未登录也能读 /api/site/info');
    check(anon.json.data.stage === 'beta', `默认口径 beta（实际 ${anon.json.data.stage}）`);
    check(anon.json.data.freeNow === true, 'beta 阶段 freeNow=true');

    for (const stage of ['paid', 'free', 'beta']) {
      const put = await req('PUT', '/api/admin/settings', { body: JSON.stringify({ site_stage: stage }), token });
      check(put.json?.success === true, `切换 ${stage} 保存成功`);
      const got = await req('GET', '/api/site/info');
      check(got.json.data.stage === stage, `  /api/site/info 立即返回 ${stage}`);
      check(got.json.data.freeNow === (stage !== 'paid'), `  freeNow 与阶段一致（${stage} -> ${got.json.data.freeNow}）`);
    }

    await req('PUT', '/api/admin/settings', { body: JSON.stringify({ site_stage: 'garbage' }), token });
    const badStage = await req('GET', '/api/site/info');
    check(badStage.json.data.stage === 'beta', '非法 stage 回落到 beta');

    // 普通用户不能改口径
    const uname = 'selftest_normal_' + Math.floor(Math.random() * 100000);
    await req('POST', '/api/register', { body: JSON.stringify({ username: uname, password: 'pass1234' }) });
    const ulogin = await req('POST', '/api/login', { body: JSON.stringify({ username: uname, password: 'pass1234' }) });
    const utoken = ulogin.json?.data?.token;
    const denied = await req('PUT', '/api/admin/settings', { body: JSON.stringify({ site_stage: 'paid' }), token: utoken });
    check(denied.status === 403, `普通用户改设置被拒(403)，实际 ${denied.status}`);
  }

  stop();
  console.log(fail ? `\n  ${fail} 项失败` : '\n  全部通过');
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error('FAILED: ' + e.message); cleanup(); process.exit(1); });
