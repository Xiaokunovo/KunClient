/* UI 原语：图标 / 格式化 / toast / 弹窗 / 确认框 / 按钮忙态 / 复制。
   原则：所有拼进 innerHTML 的服务端字符串必须先过 esc()；事件一律用 data-* + 委托，
   绝不把变量拼进 onclick 字面量（引号会截断 JS，这是老控制台反复踩过的坑）。 */

/* ── 图标：24×24 线性，统一 1.7 描边 ─────────────────────────────────── */
const P = {
  home: '<path d="M3 10.5L12 3l9 7.5M5.5 9.5V21h5v-5.5h3V21h5V9.5"/>',
  key: '<circle cx="8" cy="15" r="4.2"/><path d="M11 12L20.5 2.5M16.5 6.5l3 3M19 4l2.5 2.5"/>',
  message: '<path d="M4 4h16v12H9l-5 4z"/>',
  user: '<circle cx="12" cy="8" r="4"/><path d="M4.5 21c.5-4.4 3.8-6.5 7.5-6.5s7 2.1 7.5 6.5"/>',
  crown: '<path d="M3 8l4.5 4L12 5l4.5 7L21 8l-1.6 11H4.6z"/>',
  shield: '<path d="M12 3l8 3v6c0 4.8-3.4 8-8 9-4.6-1-8-4.2-8-9V6z"/>',
  shieldCheck: '<path d="M12 3l8 3v6c0 4.8-3.4 8-8 9-4.6-1-8-4.2-8-9V6z"/><path d="M8.5 12l2.5 2.5 4.5-5"/>',
  chart: '<path d="M4 20V10M10 20V4M16 20v-7M22 20H2"/>',
  users: '<circle cx="9" cy="8" r="3.4"/><path d="M2.5 20c.4-3.7 3-5.5 6.5-5.5s6.1 1.8 6.5 5.5"/><path d="M16 5.2a3.4 3.4 0 0 1 0 6.6M18 14.6c2.2.5 3.4 2.3 3.6 5.4"/>',
  ticket: '<path d="M3 9V6h18v3a3 3 0 0 0 0 6v3H3v-3a3 3 0 0 0 0-6z"/><path d="M12 8v1M12 12v1M12 16v1"/>',
  terminal: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7.5 9.5l3 2.5-3 2.5M13 15h4"/>',
  activity: '<path d="M3 12h3.5l2.2-6.5L12.5 18l2.3-6H21"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="M16.5 16.5L21 21"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  refresh: '<path d="M20.5 12a8.5 8.5 0 1 1-2.5-6M20.5 3.5V10h-6.5"/>',
  trash: '<path d="M4 7h16M9.5 7V4h5v3M6.5 7l1 13h9l1-13"/>',
  copy: '<rect x="9" y="9" width="11" height="11" rx="1.6"/><path d="M5 15V5h10"/>',
  check: '<path d="M4.5 12.5l4.5 4.5L19.5 6.5"/>',
  x: '<path d="M6 6l12 12M18 6L6 18"/>',
  download: '<path d="M12 4v11m0 0l-4-4m4 4l4-4M4 20h16"/>',
  upload: '<path d="M12 20V9m0 0L8 13m4-4l4 4M4 4h16"/>',
  external: '<path d="M14 4h6v6M20 4l-9 9"/><path d="M18 14v6H4V6h6"/>',
  logout: '<path d="M12 3v8M6.3 6.5a8 8 0 1 0 11.4 0"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4"/>',
  moon: '<path d="M20 13.2A8.2 8.2 0 1 1 10.8 4a6.6 6.6 0 0 0 9.2 9.2z"/>',
  chevronRight: '<path d="M9 5l7 7-7 7"/>',
  chevronDown: '<path d="M6 9l6 6 6-6"/>',
  alert: '<path d="M12 3.5l9 16H3z"/><path d="M12 9.5v4.5"/><circle cx="12" cy="17" r=".6" fill="currentColor"/>',
  filter: '<path d="M4 5h16l-6.2 7.2V19l-3.6 2v-8.8z"/>',
  more: '<circle cx="5" cy="12" r="1.3" fill="currentColor" stroke="none"/><circle cx="12" cy="12" r="1.3" fill="currentColor" stroke="none"/><circle cx="19" cy="12" r="1.3" fill="currentColor" stroke="none"/>',
  edit: '<path d="M4 20h4L18.5 9.5 14.5 5.5 4 16z"/><path d="M13 7l4 4"/>',
  lock: '<rect x="4" y="10" width="16" height="10" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>',
  mail: '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M3 7l9 6 9-6"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7v5.2l3.4 2"/>',
  globe: '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.4 2.3 3.6 5.1 3.6 8.5s-1.2 6.2-3.6 8.5c-2.4-2.3-3.6-5.1-3.6-8.5s1.2-6.2 3.6-8.5z"/>',
  file: '<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/>',
  sliders: '<path d="M4 7h16M4 12h16M4 17h16"/><circle cx="9" cy="7" r="2"/><circle cx="15" cy="12" r="2"/><circle cx="8" cy="17" r="2"/>',
  zap: '<path d="M13.5 2.5L4 14h6l-1 7.5L20 10h-6z"/>',
  eye: '<path d="M2.5 12S6 6 12 6s9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6z"/><circle cx="12" cy="12" r="2.6"/>',
  inbox: '<path d="M3 12.5L5.8 4h12.4L21 12.5V20H3z"/><path d="M3 12.5h5l1.6 2.6h4.8l1.6-2.6h5"/>',
  dot: '<circle cx="12" cy="12" r="3" fill="currentColor" stroke="none"/>',
};

