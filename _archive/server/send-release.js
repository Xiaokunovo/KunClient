/**
 * KunClient 2.0.6 功能补充 —— 发版通知群发
 *
 * 用途：把本次「刷机记录与导出 + iOS 玩法六件套」的更新告诉已绑定邮箱的用户。
 *
 * 运行（在生产服务器 /www/wwwroot/KunClient-server 目录下）：
 *   node send-release.js --dry-run                 # 只列收件人 + 生成样例 HTML，不发送
 *   node send-release.js --send                    # 真实群发
 *   node send-release.js --send --limit 1          # 只发前 1 个（自测）
 *   node send-release.js --send --to a@b.com       # 只发给指定地址（自测）
 *
 * 设计：复用 config.js 的阿里云 DirectMail 配置 + 与 server.js 一致的品牌视觉令牌；
 *      HTML + 纯文本双正文（只有 HTML 容易进垃圾箱）；
 *      逐用户发送，瞬时错误重试 3 次、永久错误直接跳过；不修改 server.js。
 */
'use strict';

const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
const DRY_RUN = args.includes('--dry-run');
const SEND = args.includes('--send');
const LIMIT = (() => {
  const i = args.indexOf('--limit');
  return i >= 0 && args[i + 1] ? parseInt(args[i + 1], 10) : 0;
})();
const TO = (() => {
  const i = args.indexOf('--to');
  return i >= 0 && args[i + 1] ? args[i + 1] : '';
})();

if (!DRY_RUN && !SEND) {
  console.error('用法: node send-release.js [--dry-run | --send] [--limit N] [--to addr]');
  process.exit(2);
}

const nodemailer = require('nodemailer');
const initSqlJs = require('sql.js');

const ROOT = __dirname;
const cfg = (() => {
  const base = path.join(ROOT, 'config.json');
  const local = path.join(ROOT, 'config.local.json');
  const read = (p) => (fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, 'utf8')) : {});
  return { ...read(base), ...read(local) };
})();

const MAIL = cfg.mail || {};
const FROM = process.env.MAIL_FROM || MAIL.from || 'KunClient <kunclient@mail.kunawa.cn>';
const SITE = cfg.public_url || 'https://kunawa.cn';
const VERSION = '2.0.7';
const BUILD_DATE = '2026-10-30';

// ── 品牌令牌（与 server.js 的 MAIL_BRAND 逐值一致）─────────────────────────
const B = {
  accent: '#7ab8f3', accentDeep: '#2f7fd0', accentInk: '#10304f',
  accentWash: '#eef5fe', accentLine: '#a9cdf5', accentWashLine: '#d8e8fb',
  page: '#eef2f8', card: '#ffffff', ink: '#101828', ink2: '#4b5a71',
  ink3: '#8a97ab', line: '#e3e9f2', lineSoft: '#eff3f9',
};
const FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif";

const esc = (s) => String(s == null ? '' : s)
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

// ── 本次更新的内容（按板块组织）─────────────────────────────────────────────
const SECTIONS = [
  {
    title: '刷机记录与导出',
    items: [
      '新增「刷机记录」：每次写入 / 擦除 / 回读 / XML 刷机完成后自动记一条（时间、设备、分区、成败、耗时），只留最近 500 条；<b>不连设备也能看</b>',
      '记录可导出 CSV / TXT，也能一键清空',
      '分区表支持右键菜单：复制分区名、复制整行、导出 CSV / TXT（GPT 与 rawprogram XML 都认）',
      'EDL 日志头新增「导出」按钮，不用再手动框选两千行',
      '设置页新增「导出诊断包」：最近 5 天日志 + 系统信息打成一个 zip，直接发给开发者就能定位问题',
    ],
  },
  {
    title: 'iOS 玩法六件套',
    items: [
      '<b>辅助触控悬浮球</b>：屏幕边上一个可拖动的小白点，点开是七个快捷入口',
      '<b>通知中心</b>：顶栏铃铛留一份通知历史，一闪就没的横幅不再错过',
      '<b>待机模式</b>：无操作一段时间后整屏大时钟 + 设备读数，点一下任意处就回来',
      '<b>专注模式「刷机模式」</b>：一键收窄到刷机与设备两页，横幅静音',
      '<b>主屏小组件</b>：设备 / 最近刷机 / 时钟 三块玻璃卡',
      '<b>快捷指令</b>：把「连接 → 读 GPT → 备份」这种每天重复的动作串成一条，一键跑完',
    ],
  },
];

const HIGHLIGHTS = [
  '设置页新增「iOS 玩法」卡片，六项开关改完立刻生效，不用重启',
  '连接设备更快：不再固定等 700ms，改成按设备实际响应递进试探',
  '全部为纯本地功能，不依赖任何服务器',
];

