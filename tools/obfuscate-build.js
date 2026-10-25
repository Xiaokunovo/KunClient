#!/usr/bin/env node
/**
 * KunClient 混淆构建（Obfuscar + 单文件打包）
 *
 * 用法：
 *   node tools/obfuscate-build.js            # 混淆构建 → publish_obf/KunClient.exe
 *   node tools/obfuscate-build.js --check    # 只检查 publish_obf 的产物特征
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 为什么不能「混淆 bin 再 publish --no-build」（本次踩了半小时）
 * ─────────────────────────────────────────────────────────────────────────────
 * 直觉上 publish 会打包 `bin/` 里的程序集。**错。** 用 MSBuild 自己吐出来的清单取证：
 *
 *   dotnet publish … --no-build --getItem:FilesToBundle
 *     KunClient.dll       <- obj\Release\net10.0-windows\win-x64\KunClient.dll   （obj！）
 *     KunClient.Core.dll  <- src\KunClient.Core\bin\Release\net10.0-windows\…     （被引用项目的 bin）
 *
 * 所以只改 `App/bin` 的话，打包出来的 exe **md5 与未混淆版一模一样**，
 * 而且不会有任何报错——最坏的一种失败：看着成功、实际没混淆。
 * 结论：**混淆前必须先问 MSBuild 要 FilesToBundle，只改清单里列出的那几个文件。**
 *
 * 另外单文件包是 publish 阶段在 bin 之外现捆的，所以顺序必须是
 * 「编译 → 混淆（改清单里的文件）→ publish --no-build」，且 publish 之后**立刻还原**，
 * 否则 obj/bin 里会留着混淆产物，下一次普通发布会静默带上它（或被混淆两次）。
 */
'use strict';

const { execFileSync } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = path.resolve(__dirname, '..');
// ⚠️ 源码在 `tests/src/`（KunFlashTool.slnx 也是这么引的），**不是** `src/`。
//    `src/` 下只剩一个空的 KunClient.App/obj 残留目录 —— 路径写成 `src/`
//    会去构建一个不存在的工程（或产出与本次改动无关的旧包）。
const PROJECT_DIR = path.join(ROOT, 'tests', 'src', 'KunClient.App');
const CSPROJ = path.join(PROJECT_DIR, 'KunClient.App.csproj');
const BIN = path.join(PROJECT_DIR, 'bin', 'Release', 'net10.0-windows', 'win-x64');
const OBFUSCAR = path.join(ROOT, 'tools', 'dotnet-tools', 'obfuscar.console.exe');
const CONFIG_TPL = path.join(ROOT, 'tools', 'obfuscate.config.xml');
const PUBDIR = path.join(ROOT, 'publish_obf');
const TMP = path.join(ROOT, '.workbuddy', 'tmp', 'obf');
const EXE = path.join(PUBDIR, 'KunClient.exe');
/** Mapping.txt 的留存位置（每次构建覆盖）。事后排查「哪个成员被改成了什么」只看它。 */
let MAPPING_OUT = null;

/** 自家需要混淆的程序集（其余第三方程序集只作为引用，不参与重命名） */
const MODULE_NAMES = ['KunClient.dll', 'KunClient.Core.dll'];

// ════════════════════════════════════════════════════════════════════════════
// 内存加载壳（#97）：真正被发布的**入口程序集**
//   把上面两个程序集以**密文**内嵌，运行期解密后 Assembly.Load(byte[])，
//   于是静态提取工具（dotnet-bundle 解包器、binwalk 式字节雕刻）抠不出可加载的程序集。
//
//   为什么只能靠它拿到「加密」这一条收益：
//     单文件包**本来就**把托管程序集从内存加载、不落盘
//     （DOTNET_BUNDLE_EXTRACT_BASE_DIR 实测为空）→「防 dll 落盘替换」不成立；
//     但 EnableCompressionInSingleFile 被禁（NETSDK1176），KunClient.dll 的字节**原样**
//     躺在 exe 里，能被直接 carve 出来。加密解决的问题只有这一个。
// ════════════════════════════════════════════════════════════════════════════
const SHELL_PROJECT_DIR = path.join(ROOT, 'tests', 'src', 'KunClient.Shell');
const SHELL_CSPROJ = path.join(SHELL_PROJECT_DIR, 'KunClient.Shell.csproj');
const SHELL_LOADER = 'KunClientLoader.exe';              // AssemblyName=KunClientLoader 的产物名
const PAYLOAD_DIR = path.join(TMP, 'payload');           // 加密载荷（放临时目录，不污染仓库）
/** 壳程序集的编译产物目录 / 文件名 / 专用混淆配置（只加密字符串、不重命名）。 */
const SHELL_BIN = path.join(SHELL_PROJECT_DIR, 'bin', 'Release', 'net10.0-windows', 'win-x64');
const SHELL_MODULE = 'KunClientLoader.dll';
const SHELL_CONFIG_TPL = path.join(ROOT, 'tools', 'obfuscate-shell.config.xml');

/**
 * 载荷加密：AES-256-CBC。密钥 = SHA-256(散落片段拼合)，IV = SHA-256(key) 的前 16 字节。
 * **必须与 `src/KunClient.Shell/ShellMain.cs` 的 KeyFragments / EnsureKey() 严格一致**，
 * 两边一起改 —— 不一致的表现是运行期 BadImageFormatException（编译期毫无征兆）。
 *
 * 🔴 2026-09-30 之前这里是 `XOR('KunClient-…-Payload-Key-v1')` 那种逐字节异或，
 *    而这句密钥会**原样**留在 KunClientLoader.exe 里（Shell 项目不走 Obfuscar）
 *    → `strings` 一次拿到密钥，XOR 直接可逆 → 那层壳对会写脚本的人等于没有。
 *    现在把密钥拆成散落片段再派生，静态串扫不再给出完整密钥；算法也从 XOR 换成 AES。
 *    效果是门槛从「strings」抬到「必须读 IL 还原派生逻辑」。
 *
 * ⚠️ 别高估：这是**混淆**不是密码学保护。客户端必须自带密钥，这在密码学上无解 ——
 *    有调试能力的人照样能运行期 dump 出解过的程序集。它挡的是自动化抠取，不是人。
 */
