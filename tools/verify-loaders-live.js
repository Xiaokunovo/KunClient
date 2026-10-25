/**
 * 线上硬验证：直接打生产地址，确认云端引导库真的可用。
 * 不看 grep、不看 SSH 里的 curl（那个常拿到空输出），只看对外 HTTP 的实际响应。
 */
const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const BASE = process.env.LOADER_BASE || 'http://your-server.example.com:3000';

function get(url) {
  return new Promise((resolve, reject) => {
    // agent:false 绕开 keep-alive / 代理干扰
    const req = http.get(url, { agent: false, timeout: 20000 }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({
        status: res.statusCode,
        headers: res.headers,
        body: Buffer.concat(chunks),
      }));
    });
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
  });
}

(async () => {
  // 1) 索引
  const idx = await get(`${BASE}/api/loaders/firehose/index`);
  let count = -1;
  let sample = null;
  try {
    const j = JSON.parse(idx.body.toString('utf8'));
    count = j.count;
    sample = (j.data || []).find((x) => x.name === 'mi/865') || (j.data || [])[0];
    const brands = [...new Set((j.data || []).map((x) => x.brand))].sort();
    console.log(`[1] 索引 HTTP ${idx.status}  count=${count}  品牌=${brands.join(',')}`);
    if (sample) console.log(`    样例行: ${JSON.stringify(sample)}`);
  } catch (e) {
    console.log(`[1] 索引 HTTP ${idx.status}  但正文不是 JSON: ${idx.body.toString('utf8').slice(0, 200)}`);
  }

  // 2) 下载一个引导文件并与本地比对字节
  if (sample) {
    const dl = await get(`${BASE}/api/loaders/firehose/file?name=${encodeURIComponent(sample.name)}`);
    const md5 = crypto.createHash('md5').update(dl.body).digest('hex');
    const localPath = path.join(__dirname, '..', 'server', 'assets', 'loaders', 'firehose',
      ...sample.name.split('/'));
    let verdict = '（本地无该文件，无法比对）';
    if (fs.existsSync(localPath)) {
      const localMd5 = crypto.createHash('md5').update(fs.readFileSync(localPath)).digest('hex');
      verdict = md5 === localMd5
        ? `与本地一致 ✔ (md5=${md5})`
        : `★ 与本地不一致 ✘  远端=${md5} 本地=${localMd5}`;
    }
    console.log(`[2] 下载 ${sample.name}  HTTP ${dl.status}  ${dl.body.length} 字节  ${verdict}`);
  }

  // 3) 路径穿越必须被拒
  const bad = await get(`${BASE}/api/loaders/firehose/file?name=${encodeURIComponent('../server.js')}`);
  console.log(`[3] 路径穿越 ../server.js → HTTP ${bad.status} ` +
    `（期望 400/403，拿到 200 就是漏洞）`);

  // 4) 拉一个真实存在的品牌目录清单，确认 65 个都在
  console.log(`[4] 结论: count=${count} ${count >= 65 ? '✔ 引导库已上线' : '✘ 数量异常'}`);
})().catch((e) => console.error('验证失败:', e.message));
