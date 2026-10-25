/**
 * 统一软配置（软口）
 * 优先级：环境变量 > server/config.local.json > server/config.json > 内置中性默认值
 *
 * - 人为修改：编辑同目录 config.json（公共默认值）；机器/环境私有值放 config.local.json
 *   （可选，优先级更高，用于本地覆盖，建议不入库）。模板见 config.example.json。
 * - 临时覆盖/运维：使用环境变量（PORT、JWT_SECRET、PUBLIC_URL、DB_PATH、ADMIN_USER、
 *   ADMIN_PASS、ADMIN_EMAIL、MAIL_HOST、MAIL_PORT、MAIL_SECURE、MAIL_USER、MAIL_PASS、
 *   MAIL_FROM、MAIL_DISABLE、MAIL_AUTH_TYPE、MAIL_OAUTH_CLIENT_ID、MAIL_OAUTH_CLIENT_SECRET、
 *   MAIL_OAUTH_REFRESH_TOKEN、JWT_SECRET_FILE）。
 * - 两个配置文件不存在或字段缺失时回退内置默认值，保证服务始终可启动。
 * - 敏感项不再硬编码在代码中：jwt_secret 为空时自动生成并持久化到 .runtime/jwt.secret；
 *   admin.password 为空时跳过管理员账号播种/同步。
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT = __dirname;
const BASE_CONFIG_FILE = path.join(ROOT, 'config.json');
const LOCAL_CONFIG_FILE = path.join(ROOT, 'config.local.json');
const SECRET_FILE = process.env.JWT_SECRET_FILE || path.join(ROOT, '.runtime', 'jwt.secret');

const DEFAULTS = {
  port: 3000,
  jwt_secret: '',
  public_url: '',
  db_path: '',
  admin: { username: 'admin', password: '', sync: false },
  developer: { username: '', password: '', sync: false },
  admin_email: '',
  mail: {
    enabled: false,
    host: 'smtp.qq.com',
    port: 465,
    secure: true,
    user: '',
    pass: '',
    from: '',
    // 认证方式：'password'（基础认证，QQ 邮箱/自建 SMTP）；'oauth2'（现代认证，Outlook.com / Microsoft 365 必需）
    authType: 'password',
    // 发送通道：'smtp'（默认，走 host/port 的 SMTP）；'graph'（Microsoft Graph /me/sendMail ——
    // 个人 Outlook.com 的 SMTP/IMAP 若被微软账号层面拒绝（535 5.7.3 / authenticated but not connected），
    // Graph 是官方推荐路径，不受 IMAP/SMTP 协议开关影响，只需 mail.oauth2 的 refreshToken 带 Graph Mail.Send 权限）
    mode: 'smtp',
    // authType='oauth2' 时使用。accessToken 可留空，nodemailer 会用 refreshToken 自动换取。
    oauth2: { clientId: '', clientSecret: '', refreshToken: '', accessToken: '' }
  }
};

/** 读取 JSON 配置文件；optional=true 时解析失败仅告警 */
function readJson(file, optional) {
  try {
    if (!fs.existsSync(file)) return {};
    return JSON.parse(fs.readFileSync(file, 'utf8')) || {};
  } catch (e) {
    if (optional) console.warn(`[配置] ${path.basename(file)} 解析失败，已忽略: ${e.message}`);
    else console.error(`[配置] ${path.basename(file)} 解析失败，已忽略并使用默认配置: ${e.message}`);
    return {};
  }
}

/** jwt_secret 缺失时：复用已持久化密钥，否则自动生成并落盘（保证重启后登录态仍有效） */
function ensureJwtSecret(secret) {
  if (secret) return secret;
  try {
    if (fs.existsSync(SECRET_FILE)) {
      const saved = String(fs.readFileSync(SECRET_FILE, 'utf8')).trim();
      if (saved) return saved;
    }
    const generated = crypto.randomBytes(32).toString('hex');
    fs.mkdirSync(path.dirname(SECRET_FILE), { recursive: true });
    fs.writeFileSync(SECRET_FILE, generated, { mode: 0o600 });
    console.warn(`[配置] 未配置 jwt_secret，已自动生成并写入 ${SECRET_FILE}（可用 JWT_SECRET 环境变量或 config.json 覆盖）`);
    return generated;
  } catch (e) {
    console.warn(`[配置] 未配置 jwt_secret 且自动落盘失败(${e.message})，本次运行使用临时密钥（重启后登录态失效）`);
    return crypto.randomBytes(32).toString('hex');
  }
}

