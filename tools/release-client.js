/**
 * 客户端发版（静态单文件 → 线上 assets → 版本号入库 → 线上验收）
 *
 * 为什么要有这个脚本：以前发版是三个**一次性**脚本（push-version.js 走 API、
 * update-version.js 改库、upload-exe.js 传包），版本号/日志/文件名全写死在代码里，
 * 每发一版都要改脚本。这里把它们合成一条可重复的流程，参数化。
 *
 * 正常流程（本脚本按顺序做完）：
 *   1. 从 `src/cpp/kc/version.h` 读版本号（**唯一真源**，别手填）
 *   2. 检查 `src/cpp/dist-static/KunClient-<ver>.exe` / `.7z` 是否已产出（--build 可自动构建）
 *   3. 上传到线上 `/assets/`，文件名**必须带版本号**（不带版本的 KunClient.exe 被 CF 缓存死过）
 *   4. 停 pm2 → 用 sql.js 改 `app_settings`（version / build_date / changelog /
 *      force_update / download_url）→ 起 pm2
 *      🔴 必须停服改库：sql.js 把整个库读进内存、save() 时整体写回，
 *         运行中改文件会被下一次 save() 覆盖掉。
 *   5. 线上验收：/api/app/version 的 version / downloadUrl / updateEnabled + 官网口径
 *
 * 用法：
 *   node tools/release-client.js --dry-run                    # 只打印计划，不动线上
 *   node tools/release-client.js --changelog-file CHANGELOG.txt
 *   node tools/release-client.js --build --changelog "· 修了 xxx"
 *   node tools/release-client.js --skip-upload --skip-db      # 只验收
 *
 * 🔴 主包选择（--payload / --packed / --suffix）：
 *    2026-10 之前本脚本写死只传 `KunClient-<ver>.exe`（strip 后未加壳 ≈42MB），而文档约定的
 *    交付物是 `KunClient-<ver>-upx.exe`（≈12MB）⇒ **加壳包从来没被发出去过**，线上实测
 *    /assets/KunClient-2.0.6.exe = 42,404,864 B。用户抱怨「文件大」的直接成因就在这。
 *    现在可显式指定：
 *      --packed                      上传 `KunClient-<ver>-upx.exe`
 *      --packed --suffix min         上传 `KunClient-<ver>-min-upx.exe`（精简 Qt + 加壳）
 *      --suffix min（不带 --packed）  上传 `KunClient-<ver>-min.exe`（精简 Qt 未加壳）
 *      --payload <path>              上传指定的任意本地文件
 *    **远端文件名永远保持 `KunClient-<ver>.exe` 不变**：downloadUrl 约定、CF 缓存键、
 *    客户端 isExeUrl 判断，以及服务端 `zipUrl = downloadUrl.replace(/\.exe$/, '.7z')`
 *    全依赖它 —— 只换内容，不换名字。
 *    7z 侧同规则：本地按同后缀找（`KunClient-<ver>[-<suffix>].7z`），**远端仍叫
 *    `KunClient-<ver>.7z`**；否则用户下到的 7z 和 exe 不是同一轮构建。
 *    指定的包找不到就**直接报错**，绝不静默退回 42MB 大包（那正是以前的 bug）。
 *
 * 回滚：把 version / download_url 改回旧值（旧包一直留在 assets 里，不会被删）。
 */
const { Client } = require('ssh2');
const fs = require('fs');
const os = require('os');
const path = require('path');
const https = require('https');
const DEPLOY = require('./deploy-config');

const ROOT = path.resolve(__dirname, '..');
const SRC = path.join(ROOT, 'src', 'cpp');
const DIST = path.join(SRC, 'dist-static');
const KEY = path.join(os.homedir(), '.ssh', 'kunkun_admin_ed25519');
const REMOTE = DEPLOY.remoteBase;
const LIVE = process.env.LIVE_BASE || 'https://kunawa.xyz';
const PM2_APP = process.env.PM2_APP || DEPLOY.pm2Name || 'KunClient-server';

