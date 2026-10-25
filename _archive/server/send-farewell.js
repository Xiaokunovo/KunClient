/**
 * KunClient 停更通知 —— 一次性群发脚本
 *
 * 用途：给「所有绑定了邮箱且未封禁」的用户发送停更/告别邮件（2.0.6 是最后一个版本）。
 *
 * 运行方式（在生产服务器 /www/wwwroot/KunClient-server 目录下）：
 *   node send-farewell.js --dry-run       # 只列收件人 + 生成一份样例 HTML，不发送
 *   node send-farewell.js --send          # 真实群发（逐用户重试，最后汇总）
 *
 * 设计：
 *   · 复用 config.local.json 的 mail 配置（阿里云 DirectMail，kunclient@mail.kunawa.cn）；
 *   · 复用 server.js 的品牌视觉令牌（MAIL_BRAND / mailDocument / mailPanel / htmlToText）；
 *   · HTML + 纯文本双正文，避免「只有 HTML」被垃圾邮件过滤拦截；
 *   · 逐用户发送，瞬时错误重试 3 次、永久错误（地址不存在等）直接跳过；
 *   · 不修改 server.js、不污染运行中的服务。
 */
'use strict';

const fs = require('fs');
const path = require('path');

// ── 参数解析 ──
const args = process.argv.slice(2);
const DRY_RUN = args.includes('--dry-run');
const SEND = args.includes('--send');
const LIMIT = (() => {
  const i = args.indexOf('--limit');
  return i >= 0 && args[i + 1] ? parseInt(args[i + 1], 10) : 0;
})();

if (!DRY_RUN && !SEND) {
  console.error('用法: node send-farewell.js [--dry-run | --send] [--limit N]');
  process.exit(2);
}

// ── 依赖 ──
const nodemailer = require('nodemailer');
const initSqlJs = require('sql.js');

// ── 配置（与 server.js 同源：环境变量 > config.local.json > config.json）──
const cfgLocal = (() => {
  const p = path.join(__dirname, 'config.local.json');
  if (fs.existsSync(p)) return JSON.parse(fs.readFileSync(p, 'utf8'));
  return {};
})();
const cfgBase = (() => {
  const p = path.join(__dirname, 'config.json');
  if (fs.existsSync(p)) return JSON.parse(fs.readFileSync(p, 'utf8'));
  return {};
})();
const cfg = { ...cfgBase, ...cfgLocal };

const MAIL = cfg.mail || {};
const FROM = process.env.MAIL_FROM || MAIL.from || 'KunClient <kunclient@mail.kunawa.cn>';

// ── 品牌令牌（与 server.js MAIL_BRAND 完全一致）────────────────────────────
const B = {
  accent: '#7ab8f3', accentDeep: '#2f7fd0', accentInk: '#10304f',
  accentWash: '#eef5fe', accentLine: '#a9cdf5', accentWashLine: '#d8e8fb',
  page: '#eef2f8', card: '#ffffff', ink: '#101828', ink2: '#4b5a71',
  ink3: '#8a97ab', line: '#e3e9f2', lineSoft: '#eff3f9',
  danger: '#c0392b', dangerWash: '#fdf0ee', dangerLine: '#f0c4bd',
};
const FONT = "-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif";

// ── 邮件原语（与 server.js 同款）───────────────────────────────────────────
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

function mailPanel(inner, tone) {
  const bg = tone === 'danger' ? B.dangerWash : B.accentWash;
  const line = tone === 'danger' ? B.dangerLine : B.accentLine;
  return `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0" style="margin:0 0 16px"><tr>`
    + `<td style="background:${bg};border:1px solid ${line};border-radius:12px;padding:16px 18px">${inner}</td></tr></table>`;
}

// 极简 HTML → 纯文本（覆盖本模板用到的标签）
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

// ── 文案 ────────────────────────────────────────────────────────────────────
const SUBJECT = '【重要】KunClient 停更说明 —— 感谢一路陪伴';

