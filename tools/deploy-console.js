/**
 * 控制台原子部署：本地 server/console → 线上 /www/wwwroot/KunClient-server/console
 *
 * 与 deploy-website.js 同一套做法（tar 打包 → SFTP → 解到 .new → mv 切换），
 * 差别只有两处：
 *   1. 控制台是**源码直出**（express.static 托管 index.html + css.v2/ + js.v2/），没有构建步骤；
 *   2. 部署前先跑 tools/check-console.js 的等价静态检查（语法 / import 对账 / data-act 注册），
 *      检查不过就不上传 —— 线上继续跑旧版，比传上去白屏好排查。
 *
 * 用法：
 *   node tools/deploy-console.js              检查 + 部署 + 线上探测
 *   node tools/deploy-console.js --dry-run    只打包上传，不动线上目录
 *   node tools/deploy-console.js --no-verify  跳过线上探测
 *
 * 回滚：node tools/ssh-run.js "cd /www/wwwroot/KunClient-server && rm -rf console && mv console.old console"
 */
const { Client } = require('ssh2');
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const DEPLOY = require('./deploy-config');

// 2026-10-22 起 server/ 归档到 _archive/：这里原来硬写 ../server/console，
// 归档后第 69 行的存在性检查会抛「本地控制台不存在」—— 看起来像文件被删了。
// 与 check-console.js / deploy-config.js 同一套回落顺序。
function resolveLocalConsole() {
  const candidates = [
    path.resolve(__dirname, '..', 'server', 'console'),
    path.resolve(__dirname, '..', '_archive', 'server', 'console'),
  ];
  for (const d of candidates) if (fs.existsSync(path.join(d, 'index.html'))) return d;
  throw new Error('找不到本地控制台（已试过：\n  ' + candidates.join('\n  ') + '）');
}
const LOCAL_BASE = resolveLocalConsole();
const REMOTE_BASE = '/www/wwwroot/KunClient-server';
const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
const APP_PORT = process.env.DEPLOY_APP_PORT || 3000;

// 部署后必须能在 /console/ 里找到的标记
const LIVE_MARKERS = ['css.v2/console.css', 'data-act="auth.login"', 'js.v2/main.js'];
const ASSET_PROBES = ['/console/css.v2/console.css', '/console/js.v2/main.js', '/console/js.v2/app.js'];

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
    throw new Error(`本地控制台不存在：${LOCAL_BASE}/index.html`);
  }

  // 0) 静态检查（语法 / import 对账 / $('id') / data-act 注册）
  console.log('[0/5] 静态检查 tools/check-console.js');
  try {
    const out = execFileSync(process.execPath, [path.join(__dirname, 'check-console.js')], { encoding: 'utf8' });
    const tail = out.trim().split('\n').slice(-2).join('\n');
    console.log(tail);
    if (!/全部检查通过/.test(out)) throw new Error(out.trim().split('\n').filter((l) => /FAIL|MISS|错误/.test(l)).join('\n') || '检查未通过');
  } catch (e) {
    throw new Error('控制台静态检查未通过，已中止（未上传）：\n' + (e.stdout || e.message));
  }

  const stamp = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 14);
  const localTar = path.join(os.tmpdir(), `kc-console-${stamp}.tar.gz`);
  const remoteTar = `/tmp/kc-console-${stamp}.tar.gz`;

  // 🔴 必须用 Windows 自带 bsdtar：Git Bash 的 GNU tar 会把 C:\... 当「主机:路径」去连
  const TAR = process.platform === 'win32' ? 'C:/Windows/System32/tar.exe' : 'tar';
  execFileSync(TAR, ['-czf', localTar, '-C', LOCAL_BASE, '.'], { stdio: 'inherit' });
  console.log(`[1/5] 本地打包 ${path.basename(localTar)}（${(fs.statSync(localTar).size / 1024).toFixed(0)} KB）`);

  const { conn, connectOptions } = connect();
  await new Promise((resolve, reject) => { conn.on('ready', resolve).on('error', reject).connect(connectOptions); });
  console.log(`[2/5] 已连接 ${DEPLOY.username}@${DEPLOY.host}`);

  const sftp = await sftpOf(conn);
  await put(sftp, localTar, remoteTar);
  console.log(`[3/5] 上传完成 → ${remoteTar}`);

  if (dryRun) {
    console.log('[4/5] --dry-run：已停在切换之前，线上目录未改动');
    conn.end();
    fs.unlinkSync(localTar);
    return;
  }

  await run(conn, `cd ${REMOTE_BASE} && rm -rf console.new && mkdir -p console.new`);
  await run(conn, `cd ${REMOTE_BASE} && tar xzf ${remoteTar} -C console.new`);
  await run(conn, `cd ${REMOTE_BASE} && chown -R www:www console.new`);
  await run(conn, `cd ${REMOTE_BASE} && test -f console.new/index.html && test -f console.new/css.v2/console.css && test -d console.new/js.v2/views`);
  const swap = await run(conn, [
    `cd ${REMOTE_BASE}`,
    'rm -rf console.old',
    'if [ -d console ]; then mv console console.old; fi',
    'mv console.new console',
    'echo SWAP_OK',
    'du -sh console',
  ].join(' && '));
  if (!/SWAP_OK/.test(swap)) throw new Error('切换失败：\n' + swap);
  console.log('[4/5] 切换完成（旧版保留为 console.old）' + swap.replace(/SWAP_OK/, '').trim());

  await run(conn, `rm -f ${remoteTar}`);

  if (doVerify) {
    const probe = await run(conn, [
      `code=$(curl -s -o /tmp/kc-con.html -w '%{http_code}' http://127.0.0.1:${APP_PORT}/console/)`,
      'echo "HTML=$code"',
      `grep -o "css/console.css" /tmp/kc-con.html | head -1`,
      `grep -o 'data-act="auth.login"' /tmp/kc-con.html | head -1`,
      `grep -o "js.v2/main.js" /tmp/kc-con.html | head -1`,
      ...ASSET_PROBES.map((p) => `curl -s -o /dev/null -w '${p}=%{http_code}\\n' http://127.0.0.1:${APP_PORT}${p}`),
    ].join('; '));
    console.log('[5/5] 线上探测\n' + probe.trim());
    if (!/HTML=200/.test(probe)) throw new Error('线上 /console/ 不是 200');
    for (const m of LIVE_MARKERS) {
      if (!probe.includes(m)) throw new Error(`线上缺少标记「${m}」——可能没切过去`);
    }
    for (const p of ASSET_PROBES) {
      if (!new RegExp(`${p.replace(/[/.]/g, '\\$&')}=200`).test(probe)) throw new Error(`${p} 不是 200`);
    }
  } else {
    console.log('[5/5] 跳过线上探测（--no-verify）');
  }

  conn.end();
  fs.unlinkSync(localTar);
  console.log('\n✅ 控制台已上线');
})().catch((e) => {
  console.error('❌ 部署失败：', e.message);
  process.exit(1);
});