const argv = process.argv.slice(2);
const has = (f) => argv.includes(f);
const val = (f) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : undefined; };
const dryRun = has('--dry-run');
const doBuild = has('--build');
const skipUpload = has('--skip-upload');
const skipDb = has('--skip-db');
const force = !has('--no-force');
const usePacked = has('--packed');
const payloadArg = val('--payload');
const suffix = val('--suffix');

const mb = (bytes) => `${(bytes / 1024 / 1024).toFixed(2)} MB`;

function checkArgs() {
  if (has('--payload') && !payloadArg) throw new Error('--payload 后面必须跟一个文件路径');
  if (has('--suffix') && !suffix) throw new Error('--suffix 后面必须跟一个后缀（如 min）');
  // `--suffix --packed` 这种把开关当值的情况：val() 会原样把 `--packed` 当后缀，
  // 而 `--packed` 恰好能过下面的字符校验 ⇒ 必须单独拦。
  if (payloadArg && payloadArg.startsWith('--')) throw new Error(`--payload 后面像是另一个开关：${payloadArg}`);
  if (suffix && suffix.startsWith('--')) throw new Error(`--suffix 后面像是另一个开关：${suffix}`);
  if (has('--payload') && usePacked) {
    throw new Error('--payload 与 --packed 不能同时用：前者是显式指定，后者按约定推导');
  }
  if (suffix && !/^[A-Za-z0-9._-]+$/.test(suffix)) throw new Error(`--suffix 只能含字母数字 . _ -：${suffix}`);
}

function version() {
  const h = fs.readFileSync(path.join(SRC, 'kc', 'version.h'), 'utf8');
  const m = h.match(/kVersionString\s*=\s*"([0-9.]+)"/);
  if (!m) throw new Error('读不到 kVersionString（kc/version.h）');
  return m[1];
}

function changelog() {
  const f = val('--changelog-file');
  const inline = val('--changelog');
  if (f) return fs.readFileSync(path.resolve(f), 'utf8').trim();
  if (inline) return String(inline).replace(/\\n/g, '\n').trim();
  return '';
}

