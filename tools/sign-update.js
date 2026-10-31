/**
 * 为更新包生成签名（ECDSA P-256），并输出客户端需要的内置公钥。
 *
 * 用法（在构建机上跑一次，每次发版都要跑）：
 *   node sign-update.js <包路径> <版本号>
 *
 * 产出：
 *   ① 打印 sha256 / sig / 客户端公钥（base64，粘进 kc/update/UpdateVerifier.cpp）
 *   ② 把签名写进 <包路径>.sig.json，供发版脚本读
 *
 * 🔴 私钥处理：
 *   · 首次运行生成密钥对，私钥写 <repo>/.keys/update-private.pem，**绝不进仓库**
 *     （.gitignore 已排除 .keys/）。
 *   · 私钥只在构建机本地存在，服务器上没有 —— 这是「服务器被拿下也签不出恶意包」
 *     的前提。
 *   · 私钥丢了 = 再也签不出新版本（只能发一个内置新公钥的版本），所以要备份。
 */
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
if (args.length < 2) {
  console.error('用法: node sign-update.js <包路径> <版本号>');
  process.exit(2);
}
const pkgPath = path.resolve(args[0]);
const version = args[1];

if (!fs.existsSync(pkgPath)) {
  console.error('包不存在:', pkgPath);
  process.exit(1);
}

// ── 密钥对 ────────────────────────────────────────────────────────────────
const keyDir = path.resolve(__dirname, '..', '.keys');
const privPath = path.join(keyDir, 'update-private.pem');

let privateKey;
if (fs.existsSync(privPath)) {
  privateKey = fs.readFileSync(privPath, 'utf8');
  console.log('使用已有私钥:', privPath);
} else {
  fs.mkdirSync(keyDir, { recursive: true });
  const { privateKey: pk, publicKey } = crypto.generateKeyPairSync('ec', {
    namedCurve: 'prime256v1',
    publicKeyEncoding: { type: 'spki', format: 'pem' },
    privateKeyEncoding: { type: 'pkcs8', format: 'pem' },
  });
  fs.writeFileSync(privPath, pk, { mode: 0o600 });
  console.log('已生成新私钥:', privPath);
  console.log('⚠️  务必备份这个文件 —— 丢了就再也签不出能通过校验的新版本。');
}

const publicKey = crypto.createPublicKey(privateKey);

// ── 计算哈希 ──────────────────────────────────────────────────────────────
const buf = fs.readFileSync(pkgPath);
const sha256 = crypto.createHash('sha256').update(buf).digest('hex');

// ── 签名 ──────────────────────────────────────────────────────────────────
// 被签名的明文必须与客户端逐字一致： version\nfile\nsha256
// file 用**远端文件名**（服务器上的名字），不是本地路径。
const fileName = path.basename(pkgPath);
const payload = `${version}\n${fileName}\n${sha256}`;
const signature = crypto.sign('sha256', Buffer.from(payload, 'utf8'), privateKey);

// ── 导出客户端公钥（未压缩点，65 字节 base64）─────────────────────────────
// SPKI 里取最后的 65 字节（0x04 || X || Y）—— P-256 的未压缩点格式。
const pubDer = publicKey.export({ type: 'spki', format: 'der' });
const point = pubDer.subarray(pubDer.length - 65);
if (point[0] !== 0x04) {
  console.error('公钥导出失败：末尾不是未压缩点');
  process.exit(1);
}
const pubB64 = point.toString('base64');

console.log('');
console.log('=== 签名结果 ===');
console.log('file    :', fileName);
console.log('version :', version);
console.log('sha256  :', sha256);
console.log('sig     :', signature.toString('base64'));
console.log('');
console.log('=== 客户端内置公钥（粘进 kc/update/UpdateVerifier.cpp 的 kUpdatePublicKeyB64）===');
console.log(pubB64);

// ── 写供发版脚本读的清单 ──────────────────────────────────────────────────
const out = {
  version,
  file: fileName,
  sha256,
  sig: signature.toString('base64'),
  signedAt: new Date().toISOString(),
};
const outPath = pkgPath + '.sig.json';
fs.writeFileSync(outPath, JSON.stringify(out, null, 2), 'utf8');
console.log('');
console.log('清单已写入:', outPath);