export function icon(name, size) {
  const s = size || 16;
  return `<svg viewBox="0 0 24 24" width="${s}" height="${s}" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${P[name] || P.dot}</svg>`;
}

/* ── 格式化 ───────────────────────────────────────────────────────────── */
export const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => (
  { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
));

export function fmtDate(t) {
  if (!t) return '—';
  const s = String(t);
  const d = new Date(s.includes('T') || s.includes('Z') ? s : s.replace(' ', 'T') + 'Z');
  if (isNaN(d)) return s;
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

export function fmtDateShort(t) {
  if (!t) return '—';
  const full = fmtDate(t);
  return full.length >= 10 ? full.slice(5) : full;
}

export function fmtSize(n) {
  const b = Number(n) || 0;
  if (!b) return '—';
  const u = ['B', 'KB', 'MB', 'GB'];
  let v = b, i = 0;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (i ? v.toFixed(1) : v) + ' ' + u[i];
}

export function badge(text, kind) {
  return `<span class="badge ${kind || ''}">${esc(text)}</span>`;
}

export function empty(title, sub, iconName) {
  return `<div class="empty">${icon(iconName || 'inbox', 22)}<b>${esc(title)}</b>${sub ? esc(sub) : ''}</div>`;
}

export function loading(text) {
  return `<div class="empty"><span class="skeleton" style="display:block;width:120px;margin:0 auto 10px"></span>${esc(text || '加载中…')}</div>`;
}

/* ── Toast ────────────────────────────────────────────────────────────── */
const TOAST_ICON = { ok: 'check', err: 'alert', warn: 'alert', '': 'dot' };

export function toast(msg, kind) {
  const root = document.getElementById('toasts');
  if (!root) return;
  const k = kind || '';
  const el = document.createElement('div');
  el.className = 'toast ' + k;
  el.innerHTML = `<span class="ic">${icon(TOAST_ICON[k] || 'dot', 15)}</span><span>${esc(msg)}</span>`;
  root.appendChild(el);
  while (root.children.length > 4) root.removeChild(root.firstChild);
  const life = k === 'err' ? 4200 : 2600;
  setTimeout(() => {
    el.classList.add('out');
    setTimeout(() => el.remove(), 200);
  }, life);
}

/* ── 弹窗 ─────────────────────────────────────────────────────────────── */
const modalStack = [];

function onDocKey(e) {
  if (e.key !== 'Escape') return;
  const top = modalStack[modalStack.length - 1];
  if (top && top.dismissable) {
    e.stopPropagation();
    top.close();
  }
}
document.addEventListener('keydown', onDocKey, true);

/**
 * @param {{title:string, body?:string, footer?:string, width?:string,
 *          onMount?:(api)=>void, onClose?:()=>void, dismissable?:boolean}} o
 */
export function openModal(o) {
  const root = document.getElementById('modalRoot');
  const mask = document.createElement('div');
  mask.className = 'mask';
  const wide = o.width === 'wide' ? ' wide' : (o.width === 'narrow' ? ' narrow' : '');
  mask.innerHTML =
    `<div class="modal${wide}" role="dialog" aria-modal="true">` +
      `<div class="modal-h"><span class="t">${esc(o.title || '')}</span>` +
        `<button class="icon-btn x" data-x aria-label="关闭">${icon('x', 16)}</button></div>` +
      `<div class="modal-b"></div>` +
      (o.footer ? `<div class="modal-f">${o.footer}</div>` : '') +
    `</div>`;
  const box = mask.querySelector('.modal');
  const body = mask.querySelector('.modal-b');
  const foot = mask.querySelector('.modal-f');
  body.innerHTML = o.body || '';

  let closed = false;
  const api = {
    el: box,
    body,
    footer: foot,
    close() {
      if (closed) return;
      closed = true;
      const i = modalStack.indexOf(api);
      if (i >= 0) modalStack.splice(i, 1);
      mask.remove();
      if (!modalStack.length) document.body.style.overflow = '';
      if (o.onClose) o.onClose();
    },
  };
  api.dismissable = o.dismissable !== false;

  mask.addEventListener('mousedown', (e) => {
    if (e.target === mask && api.dismissable) api.close();
  });
  mask.querySelector('[data-x]').addEventListener('click', () => api.close());

  root.appendChild(mask);
  document.body.style.overflow = 'hidden';
  modalStack.push(api);
  if (o.onMount) o.onMount(api);
  const focusable = box.querySelector('input, select, textarea, button.pri');
  if (focusable) setTimeout(() => focusable.focus(), 30);
  return api;
}

/** Promise 版确认框。替代原生 confirm()：能带标题、能换按钮文案、深色主题下也不刺眼。 */
export function confirmDialog(o) {
  const opt = typeof o === 'string' ? { message: o } : (o || {});
  return new Promise((resolve) => {
    let done = false;
    const finish = (v) => {
      if (done) return;
      done = true;
      resolve(v);
    };
    const m = openModal({
      title: opt.title || '确认操作',
      width: 'narrow',
      body: `<p style="font-size:12.5px;line-height:1.75;color:var(--ink-2);white-space:pre-wrap">${esc(opt.message || '')}</p>`,
      footer: `<div class="sp"></div><button class="btn" data-no>取消</button>` +
        `<button class="btn ${opt.danger ? 'danger' : 'pri'}" data-yes>${esc(opt.okText || '确认')}</button>`,
      onClose: () => finish(false),
    });
    m.footer.querySelector('[data-no]').addEventListener('click', () => m.close());
    m.footer.querySelector('[data-yes]').addEventListener('click', () => {
      finish(true);
      m.close();
    });
    setTimeout(() => {
      const b = m.footer.querySelector('[data-yes]');
      if (b) b.focus();
    }, 30);
  });
}

/* ── 按钮忙态 ─────────────────────────────────────────────────────────── */
export function busy(btn, on, text) {
  if (!btn) return;
  if (on) {
    if (!btn.dataset.idleText) btn.dataset.idleText = btn.textContent.trim();
    btn.disabled = true;
    if (text) btn.textContent = text;
  } else {
    btn.disabled = false;
    if (btn.dataset.idleText) {
      btn.textContent = btn.dataset.idleText;
      delete btn.dataset.idleText;
    } else if (text) {
      btn.textContent = text;
    }
  }
}

/** 发送验证码倒计时：按钮禁用 60 秒，显示剩余秒数。 */
export function countdown(btn, base, seconds) {
  if (!btn) return;
  let s = seconds || 60;
  const total = s;
  const label = base || '重新发送';
  btn.disabled = true;
  btn.textContent = s + 's';
  clearInterval(btn._cd);
  btn._cd = setInterval(() => {
    s -= 1;
    if (s <= 0) {
      clearInterval(btn._cd);
      btn.disabled = false;
      btn.textContent = label;
    } else {
      btn.textContent = s + 's';
    }
  }, 1000);
  return total;
}

/* ── 复制 ─────────────────────────────────────────────────────────────── */
export async function copyText(text, okMsg) {
  const s = String(text == null ? '' : text);
  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(s);
    } else {
      const ta = document.createElement('textarea');
      ta.value = s;
      ta.style.cssText = 'position:fixed;top:-999px;opacity:0';
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      ta.remove();
    }
    toast(okMsg || '已复制到剪贴板', 'ok');
  } catch (e) {
    toast('复制失败，请手动选中', 'err');
  }
}

/* ── 小组件 ───────────────────────────────────────────────────────────── */
export function statCell(label, value, opts) {
  const o = opts || {};
  return `<div class="stat"><div class="k">${esc(label)}</div>` +
    `<div class="v ${o.cls || ''}">${o.html ? value : esc(value)}</div></div>`;
}

export function switchRow(title, desc, on, act, attrs) {
  const a = attrs || '';
  return `<div class="set-row"><div style="flex:1;min-width:0">` +
    `<div class="t">${title}</div>${desc ? `<div class="d">${desc}</div>` : ''}</div>` +
    `<button class="sw ${on ? 'on' : ''}" data-act="${act}" ${a} role="switch" aria-checked="${on ? 'true' : 'false'}"></button></div>`;
}

export function field(label, inputHtml, hint) {
  return `<div class="field"><label>${label}</label>${inputHtml}${hint ? `<div class="hint">${hint}</div>` : ''}</div>`;
}