// 片段**故意乱序存放**（2026-09-30 二次增强）：真实拼合顺序由 FRAG_ORDER 索引表给出。
// 只拆成片段还不够 —— 顺序若是自然顺序，反编译时照着数组从上往下抄一遍就拼出来了。
// 必须与 `src/KunClient.Shell/ShellMain.cs` 的 FragStore / FragOrder **逐项一致**。
// ⚠️ 拼出来的种子字符串一旦变了，载荷全变 → 两侧必须同时改，否则运行期摘要校验失败。
const FRAG_STORE = ['ell', '-20', 'Kun', '-Pay', 'Sh', 'Cli', '26-', 'ent', 'load', '-v2', 'Edl'];
const FRAG_ORDER = [2, 5, 7, 1, 6, 10, 4, 0, 3, 8, 9];
const sha256 = (b) => crypto.createHash('sha256').update(b).digest();
const PAYLOAD_SEED = FRAG_ORDER.map((i) => FRAG_STORE[i]).join('');        // = "KunClient-2026-EdlShell-Payload-v2"
const PAYLOAD_KEY = sha256(Buffer.from(PAYLOAD_SEED, 'utf8'));             // 32 字节
const PAYLOAD_IV = sha256(PAYLOAD_KEY).subarray(0, 16);                    // IV 由 key 派生，不必存进载荷


/** 混淆成功判据：这些明文串不该再出现（HideStrings 会把 ldstr 换成解密调用） */
const LEAK_PROBES = [
  '/api/app/check', '/api/app/consume', '/api/app/limits', '离线状态下不能放行', '今日额度已用完',
  // ── 2026-10-09 加固新增：以下都曾是"能被静态提取"的实证样本 ──────────────
  // 判定口径：**要么被加密，要么根本不存在**。命中任何一条都说明加固回退了。
  'Kun-Firehose-2026',                  // 旧挑战盐：曾是 const string（落常量表，HideStrings 碰不到），必须已消失
  'Kun-Firehose-Env2027',               // 新挑战盐的完整串：片段化后不该以完整形式出现
  'KunClient-EnvReport-2027-SignKey',   // 新 seed 的拼合结果：同上
  'KunClient-EnvReport-2026-Key',       // 旧 seed 的拼合结果
  'ultra.xiaonan.qzz.io',               // 第三方 FDL 端点：曾是 const string
  'https://kunawa.cn',                  // 默认域名：迁出内嵌 config 后，仅以"片段拼接的 ldstr"存在
  'KUN_ALLOW_DEBUG',                    // 反调试后门名：源码/产物都不该有明文（运行期拼码）
];

/**
 * 「必须留下」的探针。⚠️ 只对 **UTF-8（#Strings 堆）** 有意义。
 * .NET 的命名空间 / 类型名在元数据的 #Strings 堆里是 **UTF-8**，不是 UTF-16——
 * 这条扫错会在混淆完全正常时也报 0 命中（我第一版就写成了 UTF-16 扫描，白高兴一场）。
 *
 * 名字必须是**完整类型全名**（含命名空间）：App 的命名空间是 `KunFlashTool.App`
 * 而不是 `KunClient.App`（后者是程序集名）。写错会得到一条假的「被改名」告警。
 */
const KEEP_ASCII = [
  'KunFlashTool',
  'KunFlashTool.App.Pages.EdlFlashPage',
  // ⚠️ 2026-10-09 移除 `SimpleDisplayName`：它属于 `Core/Transport/EdlPortInfo.cs`，
  //    而那个类随 EDL 页重写已被删除（全工程已无引用）→ 探针恒失败。
  //    同类探针只在「有 XAML 按**名字**引用成员」时才需要（DisplayMemberPath / Binding Path=）；
  //    当前工程已无这种绑定（EDL 页改用 Binding IsSelected/Name/… 这类强类型绑定，
  //    靠 RenameProperties=false 保护）。将来若新增按名字绑定的成员，把那个名字加到本数组。
];

/**
 * 「预期仍是明文」的样本，**只提示不判失败**。
 * 这些都是 `const string`：值存在元数据 Constant 表里（#Blob 堆，UTF-16 内联），
 * HideStrings 只处理方法体的 ldstr，碰不到 Constant 表。
 * 已核对：全部 `const string` 里没有任何密钥，只有配置文件名、ADB 命令、UI 文案，
 * 所以这里不需要为了加密而把 const 改成 static readonly。
 */
const EXPECTED_PLAINTEXT = ['KunClient.config.json'];

/** 必须被加密的 C# 字符串字面量（在 #US 堆里是 UTF-16LE）：这些都写在方法体里。 */
const ENCRYPT_SAMPLE = ['今日额度已用完'];

const log = (...a) => console.log('[obf]', ...a);

function run(cmd, args, opts = {}) {
  log('$', cmd, args.join(' '));
  return execFileSync(cmd, args, { stdio: 'inherit', cwd: ROOT, ...opts });
}

