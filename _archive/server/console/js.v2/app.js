/* 应用外壳：侧栏、路由、面包屑、主题、用户菜单、命令面板。
   视图只负责往 #viewInner 里画内容，不碰外壳。 */
import { api } from './core/api.js';
import { state, setMe, setTheme, setRail, clearSession, myRank, rankOf, ROLE_LABEL } from './core/state.js';
import { icon, esc, toast, confirmDialog } from './core/ui.js';

/* ── 导航模型 ─────────────────────────────────────────────────────────── */
const NAV = [
  {
    group: '工作台',
    items: [
      { route: 'overview', label: '概览', icon: 'home' },
      { route: 'keys', label: '密钥', icon: 'key' },
      { route: 'feedback', label: '反馈', icon: 'message' },
    ],
  },
  {
    group: '账号',
    items: [
      { route: 'account/profile', label: '资料', icon: 'user' },
      { route: 'account/prefs', label: '偏好', icon: 'sliders' },
      { route: 'account/security', label: '登录与安全', icon: 'shield' },
    ],
  },
  {
    group: '管理',
    items: [
      { route: 'admin/ops', label: '运营', icon: 'chart', rank: 2 },
      { route: 'admin/users', label: '用户', icon: 'users', rank: 2 },   // 代理角色已删，用户管理只对管理员开放
      { route: 'admin/security', label: '安全', icon: 'shieldCheck', rank: 2 },
      // 封禁中心：账号处置 + 来源 IP，权限收在开发者（rank 3），侧栏对管理员不可见
      { route: 'admin/bans', label: '封禁', icon: 'shield', rank: 3 },
    ],
  },
];

let routes = {};
const hooks = { onLogout: null };

const el = {
  login: null, app: null, rail: null, view: null, inner: null,
  crumb: null, whoAv: null, whoName: null, whoRole: null,
};

export function defineRoutes(map) { routes = map; }
export function setHooks(h) { Object.assign(hooks, h || {}); }

export function initShell() {
  el.login = document.getElementById('loginView');
  el.app = document.getElementById('appView');
  el.rail = document.getElementById('railNav');
  el.view = document.getElementById('view');
  el.inner = document.getElementById('viewInner');
  el.crumb = document.getElementById('crumb');
  el.whoAv = document.getElementById('whoAv');
  el.whoName = document.getElementById('whoName');
  el.whoRole = document.getElementById('whoRole');

  el.app.setAttribute('data-rail', state.rail === 'min' ? 'min' : 'wide');
  window.addEventListener('hashchange', () => { if (!el.app.hidden) renderRoute(); });
  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      if (!el.app.hidden) openPalette();
    }
  });
  window.addEventListener('kun:unauthorized', () => {
    clearSession();
    showLoginView();
    toast('登录已过期，请重新登录', 'err');
  });
}

export function showLoginView() {
  el.login.hidden = false;
  el.app.hidden = true;
  document.title = 'KunClient 控制台';
  if (hooks.onLogout) hooks.onLogout();
}

export function showAppView() {
  el.login.hidden = true;
  el.app.hidden = false;
}

/* ── 侧栏 ─────────────────────────────────────────────────────────────── */
export function renderRail() {
  const rank = myRank();
  const active = currentRoute();
  const html = NAV.map((g) => {
    const items = g.items.filter((it) => rank >= (it.rank || 0));
    if (!items.length) return '';
    return `<div class="rail-group"><div class="rail-label">${esc(g.group)}</div>` +
      items.map((it) => {
        const on = it.route === active;
        return `<button class="rail-item ${on ? 'on' : ''}" data-act="nav.go" data-route="${it.route}" title="${esc(it.label)}"${on ? ' aria-current="page"' : ''}>` +
          `${icon(it.icon, 16)}<span>${esc(it.label)}</span></button>`;
      }).join('') + '</div>';
  }).join('');
  el.rail.innerHTML = html;
}

function setCrumb(def) {
  const group = def.group || '';
  el.crumb.innerHTML = `<span>控制台</span>` +
    (group ? `<span class="sep">/</span><span>${esc(group)}</span>` : '') +
    `<span class="sep">/</span><b>${esc(def.label || def.title || '')}</b>`;
}

