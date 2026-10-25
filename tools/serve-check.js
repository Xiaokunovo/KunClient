// 本地静态服务自检：模拟 server.js 的 / 托管方式，检查页面与资源链接是否可用
const http = require('http');
const fs = require('fs');
const path = require('path');

// 站点根目录与端口支持外部配置：环境变量 SERVE_ROOT / SERVE_PORT > 默认（仓库内 server/website）
const ROOT = path.resolve(process.env.SERVE_ROOT || path.join(__dirname, '..', 'server', 'website'));
const PORT = Number(process.env.SERVE_PORT) || 4173;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.txt': 'text/plain; charset=utf-8',
  '.woff2': 'font/woff2',
};

const server = http.createServer((req, res) => {
  let rel = decodeURIComponent(req.url.split('?')[0]);
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.resolve(ROOT, '.' + (rel.startsWith('/') ? rel : '/' + rel));
  if (!file.startsWith(ROOT) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('404');
    return;
  }
  res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});

function get(p) {
  return new Promise((resolve) => {
    http.get({ host: '127.0.0.1', port: PORT, path: p }, (res) => {
      let body = '';
      res.on('data', (d) => { body += d; });
      res.on('end', () => resolve({ status: res.statusCode, body, type: res.headers['content-type'] }));
    }).on('error', (e) => resolve({ status: 0, body: e.message, type: '' }));
  });
}

server.listen(PORT, async () => {
  const home = await get('/');
  console.log('/              ->', home.status, home.type, home.body.length + ' bytes');

  const SERVER_PROVIDED = ['/console', '/admin', '/assets']; // 由 server.js 托管，静态导出中不存在
  const assets = [...new Set(home.body.match(/(?:href|src)="(\/[^"]+)"/g).map((s) => s.replace(/^(?:href|src)="/, '').replace(/"$/, '')))]
    .filter((u) => !u.startsWith('//'));
  let bad = 0;
  if (home.status !== 200) bad++;
  for (const a of assets) {
    if (SERVER_PROVIDED.some((p) => a.startsWith(p))) {
      console.log('SKIP (服务器托管) ' + a);
      continue;
    }
    const r = await get(a);
    if (r.status !== 200) bad++;
    console.log((r.status === 200 ? 'OK  ' : 'FAIL') + ' ' + String(r.status).padEnd(4) + a);
  }

  console.log('\n检查内链锚点：');
  for (const h of ['#caps', '#features', '#flow', '#shots', '#download', '#specs', '#faq', '#about']) {
    const ok = home.body.includes('id="' + h.slice(1) + '"');
    if (!ok) bad++;
    console.log((ok ? 'OK  ' : 'FAIL') + ' ' + h);
  }

  console.log('\n关键内容：');
  // 2026-10-19 v6 重写后同步：'三步开始使用' → '四步走完一次刷机'、'关于我们' → '由爱好者维护的工具'。
  // '更新内容' 撤掉：它只在拿到 /api/app/version 后才渲染，静态产物里本来就没有（不是丢失）。
  for (const s of ['下载安装', '四步走完一次刷机', '常见问题', '由爱好者维护的工具', '控制台']) {
    const ok = home.body.includes(s);
    if (!ok) bad++;
    console.log((ok ? 'OK  ' : 'FAIL') + ' ' + s);
  }

  server.close();
  console.log(bad === 0 ? '\n本地自检通过（0 个问题）' : `\n发现 ${bad} 个问题`);
  process.exit(bad === 0 ? 0 : 1);
});