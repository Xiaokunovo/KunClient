/* 请求层：统一带 token、统一错误对象、401 统一收口。
   路径一律写 /api/xxx —— 服务端会把 v1 路径内部重写到 /api/v2/xxx（见 server.js），
   这里不再重复 /v2，避免哪天重写层下线时全站 404。 */
import { state, clearSession } from './state.js';

export class ApiError extends Error {
  constructor(message, extra) {
    super(message);
    this.name = 'ApiError';
    const e = extra || {};
    this.code = e.code;
    this.status = e.status;
    this.data = e.data;
  }
}

export async function api(path, method = 'GET', body) {
  const opt = { method, headers: { 'Content-Type': 'application/json' } };
  if (state.token) opt.headers.Authorization = 'Bearer ' + state.token;
  if (body !== undefined) opt.body = JSON.stringify(body);

  let res, json;
  try {
    res = await fetch('/api' + path, opt);
    json = await res.json().catch(() => ({}));
  } catch (e) {
    throw new ApiError('网络错误，请稍后重试');
  }

  /* 401 只在「本来带着 token」时才当成登录过期。
     登录接口密码错也是 401，但那时 state.token 为空，必须原样把服务端文案抛给调用方。 */
  if (res.status === 401 && state.token) {
    clearSession();
    window.dispatchEvent(new CustomEvent('kun:unauthorized'));
    throw new ApiError('登录已过期，请重新登录', { status: 401 });
  }
  if (!json.success) {
    throw new ApiError(json.message || '请求失败', {
      code: json.code, status: res.status, data: json.data,
    });
  }
  return json;
}

/** 需要原始响应的场景（下载日志包）。 */
export async function apiBlob(path) {
  const res = await fetch('/api' + path, {
    headers: state.token ? { Authorization: 'Bearer ' + state.token } : {},
  });
  if (!res.ok) throw new ApiError('下载失败：HTTP ' + res.status, { status: res.status });
  return res.blob();
}

/** 公开接口（不需要 token，也不走 401 收口）：站点口径、版本号。 */
export async function publicJson(path) {
  try {
    const r = await fetch(path);
    return await r.json();
  } catch (e) {
    return null;
  }
}
