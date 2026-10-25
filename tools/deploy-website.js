/**
 * 官网原子部署：本地 out/ → 线上 /www/wwwroot/KunClient-server/website
 *
 * 做法（与 README「静态文件的原子替换」同口径）：
 *   1. 本地打成一个 tar.gz（几千个 chunk，逐个 SFTP 传太慢）；
 *   2. 上传到远端 /tmp；
 *   3. 远端解到 website.new → 校验 index.html 存在 → mv 切换，旧版留成 website.old。
 *
 * 不做重启：website/ 由 express.static 托管，每个请求都从磁盘重读，换目录即生效。
 *
 * 用法：
 *   node tools/deploy-website.js              部署（保留 website.old）
 *   node tools/deploy-website.js --dry-run    只打包 + 上传，不动线上目录
 *   node tools/deploy-website.js --no-verify  跳过部署后的线上探测
 *
 * 回滚：node tools/ssh-run.js "cd /www/wwwroot/KunClient-server && rm -rf website && mv website.old website"
 */
const { Client } = require('ssh2');
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const DEPLOY = require('./deploy-config');

const LOCAL_BASE = path.resolve(__dirname, '..', 'website', 'out');
const REMOTE_BASE = '/www/wwwroot/KunClient-server';
const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
const APP_PORT = process.env.DEPLOY_APP_PORT || 3000;

// 部署后必须能在首页找到的标记（证明线上换的是新版本，而不是「传了但没切换」）
const LIVE_MARKERS = ['十七个页面，收进一个窗口', '四步走完一次刷机', '由爱好者维护的工具'];

const args = new Set(process.argv.slice(2));
const dryRun = args.has('--dry-run');
const doVerify = !args.has('--no-verify');

function connect() {
  const conn = new Client();
  const auth = fs.existsSync(KEY)
    ? { privateKey: fs.readFileSync(KEY) }
    : { password: DEPLOY.password };
  return { conn, connectOptions: {
    host: DEPLOY.host, port: DEPLOY.port, username: DEPLOY.username,
    ...auth, readyTimeout: 20000,
  } };
}

function run(conn, cmd) {
  return new Promise((resolve, reject) => {
    conn.exec(cmd, (err, stream) => {
      if (err) return reject(err);
      let out = '';
      stream.on('data', (d) => { out += d; });
      stream.stderr.on('data', (d) => { out += d; });
      stream.on('close', (code) => (code === 0 ? resolve(out) : reject(new Error(out || `exit ${code}`))));
    });
  });
}

function sftpOf(conn) {
  return new Promise((resolve, reject) => conn.sftp((e, s) => (e ? reject(e) : resolve(s))));
}

function put(sftp, local, remote) {
  return new Promise((resolve, reject) => sftp.fastPut(local, remote, (e) => (e ? reject(e) : resolve())));
}

(async () => {
  if (!fs.existsSync(path.join(LOCAL_BASE, 'index.html'))) {
    throw new Error(`本地产物不存在：${LOCAL_BASE}/index.html（先 cd website && npm run build）`);
  }

  const stamp = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 14);
  const localTar = path.join(os.tmpdir(), `kc-website-${stamp}.tar.gz`);
  const remoteTar = `/tmp/kc-website-${stamp}.tar.gz`;

  // 1) 打包
  // 🔴 必须用 Windows 自带的 bsdtar（System32\tar.exe）：Git Bash 里的 GNU tar 会把
  //    `C:\Users\...` 当成「远端主机:路径」去连，直接报 Cannot connect to C。
  const TAR = process.platform === 'win32' ? 'C:/Windows/System32/tar.exe' : 'tar';
  execFileSync(TAR, ['-czf', localTar, '-C', LOCAL_BASE, '.'], { stdio: 'inherit' });
  const size = (fs.statSync(localTar).size / 1024 / 1024).toFixed(2);
  console.log(`[1/5] 本地打包 ${path.basename(localTar)}（${size} MB）`);

  const { conn, connectOptions } = connect();
  await new Promise((resolve, reject) => { conn.on('ready', resolve).on('error', reject).connect(connectOptions); });
  console.log(`[2/5] 已连接 ${DEPLOY.username}@${DEPLOY.host}`);

  // 2) 上传
  const sftp = await sftpOf(conn);
  await put(sftp, localTar, remoteTar);
  console.log(`[3/5] 上传完成 → ${remoteTar}`);

  if (dryRun) {
    console.log('[4/5] --dry-run：已停在切换之前，线上目录未改动');
    conn.end();
    fs.unlinkSync(localTar);
    return;
  }

  // 3) 解包 + 原子切换（先解到 .new 并自检，再 mv；旧版留 .old）
  await run(conn, `cd ${REMOTE_BASE} && rm -rf website.new && mkdir -p website.new`);
  await run(conn, `cd ${REMOTE_BASE} && tar xzf ${remoteTar} -C website.new`);
  // 线上进程以 www 运行（pm2 用户 www），root 解出来的目录要换属主，否则后续 www 侧写不了
  await run(conn, `cd ${REMOTE_BASE} && chown -R www:www website.new`);
  await run(conn, `cd ${REMOTE_BASE} && test -f website.new/index.html && test -d website.new/_next`);
  const swap = await run(conn, [
    `cd ${REMOTE_BASE}`,
    'rm -rf website.old',
    'if [ -d website ]; then mv website website.old; fi',
    'mv website.new website',
    'echo SWAP_OK',
    'ls website | wc -l',
    'du -sh website',
  ].join(' && '));
  if (!/SWAP_OK/.test(swap)) throw new Error('切换失败：\n' + swap);
  console.log('[4/5] 切换完成（旧版保留为 website.old）\n' + swap.split('\n').filter(Boolean).slice(-2).join('\n'));

  // 4) 清远端临时包
  await run(conn, `rm -f ${remoteTar}`);

  // 5) 线上验收：进程还在、首页 200、新文案在
  if (doVerify) {
    const probe = await run(conn, [
      `code=$(curl -s -o /tmp/kc-live.html -w '%{http_code}' http://127.0.0.1:${APP_PORT}/)`,
      'echo "HTTP=$code"',
      `grep -o "十七个页面，收进一个窗口" /tmp/kc-live.html | head -1 || true`,
      `grep -o "四步走完一次刷机" /tmp/kc-live.html | head -1 || true`,
      `grep -o "由爱好者维护的工具" /tmp/kc-live.html | head -1 || true`,
      `curl -s -o /dev/null -w 'console=%{http_code}\\n' http://127.0.0.1:${APP_PORT}/console/`,
      `curl -s -o /dev/null -w 'privacy=%{http_code}\\n' http://127.0.0.1:${APP_PORT}/privacy/`,
      `curl -s -o /dev/null -w 'api=%{http_code}\\n' http://127.0.0.1:${APP_PORT}/api/app/version`,
    ].join('; '));
    console.log('[5/5] 线上探测\n' + probe.trim());
    if (!/HTTP=200/.test(probe)) throw new Error('线上首页不是 200');
    for (const m of LIVE_MARKERS) {
      if (!probe.includes(m)) throw new Error(`线上缺少标记「${m}」——可能没切过去`);
    }
    if (!/console=200/.test(probe)) throw new Error('控制台 404/500，检查 server.js 静态挂载');
  } else {
    console.log('[5/5] 跳过线上探测（--no-verify）');
  }

  conn.end();
  fs.unlinkSync(localTar);
  console.log('\n✅ 官网已上线');
})().catch((e) => {
  console.error('❌ 部署失败：', e.message);
  process.exit(1);
});