export function currentRoute() {
  const h = location.hash.replace(/^#\/?/, '').replace(/\/$/, '');
  return routes[h] ? h : 'overview';
}

export function go(route, opts) {
  const o = opts || {};
  const target = routes[route] ? route : 'overview';
  if (o.replace) {
    const url = location.pathname + location.search + '#/' + target;
    history.replaceState(null, '', url);
    if (!el.app.hidden) renderRoute();
    return;
  }
  if (currentRoute() === target && location.hash) {
    renderRoute();
    return;
  }
  location.hash = '#/' + target;
}

/** 重新渲染当前页（数据刷新用，不改变路由）。 */
export function refresh() {
  renderRoute();
}

async function renderRoute() {
  const route = currentRoute();
  const def = routes[route];
  if (!def) { go('overview', { replace: true }); return; }
  if (myRank() < (def.rank || 0)) {
    toast('权限不足，无法访问该页面', 'err');
    go('overview', { replace: true });
    return;
  }
  state.route = route;
  renderRail();
  setCrumb(def);
  document.title = (def.label || def.title || '控制台') + ' · KunClient';
  el.inner.innerHTML = '';
  el.view.scrollTop = 0;
  try {
    await def.render({ el: el.inner, route });
  } catch (e) {
    el.inner.innerHTML = `<div class="card"><div class="empty">` +
      `<b>页面加载失败</b>${esc(e && e.message ? e.message : '未知错误')}</div></div>`;
  }
}

/* ── 用户信息 ─────────────────────────────────────────────────────────── */
export function paintWho() {
  const me = state.me || {};
  el.whoName.textContent = me.username || '-';
  el.whoRole.textContent = ROLE_LABEL[me.role] || '普通用户';
  renderAvatar(el.whoAv, 26);
}

export function renderAvatar(node, size) {
  if (!node) return;
  const me = state.me || {};
  node.style.width = (size || 26) + 'px';
  node.style.height = (size || 26) + 'px';
  if (me.avatar) {
    node.innerHTML = `<img src="${esc(me.avatar)}" alt="">`;
  } else {
    node.textContent = String(me.username || '?').slice(0, 1).toUpperCase();
  }
}

/** 改用户名 / 改密码后服务端会让旧 token 失效：这里统一收口，回到登录页。 */
export function forceLogout(msg) {
  clearSession();
  showLoginView();
  if (msg) toast(msg);
}

export async function enterApp() {
  let j;
  try {
    j = await api('/profile');
  } catch (e) {
    clearSession();
    showLoginView();
    return false;
  }
  setMe(j.data);
  showAppView();
  paintWho();
  renderRail();
  if (!location.hash) go('overview', { replace: true });
  else renderRoute();
  return true;
}

/* ── 主题 / 侧栏 / 退出 ───────────────────────────────────────────────── */
function toggleTheme() {
  setTheme(state.theme === 'dark' ? 'light' : 'dark');
}

function toggleRail() {
  const next = state.rail === 'min' ? 'wide' : 'min';
  setRail(next);
  el.app.setAttribute('data-rail', next);
  const btn = document.querySelector('.rail-toggle');
  if (btn) {
    btn.title = next === 'min' ? '展开侧栏' : '收起侧栏';
    btn.setAttribute('aria-label', btn.title);
  }
}

async function logout() {
  const ok = await confirmDialog({
    title: '退出登录',
    message: '退出后需要重新输入密码才能进入控制台。',
    okText: '退出',
  });
  if (!ok) return;
  clearSession();
  showLoginView();
  toast('已退出登录');
}

/* ── 用户菜单 ─────────────────────────────────────────────────────────── */
let menuEl = null;
function closeMenu() {
  if (menuEl) { menuEl.remove(); menuEl = null; }
}
function openUserMenu(anchor) {
  closeMenu();
  const m = document.createElement('div');
  m.className = 'menu';
  m.innerHTML =
    `<button class="menu-item" data-m="account">${icon('user', 14)}账号设置</button>` +
    `<button class="menu-item" data-m="theme">${icon(state.theme === 'dark' ? 'sun' : 'moon', 14)}切换到${state.theme === 'dark' ? '浅色' : '深色'}主题</button>` +
    `<button class="menu-item" data-m="copyid">${icon('copy', 14)}复制我的 ID</button>` +
    `<div class="menu-sep"></div>` +
    `<button class="menu-item danger" data-m="logout">${icon('logout', 14)}退出登录</button>`;
  document.body.appendChild(m);
  const r = anchor.getBoundingClientRect();
  m.style.top = (r.bottom + 6) + 'px';
  m.style.right = Math.max(8, window.innerWidth - r.right) + 'px';
  menuEl = m;

  m.addEventListener('click', (e) => {
    const b = e.target.closest('[data-m]');
    if (!b) return;
    const k = b.dataset.m;
    closeMenu();
    if (k === 'account') go('account/profile');
    if (k === 'theme') toggleTheme();
    if (k === 'copyid') {
      const id = state.me && state.me.id;
      if (id) navigator.clipboard.writeText('#' + id).then(() => toast('已复制 #' + id, 'ok'));
    }
    if (k === 'logout') logout();
  });
  setTimeout(() => {
    document.addEventListener('mousedown', function outside(ev) {
      if (!m.contains(ev.target)) { closeMenu(); document.removeEventListener('mousedown', outside); }
    });
  }, 0);
}

/* ── 命令面板 ─────────────────────────────────────────────────────────── */
let paletteOpen = false;
function paletteItems() {
  const rank = myRank();
  const out = [];
  NAV.forEach((g) => {
    g.items.filter((it) => rank >= (it.rank || 0)).forEach((it) => {
      out.push({ group: g.group, label: it.label, icon: it.icon, hint: '页面', run: () => go(it.route) });
    });
  });
  out.push({ group: '操作', label: '刷新当前页', icon: 'refresh', hint: 'R', run: () => refresh() });
  out.push({ group: '操作', label: '切换主题', icon: state.theme === 'dark' ? 'sun' : 'moon', hint: '', run: () => toggleTheme() });
  out.push({ group: '操作', label: '生成一次性密钥', icon: 'key', hint: '', run: () => go('keys') });
  if (rank >= 2) out.push({ group: '操作', label: '发布公告', icon: 'message', hint: '运营', run: () => go('admin/ops') });
  if (rank >= 1) out.push({ group: '操作', label: '新增用户', icon: 'plus', hint: '管理', run: () => go('admin/users') });
  out.push({ group: '操作', label: '退出登录', icon: 'logout', hint: '', run: () => logout() });
  return out;
}

export function openPalette() {
  if (paletteOpen) return;
  paletteOpen = true;
  const all = paletteItems();
  let list = all;
  let sel = 0;

  const mask = document.createElement('div');
  mask.className = 'mask palette-mask';
  mask.innerHTML =
    `<div class="palette" role="dialog" aria-modal="true">` +
      `<div class="palette-input">${icon('search', 16)}` +
        `<input id="palQ" placeholder="搜索页面或操作…" autocomplete="off" spellcheck="false">` +
        `<kbd class="kbd">Esc</kbd></div>` +
      `<div class="palette-list" id="palList"></div>` +
      `<div class="palette-foot"><span><kbd class="kbd">↑</kbd> <kbd class="kbd">↓</kbd> 选择</span>` +
        `<span><kbd class="kbd">Enter</kbd> 打开</span><span class="sp"></span><span>KunClient 控制台</span></div>` +
    `</div>`;
  document.body.appendChild(mask);
  document.body.style.overflow = 'hidden';

  const input = mask.querySelector('#palQ');
  const listEl = mask.querySelector('#palList');

  const close = () => {
    paletteOpen = false;
    mask.remove();
    document.body.style.overflow = '';
    document.removeEventListener('keydown', onKey, true);
  };

  function draw() {
    let lastGroup = '';
    listEl.innerHTML = list.length ? list.map((it, i) => {
      const head = it.group !== lastGroup ? `<div class="palette-group">${esc(it.group)}</div>` : '';
      lastGroup = it.group;
      return head + `<button class="palette-item ${i === sel ? 'sel' : ''}" data-i="${i}">` +
        `${icon(it.icon, 15)}<span>${esc(it.label)}</span>` +
        (it.hint ? `<span class="hint">${esc(it.hint)}</span>` : '') + `</button>`;
    }).join('') : `<div class="empty">没有匹配的页面或操作</div>`;
    const cur = listEl.querySelector('.palette-item.sel');
    if (cur) cur.scrollIntoView({ block: 'nearest' });
  }

  function filter(q) {
    const s = q.trim().toLowerCase();
    list = s ? all.filter((it) => (it.label + ' ' + it.group + ' ' + (it.hint || '')).toLowerCase().includes(s)) : all;
    sel = 0;
    draw();
  }

  function pick(i) {
    const it = list[i];
    if (!it) return;
    close();
    it.run();
  }

  function onKey(e) {
    if (e.key === 'Escape') { e.preventDefault(); close(); return; }
    if (e.key === 'ArrowDown') { e.preventDefault(); sel = Math.min(sel + 1, list.length - 1); draw(); return; }
    if (e.key === 'ArrowUp') { e.preventDefault(); sel = Math.max(sel - 1, 0); draw(); return; }
    if (e.key === 'Enter') { e.preventDefault(); pick(sel); }
  }

  input.addEventListener('input', () => filter(input.value));
  listEl.addEventListener('click', (e) => {
    const b = e.target.closest('[data-i]');
    if (b) pick(Number(b.dataset.i));
  });
  mask.addEventListener('mousedown', (e) => { if (e.target === mask) close(); });
  document.addEventListener('keydown', onKey, true);
  draw();
  setTimeout(() => input.focus(), 20);
}

/* ── 外壳动作注册 ─────────────────────────────────────────────────────── */
import { registerActions } from './core/actions.js';
registerActions({
  'nav.go': (b) => go(b.dataset.route),
  'shell.theme': () => toggleTheme(),
  'shell.rail': () => toggleRail(),
  'shell.logout': () => logout(),
  'shell.usermenu': (b) => openUserMenu(b),
  'palette.open': () => openPalette(),
});
