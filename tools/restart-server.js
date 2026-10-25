/**
 * 重启线上登录服务。
 *
 * 线上由 pm2 托管，应用名 **`KunClient-server`**（script path / cwd 都指向
 * /www/wwwroot/KunClient-server，2026-10-20 用 `pm2 list` 实测；旧文档写的 `kunserver` 已不存在），
 * 所以正解是 `pm2 restart KunClient-server` —— pm2 会按记录用正确的 cwd 重新拉起。
 *
 * 为什么不再用以前那套 `pkill -9 -f "node.*server.js"` + `nohup node server.js &`：
 *   1) pkill -f 的模式会匹配到**执行这条命令的 shell 自己**（命令行里就含 "node server.js"），
 *      shell 会在跑到 nohup 之前被 SIGKILL —— 表现为「重启命令跑完，什么输出都没有」，
 *      而进程其实是被 pm2 顺手拉起来的（2026-09-21 实测踩到）。
 *   2) 即使没被自杀，nohup 那份会和 pm2 拉起的那份抢同一个端口（EADDRINUSE），
 *      在 pm2 里表现为 `unstable restarts` 增长。
 * 只有在「pm2 里找不到该应用」时才退回端口法兜底（按端口找 pid 杀，不用模式匹配）。
 */
const { Client } = require('ssh2');
const DEPLOY = require('./deploy-config');

// 优先级：PM2_APP 环境变量 > deploy.pm2_name（config.json）> 线上实测名
const APP_NAME = process.env.PM2_APP || DEPLOY.pm2Name || 'KunClient-server';
const conn = new Client();

conn.on('ready', () => {
  // 先看 pm2 里有没有这个应用；有就交给 pm2，没有才兜底。
  const cmd = [
    `if pm2 describe ${APP_NAME} >/dev/null 2>&1; then`,
    `  pm2 restart ${APP_NAME};`,
    `  sleep 3;`,
    `  pm2 describe ${APP_NAME} | grep -E 'status|restarts' | head -4;`,
    `else`,
    `  echo 'pm2 无 ${APP_NAME}，退回端口法';`,
    `  PIDS=$(lsof -t -i:${DEPLOY.appPort} 2>/dev/null);`,
    `  [ -n "$PIDS" ] && kill -9 $PIDS;`,
    `  sleep 2;`,
    `  cd ${DEPLOY.remoteBase} && nohup node server.js > .runtime/boot.log 2>&1 &`,
    `  sleep 3;`,
    `fi`,
    `curl -s -o /dev/null -w "HTTP %{http_code}\\n" http://localhost:${DEPLOY.appPort}/`,
  ].join('\n');   // 必须换行拼接：这些片段本身带 then/else/fi，用空格拼会变成非法 sh 语法

  conn.exec(cmd, (err, stream) => {
    if (err) { console.error(err.message); conn.end(); return; }
    let out = '';
    stream.on('data', d => out += d);
    stream.stderr.on('data', d => out += d);
    stream.on('close', () => { console.log(out.trim()); conn.end(); });
  });
}).on('error', (e) => { console.error('SSH:', e.message); process.exit(1); });

// 认证与其它部署脚本同口径：优先 ~/.ssh 密钥，config.local.json 的密码已失效（2026-10-17 实测）
const fs = require('fs');
const os = require('os');
const path = require('path');
const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
conn.connect({
  host: DEPLOY.host, port: DEPLOY.port, username: DEPLOY.username,
  ...(fs.existsSync(KEY) ? { privateKey: fs.readFileSync(KEY) } : { password: DEPLOY.password }),
  readyTimeout: 20000,
});
