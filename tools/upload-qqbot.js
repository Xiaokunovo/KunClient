/**
 * 上传 QQ 机器人代码到线上并重启。
 *
 * 线上形态：pm2 应用 `kunqqbot`，cwd /www/wwwroot/KunClient-qqbot，
 * 用目录内的 .venv/bin/python main.py 跑。
 *
 * 三条硬约定：
 *  1. **绝不覆盖 config.json / data.json**：前者含 AppSecret、后者是线上用户数据。
 *     只传代码与配置模板，这两个永远排除在外（本地那份是开发用的，传上去会把线上冲掉）。
 *  2. **先备份再传**：远端 backups/code-<时间戳>.tar.gz，出问题能原地回滚。
 *  3. **传完先 py_compile 再重启**：语法错就不重启，线上继续跑旧代码 ——
 *     比「重启完机器人挂了」好排查得多。
 *
 * 用法：
 *   node tools/upload-qqbot.js                备份 + 上传 + 校验 + 重启
 *   node tools/upload-qqbot.js --no-restart   只上传校验，不动线上进程
 *   node tools/upload-qqbot.js --no-backup    跳过备份（改动极小、赶时间时用）
 *
 * 认证：优先用 ~/.ssh/kunkun_admin_ed25519 密钥；server/config.local.json 里的
 * deploy.password 目前是失效的（2026-10-17 实测 "All configured authentication
 * methods failed"），只在没有密钥时才回落到密码。
 */
const { Client } = require('ssh2');
const fs = require('fs');
const os = require('os');
const path = require('path');
const DEPLOY = require('./deploy-config');

const LOCAL_BASE = path.resolve(__dirname, '..', 'qqbot');
const REMOTE_BASE = '/www/wwwroot/KunClient-qqbot';
const PM2_APP = 'kunqqbot';
const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');

// 永不外传：线上凭据与线上数据
const EXCLUDE_FILES = new Set(['config.json', 'data.json', 'data.json.bak', 'probe_data.json.bak']);
const EXCLUDE_DIRS = new Set(['.venv', '__pycache__', '.git', 'backups', 'node_modules']);

const args = new Set(process.argv.slice(2));
const doBackup = !args.has('--no-backup');
const doRestart = !args.has('--no-restart');

/** 收集要传的文件（相对路径），按扩展名白名单，避免把日志/缓存/凭据带上去。 */
function collect(dir = LOCAL_BASE, rel = '', out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const relPath = rel ? `${rel}/${entry.name}` : entry.name;
    if (entry.isDirectory()) {
      if (EXCLUDE_DIRS.has(entry.name)) continue;
      collect(path.join(dir, entry.name), relPath, out);
      continue;
    }
    if (EXCLUDE_FILES.has(entry.name)) continue;
    if (/\.(py|json|md|txt)$/.test(entry.name) || entry.name === 'requirements.txt') {
      out.push(relPath);
    }
  }
  return out;
}

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

/** 跑一条远端命令，返回输出（失败抛出）。 */
function run(conn, cmd) {
  return new Promise((resolve, reject) => {
    conn.exec(cmd, (err, stream) => {
      if (err) return reject(err);
      let out = '';
      stream.on('data', d => out += d);
      stream.stderr.on('data', d => out += d);
      stream.on('close', (code) => code === 0 ? resolve(out) : reject(new Error(out || `exit ${code}`)));
    });
  });
}

function sftpOf(conn) {
  return new Promise((resolve, reject) => conn.sftp((e, s) => e ? reject(e) : resolve(s)));
}

function put(sftp, local, remote) {
  return new Promise((resolve, reject) => sftp.fastPut(local, remote, (e) => e ? reject(e) : resolve()));
}

(async () => {
  const files = collect();
  const { conn, connectOptions } = connect();

  await new Promise((resolve, reject) => {
    conn.on('ready', resolve).on('error', reject).connect(connectOptions);
  });
  console.log(`[0/4] 已连接 ${DEPLOY.username}@${DEPLOY.host}，待上传 ${files.length} 个文件`);

  // 1) 备份远端代码
  if (doBackup) {
    const stamp = new Date().toISOString().replace(/[-:T]/g, '').slice(0, 14);
    const tar = `backups/code-${stamp}.tar.gz`;
    const out = await run(conn,
      `cd ${REMOTE_BASE} && mkdir -p backups && tar czf ${tar} ` +
      `--exclude=__pycache__ --exclude=.venv core handlers rom tools main.py menu.json panels.json README.md 2>/dev/null; ` +
      `ls -lh ${tar}`);
    console.log(`[1/4] 远端备份 ${tar.trim()}`);
  } else {
    console.log('[1/4] 跳过备份（--no-backup）');
  }

  // 2) 上传（自动建目录；config.json / data.json 已排除）
  const sftp = await sftpOf(conn);
  const dirs = new Set();
  for (const rel of files) {
    const slash = rel.lastIndexOf('/');
    if (slash > 0) dirs.add(rel.slice(0, slash));
  }
  for (const d of dirs) await run(conn, `mkdir -p ${REMOTE_BASE}/${d}`);
  for (const rel of files) {
    await put(sftp, path.join(LOCAL_BASE, rel), `${REMOTE_BASE}/${rel}`);
  }
  console.log(`[2/4] 上传完成 ${files.length} 个文件`);

  // 3) 远端语法校验：不通过就不重启，线上继续跑旧代码
  const check = await run(conn,
    `cd ${REMOTE_BASE} && .venv/bin/python -m py_compile main.py core/*.py handlers/*.py tools/*.py rom/*.py ` +
    `&& .venv/bin/python -c "import json;json.load(open('menu.json'));json.load(open('panels.json'))" ` +
    `&& echo VERIFY_OK`);
  if (!/VERIFY_OK/.test(check)) {
    throw new Error('远端校验未通过，已中止（未重启）:\n' + check);
  }
  console.log('[3/4] 远端 py_compile + JSON 校验通过');

  // 3.5) 菜单 + 帮助渲染自测：真的走一遍命令路径，拦住「改了文案忘同步常量」这类
  // 只有用户发消息才会暴露的错（真实踩过：删掉 SEP 后帮助直接回「命令执行出错」）。
  const smoke = await run(conn,
    `cd ${REMOTE_BASE} && .venv/bin/python main.py --menutest 2>&1 | tail -30`);
  if (!/菜单自测全部通过/.test(smoke)) {
    throw new Error('菜单/帮助自测未通过，已中止（未重启）:\n' + smoke.trim());
  }
  console.log('[3/4] 菜单 + 帮助渲染自测通过');

  // 4) 重启并回读启动日志
  if (!doRestart) {
    console.log('[4/4] 跳过重启（--no-restart），线上仍是旧进程');
  } else {
    await run(conn, `pm2 restart ${PM2_APP} >/dev/null 2>&1`);
    await new Promise(r => setTimeout(r, 7000));
    const out = await run(conn,
      `pm2 list | grep -E '${PM2_APP}'; echo '--- 启动日志 ---'; ` +
      `grep -E '启动完成|自定义菜单|指令面板|鉴权成功|ERROR|Traceback' /root/.pm2/logs/${PM2_APP}-out.log | tail -8`);
    console.log('[4/4] 重启完成\n' + out.trim());
  }
  conn.end();
})().catch((e) => {
  console.error('❌ 部署失败：', e.message);
  process.exit(1);
});