function runCapture(cmd, args) {
  log('$', cmd, args.join(' '), '(capture)');
  return execFileSync(cmd, args, { cwd: ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
}

function rmrf(p) {
  if (!fs.existsSync(p)) return;
  try {
    fs.rmSync(p, { recursive: true, force: true });
  } catch (e) {
    // 本机沙箱的 safe-delete 会按「每轮累计删除条数」拦截，递归删暂存目录很容易超阈值。
    // 这不是脚本的问题，但原始堆栈看着像脚本崩了 —— 换成可操作的提示。
    if (String(e && e.message).indexOf('SAFE_DELETE') >= 0) {
      console.error('!! 删除 ' + p + ' 被本机 safe-delete 拦下（宿主机安全策略，非脚本问题）。');
      console.error('   加环境变量重跑：CODEBUDDY_SAFE_DELETE_ENABLED=0 node tools/obfuscate-build.js');
      process.exit(6);
    }
    throw e;
  }
}
function md5(p) { return crypto.createHash('md5').update(fs.readFileSync(p)).digest('hex'); }

/** 按字节计数（不是行数）：.NET 字符串字面量在 #US 堆里是 UTF-16LE。 */
function countUtf16(file, s) {
  const buf = fs.readFileSync(file);
  const needle = Buffer.from(s, 'utf16le');
  let n = 0, i = 0;
  while ((i = buf.indexOf(needle, i)) !== -1) { n++; i += 2; }
  return n;
}

/** 同上，但扫 UTF-8：命名空间/类型名/成员名在 #Strings 堆里是 UTF-8。 */
function countAscii(file, s) {
  const buf = fs.readFileSync(file);
  const needle = Buffer.from(s, 'ascii');
  let n = 0, i = 0;
  while ((i = buf.indexOf(needle, i)) !== -1) { n++; i += 1; }
  return n;
}

/** 项目里全部 `x:Class="…"`（即所有 XAML 视图/窗口/控件类）。 */
function collectXamlClasses() {
  const root = PROJECT_DIR;
  const out = new Set();
  const walk = dir => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) { if (e.name !== 'obj' && e.name !== 'bin') walk(p); continue; }
      if (!e.name.endsWith('.xaml')) continue;
      const m = /x:Class="([^"]+)"/.exec(fs.readFileSync(p, 'utf8'));
      if (m) out.add(m[1]);
    }
  };
  walk(root);
  return out;
}

/**
 * 解析 Mapping.txt，取出「被重命名的类型」全名集合。
 * 格式：`[KunClient]KunFlashTool.App.Pages.EdlFlashPage/<闭包>d__9 -> [KunClient]A.B`
 */
function parseRenamedTypes(mappingFile) {
  const renamed = new Set();
  for (const line of fs.readFileSync(mappingFile, 'utf8').split(/\r?\n/)) {
    const i = line.indexOf(' -> ');
    if (i < 0 || !line.startsWith('[')) continue;
    renamed.add(line.slice(0, i).replace(/^\[[^\]]*\]/, '').trim());
  }
  return renamed;
}

/**
 * 断言「没有任何 XAML 视图类被改名」。
 * 这是本工程混淆安全的**核心不变量**：BAML 的 x:Class 是按类型名字符串解析的，
 * 改了就抛异常、页面直接打不开，而冒烟只开登录窗根本测不到另外 30 个视图。
 */
function assertXamlClassesKept(mappingFile) {
  const classes = collectXamlClasses();
  if (classes.size === 0) {
    console.error('!! 一个 x:Class 都没扫到，说明扫描逻辑坏了（不是「没问题」）');
    process.exit(3);
  }
  const renamed = parseRenamedTypes(mappingFile);
  const bad = [];
  for (const c of classes) {
    // 类型本身被改，或它的外层类型被改，x:Class 都会解析失败
    if (renamed.has(c) || [...renamed].some(r => c.startsWith(r + '/') || c.startsWith(r + '+'))) {
      bad.push(c);
    }
  }
  log(`x:Class 断言：${classes.size} 个视图类，被改名 ${bad.length} 个`);
  if (bad.length) {
    console.error('!! 这些 XAML 视图类被混淆器改名了，BAML 会解析失败：\n   ' + bad.join('\n   '));
    console.error('   修法：确认 AnalyzeXaml=true（会把 XAML 引用到的类型自动跳过）。');
    process.exit(3);
  }
}

/**
 * WPF / BCL 共享框架目录。框架依赖产物里没有 PresentationFramework，Obfuscar 解析引用时要能找到。
 * 版本目录按版本号数值排序取最新（别按字符串：10.0.10 要排在 10.0.2 之后）。
 */
function frameworkDirs() {
  const base = path.join(process.env.ProgramFiles || 'C:\\Program Files', 'dotnet', 'shared');
  const out = [];
  for (const name of ['Microsoft.WindowsDesktop.App', 'Microsoft.NETCore.App']) {
    const dir = path.join(base, name);
    if (!fs.existsSync(dir)) continue;
    const vers = fs.readdirSync(dir).filter(v => /^\d+(\.\d+)*$/.test(v)).sort((a, b) => {
      const pa = a.split('.').map(Number), pb = b.split('.').map(Number);
      for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
        const d = (pa[i] || 0) - (pb[i] || 0);
        if (d) return d;
      }
      return 0;
    });
    if (vers.length) out.push(path.join(dir, vers[vers.length - 1]));
  }
  return out;
}

/** 问 MSBuild 要打包清单，从中取出自家程序集的真实路径（相对路径按项目目录解析）。 */
function bundleSources(probeDir) {
  rmrf(probeDir);
  let raw = '';
  try {
    raw = runCapture('dotnet', ['publish', CSPROJ, '-c', 'Release', '-r', 'win-x64',
      '--no-build', '-o', probeDir, '--getItem:FilesToBundle']);
  } catch (e) {
    raw = (e.stdout || '') + (e.stderr || '');
  }
  const json = raw.slice(raw.indexOf('{'));
  let doc;
  try { doc = JSON.parse(json); } catch {
    console.error('!! 解析 FilesToBundle 失败，原始输出前 800 字：\n' + raw.slice(0, 800));
    process.exit(2);
  }
  const found = {};
  const visit = (node, key) => {
    if (Array.isArray(node)) { node.forEach(v => visit(v, key)); return; }
    if (node && typeof node === 'object') {
      const id = node.Identity;
      if (typeof id === 'string') {
        const base = path.basename(id);
        if (MODULE_NAMES.includes(base) && !found[base]) {
          found[base] = path.isAbsolute(id) ? id : path.resolve(PROJECT_DIR, id);
        }
      }
      Object.keys(node).forEach(k => visit(node[k], k));
    }
  };
  visit(doc, null);
  return found;
}

function featureReport(file) {
  const o = {};
  for (const s of LEAK_PROBES) o['leak:' + s] = countUtf16(file, s);
  for (const s of KEEP_ASCII) o['keep:' + s] = countAscii(file, s);
  for (const s of ENCRYPT_SAMPLE) o['cipher:' + s] = countUtf16(file, s);
  for (const s of EXPECTED_PLAINTEXT) o['const:' + s] = countUtf16(file, s);
  return o;
}

