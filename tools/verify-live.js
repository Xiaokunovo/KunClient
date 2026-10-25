// 线上验证：官网页面、静态资源、版本接口与下载直链是否都正常
//
// 2026-10-19：改为打**公网入口**（默认 https://kunawa.xyz，可用 LIVE_BASE 覆盖）。
// 原来打 DEPLOY.host:appPort（your-server.example.com:3000）——线上 node 只监听 127.0.0.1:3000，
// 从外面打过去拿到的是别的响应体（没有 /_next 链接、所有文案断言全红），等于在验错对象。
const https = require('https');
const http = require('http');

const BASE = process.env.LIVE_BASE || 'https://kunawa.xyz';
const lib = BASE.startsWith('https') ? https : http;
const baseUrl = new URL(BASE);

function get(p, method = 'GET') {
  return new Promise((resolve) => {
    const req = lib.request({
      host: baseUrl.hostname,
      port: baseUrl.port || (baseUrl.protocol === 'https:' ? 443 : 80),
      path: p,
      method,
      headers: { 'User-Agent': 'verify-live', Accept: '*/*' },
    }, (res) => {
      let len = 0;
      let body = '';
      res.on('data', (d) => { len += d.length; if (body.length < 400000) body += d; });
      res.on('end', () => resolve({ status: res.statusCode, len, body, type: res.headers['content-type'] || '', disp: res.headers['content-disposition'] || '' }));
    });
    req.on('error', (e) => resolve({ status: 0, len: 0, body: e.message, type: '', disp: '' }));
    req.setTimeout(20000, () => { req.destroy(); resolve({ status: 0, len: 0, body: 'timeout', type: '', disp: '' }); });
    req.end();
  });
}

(async () => {
  let bad = 0;
  const ok = (cond, label) => { if (!cond) bad++; console.log((cond ? 'OK   ' : 'FAIL ') + label); };

  console.log(`目标：${BASE}`);
  const home = await get('/');
  console.log(`\n首页: ${home.status} ${home.len} bytes ${home.type}`);
  ok(home.status === 200, '首页返回 200');

  // v6「工程制图」口径：文案 + 结构双保险。结构标记丢了 = 3D 层整块没了。
  const MUST = [
    '把设备', '救回来', '十七个页面，收进一个窗口', '三块模块，覆盖全部功能',
    '四步走完一次刷机', '两条芯片线，同一套界面', '下载安装', '常见问题',
    '由爱好者维护的工具', '控制台',
    'hero-floor', 'deck', 'proto-row', 'caps-grid', 'mods', 'faq-item',
    'id="download"', 'id="about"',
  ];
  // 旧版残留：深色段拼接 / 旧 3D / 旧文案。出现任意一个都说明线上还是旧包。
  const GONE = [
    '/user', '#2D7AF0', 'LoginModal', '全机型支持', '通常已自带',
    '三步开始使用', '关于我们', 'hero-grid', 'hero-canvas',
    'orbit-item', 'orbit-ring', 'scene-bridge', 'deck-panel',
  ];
  console.log('\n新内容检查：');
  for (const s of MUST) ok(home.body.includes(s), `包含「${s}」`);
  console.log('\n旧内容清除检查：');
  for (const s of GONE) ok(!home.body.includes(s), `已移除「${s}」`);

  console.log('\n静态资源：');
  const matches = home.body.match(/(?:href|src)="(\/_next\/[^"]+)"/g) || [];
  const urls = [...new Set(matches.map((s) => s.replace(/^(?:href|src)="/, '').replace(/"$/, '')))];
  ok(urls.length > 0, `首页引用了 ${urls.length} 个 /_next 资源`);
  for (const u of urls) {
    const r = await get(u);
    ok(r.status === 200, `${u} (${r.len} bytes)`);
  }
  const css = urls.filter((u) => u.endsWith('.css'));
  ok(css.length > 0, `样式表已加载（${css.length} 个）`);

  // 官网已改为纯文字品牌标识（不再有独立 logo 图），这里只校验站图标与品牌文字
  const i = await get('/favicon.ico');
  ok(i.status === 200 && i.len > 100, `/favicon.ico (${i.len} bytes)`);
  ok(home.body.includes('<em>Client</em>'), '品牌标识含彩色 Client');

  console.log('\n接口与下载：');
  const v = await get('/api/app/version');
  ok(v.status === 200, `/api/app/version 返回 ${v.status}`);
  let info = null;
  try { info = JSON.parse(v.body).data; } catch (e) {}
  ok(!!info, '版本接口返回 data');

  // 官网展示开关：内测阶段 / 官网下载关闭时，官网会刻意收起全部版本号与下载入口。
  const si = await get('/api/site/info');
  let siteInfo = null;
  try { siteInfo = JSON.parse(si.body).data; } catch (e) {}
  const siteDownloadOpen = !siteInfo || siteInfo.downloadEnabled !== false;

  if (info) {
    console.log(`     版本 ${info.version} / 发布 ${info.buildDate} / 下载 ${info.downloadUrl || '（未下发）'}`
      + ` / 官网下载 ${siteDownloadOpen ? '开' : '关'} / updateEnabled ${info.updateEnabled}`);
    // 版本号可能是 3 段（2.0.0）也可能 4 段（2.0.0.5，Qt 构建号），两种都算合法
    ok(/^\d+\.\d+\.\d+(?:\.\d+)?$/.test(info.version), `版本号格式合法（当前 ${info.version}）`);
    if (siteDownloadOpen) {
      // 2026-10-21：官网版本号是**运行时** fetch 出来的（Hero 的 pill 由 JS 拼），
      // 静态 HTML 里没有 → 不能用 home.body 断言，那是 curl 视角，验不到 hydration 之后。
      console.log('     （官网版本号由前端渲染，静态 HTML 不含，跳过「版本占位」断言）');
    } else {
      console.log('     （官网下载已关闭 → 官网刻意收起版本号，跳过「版本占位」断言）');
    }
    // 云端更新 / 内测阶段关闭时，version 接口刻意不下发 downloadUrl（updateEnabled=false）。
    // 🔴 此时**不能**拿空串去 replace 再打直链 —— `get('')` 会退化成打首页并拿到 200 = **假绿**。
    if (info.updateEnabled === false || !info.downloadUrl) {
      ok(info.updateEnabled === false, `下载通道已按开关关闭（updateEnabled=${info.updateEnabled}），跳过直链校验`);
    } else {
      const p = info.downloadUrl.replace(/^https?:\/\/[^/]+/, '');
      const dl = await get(p, 'HEAD');
      ok(dl.status === 200, `下载直链 ${p} 返回 ${dl.status} (${dl.len} bytes)`);
    }
  }

  const consolePage = await get('/console/');
  ok(consolePage.status === 200, `/console/ 返回 ${consolePage.status}`);
  const adminPage = await get('/admin/');
  ok(adminPage.status === 200, `/admin/ 返回 ${adminPage.status}`);
  const privacy = await get('/privacy/');
  ok(privacy.status === 200 && privacy.body.includes('隐私政策'), `/privacy/ 返回 ${privacy.status}`);
  const notFound = await get('/no-such-page-xyz');
  ok(notFound.status === 404 || notFound.status === 200, `未知路径返回 ${notFound.status}（404 优先）`);

  console.log(bad === 0 ? '\n线上验证全部通过' : `\n线上验证发现 ${bad} 个问题`);
  process.exit(bad === 0 ? 0 : 1);
})();
