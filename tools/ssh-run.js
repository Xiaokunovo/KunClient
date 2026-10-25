/**
 * 在线上服务器执行一条命令（只读排查 / 一次性操作）。
 *
 * 用法：node tools/ssh-run.js "pm2 list | grep kunqqbot"
 *
 * 认证同 upload-qqbot.js：优先 ~/.ssh/kunkun_admin_ed25519 密钥；
 * server/config.local.json 的 deploy.password 已失效（2026-10-17 实测），仅作回落。
 * 连接参数（host/user/port）仍走 deploy-config，与其它部署脚本一致。
 */
const { Client } = require('ssh2');
const fs = require('fs');
const os = require('os');
const path = require('path');
const DEPLOY = require('./deploy-config');

const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
const cmd = process.argv.slice(2).join(' ');
if (!cmd) {
  console.error('用法：node tools/ssh-run.js "<远端命令>"');
  process.exit(1);
}

const conn = new Client();
conn.on('ready', () => {
  conn.exec(cmd, (err, stream) => {
    if (err) { console.error('EXEC ERR:', err.message); conn.end(); process.exit(1); }
    stream.on('close', (code) => { conn.end(); process.exit(code || 0); })
      .on('data', d => process.stdout.write(d.toString()))
      .stderr.on('data', d => process.stderr.write(d.toString()));
  });
}).on('error', (e) => { console.error('SSH ERR:', e.message); process.exit(2); })
  .connect({
    host: DEPLOY.host, port: DEPLOY.port, username: DEPLOY.username,
    ...(fs.existsSync(KEY) ? { privateKey: fs.readFileSync(KEY) } : { password: DEPLOY.password }),
    readyTimeout: 20000,
  });
