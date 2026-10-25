#!/usr/bin/env node
'use strict';
/**
 * 一次性获取 Outlook.com / Microsoft 365 的 OAuth2 refresh_token（设备代码流）。
 *
 * 用法：
 *   node tools/outlook-oauth-token.js --client-id <APPLICATION_CLIENT_ID> [--tenant consumers] [--user you@outlook.com]
 *
 * 前置（只需做一次，在 https://portal.azure.com → Microsoft Entra ID → 应用注册）：
 *   1) 新建注册；「受支持的账户类型」选「任何组织目录中的账户和个人 Microsoft 账户」。
 *   2) 该应用 → 身份验证 → 页面最底部「允许公共客户端流」= 是（否则设备代码流会被拒）。
 *   3) 该应用 → API 权限 → 添加权限：
 *        · Office 365 Exchange Online → 委派权限：IMAP.AccessAsUser.All、SMTP.Send
 *        · Microsoft Graph → 委派权限：offline_access（用于拿到 refresh_token）
 *      然后「代表 <你的账号> 授予同意」。
 *   4) 复制「应用程序(客户端) ID」= clientId（公共客户端不需要 client secret）。
 *
 * 完成后把本脚本输出的 refresh_token 填到：
 *   server/config.local.json → mail.oauth2.refreshToken
 * 并把 mail.authType 设为 "oauth2"（clientSecret 可留空）。
 */
const https = require('https');
const fs = require('fs');
const path = require('path');

function arg(name, def) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : def;
}

const clientId = arg('client-id', process.env.OUTLOOK_CLIENT_ID || '');
const tenant = arg('tenant', 'consumers');
const user = arg('user', '');
// --graph：申请 Microsoft Graph 的 Mail.Send（发送走 /me/sendMail，不依赖 IMAP/SMTP 协议开关）。
// 缺省申请 outlook.office.com 的 IMAP/SMTP 协议权限。
const useGraph = process.argv.indexOf('--graph') >= 0;
const scopes = useGraph
  ? ['offline_access', 'https://graph.microsoft.com/Mail.Send'].join(' ')
  : [
      'offline_access',
      'https://outlook.office.com/IMAP.AccessAsUser.All',
      'https://outlook.office.com/SMTP.Send'
    ].join(' ');

if (!clientId) {
  console.error('缺少 --client-id（Azure 应用注册里的 Application (client) ID）。');
  process.exit(1);
}

function post(host, urlPath, form) {
  const body = new URLSearchParams(form).toString();
  return new Promise((resolve, reject) => {
    const req = https.request({
      host,
      path: urlPath,
      method: 'POST',
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded',
        'Content-Length': Buffer.byteLength(body)
      }
    }, (res) => {
      let data = '';
      res.on('data', (c) => (data += c));
      res.on('end', () => {
        try { resolve(JSON.parse(data)); } catch (e) { reject(new Error('解析响应失败: ' + data)); }
      });
    });
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

(async () => {
  const dc = await post('login.microsoftonline.com', `/${tenant}/oauth2/v2.0/devicecode`, {
    client_id: clientId,
    scope: scopes
  });
  if (dc.error) {
    console.error('申请设备代码失败:', dc.error, dc.error_description || '');
    process.exit(1);
  }
  console.log('\n====================================================');
  console.log('  请在浏览器打开：' + dc.verification_uri);
  console.log('  输入代码：' + dc.user_code);
  console.log('  用 ' + (user || '你要发信的 Outlook 账号') + ' 登录并同意授权');
  console.log('====================================================\n');
  console.log('等待授权中（有效期约 ' + Math.round((dc.expires_in || 900) / 60) + ' 分钟）...');

  const interval = (dc.interval || 5) * 1000;
  const deadline = Date.now() + (dc.expires_in || 900) * 1000;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, interval));
    const tok = await post('login.microsoftonline.com', `/${tenant}/oauth2/v2.0/token`, {
      grant_type: 'urn:ietf:params:oauth:grant-type:device_code',
      client_id: clientId,
      device_code: dc.device_code
    });
    if (tok.error === 'authorization_pending') continue;
    if (tok.error === 'slow_down') { await new Promise((r) => setTimeout(r, 5000)); continue; }
    if (tok.error) {
      console.error('授权失败:', tok.error, tok.error_description || '');
      process.exit(1);
    }
    console.log('\n授权成功。');
    if (!tok.refresh_token) {
      console.error('⚠️ 未返回 refresh_token（确认 scope 里包含 offline_access）。');
    } else {
      const dir = path.join(__dirname, '..', '.runtime');
      fs.mkdirSync(dir, { recursive: true });
      const file = path.join(dir, 'outlook-oauth.json');
      fs.writeFileSync(file, JSON.stringify({
        _说明: 'Outlook OAuth2 凭据。把 refreshToken 填到 config.local.json → mail.oauth2.refreshToken，并设 mail.authType="oauth2"' + (useGraph ? '、mail.mode="graph"' : '') + '。',
        clientId,
        tenant,
        graph: useGraph,
        user: user || undefined,
        refreshToken: tok.refresh_token,
        obtainedAt: new Date().toISOString()
      }, null, 2), { mode: 0o600 });
      console.log('refresh_token:\n' + tok.refresh_token + '\n');
      console.log('已写入 ' + file + '（可直接复制到 config.local.json）');
    }
    process.exit(0);
  }
  console.error('超时：未在有效期内完成授权。');
  process.exit(1);
})();