// ════════════════════════════════════════════════════════════════════════════
// 内存加载壳：载荷生成 / 进包断言
// ════════════════════════════════════════════════════════════════════════════

/**
 * 明文程序集 → 载荷字节：**先 deflate 压缩，再 AES-256-CBC 加密**。
 *
 * 为什么必须压缩：压缩前的载荷就是原样大小（KunClient 995 KB + Core 312 KB），
 * 而单文件包本身是**不压缩**的（EnableCompressionInSingleFile 被禁），
 * 于是壳版比纯混淆版凭空大了 470 KB。deflate 后只剩约一半。
 *
 * 顺序不能反：先压缩再加密（加密后是伪随机数据，压不动）。
 * C# 侧对应 `AES 解密 → DeflateStream 解压` —— 两边必须一起改。
 */
function payloadBytes(plain) {
  const c = crypto.createCipheriv('aes-256-cbc', PAYLOAD_KEY, PAYLOAD_IV);
  return Buffer.concat([c.update(zlib.deflateRawSync(plain)), c.final()]);
}

/** 逆过程，仅供**自检**：AES 解密 → inflate。必须与 C# 的 Decrypt() 同构。 */
function payloadPlain(enc) {
  const d = crypto.createDecipheriv('aes-256-cbc', PAYLOAD_KEY, PAYLOAD_IV);
  return zlib.inflateRawSync(Buffer.concat([d.update(enc), d.final()]));
}

/** 生成壳用的加密载荷：取混淆产物 → deflate → AES-256-CBC → 写 .enc（含「解回来一致」+ PE 魔数自检）。 */
function writeShellPayloads() {
  rmrf(PAYLOAD_DIR);
  fs.mkdirSync(PAYLOAD_DIR, { recursive: true });
  for (const n of MODULE_NAMES) {
    const src = path.join(TMP, 'out', n);
    if (!fs.existsSync(src)) { console.error('!! 找不到混淆产物 ' + src); process.exit(3); }
    const plain = fs.readFileSync(src);
    const enc = payloadBytes(plain);
    // 自检：走一遍完整逆过程（AES 解密 → inflate），必须逐字节一致。
    // 不查的话，密钥/算法/IV 写错会变成「运行期 Assembly.Load 报 BadImageFormat」，很难往回追。
    const back = payloadPlain(enc);
    if (!back.equals(plain)) {
      console.error('!! ' + n + ' 载荷往返自检失败（AES+deflate 本该可逆，绝不该发生）');
      process.exit(3);
    }
    // 再加一道：解出来必须是 PE 映像。ShellMain.cs 里也有这道校验（两者合起来才能在
    // 「构建端算错」和「运行端被篡改」两种情况下都及时报错）。
    if (back.length < 2 || back[0] !== 0x4d || back[1] !== 0x5a) {
      console.error('!! ' + n + ' 解出来的不是 PE 映像（缺 MZ 魔数）');
      process.exit(3);
    }
    // 摘要自检：写下去的 .sha 与明文重新算出来的必须一致。
    // 这条把「构建端」和「运行端」两侧夹住 —— 壳里那道校验一旦对不上，
    // 至少能确定是壳的问题而不是载荷的问题。
    const digest = sha256(plain);
    if (!digest.equals(sha256(back))) {
      console.error('!! ' + n + ' 摘要自检失败（往返后字节不一致）');
      process.exit(3);
    }
    fs.writeFileSync(path.join(PAYLOAD_DIR, n + '.enc'), enc);
    log('载荷:', n + '.enc', enc.length, 'bytes（明文 ' + plain.length + '，压缩率 '
        + (100 * enc.length / plain.length).toFixed(1) + '%）');

    // 摘要（2026-09-30 增强）：明文的 SHA-256，内嵌进壳，运行期解密后逐字节比对。
    // 它**不是**为了保密（摘要在包里是可见的，要改的人连摘要一起改就行），
    // 而是为了把「密钥算错 / 载荷坏了 / 被替换」和「程序本身有 bug」三类故障分开 ——
    // 上次换算法时只能靠 'MZ' 两个字节猜，最难排查的就是这一类。
    fs.writeFileSync(path.join(PAYLOAD_DIR, n + '.sha'), sha256(plain));
  }
}

/**
 * 硬断言：壳的**打包清单里不许出现** KunClient.dll / KunClient.Core.dll。
 *
 * 这是整个 #97 的命门。它俩一旦以明文进包，加密载荷完全白做 ——
 * 而且**不会有任何报错**：程序照样跑得好好的，只是密文旁边多躺了一份明文。
 * 触发条件通常是有人把壳 csproj 的 `ReferenceOutputAssembly="false"` 改回了普通引用。
 */
function assertShellExcludesModules() {
  let raw = '';
  try {
    raw = runCapture('dotnet', ['publish', SHELL_CSPROJ, '-c', 'Release', '-r', 'win-x64',
      '--no-build', '-o', path.join(TMP, 'shellprobe'),
      '-p:ShellPayloadDir=' + PAYLOAD_DIR, '--getItem:FilesToBundle']);
  } catch (e) { raw = (e.stdout || '') + (e.stderr || ''); }

  const names = [];
  try {
    const doc = JSON.parse(raw.slice(raw.indexOf('{')));
    const visit = node => {
      if (Array.isArray(node)) return node.forEach(visit);
      if (node && typeof node === 'object') {
        if (typeof node.Identity === 'string') names.push(path.basename(node.Identity));
        Object.keys(node).forEach(k => visit(node[k]));
      }
    };
    visit(doc);
  } catch {
    console.error('!! 拿不到壳的打包清单 → 无法断言「明文程序集没进包」。宁可不发也不冒险，放弃。');
    console.error(raw.slice(0, 600));
    process.exit(7);
  }

  const bad = MODULE_NAMES.filter(n => names.includes(n));
  if (bad.length) {
    console.error('!! 壳的打包清单里出现了: ' + bad.join(', '));
    console.error('   → 加密载荷白做了（明文程序集也在包里）。检查 KunClient.Shell.csproj 的');
    console.error('     ProjectReference 是否仍带 ReferenceOutputAssembly="false"。');
    process.exit(7);
  }
  log('壳进包断言:', MODULE_NAMES.join(' / '), '都不在包里 ✓（只以密文载荷形式存在）');
}

