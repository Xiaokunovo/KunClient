/**
 * KunClient 开源通知 —— 一次性群发脚本
 *
 * 用途：告知用户项目已开源（GitHub + Gitee 双仓库），附仓库地址。
 *
 * 运行（在生产服务器 /www/wwwroot/KunClient-server 目录下）：
 *   node send-opensource.js --dry-run       # 预览：列收件人 + 生成样例 HTML
 *   node send-opensource.js --send          # 真实群发
 *   node send-opensource.js --send --limit 1  # 只发前 1 个（自测）
 *
 * 设计：复用 config.local.json 的邮件配置 + 品牌视觉令牌，
 *       HTML + 纯文本双正文（只有 HTML 容易进垃圾箱）。
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

if (!DRY_RUN && !SEND) {
  console.error('用法: node send-opensource.js [--dry-run | --send] [--limit N]');
  process.exit(2);
}

const nodemailer = require('nodemailer');
const initSqlJs = require('sql.js');

const cfgLocal = (() => {
  const p = path.join(__dirname, 'config.local.json');
  return fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, 'utf8')) : {};
})();
const cfgBase = (() => {
  const p = path.join(__dirname, 'config.json');
  return fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, 'utf8')) : {};
})();
const cfg = { ...cfgBase, ...cfgLocal };

const MAIL = cfg.mail || {};
const FROM = process.env.MAIL_FROM || MAIL.from || 'KunClient <kunclient@mail.kunawa.cn>';

// ── 品牌令牌（与 server.js MAIL_BRAND 一致）────────────────────────────────
const B = {
  accent: '#7ab8f3', accentDeep: '#2f7fd0', accentInk: '#10304f',
  accentWash: '#eef5fe', accentLine: '#a9cdf5',
  page: '#eef2f8', card: '#ffffff', ink: '#101828', ink2: '#4b5a71',
  ink3: '#8a97ab', line: '#e3e9f2', lineSoft: '#eff3f9',
};
const FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif";
const MONO = "'SF Mono',SFMono-Regular,Consolas,'Cascadia Mono','Courier New',monospace";

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function mailBrandRow(kicker) {
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"><tr>`
    + `<td style="vertical-align:middle">`
    + `<span style="display:inline-block;font-size:15px;font-weight:700;letter-spacing:.2px;color:${B.ink}">KunClient</span>`
    + `</td>`
    + `<td align="right" style="vertical-align:middle;font-size:11.5px;color:${B.ink3};letter-spacing:.4px;white-space:nowrap">${esc(kicker || '')}</td>`
    + `</tr></table>`;
}

function mailDocument({ kicker, title, subtitle, body, footNote }) {
  return `<div style="margin:0;padding:30px 12px;background:${B.page}">`
    + `<div style="max-width:560px;margin:0 auto;background:${B.card};border:1px solid ${B.line};border-radius:14px;overflow:hidden;color:${B.ink};font-family:${FONT}">`
    + `<div style="height:4px;background:${B.accent};font-size:0;line-height:0">&nbsp;</div>`
    + `<div style="padding:22px 28px 0">${mailBrandRow(kicker)}</div>`
    + `<div style="padding:20px 28px 0">`
    + `<div style="font-size:21px;font-weight:700;letter-spacing:-.2px;line-height:1.35">${title}</div>`
    + (subtitle ? `<div style="margin-top:7px;font-size:13px;line-height:1.7;color:${B.ink2}">${subtitle}</div>` : '')
    + `</div>`
    + `<div style="padding:18px 28px 26px;font-size:14px;line-height:1.75;color:${B.ink}">${body}</div>`
    + `<div style="padding:14px 28px 18px;border-top:1px solid ${B.lineSoft};font-size:11.5px;line-height:1.7;color:${B.ink3}">`
    + (footNote ? footNote + '<br>' : '')
    + `本邮件由 KunClient 系统发送，请勿直接回复。</div>`
    + `</div></div>`;
}

function mailPanel(inner) {
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0" style="margin:0 0 16px"><tr>`
    + `<td style="background:${B.accentWash};border:1px solid ${B.accentLine};border-radius:12px;padding:16px 18px">${inner}</td></tr></table>`;
}

function mailButton(href, label, variant) {
  const solid = variant !== 'ghost';
  const bg = solid ? B.accent : B.card;
  const fg = solid ? B.accentInk : B.accentDeep;
  const border = solid ? B.accent : B.accentLine;
  return `<table role="presentation" cellpadding="0" cellspacing="0" border="0" style="display:inline-block;margin:0 8px 8px 0"><tr>`
    + `<td style="background:${bg};border:1px solid ${border};border-radius:8px">`
    + `<a href="${href}" style="display:inline-block;padding:11px 24px;font-size:13.5px;font-weight:700;color:${fg};text-decoration:none;border-radius:8px;font-family:${FONT};white-space:nowrap">${label}</a>`
    + `</td></tr></table>`;
}

function htmlToText(html) {
  return String(html || '')
    .replace(/<style[\s\S]*?<\/style>/gi, '')
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<\/(p|div|tr|table|h1|h2|h3|li)>/gi, '\n')
    .replace(/<[^>]+>/g, '')
    .replace(/&nbsp;/g, ' ')
    .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&amp;/g, '&')
    .replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n')
    .trim();
}

// ── 仓库地址 ────────────────────────────────────────────────────────────────
const REPO_GITHUB = 'https://github.com/Xiaokunovo/KunClientV1.0.0';
const REPO_GITEE = 'https://gitee.com/Xiaokunovoawa/kun-client';

const SUBJECT = '【开源】KunClient 全部源码已公开 —— GitHub & Gitee';

function buildHtml(user) {
  const body =
    mailPanel(
      `<div style="font-size:14px;line-height:1.9;color:${B.ink}">${esc(user.username)}，你好：</div>`
      + `<div style="margin-top:6px;font-size:14px;line-height:1.9;color:${B.ink}">`
      + `之前说过停更后会开源 —— 现在做到了。KunClient 的<strong>全部源码</strong>已经公开，`
      + `包括 C++/Qt6 客户端、C# 旧版、服务端、官网和 QQ 机器人。</div>`
    )
    + `<div style="font-size:12px;font-weight:700;letter-spacing:.4px;color:${B.ink3};margin:0 0 8px">仓库地址</div>`
    + `<div style="background:${B.accentWash};border:1px solid ${B.accentLine};border-radius:12px;padding:14px 18px;margin:0 0 18px">`
    + `<div style="font-size:13px;line-height:2">`
    + `<span style="color:${B.ink3}">GitHub</span><br>`
    + `<a href="${REPO_GITHUB}" style="color:${B.accentDeep};font-family:${MONO};font-size:12.5px;word-break:break-all">${REPO_GITHUB}</a>`
    + `</div>`
    + `<div style="font-size:13px;line-height:2;margin-top:10px">`
    + `<span style="color:${B.ink3}">Gitee</span><br>`
    + `<a href="${REPO_GITEE}" style="color:${B.accentDeep};font-family:${MONO};font-size:12.5px;word-break:break-all">${REPO_GITEE}</a>`
    + `</div>`
    + `</div>`
    + mailButton(REPO_GITHUB, 'GitHub', 'solid') + mailButton(REPO_GITEE, 'Gitee', 'ghost')
    + `<div style="margin:8px 0 16px"></div>`
    + mailPanel(
      `<div style="font-size:13.5px;line-height:1.9;color:${B.ink2}">`
      + `<strong style="color:${B.ink}">开源内容</strong><br>`
      + `· C++/Qt6 桌面客户端（EDL / Fastboot / MTK / 欧加 / 小米全栈）<br>`
      + `· C#/WPF 版客户端（v1.x，已退役）<br>`
      + `· Node.js 服务端 · Next.js 官网 · QQ 机器人<br>`
      + `· 设计文档、构建与部署脚本</div>`
    )
    + `<div style="font-size:14px;line-height:1.9;color:${B.ink}">`
    + `协议是 <strong>MIT</strong>，可以自由使用、修改、分发。`
    + `所有真实密钥和服务器地址都已从仓库中清除，想自己部署需要自备后端。</div>`
    + `<div style="margin-top:14px;font-size:14px;line-height:1.9;color:${B.ink}">`
    + `这个项目到这里就真的结束了。谢谢一路以来的使用和反馈。<br>江湖再见 👋</div>`
    + `<div style="margin-top:14px;font-size:13px;color:${B.ink3};line-height:1.8">— 小坤ovo</div>`;

  return mailDocument({
    kicker: '开源通知',
    title: '全部源码已公开',
    subtitle: 'KunClient 正式开源 · MIT 协议',
    body,
    footNote: '你收到这封邮件，是因为该邮箱绑定在 KunClient 账号上。',
  });
}

function buildText(user) {
  return htmlToText(buildHtml(user));
}

async function main() {
  if (!MAIL.enabled) {
    console.error('✗ 邮件服务未启用（config.local.json mail.enabled != true）');
    process.exit(1);
  }

  const transporter = nodemailer.createTransport({
    host: process.env.MAIL_HOST || MAIL.host,
    port: Number(process.env.MAIL_PORT || MAIL.port || 465),
    secure: process.env.MAIL_SECURE ? process.env.MAIL_SECURE === 'true' : !!MAIL.secure,
    auth: {
      user: process.env.MAIL_USER || MAIL.user,
      pass: process.env.MAIL_PASS || MAIL.pass,
    },
  });

  const dbPath = process.env.DB_PATH || cfg.db_path || path.join(__dirname, 'database.sqlite');
  const SQL = await initSqlJs();
  const db = new SQL.Database(fs.readFileSync(dbPath));
  const rows = [];
  const res = db.exec(
    "SELECT id, username, email FROM users WHERE email IS NOT NULL AND email != '' AND status != 'banned'"
  );
  if (res.length) for (const r of res[0].values) rows.push({ id: r[0], username: r[1], email: r[2] });
  db.close();

  console.log(`收件人：${rows.length} 个（有邮箱且未封禁）`);
  if (LIMIT > 0) { console.log(`（--limit ${LIMIT}：只处理前 ${LIMIT} 个）`); rows.length = LIMIT; }
  if (rows.length === 0) { console.log('没有可发送的用户。'); process.exit(0); }

  if (DRY_RUN) {
    console.log('\n── 收件人清单 ──');
    for (const u of rows) console.log(`  ${u.id}\t${u.email}\t${u.username}`);
    const samplePath = path.join(__dirname, 'opensource-preview.html');
    fs.writeFileSync(samplePath, buildHtml(rows[0]), 'utf8');
    console.log(`\n样例 HTML: ${samplePath}`);
    console.log('\n── 纯文本 ──\n' + buildText(rows[0]));
    console.log('\n确认无误后用 --send 真实发送。');
    process.exit(0);
  }

  if (!SEND) process.exit(0);

  let sent = 0, failed = 0;
  const failedList = [];
  for (const u of rows) {
    try {
      await transporter.sendMail({
        from: FROM, to: u.email, subject: SUBJECT,
        html: buildHtml(u), text: buildText(u),
      });
      sent++;
      console.log(`✓ ${u.email}（${u.username}）`);
    } catch (e) {
      failed++;
      failedList.push({ email: u.email, username: u.username, err: (e && e.message) || String(e) });
      console.error(`✗ ${u.email}（${u.username}）: ${(e && e.message) || e}`);
    }
    if (sent + failed < rows.length) await new Promise(r => setTimeout(r, 120));
  }

  console.log('\n── 汇总 ──');
  console.log(`成功: ${sent}  失败: ${failed}  共: ${rows.length}`);
  if (failedList.length) {
    console.log('\n失败明细:');
    for (const f of failedList) console.log(`  ${f.email}（${f.username}）: ${f.err}`);
  }
  process.exit(failed ? 1 : 0);
}

main().catch(e => { console.error('脚本异常:', e); process.exit(1); });
