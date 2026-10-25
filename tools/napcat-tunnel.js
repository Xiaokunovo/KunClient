/**
 * NapCat WebUI 本地隧道 —— 把服务器上的 NapCat WebUI（容器内 6099）映射到本机。
 *
 * 用法：node tools/napcat-tunnel.js
 * 然后浏览器打开 http://127.0.0.1:16099/webui?token=<token>
 *
 * 为什么不直接发布端口：6099 没有映射到宿主机，且 WebUI 暴露公网会被扫。
 * 走 SSH 隧道只有本机能访问，不碰 UFW。
 */
const { Client } = require('ssh2');
const net = require('net');
const fs = require('fs');
const os = require('os');
const path = require('path');
const DEPLOY = require('./deploy-config');

const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
const LOCAL_PORT = Number(process.env.TUNNEL_PORT || 16099);
const TARGET_HOST = process.env.TARGET_HOST || '172.17.0.2';
const TARGET_PORT = Number(process.env.TARGET_PORT || 6099);

const conn = new Client();

conn.on('ready', () => {
  console.log('[tunnel] SSH ready -> ' + DEPLOY.host);
  const server = net.createServer((sock) => {
    conn.forwardOut('127.0.0.1', LOCAL_PORT, TARGET_HOST, TARGET_PORT, (err, stream) => {
      if (err) { console.error('[tunnel] forwardOut failed:', err.message); sock.destroy(); return; }
      sock.pipe(stream).pipe(sock);
      stream.on('close', () => sock.destroy());
      sock.on('error', () => stream.destroy());
    });
  });
  server.listen(LOCAL_PORT, '127.0.0.1', () => {
    console.log('[tunnel] listening http://127.0.0.1:' + LOCAL_PORT + ' -> ' + TARGET_HOST + ':' + TARGET_PORT);
  });
  server.on('error', (e) => { console.error('[tunnel] listen error:', e.message); process.exit(1); });
});

conn.on('error', (e) => { console.error('[tunnel] SSH error:', e.message); process.exit(2); });
conn.on('close', () => { console.error('[tunnel] SSH closed'); process.exit(3); });

conn.connect({
  host: DEPLOY.host,
  port: DEPLOY.port,
  username: DEPLOY.username,
  ...(fs.existsSync(KEY) ? { privateKey: fs.readFileSync(KEY) } : { password: DEPLOY.password }),
  readyTimeout: 20000,
  keepaliveInterval: 15000,
});
