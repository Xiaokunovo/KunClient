#!/usr/bin/env node
'use strict';
/**
 * Gmail OAuth2 引导脚本：生成授权 URL → 捕获回跳 code → 换取 refresh_token → 写回 server/config.local.json。
 *
 * 前置（用户手动做，账号侧）：
 *   1. https://console.cloud.google.com/ 新建/选一个项目
 *   2. 「API 和服务」→「OAuth 同意屏幕」：User Type=外部，填应用名，保存；测试用户里加上 xiaokunovoovo@gmail.com
 *   3. 「凭据」→「创建凭据」→「OAuth 客户端 ID」：应用类型选「桌面应用」
 *   4. 把拿到的 客户端 ID / 客户端密钥 给我（或填进 config 后跑本脚本不带 --client-id）
 *   5. 在本客户端 ID 的「已获授权的重定向 URI」里加上：http://localhost:3000/oauth2callback
 *
 * 用法：
 *   node tools/gmail-oauth-setup.js --client-id <ID> --client-secret <SECRET>
 *   或先把 clientId/clientSecret 填进 config.local.json 的 mail.oauth2，再直接：
 *   node tools/gmail-oauth-setup.js
 *
 * 跨机/无浏览器回跳时，用 --code 手动给 code：
 *   先不带 --code 跑一次拿到授权 URL，自己在任意浏览器登录授权，从回跳地址里抄出 ?code=...，
 *   再：node tools/gmail-oauth-setup.js --client-id <ID> --client-secret <SECRET> --code <CODE>
 */
const http = require('http');
const https = require('https');
const url = require('url');
const fs = require('fs');
const path = require('path');

const REDIRECT = 'http://localhost:3000/oauth2callback';
const TOKEN_URL = 'https://oauth2.googleapis.com/token';
const AUTH_URL = 'https://accounts.google.com/o/oauth2/v2/auth';
const SCOPE = 'https://mail.google.com/';
const LOGIN_HINT = 'xiaokunovoovo@gmail.com';
const CFG_PATH = path.join(__dirname, '..', 'server', 'config.local.json');

function parseArgs() {
  const args = process.argv.slice(2);
  const o = { clientId: '', clientSecret: '', code: '' };
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--client-id') o.clientId = args[i + 1] || '';
    else if (args[i] === '--client-secret') o.clientSecret = args[i + 1] || '';
    else if (args[i] === '--code') o.code = args[i + 1] || '';
  }
  return o;
}

function loadFromConfig(o) {
  try {
    const c = JSON.parse(fs.readFileSync(CFG_PATH, 'utf8'));
    const oc = (c.mail && c.mail.oauth2) || {};
    o.clientId = o.clientId || oc.clientId || '';
    o.clientSecret = o.clientSecret || oc.clientSecret || '';
  } catch (e) {
    console.error('读 config.local.json 失败:', e.message);
    process.exit(1);
  }
  return o;
}

function buildAuthUrl(clientId) {
  const p = new URLSearchParams({
    client_id: clientId,
    redirect_uri: REDIRECT,
    response_type: 'code',
    scope: SCOPE,
    access_type: 'offline',
    prompt: 'consent',
    login_hint: LOGIN_HINT
  });
  return AUTH_URL + '?' + p.toString();
}

function exchange(clientId, clientSecret, code) {
  const body = new URLSearchParams({
    client_id: clientId,
    client_secret: clientSecret,
    code,
    grant_type: 'authorization_code',
    redirect_uri: REDIRECT
  }).toString();
  const u = new URL(TOKEN_URL);
  const req = https.request(
    {
      host: u.host,
      path: u.pathname + u.search,
      method: 'POST',
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded',
        'Content-Length': Buffer.byteLength(body)
      }
    },
    (res) => {
      let data = '';
      res.on('data', (d) => (data += d));
      res.on('end', () => {
        let tok;
        try {
          tok = JSON.parse(data);
        } catch (e) {
          console.error('token 响应不是 JSON:', data.slice(0, 300));
          process.exit(1);
        }
        if (!tok.refresh_token) {
          console.error(
            '未返回 refresh_token。请确认：(1) 授权 URL 含 access_type=offline&prompt=consent；' +
              '(2) 该 Google 账号已在 OAuth 同意屏幕的「测试用户」里；(3) 重定向 URI 已登记。\n返回: ' +
              data.slice(0, 300)
          );
          process.exit(1);
        }
        writeRefresh(clientId, clientSecret, tok.refresh_token, tok.access_token);
      });
    }
  );
  req.on('error', (e) => {
    console.error('换 token 请求失败:', e.message);
    process.exit(1);
  });
  req.write(body);
  req.end();
}

function writeRefresh(clientId, clientSecret, refreshToken, accessToken) {
  const c = JSON.parse(fs.readFileSync(CFG_PATH, 'utf8'));
  c.mail = c.mail || {};
  c.mail.oauth2 = Object.assign({}, c.mail.oauth2, {
    clientId,
    clientSecret,
    refreshToken,
    accessToken: accessToken || '',
    accessUrl: 'https://oauth2.googleapis.com/token'
  });
  c.mail.authType = 'oauth2';
  c.mail.user = c.mail.user || LOGIN_HINT;
  fs.writeFileSync(CFG_PATH, JSON.stringify(c, null, 2));
  console.log('\n✔ 已写入 refresh_token 到 config.local.json（mail.authType=oauth2）。');
  console.log('  现在实发验证：node tools/outlook-test-send.js');
}

function main() {
  const o = loadFromConfig(parseArgs());
  if (!o.clientId || !o.clientSecret) {
    console.error(
      '缺少 clientId / clientSecret。\n用法: node tools/gmail-oauth-setup.js --client-id <ID> --client-secret <SECRET> [--code <CODE>]'
    );
    process.exit(1);
  }
  if (o.code) {
    exchange(o.clientId, o.clientSecret, o.code);
    return;
  }
  const authUrl = buildAuthUrl(o.clientId);
  const server = http.createServer((req, res) => {
    const q = url.parse(req.url, true).query;
    if (q.code) {
      res.end('<html><body style="font-family:sans-serif"><h3>✔ 授权成功，可关闭此页面</h3></body></html>');
      server.close();
      exchange(o.clientId, o.clientSecret, q.code);
    } else if (q.error) {
      res.statusCode = 400;
      res.end('<html><body><h3>授权失败: ' + String(q.error) + '</h3></body></html>');
      server.close();
    } else {
      res.statusCode = 404;
      res.end('not found');
    }
  });
  server.listen(3000, () => {
    console.log('请在浏览器打开下面地址，用 ' + LOGIN_HINT + ' 登录并授权：\n');
    console.log(authUrl);
    console.log('\n授权后会自动回跳到本机 :3000，脚本将自动换取 refresh_token 并写入配置。');
    console.log('（若浏览器不在本机/回跳失败，复制回跳地址里的 ?code=，改用 --code 手动模式。）');
  });
}

main();
