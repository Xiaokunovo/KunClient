#!/usr/bin/env node
'use strict';
/**
 * 实发一封测试邮件，验证 config.local.json → mail 的发信链路。
 *
 * 用法：
 *   node tools/outlook-test-send.js [收件邮箱]
 *   缺省收件人 = xiaokunovoovo@gmail.com
 *
 * 按 mail.mode 自动路由：
 *   mode='graph' → POST https://graph.microsoft.com/v1.0/me/sendMail（与 server.js 的 sendViaGraph 同构）
 *   其余         → SMTP（authType: oauth2 / password）
 */
const https = require('https');
const cfg = require('../server/config');
const nodemailer = require('../server/node_modules/nodemailer');

function httpsPost(host, path, headers, body) {
  return new Promise((resolve, reject) => {
    const req = https.request(
      { host, path, method: 'POST', headers: Object.assign({ 'Content-Length': Buffer.byteLength(body) }, headers) },
      (res) => {
        let data = '';
        res.on('data', (c) => (data += c));
        res.on('end', () => resolve({ status: res.statusCode, body: data }));
      }
    );
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

function graphAddr(v) {
  if (v && typeof v === 'object') v = v.address || v.text || '';
  const m = /<([^>]+)>/.exec(String(v || ''));
  return (m ? m[1] : String(v || '')).trim();
}

async function sendViaGraph(opts, m) {
  const o = m.oauth2 || {};
  const tenant = o.tenant || 'consumers';
  const body = new URLSearchParams({
    grant_type: 'refresh_token',
    client_id: o.clientId || '',
    refresh_token: o.refreshToken || ''
  }).toString();
  const tr = await httpsPost(
    'login.microsoftonline.com',
    `/${tenant}/oauth2/v2.0/token`,
    { 'Content-Type': 'application/x-www-form-urlencoded' },
    body
  );
  const tok = JSON.parse(tr.body);
  if (!tok.access_token) throw new Error('Graph token 失败: ' + tr.body.slice(0, 200));
  const message = {
    subject: opts.subject,
    body: { contentType: opts.html ? 'HTML' : 'Text', content: opts.html || opts.text || '' },
    toRecipients: [].concat(opts.to).map((t) => ({ emailAddress: { address: graphAddr(t) } }))
  };
  const fa = graphAddr(opts.from);
  if (fa) message.from = { emailAddress: { address: fa } };
  const r = await httpsPost(
    'graph.microsoft.com',
    '/v1.0/me/sendMail',
    { Authorization: 'Bearer ' + tok.access_token, 'Content-Type': 'application/json' },
    JSON.stringify({ message, saveToSentItems: true })
  );
  if (r.status < 200 || r.status >= 300) {
    throw new Error('Graph sendMail http=' + r.status + ': ' + r.body.slice(0, 300));
  }
  return { messageId: 'graph-' + Date.now() };
}

function buildMailAuth(m) {
  if (String(m.authType || 'password').toLowerCase() === 'oauth2') {
    const o = m.oauth2 || {};
    const tenant = o.tenant || 'consumers';
    const auth = {
      type: 'OAuth2',
      user: m.user,
      clientId: o.clientId,
      refreshToken: o.refreshToken,
      accessUrl:
        o.accessUrl || `https://login.microsoftonline.com/${tenant}/oauth2/v2.0/token`
    };
    if (o.clientSecret) auth.clientSecret = o.clientSecret;
    if (o.accessToken) auth.accessToken = o.accessToken;
    return auth;
  }
  return { user: m.user, pass: m.pass };
}

(async () => {
  const to = process.argv[2] || 'xiaokunovoovo@gmail.com';
  const m = cfg.mail;
  if (!m || !m.enabled) {
    console.error('mail 未启用（config.mail.enabled != true）');
    process.exit(1);
  }
  const mode = String(m.mode || 'smtp').toLowerCase();
  console.log(`发件配置: mode=${mode} host=${m.host}:${m.port} user=${m.user} authType=${m.authType || 'password'}`);
  const opts = {
    from: m.from,
    to,
    subject: mode === 'graph'
      ? 'KunClient 邮件链路测试（Graph sendMail）'
      : (String(m.authType || 'password').toLowerCase() === 'oauth2'
        ? 'KunClient 邮件链路测试（Gmail OAuth2）'
        : 'KunClient 邮件链路测试（SMTP 基础认证）'),
    text: '这是一封 KunClient 服务端发件链路的测试邮件。\n收到即代表发信通道正常。'
  };
  let info;
  if (mode === 'graph') {
    info = await sendViaGraph(opts, m);
  } else {
    const transporter = nodemailer.createTransport({
      host: m.host, port: m.port, secure: !!m.secure, auth: buildMailAuth(m)
    });
    info = await transporter.sendMail(opts);
  }
  console.log('发送成功:', info.messageId, '->', to);
})().catch((e) => {
  console.error('发送失败:', e.message);
  if (e.code) console.error('code:', e.code);
  process.exit(1);
});