/**
 * 最终产物的**双向**字节断言：
 *   ① 混淆后 KunClient.dll 的**明文字节**不许出现在 exe 里（否则等于没加密）
 *   ② 对应的**密文字节**必须出现在 exe 里（否则载荷根本没进包，或密钥不一致）
 * 取 DLL 中段 512 字节做窗口 —— 足够独特，误命中的概率可以忽略。
 */
function assertPayloadInExe(exe) {
  const hay = fs.readFileSync(exe);
  const off = 0x800, len = 512;
  for (const n of MODULE_NAMES) {
    const src = path.join(TMP, 'out', n);
    if (!fs.existsSync(src)) { log('（跳过载荷断言：找不到 ' + src + '）'); continue; }
    const plain = fs.readFileSync(src);
    if (plain.length < off + len) continue;
    const wPlain = plain.subarray(off, off + len);
    const wEnc = payloadBytes(plain).subarray(off, off + len);

    if (hay.indexOf(wPlain) >= 0) {
      console.error('!! ' + n + ' 的**明文字节**出现在 exe 里 → 载荷没生效，或它被明文打包进去了');
      process.exit(8);
    }
    if (hay.indexOf(wEnc) < 0) {
      console.error('!! 在 exe 里找不到 ' + n + ' 的**密文字节** → 载荷没进包（或两边密钥不一致）');
      process.exit(8);
    }
    log('载荷双向断言:', n, '明文不在 exe ✓ / 密文已在 exe ✓');
  }
}

/** 壳混淆后：这些明文串必须消失（都是方法体里的 ldstr）。 */
const SHELL_LEAK_PROBES = ['-Pay', 'Edl', 'megadumper', '检测到调试器正在附加到本进程'];
/** 壳混淆后：这些名字必须原样保留（入口点 / 反射依赖 / 事后排查要用）。 */
const SHELL_KEEP_NAMES = ['ShellMain', 'KunFlashTool.Shell', 'BackdoorName', 'FragStore'];

function assertShellStringsEncrypted(file) {
  const bad = SHELL_LEAK_PROBES.filter(s => countUtf16(file, s) > 0);
  if (bad.length) {
    console.error('!! 壳混淆后仍有明文串:', bad.join(', '), '→ HideStrings 没跑到位');
    process.exit(4);
  }
  log('壳字符串加密断言:', SHELL_LEAK_PROBES.length, '条探针全部无命中 ✓');
}

function assertShellNamesKept(file) {
  const missing = SHELL_KEEP_NAMES.filter(s => countAscii(file, s) === 0);
  if (missing.length) {
    console.error('!! 壳里这些名字不见了:', missing.join(', '));
    console.error('   → 壳被重命名了（入口点 StartupObject / Assembly.GetType 反射会崩）。');
    console.error('   检查 obfuscate-shell.config.xml 的 MarkedOnly=true 与 HidePrivateApi=false。');
    process.exit(4);
  }
  log('壳名字保留断言:', SHELL_KEEP_NAMES.length, '项全部在位 ✓');
}

/**
 * 🔴 最终 exe 的壳断言 —— 这条是补上「混淆了 A、发出去了 B」这个漏洞的。
 *
 * 2026-10-09 实测：壳的混淆产物**确实是加密的**（对 outDir 的断言全过），但发布取的是
 * obj 里那份未混淆的 dll → 扫最终 exe 仍能拿到 FragStore 片段与整个工具名单。
 * 教训：**断言必须打在「用户真正拿到的东西」上**，只验中间产物等于没验。
 */
const SHELL_EXE_LEAK_PROBES = ['-Pay', 'megadumper', 'cheatengine', 'extremedumper', '检测到调试器正在附加到本进程'];

function assertShellStringsInExe(exe) {
  const bad = SHELL_EXE_LEAK_PROBES.filter(s => countUtf16(exe, s) > 0);
  if (bad.length) {
    console.error('!! 最终 exe 里仍能扫到壳的明文串:', bad.join(', '));
    console.error('   → 壳的字符串加密没进包。检查 obfuscateShell() 混淆的是不是');
    console.error('     MSBuild 清单里那一份（发布取的是 obj\\…\\KunClientLoader.dll，不是 bin 里那份）。');
    process.exit(8);
  }
  log('壳进包断言:', SHELL_EXE_LEAK_PROBES.length, '条壳明文探针在 exe 里全部无命中 ✓');
}

/**
 * 问 MSBuild 要**壳自己**的 dll 真实路径。
 *
 * 🔴 为什么必须问、不能写死 `bin/…/KunClientLoader.dll`（2026-10-09 实测踩到）：
 *    单文件发布对**当前项目自身输出**取的是 `obj\Release\…\KunClientLoader.dll`（IntermediateAssembly），
 *    **不是** `bin` 里那份 —— 两处是 MSBuild 复制出来的两个独立文件。
 *    只混淆 bin 的后果：混淆"成功"（产物确实是加密的）、断言也全过、发布也成功，
 *    但**打进 exe 的是 obj 里那份未混淆的**。实测扫最终 exe 仍然能拿到
 *    FragStore 片段 `-Pay` 与整个 DumpToolProcesses 名单 —— 壳的字符串加密等于没做。
 *    这正是本文件开头那段注释警告过的坑（App 那边已经用 bundleSources 绕开了），
 *    壳这边当初图省事写死了 bin，就中招了。
 */