// ── 邮件模板（与 server.js 的 mailDocument / mailBulletPanel 同构）───────────
function brandRow(kicker) {
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"><tr>`
    + `<td style="font-size:15px;font-weight:800;color:${B.accentInk};letter-spacing:-.2px;`
    + `font-family:${FONT}">KunClient</td>`
    + `<td align="right" style="font-size:11px;font-weight:700;letter-spacing:.6px;`
    + `color:${B.ink3};text-transform:uppercase;font-family:${FONT}">${esc(kicker)}</td>`
    + `</tr></table>`;
}

function document_({ kicker, title, subtitle, body, footNote }) {
  return `<div style="margin:0;padding:30px 12px;background:${B.page}">`
    + `<div style="max-width:560px;margin:0 auto;background:${B.card};border:1px solid ${B.line};`
    + `border-radius:14px;overflow:hidden;color:${B.ink};font-family:${FONT}">`
    + `<div style="height:4px;background:${B.accent};font-size:0;line-height:0">&nbsp;</div>`
    + `<div style="padding:22px 28px 0">${brandRow(kicker)}</div>`
    + `<div style="padding:20px 28px 0">`
    + `<div style="font-size:21px;font-weight:700;letter-spacing:-.2px;line-height:1.35">${title}</div>`
    + (subtitle ? `<div style="margin-top:7px;font-size:13px;line-height:1.7;color:${B.ink2}">${subtitle}</div>` : '')
    + `</div>`
    + `<div style="padding:18px 28px 26px;font-size:14px;line-height:1.75;color:${B.ink}">${body}</div>`
    + `<div style="padding:14px 28px 18px;border-top:1px solid ${B.lineSoft};`
    + `font-size:11.5px;line-height:1.7;color:${B.ink3}">`
    + (footNote ? footNote + '<br>' : '')
    + `本邮件由 KunClient 系统自动发送，请勿直接回复。</div>`
    + `</div></div>`;
}

function button(href, label) {
  return `<table role="presentation" cellpadding="0" cellspacing="0" border="0"`
    + ` style="display:inline-block;margin:0 8px 8px 0"><tr>`
    + `<td style="background:${B.accent};border:1px solid ${B.accent};border-radius:8px">`
    + `<a href="${href}" style="display:inline-block;padding:11px 24px;font-size:13.5px;`
    + `font-weight:700;color:${B.accentInk};text-decoration:none;border-radius:8px;`
    + `font-family:${FONT};white-space:nowrap">${esc(label)}</a></td></tr></table>`;
}

function metaRows(rows) {
  const live = rows.filter(([, v]) => v);
  if (!live.length) return '';
  return `<table role="presentation" cellpadding="0" cellspacing="0" border="0"`
    + ` style="border-collapse:collapse;margin:0 0 18px">`
    + live.map(([k, v], i) => `<tr>`
      + `<td style="padding:3px 16px 3px 0;font-size:12.5px;color:${B.ink3};white-space:nowrap">${esc(k)}</td>`
      + `<td style="padding:3px 0;font-size:12.5px;font-weight:700;`
      + (i === 0 ? `color:${B.accentDeep};` : '') + `">${v}</td>`
      + `</tr>`).join('')
    + `</table>`;
}

function bulletPanel(title, items) {
  if (!items || !items.length) return '';
  return `<div style="font-size:12px;font-weight:700;letter-spacing:.4px;color:${B.ink3};`
    + `margin:0 0 8px">${esc(title)}</div>`
    + `<div style="background:${B.accentWash};border:1px solid ${B.accentWashLine};`
    + `border-radius:12px;padding:14px 18px;margin:0 0 18px;font-size:13.5px;line-height:1.9">`
    + items.map(t => `<div style="margin:3px 0;padding-left:14px;text-indent:-14px">`
      + `<span style="color:${B.accentDeep}">&#9642;</span>&nbsp;${t}</div>`).join('')
    + `</div>`;
}

function buildHtml(nickname) {
  const hi = nickname
    ? `<p style="margin:0 0 14px">${esc(nickname)} 你好：</p>`
    : `<p style="margin:0 0 14px">你好：</p>`;

  const body = hi
    + `<p style="margin:0 0 16px">KunClient <b>${VERSION}</b> 功能补充版已发布。这次加了两块东西：
       一个是把「刷机之后发生过什么」记下来并能导出，另一个是把 iOS 上那几样好玩的搬了进来。</p>`
    + metaRows([
        ['版本', VERSION],
        ['发布', BUILD_DATE],
        ['下载', '约 7.9 MB（免解压，双击即用）'],
      ])
    + button(`${SITE}/#download`, '下载新版本')
    + button(`${SITE}/`, '打开官网')
    + `<div style="height:6px"></div>`
    + SECTIONS.map(s => bulletPanel(s.title, s.items)).join('')
    + bulletPanel('其他改进', HIGHLIGHTS)
    + `<p style="margin:4px 0 0;font-size:12.5px;color:${B.ink2}">`
    + `新版首次启动会沿用你原来的设置，不需要重新配置。`
    + `</p>`;

  return document_({
    kicker: `Release ${VERSION}`,
    title: `KunClient ${VERSION} 已发布`,
    subtitle: '刷机记录与导出 · iOS 玩法六件套',
    body,
    footNote: `你收到这封邮件，是因为你在 KunClient 注册时填写了邮箱。`,
  });
}

