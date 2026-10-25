// 云端 firehose 引导库索引。
//
// 目录结构：assets/loaders/firehose/<品牌>/<机型>，机型文件通常没有扩展名。
// OPLUS 签名通道的整套文件用**同目录后缀**标识：
//     oppo/450            ← programmer（引导本体）
//     oppo/450.digest     ← OPLUS digest 签名表
//     oppo/450.sig        ← OPLUS signature 签名表
// 三者归成**同一条索引**，客户端点一次下载就能把整套拿走并自动填进签名通道。
// 单独把 .digest/.sig 也列成条目的话，列表里会出现三个"同名机型"，用户只下了
// programmer 就以为整套下完了，签名通道照样进不去 Firehose。
const fs = require('fs');
const path = require('path');

/** 附属文件扩展名 → 条目字段名（小写比较） */
const COMPANION_EXTS = { '.digest': 'digest', '.sig': 'signature' };

/**
 * 扫描引导库目录，返回按 name 升序的索引数组。
 * 每条形如 { brand, model, name, size, digest?, digestSize?, signature?, signatureSize? }。
 * 没有对应 programmer 的孤儿附属文件不会进列表（下下来也没法下发）。
 *
 * @param {string} rootDir 引导库根目录（assets/loaders/firehose）
 * @returns {Array<object>}
 */
function buildFirehoseIndex(rootDir) {
  /** rel → 主条目 */
  const mains = new Map();
  /** 主条目 rel → { digest: {name,size}, signature: {name,size} } */
  const companions = new Map();

  const walk = (dir, prefix) => {
    let entries;
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); }
    catch { return; }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isDirectory()) { walk(full, rel); continue; }
      if (!entry.isFile()) continue;
      let size = 0;
      try { size = fs.statSync(full).size; } catch { }

      const ext = path.extname(entry.name).toLowerCase();
      const kind = COMPANION_EXTS[ext];
      if (kind) {
        // 扩展名长度与 rel 里那段一致（大小写不同的 .DIGEST 同长），按长度切掉
        const base = rel.slice(0, rel.length - ext.length);
        const rec = companions.get(base) || {};
        rec[kind] = { name: rel, size };
        companions.set(base, rec);
        continue;
      }

      const slash = rel.indexOf('/');
      mains.set(rel, {
        brand: slash > 0 ? rel.slice(0, slash) : '',
        model: slash > 0 ? rel.slice(slash + 1) : rel,
        name: rel,
        size
      });
    }
  };

  walk(rootDir, '');

  const list = [];
  for (const entry of mains.values()) {
    const rec = companions.get(entry.name);
    if (rec) {
      if (rec.digest) { entry.digest = rec.digest.name; entry.digestSize = rec.digest.size; }
      if (rec.signature) { entry.signature = rec.signature.name; entry.signatureSize = rec.signature.size; }
    }
    list.push(entry);
  }
  list.sort((a, b) => a.name.localeCompare(b.name));
  return list;
}

module.exports = { buildFirehoseIndex };