function shellSourcePath() {
  const probeDir = path.join(TMP, 'shellsrcprobe');
  rmrf(probeDir);
  let raw = '';
  try {
    raw = runCapture('dotnet', ['publish', SHELL_CSPROJ, '-c', 'Release', '-r', 'win-x64',
      '--no-build', '-o', probeDir, '-p:ShellPayloadDir=' + PAYLOAD_DIR, '--getItem:FilesToBundle']);
  } catch (e) { raw = (e.stdout || '') + (e.stderr || ''); }

  let doc;
  try { doc = JSON.parse(raw.slice(raw.indexOf('{'))); }
  catch {
    console.error('!! 拿不到壳的打包清单，无法确定该混淆哪个文件。宁可不发也不冒险，放弃。');
    console.error(raw.slice(0, 600));
    process.exit(7);
  }

  let found = null;
  const visit = node => {
    if (Array.isArray(node)) return node.forEach(visit);
    if (node && typeof node === 'object') {
      const id = node.Identity;
      if (typeof id === 'string' && path.basename(id) === SHELL_MODULE && !found) {
        found = path.isAbsolute(id) ? id : path.resolve(SHELL_PROJECT_DIR, id);
      }
      Object.keys(node).forEach(k => visit(node[k]));
    }
  };
  visit(doc);
  return found;
}

/**
 * 混淆壳程序集 —— **只加密字符串，不重命名任何名字**。
 *
 * 为什么不能并进上面那次 Obfuscar：壳是**入口程序集**（StartupObject=ShellMain），
 * 还要 `Assembly.GetType("KunFlashTool.App.App")` 反射拉起真正的应用 —— 一个名字都不能动；
 * 而 App/Core 那次用的是「类型名保留、方法名放开」的档位。两者策略相反，只能两份配置。
 * 收益与实测记录见 tools/obfuscate-shell.config.xml 顶部。
 *
 * 返回一个「把原文件还原」的函数；无论中途怎么退出，调用方都要还原
 * （理由同 App：不还原的话下次普通发布会静默带上混淆产物，或把它混淆两次）。
 */
function obfuscateShell() {
  const dll = shellSourcePath();       // 🔴 必须是 MSBuild 清单里的那份（通常是 obj），不是写死的 bin
  if (!dll || !fs.existsSync(dll)) {
    console.error('!! 清单里取不到壳程序集（' + SHELL_MODULE + '），放弃（宁可不混淆也不要混淆错文件）');
    process.exit(4);
  }
  log('MSBuild 说壳程序集来自:', dll);

  const pristine = md5(dll);
  const backup = path.join(TMP, 'shell-pristine.dll');
  fs.copyFileSync(dll, backup);

  const inDir = path.join(TMP, 'shell-in');
  const outDir = path.join(TMP, 'shell-out');
  rmrf(inDir);
  rmrf(outDir);
  fs.mkdirSync(inDir, { recursive: true });
  fs.mkdirSync(outDir, { recursive: true });
  fs.copyFileSync(dll, path.join(inDir, SHELL_MODULE));

  const searchElements = [inDir, SHELL_BIN, ...frameworkDirs()]
    .map(p => '  <AssemblySearchPath path="' + p.replace(/\\/g, '/') + '" />').join('\n');
  const cfg = fs.readFileSync(SHELL_CONFIG_TPL, 'utf8')
    .replace(/@BIN@/g, inDir.replace(/\\/g, '/'))
    .replace(/@OUT@/g, outDir.replace(/\\/g, '/'))
    .replace(/@SEARCH_ELEMENTS@/g, searchElements);
  const cfgPath = path.join(TMP, 'obfuscate-shell.xml');
  fs.writeFileSync(cfgPath, cfg, 'utf8');

  run(OBFUSCAR, [cfgPath]);

  const obf = path.join(outDir, SHELL_MODULE);
  if (!fs.existsSync(obf)) { console.error('!! Obfuscar 没产出混淆后的壳 ' + obf); process.exit(4); }
  if (md5(obf) === pristine) { console.error('!! 壳混淆前后一模一样，说明没真正混淆'); process.exit(4); }

  assertShellStringsEncrypted(obf);
  assertShellNamesKept(obf);

  fs.copyFileSync(obf, dll);
  log('壳已混淆:', SHELL_MODULE, '->', fs.statSync(dll).size, 'bytes');

  return () => {
    if (!fs.existsSync(backup)) return;
    fs.copyFileSync(backup, dll);
    const ok = md5(dll) === pristine;
    log(ok ? '壳已还原为未混淆版本（md5 校验通过）' : '!! 壳还原校验失败（md5 不符）');
  };
}

