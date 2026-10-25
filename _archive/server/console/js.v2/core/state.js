/* 全局状态与常量 —— 所有视图共享的唯一事实来源。
   与 server.js 的 ROLE_RANK / LEGAL_VERSION 必须一致，改一边就要改另一边。 */

export const LEGAL_VERSION = 6;

// 2026-10-21：代理（agent）角色已删除，只剩三级。数值保留空洞（0/2/3），
// 与服务端 server.js 的 ROLE_RANK 逐字一致（改一边必须改另一边）。
export const ROLE_RANK = { user: 0, admin: 2, developer: 3 };
export const ROLE_LABEL = { user: '普通用户', admin: '管理员', developer: '开发者' };
export const ROLE_BADGE = { user: '', admin: 'info', developer: 'warn' };

/* 环境信号码 → 中文。码名必须与 server.js 的 ENV_SIGNAL_LEVEL 逐字一致，这里只做显示映射。 */
export const ENV_SIGNAL_CN = {
  challenge_fail: '客户端被修改或版本不对',
  nonce_invalid: '上报凭据无效（重复或过期）',
  multi_instance: '同一账号多开',
  bad_hmac: '数据校验失败（疑似被篡改）',
  integrity_fail: '客户端文件被改动',
  debug_bypassed: '反调试被绕过',
  inline_hook: '客户端被注入 Hook',
  injected_module: '加载了可疑第三方模块',
  intercept_tool: '电脑上开着抓包工具',
  proxy_intercept: '系统代理被改到抓包端口',
  root_ca_injected: '系统装了抓包根证书',
  known_tool: '装着逆向/调试工具',
  debugger: '调试器',
  vm: '虚拟机',
  suspicious_module: '可疑模块',
  remote_session: '远程桌面会话',
};

export const state = {
  token: localStorage.getItem('kun_token') || '',
  me: null,
  /* 由 /api/users 回传的 actorRank：管理员/开发者各自的实际等级。
     ME.role 是账号角色，两者理论上一致，但列表接口才是权威（服务端算的）。 */
  actorRank: 0,
  theme: document.documentElement.getAttribute('data-theme') || 'light',
  rail: localStorage.getItem('kun_rail') === 'min' ? 'min' : 'wide',
  route: '',
  /* 各页缓存 */
  keys: [],
  users: [],
  envReports: [],
  envLevel: '',
  appeals: [],
  appealStatus: '',
  clientLogs: [],
  retention: 30,
  modules: null,
  features: [],
  newKey: '',
};

export const rankOf = (role) => (ROLE_RANK[role] === undefined ? 0 : ROLE_RANK[role]);
export const myRank = () => rankOf(state.me && state.me.role);
export const isAdmin = () => myRank() >= 2;

/**
 * 信号码 → 人话（封禁原因 / 环境告警共用）。
 *
 * 服务端存进 ban_reason 的写法有四种：
 *   "intercept_tool: 明细"、"proxy_intercept ×3: 明细"、
 *   "root_ca_injected 组合命中（24h 内 ≥2 类…）"、"手动封禁"
 * 所以只能用 startsWith 匹配信号码，**不能按冒号切第一段**（后两种切不出来）。
 * 命中后**只替换信号码、保留明细** —— 运维排查时明细（端口号 / 进程名 / 证书名）
 * 才是真正有用的部分，整段替换成一句「环境异常」等于把线索扔了。
 */
export function friendlySignalReason(raw) {
  const s = String(raw || '').trim();
  for (const k of Object.keys(ENV_SIGNAL_CN)) {
    if (s === k) return ENV_SIGNAL_CN[k];
    if (s.startsWith(k + ':') || s.startsWith(k + ' ') || s.startsWith(k + '（')) {
      return s.replace(k, ENV_SIGNAL_CN[k]);
    }
  }
  return s;
}
export const isDev = () => myRank() >= 3;

/* 用户名校验的唯一出口。
   与 server.js 的 usernameError() 一一对应：**先判纯数字、再判字符集**。
   `\d` 只认 ASCII，全角 １２３ 会漏过去，所以纯数字判定用 Unicode 属性类。 */
const USERNAME_RE = /^[\u4e00-\u9fffA-Za-z0-9_.\-@]{2,24}$/;
export const USERNAME_ALL_DIGITS_MSG = '用户名不能是纯数字，请混入字母、中文或下划线（例如 kun123）';
export function usernameErr(u) {
  const s = String(u || '').trim();
  if (s && /^[\p{Nd}\p{Nl}\p{No}]+$/u.test(s)) return USERNAME_ALL_DIGITS_MSG;
  if (!USERNAME_RE.test(s)) return '用户名需 2-24 位，支持中文、字母、数字、下划线、中划线、点和 @';
  return null;
}

export function setTheme(t) {
  state.theme = t === 'dark' ? 'dark' : 'light';
  document.documentElement.setAttribute('data-theme', state.theme);
  try { localStorage.setItem('kun_theme', state.theme); } catch (e) { }
}

export function setRail(v) {
  state.rail = v === 'min' ? 'min' : 'wide';
  try { localStorage.setItem('kun_rail', state.rail); } catch (e) { }
}

export function setMe(u) {
  state.me = Object.assign({}, state.me, u || {});
  if (state.me.role) state.actorRank = rankOf(state.me.role);
}

export function saveSession(d) {
  state.token = (d && d.token) || '';
  state.me = {
    username: d && d.username,
    role: d && d.role,
    signature: (d && d.signature) || '',
  };
  state.actorRank = rankOf(state.me.role);
  try {
    localStorage.setItem('kun_token', state.token);
    localStorage.setItem('kun_me', JSON.stringify(state.me));
  } catch (e) { }
}

export function clearSession() {
  state.token = '';
  state.me = null;
  state.actorRank = 0;
  try {
    localStorage.removeItem('kun_token');
    localStorage.removeItem('kun_me');
  } catch (e) { }
}