function load() {
  const file = readJson(BASE_CONFIG_FILE, false);
  const local = readJson(LOCAL_CONFIG_FILE, true);
  const env = process.env;
  const c = { ...DEFAULTS, ...file, ...local };
  c.admin = { ...DEFAULTS.admin, ...(file.admin || {}), ...(local.admin || {}) };
  c.developer = { ...DEFAULTS.developer, ...(file.developer || {}), ...(local.developer || {}) };
  c.mail = { ...DEFAULTS.mail, ...(file.mail || {}), ...(local.mail || {}) };
  // oauth2 是嵌套对象，需单独深合并（浅合并会被 local 的整体覆盖掉）
  c.mail.oauth2 = {
    ...DEFAULTS.mail.oauth2,
    ...((file.mail && file.mail.oauth2) || {}),
    ...((local.mail && local.mail.oauth2) || {})
  };

  if (env.PORT) c.port = Number(env.PORT) || c.port;
  if (env.JWT_SECRET) c.jwt_secret = env.JWT_SECRET;
  if (env.PUBLIC_URL) c.public_url = env.PUBLIC_URL;
  if (env.DB_PATH) c.db_path = env.DB_PATH;
  if (env.ADMIN_USER) c.admin.username = env.ADMIN_USER;
  if (env.ADMIN_PASS) c.admin.password = env.ADMIN_PASS;
  if (env.DEVELOPER_USER) c.developer.username = env.DEVELOPER_USER;
  if (env.DEVELOPER_PASS) c.developer.password = env.DEVELOPER_PASS;
  if (env.ADMIN_EMAIL) c.admin_email = env.ADMIN_EMAIL;
  if (env.MAIL_HOST) c.mail.host = env.MAIL_HOST;
  if (env.MAIL_PORT) c.mail.port = Number(env.MAIL_PORT) || c.mail.port;
  if (env.MAIL_SECURE) c.mail.secure = !(env.MAIL_SECURE === '0' || env.MAIL_SECURE === 'false');
  if (env.MAIL_USER) c.mail.user = env.MAIL_USER;
  if (env.MAIL_PASS) c.mail.pass = env.MAIL_PASS;
  if (env.MAIL_FROM) c.mail.from = env.MAIL_FROM;
  if (env.MAIL_AUTH_TYPE) c.mail.authType = env.MAIL_AUTH_TYPE;
  if (env.MAIL_OAUTH_CLIENT_ID) c.mail.oauth2.clientId = env.MAIL_OAUTH_CLIENT_ID;
  if (env.MAIL_OAUTH_CLIENT_SECRET) c.mail.oauth2.clientSecret = env.MAIL_OAUTH_CLIENT_SECRET;
  if (env.MAIL_OAUTH_REFRESH_TOKEN) c.mail.oauth2.refreshToken = env.MAIL_OAUTH_REFRESH_TOKEN;
  if (env.MAIL_DISABLE === '1' || env.MAIL_DISABLE === 'true') c.mail.enabled = false;

  c.port = Number(c.port) || DEFAULTS.port;
  c.jwt_secret = ensureJwtSecret(String(c.jwt_secret || '').trim());
  c.publicUrl = String(c.public_url || '').replace(/\/+$/, '') || `http://localhost:${c.port}`;
  c.dbFile = c.db_path ? path.resolve(ROOT, c.db_path) : path.join(ROOT, 'database.sqlite');

  if (c.mail.enabled) {
    const mailAuthType = String(c.mail.authType || 'password').toLowerCase();
    if (mailAuthType === 'oauth2') {
      if (!c.mail.user || !c.mail.oauth2.clientId || !c.mail.oauth2.refreshToken) {
        console.warn('[配置] mail.authType=oauth2 但 mail.user / oauth2.clientId / oauth2.refreshToken 未齐全，发信会失败');
      }
    } else if (!c.mail.user || !c.mail.pass) {
      console.warn('[配置] mail.enabled=true 但 mail.user / mail.pass 未配置，发信会失败；如需关闭请设 mail.enabled=false 或环境变量 MAIL_DISABLE=1');
    }
  }
  if (!String(c.admin.password || '').trim()) {
    console.warn('[配置] admin.password 未配置（config.json → admin.password 或环境变量 ADMIN_PASS），将跳过管理员账号播种/同步');
  }
  return c;
}

module.exports = load();
