/**
 * 部署/运维脚本的统一软配置。
 * 优先级：环境变量 > server/config.local.json → server/config.json 的 deploy 节 > 内置默认值。
 * 字段：host / port(SSH) / username / password / remoteBase / appPort
 * 可用环境变量：DEPLOY_HOST、DEPLOY_SSH_PORT、DEPLOY_USER、DEPLOY_PASSWORD、
 *              DEPLOY_REMOTE_BASE、DEPLOY_APP_PORT
 * 配置模板见 server/config.example.json 的 deploy 节。
 */
const fs = require('fs');
const path = require('path');

function readJson(file) {
  try {
    if (!fs.existsSync(file)) return {};
    return JSON.parse(fs.readFileSync(file, 'utf8')) || {};
  } catch (e) {
    console.error(`[部署配置] ${path.basename(file)} 读取失败，已忽略该文件: ${e.message}`);
    return {};
  }
}

const ROOT = path.join(__dirname, '..');

// 2026-10-22：server/ 被归档到 _archive/ 之后，下面两行原本直接返回 {}，
// host 静默回落到内置的 127.0.0.1 —— 发版只报 `ECONNREFUSED 127.0.0.1:22`，
// 从错误里完全看不出是「配置没读到」。两处都试一遍。
function readFirst(candidates) {
  for (const rel of candidates) {
    const file = path.join(ROOT, rel);
    if (fs.existsSync(file)) return readJson(file);
  }
  return {};
}

const base = readFirst(['server/config.json', '_archive/server/config.json']);
const local = readFirst(['server/config.local.json', '_archive/server/config.local.json']);
const d = { ...(base.deploy || {}), ...(local.deploy || {}) };
const env = process.env;

const DEPLOY = {
  host: env.DEPLOY_HOST || d.host || '127.0.0.1',
  port: Number(env.DEPLOY_SSH_PORT || d.ssh_port) || 22,
  username: env.DEPLOY_USER || d.user || 'root',
  password: env.DEPLOY_PASSWORD || d.password || '',
  remoteBase: env.DEPLOY_REMOTE_BASE || d.remote_base || '/www/wwwroot/KunClient-server',
  appPort: Number(env.DEPLOY_APP_PORT || local.port || base.port) || 3000,
  // E10：pm2 应用名可配置（deploy.pm2_name / DEPLOY_PM2_NAME）。
  // 留空时 deploy-server.js 会按 cwd 在 pm2 列表里动态找，再回落 'kunclient'。
  pm2Name: env.DEPLOY_PM2_NAME || d.pm2_name || ''
};

module.exports = DEPLOY;