function connect() {
  const conn = new Client();
  const auth = fs.existsSync(KEY) ? { privateKey: fs.readFileSync(KEY) } : { password: DEPLOY.password };
  return new Promise((resolve, reject) => {
    conn.on('ready', () => resolve(conn)).on('error', reject)
      .connect({ host: DEPLOY.host, port: DEPLOY.port, username: DEPLOY.username, ...auth, readyTimeout: 20000 });
  });
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

function put(sftp, local, remote) {
  return new Promise((resolve, reject) => sftp.fastPut(local, remote, (e) => (e ? reject(e) : resolve())));
}

function get(url) {
  return new Promise((resolve, reject) => {
    https.get(url, (res) => {
      let b = '';
      res.on('data', (d) => { b += d; });
      res.on('end', () => resolve({ status: res.statusCode, body: b }));
    }).on('error', reject);
  });
}

/**
 * 解析「主下载包」及其配套 7z。
 *
 * 🔴 关键约定：**远端文件名恒为 `KunClient-<ver>.exe` / `KunClient-<ver>.7z`**，
 *    只有本地来源文件随 --packed / --payload / --suffix 变化。
 *    原因：服务端 `zipUrl = downloadUrl.replace(/\.exe$/, '.7z')`、官网 Download.tsx 同规则，
 *    CF 缓存键、客户端 isExeUrl 判断也都认 `.exe` 后缀 —— 改远端名会连锁打穿三处。
 */
function resolvePayload(ver) {
  const remoteExe = `KunClient-${ver}.exe`;
  const remoteZip = `KunClient-${ver}.7z`;
  const sfx = suffix ? `-${suffix}` : '';

  let localExe;
  let localExeName;
  let label;
  const warns = [];

  if (payloadArg) {
    localExe = path.resolve(payloadArg);
    localExeName = path.basename(localExe);
    label = `--payload 指定`;
    if (!fs.existsSync(localExe)) {
      throw new Error(`--payload 指定的文件不存在：${localExe}`);
    }
    if (path.dirname(localExe) !== DIST) {
      warns.push(`--payload 不在 dist-static 里：${localExe}（确认是正式构建产物，不是草稿）`);
    }
  } else if (usePacked) {
    localExeName = `KunClient-${ver}${sfx}-upx.exe`;
    localExe = path.join(DIST, localExeName);
    label = `--packed${suffix ? ` --suffix ${suffix}` : ''}`;
  } else {
    localExeName = `KunClient-${ver}${sfx}.exe`;
    localExe = path.join(DIST, localExeName);
    label = suffix ? `--suffix ${suffix}` : '默认';
  }

  // 7z 必须与主包同源（同后缀）。--packed 的 7z 是从未加壳 exe 压的，所以不带 -upx。
  const localZipName = `KunClient-${ver}${sfx}.7z`;
  const localZip = path.join(DIST, localZipName);

  // --payload 只指定 exe，7z 仍按 --suffix 推导 ⇒ 传带后缀的包时必须同时带 --suffix，
  // 否则会拿无后缀的 .7z 配上带后缀的 exe（两者不是同一轮构建）。
  if (payloadArg && !suffix && localExeName !== `KunClient-${ver}.exe`) {
    warns.push(`--payload 传的是「${localExeName}」，但 7z 会按无后缀找 ${localZipName}。` +
      `若该包是后缀产物，请同时加 --suffix（如 --suffix min）`);
  }

  return { remoteExe, remoteZip, localExe, localExeName, localZip, localZipName, label, warns };
}

(async () => {
  checkArgs();

  const ver = version();
  const log = changelog();
  const p = resolvePayload(ver);
  const exeName = p.remoteExe;
  const zipName = p.remoteZip;
  const exePath = p.localExe;
  const zipPath = p.localZip;

  const exeBytes = fs.existsSync(exePath) ? fs.statSync(exePath).size : 0;
  const zipBytes = fs.existsSync(zipPath) ? fs.statSync(zipPath).size : 0;

  console.log(`版本号（kc/version.h）：${ver}`);
  console.log(`主包来源（${p.label}）：${p.localExeName} ${exeBytes ? `✓ ${mb(exeBytes)}` : '✗ 缺失'}`);
  console.log(`7z 来源：${p.localZipName} ${zipBytes ? `✓ ${mb(zipBytes)}` : '✗ 缺失'}`);
  console.log(`远端文件名：${REMOTE}/assets/${exeName}（固定，不随来源变化）`);
  if (exeBytes) {
    console.log(`预计线上体积变化：${mb(exeBytes)}（下载体积以此为准，7z 另算）`);
  }
  for (const w of p.warns) console.log(`⚠️ ${w}`);

  if (!log) console.log('⚠️ 没有传更新日志（--changelog / --changelog-file），线上 changelog 会保持旧值');

  if (doBuild) {
    // 构建模式：build-static.sh 默认产出 UPX；带 --suffix 时把后缀透传（见 task-1 的 CLI）
    const buildArgs = ['--7z'];
    if (suffix) buildArgs.push('--suffix', suffix);
    if (dryRun) console.log(`[dry-run] 会执行 src/cpp/build-static.sh ${buildArgs.join(' ')}`);
    else {
      console.log(`构建中（build-static.sh ${buildArgs.join(' ')}）…`);
      const { execFileSync } = require('child_process');
      execFileSync('bash', [path.join(SRC, 'build-static.sh'), ...buildArgs], { cwd: SRC, stdio: 'inherit' });
    }
  }

  // 显式选了包却找不到 —— 必须硬失败。⚠️ 绝不静默退回 42MB 大包，那正是本次要修的 bug。
  if (!fs.existsSync(exePath)) {
    throw new Error(`找不到主包 ${exePath}（${p.label}）。` +
      `先跑 src/cpp/build-static.sh --7z${suffix ? ` --suffix ${suffix}` : ''}，或加 --build；` +
      `确认参数写对了，别退回默认大包`);
  }
  // 7z 与 exe 必须同轮：默认模式下沿用旧行为（缺了就跳过上传），显式选包模式下缺 7z 视为错误，
  // 否则线上会留着上一轮的 .7z，用户下到的 7z 与 exe 不是同一版。
  const explicitPick = Boolean(payloadArg || usePacked || suffix);
  if (explicitPick && !fs.existsSync(zipPath)) {
    throw new Error(`找不到配套 7z ${zipPath}。zipUrl 由 exe 地址派生，7z 与 exe 必须同轮构建；` +
      `先跑 build-static.sh --7z${suffix ? ` --suffix ${suffix}` : ''}`);
  }

  if (dryRun) {
    console.log('\n[dry-run] 将执行：');
    console.log(`  1. 上传 ${p.localExeName}（${mb(exeBytes)}） → ${REMOTE}/assets/${exeName}`);
    if (fs.existsSync(zipPath)) {
      console.log(`  2. 上传 ${p.localZipName}（${mb(zipBytes)}） → ${REMOTE}/assets/${zipName}`);
    } else {
      console.log(`  2. 跳过 7z（${p.localZipName} 不存在，线上旧 .7z 保持不动）`);
    }
    console.log(`  3. pm2 stop ${PM2_APP} → 改 app_settings（version=${ver}, download_url=/assets/${exeName}, force_update=${force ? 1 : 0}）→ pm2 start`);
    console.log(`  4. 验收 ${LIVE}/api/app/version（含 sizeText 体积核对）`);
    console.log('\n[dry-run] 未连接服务器、未改线上数据。');
    return;
  }

  const conn = await connect();
  console.log(`已连接 ${DEPLOY.username}@${DEPLOY.host}`);

  if (!skipUpload) {
    const sftp = await new Promise((res, rej) => conn.sftp((e, s) => (e ? rej(e) : res(s))));
    // 本地来源名 → 远端固定名：只换内容，不换名字（downloadUrl / zipUrl 派生都靠它）
    for (const [localFile, remoteName] of [[exePath, exeName], [zipPath, zipName]]) {
      if (!fs.existsSync(localFile)) { console.log(`跳过（不存在）：${path.basename(localFile)}`); continue; }
      await put(sftp, localFile, `${REMOTE}/assets/${remoteName}`);
      await run(conn, `chown www:www ${REMOTE}/assets/${remoteName}`);
      console.log(`上传 ${path.basename(localFile)} → assets/${remoteName}（${mb(fs.statSync(localFile).size)}）`);
    }
  }

  if (!skipDb) {
    const payload = {
      version: ver,
      build_date: new Date().toISOString().slice(0, 10),
      changelog: log || undefined,
      force_update: force ? '1' : '0',
      download_url: `/assets/${exeName}`,
    };
    const tmp = path.join(os.tmpdir(), `kc-release-${ver}.json`);
    fs.writeFileSync(tmp, JSON.stringify(payload, null, 2));
    const remoteJson = `/tmp/kc-release-${ver}.json`;
    const sftp = await new Promise((res, rej) => conn.sftp((e, s) => (e ? rej(e) : res(s))));
    await put(sftp, tmp, remoteJson);

    // 改库脚本：必须用服务端自己的 node_modules 里的 sql.js（cwd 设在项目根）
    const script = `
const fs = require('fs');
const initSqlJs = require('sql.js');
const [dbPath, jsonPath] = process.argv.slice(2);
(async () => {
  const patch = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
  const SQL = await initSqlJs();
  const db = new SQL.Database(fs.readFileSync(dbPath));
  const keys = ['version', 'build_date', 'changelog', 'force_update', 'download_url'];
  for (const k of keys) {
    if (patch[k] === undefined) continue;
    db.run('INSERT OR REPLACE INTO app_settings (key, value) VALUES (?, ?)', [k, String(patch[k])]);
  }
  fs.writeFileSync(dbPath, Buffer.from(db.export()));
  const rows = db.exec("SELECT key, value FROM app_settings WHERE key IN ('version','download_url','force_update')");
  console.log('NOW', JSON.stringify(rows[0] ? rows[0].values : []));
  db.close();
})();
`.trim();
    fs.writeFileSync(path.join(os.tmpdir(), 'kc-set-version.js'), script);
    // 🔴 脚本必须落在**项目根目录**里：node 按**脚本所在目录**找 node_modules，
    //    放 /tmp 会 Cannot find module 'sql.js'（2026-10-21 踩过）。
    const remoteScript = `${REMOTE}/.kc-set-version.js`;
    await put(sftp, path.join(os.tmpdir(), 'kc-set-version.js'), remoteScript);

    console.log(`停服 → 改库 → 起服（pm2 ${PM2_APP}）`);
    await run(conn, `pm2 stop ${PM2_APP} >/dev/null 2>&1 || true`);
    try {
      const out = await run(conn, `cd ${REMOTE} && node ${remoteScript} ${REMOTE}/database.sqlite ${remoteJson}`);
      console.log(out.trim());
    } finally {
      await run(conn, `pm2 start ${PM2_APP} >/dev/null 2>&1 || pm2 restart ${PM2_APP} >/dev/null 2>&1 || true`);
      await run(conn, `rm -f ${remoteJson} ${remoteScript}`);
    }
    await new Promise((r) => setTimeout(r, 3000));
  }

  // 线上验收
  const v = JSON.parse((await get(`${LIVE}/api/app/version`)).body).data;
  console.log(`\n线上：version=${v.version} force=${v.forceUpdate} updateEnabled=${v.updateEnabled}`);
  console.log(`      downloadUrl=${v.downloadUrl}  zip=${v.zipUrl}`);
  console.log(`      体积 sizeText=${v.sizeText || '(空)'}`);
  console.log(`      changelog ${String(v.changelog || '').split('\n').length} 行`);
  if (v.version !== ver) throw new Error(`版本号没生效（线上 ${v.version} ≠ ${ver}）`);
  if (v.downloadUrl && !v.downloadUrl.endsWith(exeName)) throw new Error(`downloadUrl 不是新包：${v.downloadUrl}`);

  // 🔴 体积核对：sizeText 是服务端对 /assets 上那个文件 stat 出来的，等于「用户实际下载的体积」。
  //    这条断言就是防止「又传成 42MB 未加壳包」——本地发的是哪个包，线上就该是多大。
  if (v.sizeText) {
    const localMb = exeBytes / 1024 / 1024;
    const got = parseFloat(String(v.sizeText));
    console.log(`      本地主包 ${exeBytes} B（${mb(exeBytes)}） → 线上 sizeText ${v.sizeText}`);
    if (Number.isFinite(got) && Math.abs(got - localMb) > 0.15) {
      throw new Error(`线上体积与本地包不一致：sizeText=${v.sizeText}，本地 ${localMb.toFixed(2)} MB` +
        `（可能又传成了未加壳大包，或 CF 缓存未过期）`);
    }
  } else {
    console.log('      ⚠️ 线上没有 sizeText（updateEnabled=false 或文件 stat 失败），无法自动核对体积');
  }

  if (v.downloadUrl) {
    const head = await new Promise((resolve) => {
      https.request(v.downloadUrl.startsWith('http') ? v.downloadUrl : LIVE + v.downloadUrl, { method: 'HEAD' }, (r) => resolve({ code: r.statusCode, len: r.headers['content-length'] })).on('error', () => resolve({ code: 0 })).end();
    });
    console.log(`      包直链 ${head.code}${head.len ? `（content-length ${head.len} B）` : ''}`);
    if (head.code !== 200) throw new Error('安装包直链不可用');
    // CF 缓存 /assets/*.exe 4h（见 MEMORY）—— 体积对不上多半是缓存还没过期，别误判成发版失败。
    if (head.len && Math.abs(Number(head.len) - exeBytes) > 1024) {
      console.log(`      ⚠️ 直链 content-length ${head.len} B ≠ 本地 ${exeBytes} B，可能是 CF 缓存（4h）还没过期，稍后复核`);
    }
  }
  conn.end();
  console.log('\n✅ 发版完成');
  // 🔴 必须显式退出：ssh2 的 conn.end() 不保证清掉内部 keepalive 定时器，
  //    进程会「活干完了但一直不退」（2026-10-21 两次都卡到超时才被杀）。
  process.exit(0);
})().catch((e) => { console.error('❌ 发版失败：', e.message); process.exit(1); });