function check() {
  if (!fs.existsSync(EXE)) { console.error('!! 找不到 ' + EXE + '，先跑一次完整流程'); process.exit(2); }
  console.log('exe     :', EXE);
  console.log('size    :', fs.statSync(EXE).size, 'bytes');
  console.log('md5     :', md5(EXE));

  let bad = false;

  // ① 闸门路径等敏感字面量：exe 里必须一个都搜不到
  const leaks = LEAK_PROBES.filter(s => countUtf16(EXE, s) > 0);
  console.log('泄漏探针:', (LEAK_PROBES.length - leaks.length) + '/' + LEAK_PROBES.length, '已无命中');
  if (leaks.length) {
    console.error('!! 仍有明文串:', leaks.join(', '), '→ 字符串加密没跑到位');
    bad = true;
  }

  // ②「类型名保留」与「字面量已加密」只能对着**加密前**的混淆产物查。
  //    载荷是密文，在 exe 里按字符串查必然 0 命中 —— 那会得到一条**假失败**
  //    （第一版把 KEEP_ASCII 直接扫 exe，白折腾了一轮）。
  const outDll = path.join(TMP, 'out', MODULE_NAMES[0]);
  if (fs.existsSync(outDll)) {
    const rep = featureReport(outDll);
    console.log('--- 混淆产物（加密前）', outDll, '---');
    console.log('字节命中:', JSON.stringify(rep, null, 0));

    // 真正把关「字符串加密有没有生效」的地方在这里 —— exe 里那几条查已经是密文了，
    // 恒为 0 命中，等于没查（削弱检查是隐藏的风险，所以必须补这一条）。
    const leakedInDll = LEAK_PROBES.filter(k => rep['leak:' + k] > 0);
    if (leakedInDll.length) {
      console.error('!! 混淆产物里仍有明文串:', leakedInDll.join(', '), '→ 字符串加密没跑到位');
      bad = true;
    }

    const missing = KEEP_ASCII.filter(k => rep['keep:' + k] === 0);
    if (missing.length) {
      console.error('!! 这些名字在混淆产物里找不到了:', missing.join(', '), '→ 可能被改名了（WPF 会崩）');
      bad = true;
    }
    const notEnc = ENCRYPT_SAMPLE.filter(k => rep['cipher:' + k] > 0);
    if (notEnc.length) {
      console.error('!! 这些写在方法体里的字面量仍是明文，字符串加密没生效:', notEnc.join(', '));
      bad = true;
    }
    const consts = EXPECTED_PLAINTEXT.filter(k => rep['const:' + k] > 0);
    if (consts.length) log('（已知）这些 const string 仍是明文，属预期:', consts.join(', '));

    assertPayloadInExe(EXE);
  } else {
    log('（找不到 ' + outDll + ' → 跳过「类型名/字面量」与载荷断言；跑一次完整流程即可全检）');
  }

  // ③ 壳的字符串加密必须体现在**发布产物**上（不只是混淆产物）
  const shellLeaks = SHELL_EXE_LEAK_PROBES.filter(s => countUtf16(EXE, s) > 0);
  console.log('壳明文探针:', (SHELL_EXE_LEAK_PROBES.length - shellLeaks.length) + '/' + SHELL_EXE_LEAK_PROBES.length, '已无命中');
  if (shellLeaks.length) {
    console.error('!! 最终 exe 里仍有壳明文串:', shellLeaks.join(', '), '→ 壳的字符串加密没进包');
    bad = true;
  }

  if (bad) process.exit(3);
  console.log('CHECK OK：接口路径无明文；类型名保留完整；密文载荷已进包且明文不在包内；壳字符串已加密');
  return 0;
}

/**
 * 源码级前置检查：**禁止用 STJ 反射序列化匿名类型**。
 *
 * 匿名类型只有「带参数的构造函数」（编译器生成的），而 Obfuscar 的 `RenameParams()` 会把
 * 「没有自定义特性的参数」的名字**清成 null**：
 *
 *     foreach (ParameterDefinition parameter in method.Parameters)
 *         if (parameter.CustomAttributes.Count == 0) parameter.Name = null;
 *
 * System.Text.Json 构建 JsonTypeInfo 时要读构造函数参数名，于是抛：
 *     NotSupportedException: The deserialization constructor for type '...'
 *     contains parameters with null names.
 *
 * 最恶的地方是它**编译期毫无征兆**，只在运行期炸，炸的还是登录/注册/额度闸门这些核心通路；
 * 而且闸门那边异常被 `catch { }` 吞掉，表面现象是 `Reason=offline`（看着像网络不通）。
 * 2026-09-25 实测：混淆版一旦上线，客户端连登录都登不上、闸门全挡。
 * 正解：用 `Services/JsonBody.Of(...)`（内部是 JsonObject，走内置转换器，不做构造函数反射）。
 */
function assertNoAnonymousJsonSerialization() {
  const bad = [];
  const walk = dir => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) { if (e.name !== 'obj' && e.name !== 'bin') walk(p); continue; }
      if (!e.name.endsWith('.cs')) continue;
      fs.readFileSync(p, 'utf8').split(/\r?\n/).forEach((line, i) => {
        const t = line.trimStart();
        if (t.startsWith('//')) return;                     // 注释里提到不算
        if (/JsonSerializer\s*\.\s*Serialize\s*\(\s*new\s*\{/.test(line)) {
          bad.push(`${path.relative(ROOT, p)}:${i + 1}`);
        }
      });
    }
  };
  walk(path.join(ROOT, 'tests', 'src'));
  if (bad.length) {
    console.error('!! 这些位置用 STJ 反射序列化了**匿名类型**，混淆后会在运行期抛 NotSupportedException：');
    for (const b of bad) console.error('   ' + b);
    console.error('   改用 Services/JsonBody.Of(...)（原因见 src/KunClient.App/Services/JsonBody.cs）。');
    process.exit(5);
  }
  log('源码前置检查：无 STJ 匿名类型序列化 ✓');
}