function buildText(nickname) {
  const lines = [];
  lines.push(nickname ? `${nickname} 你好：` : '你好：');
  lines.push('');
  lines.push(`KunClient ${VERSION} 功能补充版已发布。`);
  lines.push(`版本：${VERSION}    发布：${BUILD_DATE}    下载约 7.9 MB`);
  lines.push('');
  lines.push(`下载：${SITE}/#download`);
  lines.push(`官网：${SITE}/`);
  lines.push('');
  for (const s of SECTIONS) {
    lines.push(`【${s.title}】`);
    for (const it of s.items) {
      // 纯文本侧剥掉 HTML 标签
      lines.push('  · ' + it.replace(/<[^>]+>/g, ''));
    }
    lines.push('');
  }
  lines.push('【其他改进】');
  for (const it of HIGHLIGHTS) lines.push('  · ' + it);
  lines.push('');
  lines.push('新版首次启动会沿用你原来的设置，不需要重新配置。');
  lines.push('');
  lines.push('你收到这封邮件，是因为你在 KunClient 注册时填写了邮箱。');
  return lines.join('\n');
}

// ── 收件人 ──────────────────────────────────────────────────────────────────
async function loadRecipients() {
  if (TO) return [{ username: '', email: TO }];
  const SQL = await initSqlJs();
  const dbPath = process.env.DB_PATH || cfg.db_path || path.join(ROOT, 'database.sqlite');
  const db = new SQL.Database(fs.readFileSync(dbPath));
  const res = db.exec(
    "select username, email from users where email is not null and email<>'' and status='active'"
  );
  if (!res.length) return [];
  return res[0].values.map(r => ({ username: r[0] || '', email: r[1] || '' }));
}

// ── 发送 ────────────────────────────────────────────────────────────────────
async function main() {
  const transporter = nodemailer.createTransport({
    host: MAIL.host, port: MAIL.port, secure: MAIL.secure,
    auth: { user: MAIL.user, pass: MAIL.pass },
  });

  let list = await loadRecipients();
  if (LIMIT > 0) list = list.slice(0, LIMIT);
  console.log(`收件人：${list.length} 个`);

  if (DRY_RUN) {
    console.log('\n── 收件人清单 ──');
    list.slice(0, 10).forEach(r => console.log(`  ${r.email}${r.username ? '  (' + r.username + ')' : ''}`));
    if (list.length > 10) console.log(`  … 其余 ${list.length - 10} 个`);
    const sample = buildHtml(list[0] ? list[0].username : '');
    const out = path.join(ROOT, 'release-mail-preview.html');
    fs.writeFileSync(out, sample, 'utf8');
    console.log(`\n样例 HTML 已写入：${out}（${(sample.length / 1024).toFixed(1)} KB）`);
    console.log('\n[dry-run] 未发送任何邮件。');
    return;
  }

  let ok = 0, fail = 0;
  const failed = [];
  for (let i = 0; i < list.length; ++i) {
    const r = list[i];
    const html = buildHtml(r.username);
    const text = buildText(r.username);
    let sent = false, lastErr = '';
    for (let attempt = 1; attempt <= 3 && !sent; ++attempt) {
      try {
        await transporter.sendMail({
          from: FROM, to: r.email,
          subject: `KunClient ${VERSION} 已发布 · 刷机记录与导出 + iOS 玩法六件套`,
          text, html,
        });
        sent = true;
      } catch (e) {
        lastErr = e.message || String(e);
        const permanent = e.responseCode >= 500 && e.responseCode < 600 && e.responseCode !== 550;
        if (e.responseCode === 550 || /no such user|invalid recipient/i.test(lastErr)) break;
        if (attempt < 3) await new Promise(res => setTimeout(res, 1200 * attempt));
      }
    }
    if (sent) {
      ok++;
      console.log(`[${i + 1}/${list.length}] OK   ${r.email}`);
    } else {
      fail++;
      failed.push(`${r.email}: ${lastErr}`);
      console.log(`[${i + 1}/${list.length}] FAIL ${r.email} — ${lastErr}`);
    }
    await new Promise(res => setTimeout(res, 250));   // 别把 SMTP 打太急
  }

  console.log(`\n完成：成功 ${ok}，失败 ${fail}`);
  if (failed.length) {
    console.log('\n失败清单：');
    failed.forEach(f => console.log('  ' + f));
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(1); });