function buildHtml(user) {
  const body =
    mailPanel(
      `<div style="font-size:14px;line-height:1.9;color:${B.ink}">` +
      `${esc(user.username)}，你好：</div>` +
      `<div style="margin-top:6px;font-size:14px;line-height:1.9;color:${B.ink}">` +
      `写下这封信的时候，KunClient 已经走到了 <strong>2.0.6</strong>。这也是它的<strong>最后一个版本</strong>。</div>`
    ) +
    `<div style="font-size:14px;line-height:1.9;color:${B.ink};margin:0 0 16px">` +
    `因为一些个人原因，作者决定<strong>退圈</strong>了，KunClient 就此停更，不再发布新版本。</div>` +
    mailPanel(
      `<div style="font-size:14px;line-height:1.9;color:${B.ink}">` +
      `停更不等于结束：</div>` +
      `<div style="margin-top:6px;font-size:13.5px;line-height:1.9;color:${B.ink2}">` +
      `· <strong>2.0.6 是最后一个版本</strong>，已下载的安装包可以继续用；<br>` +
      `· <strong>服务器也会一并关闭</strong>，届时登录、云端更新、云端引导库、环境检测这些联网功能将停止服务；<br>` +
      `· 刷机、分区、备份、解包等<strong>本地功能不受影响</strong>，工具会继续以离线方式可用；<br>` +
      `· 之后我会把<strong>项目开源</strong>，代码会放出来，欢迎有能力的朋友接手或二次开发；<br>` +
      `· 开源地址确定后会在这里公布，敬请留意。</div>`
    ) +
    `<div style="font-size:14px;line-height:1.9;color:${B.ink}">` +
    `从第一个版本到现在，谢谢每一位用户的使用、反馈和包容。这段路不长，但很值得。江湖再见 👋</div>` +
    `<div style="margin-top:14px;font-size:13px;color:${B.ink3};line-height:1.8">— 小坤ovo</div>`;

  return mailDocument({
    kicker: '停更说明',
    title: '这是最后一个版本',
    subtitle: 'KunClient v2.0.6 停更通知',
    body,
    footNote: '你收到这封邮件，是因为该邮箱绑定在 KunClient 账号上。',
  });
}

function buildText(user) {
  return htmlToText(buildHtml(user));
}

// ── 主流程 ──
async function main() {
  // 1. 建 transporter（与 server.js mailTransporter 同口径）
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

  // 2. 读用户表
  const dbPath = process.env.DB_PATH || cfg.db_path || path.join(__dirname, 'database.sqlite');
  const SQL = await initSqlJs();
  const db = new SQL.Database(fs.readFileSync(dbPath));
  const rows = [];
  const res = db.exec(
    "SELECT id, username, email FROM users WHERE email IS NOT NULL AND email != '' AND status != 'banned'"
  );
  if (res.length) {
    for (const r of res[0].values) {
      rows.push({ id: r[0], username: r[1], email: r[2] });
    }
  }
  db.close();

  console.log(`收件人：${rows.length} 个（有邮箱且未封禁）`);
  if (LIMIT > 0) {
    console.log(`（--limit ${LIMIT}：只处理前 ${LIMIT} 个）`);
    rows.length = LIMIT;
  }

  if (rows.length === 0) {
    console.log('没有可发送的用户。');
    process.exit(0);
  }

  // 3. dry-run：只列 + 生成样例
  if (DRY_RUN) {
    console.log('\n── 收件人清单 ──');
    for (const u of rows) {
      console.log(`  ${u.id}\t${u.email}\t${u.username}`);
    }
    const sample = buildHtml(rows[0]);
    const samplePath = path.join(__dirname, 'farewell-preview.html');
    fs.writeFileSync(samplePath, sample, 'utf8');
    console.log(`\n样例 HTML 已写入: ${samplePath}`);
    console.log('样例纯文本:\n' + buildText(rows[0]));
    console.log('\n确认无误后，用 --send 真实发送。');
    process.exit(0);
  }

  // 4. 真实发送
  if (!SEND) process.exit(0);

  let sent = 0, failed = 0;
  const failedList = [];
  for (const u of rows) {
    try {
      await transporter.sendMail({
        from: FROM,
        to: u.email,
        subject: SUBJECT,
        html: buildHtml(u),
        text: buildText(u),
      });
      sent++;
      console.log(`✓ ${u.email}（${u.username}）`);
    } catch (e) {
      failed++;
      failedList.push({ email: u.email, username: u.username, err: (e && e.message) || String(e) });
      console.error(`✗ ${u.email}（${u.username}）: ${(e && e.message) || e}`);
    }
    // 温和限速，避免触发阿里云发信频率限制
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