function main() {
  if (process.argv.includes('--check')) return check();

  if (!fs.existsSync(OBFUSCAR)) {
    console.error('!! 找不到 Obfuscar：' + OBFUSCAR);
    console.error('   dotnet tool install obfuscar.globaltool --version 2.2.50 --tool-path tools/dotnet-tools');
    process.exit(2);
  }

  assertNoAnonymousJsonSerialization();

  rmrf(TMP);
  for (const d of ['in', 'out', 'backup']) fs.mkdirSync(path.join(TMP, d), { recursive: true });

  // ── 1. 干净编译 ────────────────────────────────────────────────
  run('dotnet', ['build', CSPROJ, '-c', 'Release', '-r', 'win-x64', '-v:m']);

  // ── 2. 问 MSBuild 要真实来源 ───────────────────────────────────
  const sources = bundleSources(path.join(TMP, 'probe'));
  log('MSBuild 说这两个程序集来自：');
  for (const n of MODULE_NAMES) log('   ', n, '<-', sources[n] || '(未在清单里！)');
  for (const n of MODULE_NAMES) {
    if (!sources[n] || !fs.existsSync(sources[n])) {
      console.error('!! 清单里取不到 ' + n + '，放弃（宁可不混淆也不要混淆错文件）');
      process.exit(2);
    }
  }

  // ── 3. 备份 → 混淆（在暂存目录里做，不动原文件） ────────────────
  const pristine = {};
  for (const n of MODULE_NAMES) {
    pristine[n] = md5(sources[n]);
    fs.copyFileSync(sources[n], path.join(TMP, 'backup', n));
    fs.copyFileSync(sources[n], path.join(TMP, 'in', n));
  }
  // 暂存目录里补上其它依赖，方便 Obfuscar 解析引用
  for (const f of fs.readdirSync(BIN)) {
    if (f.endsWith('.dll') && !fs.existsSync(path.join(TMP, 'in', f))) {
      fs.copyFileSync(path.join(BIN, f), path.join(TMP, 'in', f));
    }
  }

  const searchDirs = [path.join(TMP, 'in'), BIN, ...frameworkDirs()];
  const searchElements = searchDirs
    .map(p => '  <AssemblySearchPath path="' + p.replace(/\\/g, '/') + '" />').join('\n');
  const cfg = fs.readFileSync(CONFIG_TPL, 'utf8')
    .replace(/@BIN@/g, path.join(TMP, 'in').replace(/\\/g, '/'))
    .replace(/@OUT@/g, path.join(TMP, 'out').replace(/\\/g, '/'))
    .replace(/@SEARCH_ELEMENTS@/g, searchElements);
  const cfgPath = path.join(TMP, 'obfuscate.xml');
  fs.writeFileSync(cfgPath, cfg, 'utf8');

  let restored = false;
  const restore = () => {
    if (restored) return;
    restored = true;
    for (const n of MODULE_NAMES) {
      const b = path.join(TMP, 'backup', n);
      if (fs.existsSync(b)) fs.copyFileSync(b, sources[n]);
    }
    const bad = MODULE_NAMES.filter(n => md5(sources[n]) !== pristine[n]);
    log(bad.length ? '!! 还原校验失败: ' + bad.join(', ') : '已还原为未混淆版本（md5 校验通过）');
  };

  try {
    run(OBFUSCAR, [cfgPath]);
    for (const n of MODULE_NAMES) {
      const o = path.join(TMP, 'out', n);
      if (!fs.existsSync(o)) { console.error('!! Obfuscar 没产出 ' + o); process.exit(3); }
      if (md5(o) === pristine[n]) { console.error('!! ' + n + ' 混淆前后一模一样，说明没真正混淆'); process.exit(3); }
      fs.copyFileSync(o, sources[n]);
    }
    const leaks = LEAK_PROBES.filter(s => countUtf16(path.join(TMP, 'out', MODULE_NAMES[0]), s) > 0);
    if (leaks.length) log('提示：这些串仍留在程序集里（通常是 const，值在 Constant 表里被 HideStrings 漏掉）:', leaks.join(', '));

    // ── 3b. 硬断言：XAML 视图类一个都不许被改名 ─────────────────
    const mapping = path.join(TMP, 'out', 'Mapping.txt');
    if (!fs.existsSync(mapping)) {
      console.error('!! 没有产出 Mapping.txt，无法断言 XAML 视图类是否被改名，放弃');
      process.exit(3);
    }
    assertXamlClassesKept(mapping);
    MAPPING_OUT = path.join(TMP, '..', 'obf-mapping.txt');
    fs.copyFileSync(mapping, MAPPING_OUT);

    // ── 4. 生成壳的加密载荷（取刚混淆好的产物） ──────────────────
    writeShellPayloads();
  } finally {
    restore();
  }

  // ── 5. 构建壳 ──────────────────────────────────────────────────
  //    刻意放在 restore() **之后**：壳的依赖图里本不该有 KunClient 程序集
  //    （靠 ReferenceOutputAssembly="false"）；万一这个前提失效、明文混进包里，
  //    进来的也会是**未混淆**那份 —— 下面的断言一定能抓到，不会静默发出明文。
  run('dotnet', ['build', SHELL_CSPROJ, '-c', 'Release', '-r', 'win-x64', '-v:m',
                 '-p:ShellPayloadDir=' + PAYLOAD_DIR]);

  // ── 5b. 混淆壳（只加密字符串）+ 5c. 单文件发布（--no-build 拿刚混淆的 dll）──
  //    顺序不能反：publish 打包的是 bin 里的程序集，所以混淆必须在 publish **之前**落到 bin。
  //    而且**必须 --no-build** —— 否则 publish 会再编译一次把混淆产物覆盖掉，
  //    结果是「看着成功、实际没混淆」（App 那边踩过同一个坑，见文件头注释）。
  //    壳的还原放在 finally：publish 失败也不能把混淆版留在 bin 里。
  const shellRestore = obfuscateShell();
  try {
    rmrf(PUBDIR);
    run('dotnet', ['publish', SHELL_CSPROJ, '-c', 'Release', '-r', 'win-x64', '-o', PUBDIR,
                   '--no-build', '-p:ShellPayloadDir=' + PAYLOAD_DIR]);
  } finally {
    shellRestore();
  }

  // ── 6. 硬断言：明文程序集不许进包 ─────────────────────────────
  assertShellExcludesModules();

  // ── 7. 改成用户下载时看到的文件名 ─────────────────────────────
  const built = path.join(PUBDIR, SHELL_LOADER);
  if (!fs.existsSync(built)) { console.error('!! 壳没有产出 ' + built); process.exit(4); }
  if (path.resolve(built) !== path.resolve(EXE)) {
    if (fs.existsSync(EXE)) fs.unlinkSync(EXE);
    fs.renameSync(built, EXE);
  }
  // 单文件发布用不到这些旁挂文件（runtimeconfig / deps 都已嵌进包）。
  // 留着只会让人以为要一起发出去 —— 上一轮就漏下过一个旧的 KunClient.runtimeconfig.json。
  for (const f of fs.readdirSync(PUBDIR)) {
    if (/\.(pdb|deps\.json|runtimeconfig\.json)$/i.test(f)) {
      fs.unlinkSync(path.join(PUBDIR, f));
      log('清理旁挂文件:', f);
    }
  }

  if (!fs.existsSync(EXE)) { console.error('!! 没有产出 exe'); process.exit(4); }
  // 断言打在**发布产物**上（不是中间产物）—— 见 assertShellStringsInExe 的注释。
  assertShellStringsInExe(EXE);
  log('产物:', EXE, fs.statSync(EXE).size, 'bytes', md5(EXE));
  if (MAPPING_OUT) log('改名映射表:', MAPPING_OUT);
  return check();
}

process.exit(main() || 0);
