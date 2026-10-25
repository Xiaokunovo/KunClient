/* 管理 › 用户：用户管理、反馈管理、登录日志 + 两个弹窗。
   权限分两条线：路由/卡片靠 rank（管理员 2 / 开发者 3），
   单行操作靠服务端下发的 canManage（同级或更高等级只读）。 */
import { api } from '../../core/api.js';
import {
  state, myRank, ROLE_LABEL, ROLE_BADGE, usernameErr,
} from '../../core/state.js';
import { registerActions } from '../../core/actions.js';
import {
  icon, esc, fmtDate, toast, busy, empty, loading,
  confirmDialog, openModal, copyText,
} from '../../core/ui.js';
import { pageHead, card } from '../../core/layout.js';

const $ = (id) => document.getElementById(id);

let USERS = [];
let MODAL_USER = null;
let userModalApi = null;

/* ══════════════ 用户列表 ══════════════ */
function statusCell(u) {
  if (u.banActive) {
    if (u.banKind === 'manual') {
      return `<span class="badge bad" title="管理员手工永久封禁"><span class="dot"></span>已封禁 · 永久</span>`;
    }
    const until = u.banUntil ? String(u.banUntil).slice(0, 10) : '';
    return `<span class="badge bad" title="环境检测自动封禁${until ? '，到期 ' + until : ''}"><span class="dot"></span>环境封禁${until ? ' · ' + until : ''}</span>`;
  }
  return u.status === 'active'
    ? `<span class="badge ok"><span class="dot"></span>正常</span>`
    : `<span class="badge"><span class="dot"></span>禁用</span>`;
}

function onlineCell(u) {
  if (u.online) return `<span class="badge ok"><span class="dot"></span>在线</span>`;
  const seen = u.lastSeenAt || u.last_login;
  return seen ? `<span class="badge"><span class="dot"></span>离线</span>` : `<span class="muted">—</span>`;
}

function renderUsers() {
  const box = $('admUsers');
  if (!box) return;
  const q = ($('usrSearch') ? $('usrSearch').value : '').trim().toLowerCase();
  const st = $('usrStatusSel') ? $('usrStatusSel').value : '';
  const role = $('usrRoleSel') ? $('usrRoleSel').value : '';

  const matchStatus = (u) => {
    if (!st) return true;
    if (st === 'active') return !u.banActive && u.status === 'active';
    if (st === 'banned-env') return u.banActive && u.banKind !== 'manual';
    if (st === 'banned-manual') return u.banActive && u.banKind === 'manual';
    if (st === 'disabled') return u.status === 'disabled';
    return true;
  };
  const filtered = USERS.filter((u) => {
    if (q && !((u.username || '').toLowerCase().includes(q) ||
      (u.email || '').toLowerCase().includes(q) ||
      String(u.id).includes(q))) return false;
    if (role && u.role !== role) return false;
    return matchStatus(u);
  });

  if (!USERS.length) {
    box.innerHTML = empty(myRank() < 2 ? '名下暂无用户' : '暂无用户', '', 'users');
    return;
  }
  if (!filtered.length) {
    box.innerHTML = empty('没有符合筛选条件的用户', '换个关键词或清空筛选条件试试。', 'filter');
    return;
  }

  box.innerHTML = `<div class="tbl-wrap"><table class="tbl"><thead><tr>` +
    `<th style="width:60px">ID</th><th>用户名</th><th>邮箱</th><th style="width:110px">角色</th>` +
    `<th style="width:150px">状态</th>` +
    `<th style="width:74px">在线</th><th style="width:130px">最近登录</th><th style="width:300px"></th>` +
    `</tr></thead><tbody>` +
    filtered.map((u) => {
      const ro = u.canManage === false;
      const lock = ro ? `disabled title="权限不足：不能管理同级或更高等级"` : '';
      const roleBadge = `<span class="badge ${ROLE_BADGE[u.role] || ''}">${esc(ROLE_LABEL[u.role] || '普通用户')}</span>` +
        (ro ? ` <span class="badge">只读</span>` : '');
      const unban = (u.banActive && u.banKind !== 'manual' && myRank() >= 3)
        ? `<button class="btn sm" data-act="au.unban" data-id="${Number(u.id)}">解封</button>` : '';
      return `<tr>` +
        `<td class="mono muted">#${Number(u.id)}</td>` +
        `<td class="strong">${esc(u.username)}</td>` +
        `<td class="muted">${esc(u.email || '—')}</td>` +
        `<td>${roleBadge}</td>` +
        `<td>${statusCell(u)}</td>` +
        `<td>${onlineCell(u)}</td>` +
        `<td class="muted nowrap">${esc(fmtDate(u.last_login))}</td>` +
        `<td><div class="cell-actions">` +
          `<button class="btn sm ghost" ${lock} data-act="au.manage" data-id="${Number(u.id)}">管理</button>` +
          unban +
          `<button class="btn sm danger" ${lock} data-act="au.kick" data-id="${Number(u.id)}">下线</button>` +
          `<button class="btn sm danger" ${lock} data-act="au.del" data-id="${Number(u.id)}">删除</button>` +
        `</div></td>` +
      `</tr>`;
    }).join('') +
    `</tbody></table></div>` +
    `<div class="card-f small muted">共 ${USERS.length} 个用户，当前显示 ${filtered.length} 个</div>`;
}

async function loadUsers() {
  if (myRank() < 1) return;
  const box = $('admUsers');
  if (box) box.innerHTML = loading();
  try {
    const j = await api('/users');
    USERS = j.data || [];
    if (typeof j.actorRank === 'number') state.actorRank = j.actorRank;
    renderUsers();
  } catch (e) {
    if (box) box.innerHTML = empty('加载失败', e.message, 'alert');
    toast(e.message, 'err');
  }
}

/* ══════════════ 用户弹窗 ══════════════ */
function userModalBody(u) {
  const opt = (v, label, sel) => `<option value="${v}"${sel === v ? ' selected' : ''}>${label}</option>`;
  return `<div class="sec"><div class="t">概要</div>` +
      `<div class="cur">ID <b>#${Number(u.id)}</b> · 角色 <b>${esc(ROLE_LABEL[u.role] || '')}</b> · 状态 <b>${u.status === 'active' ? '正常' : '禁用'}</b> · 注册 ${esc(fmtDate(u.created_at))}</div>` +
      `<div class="cur">最近登录 ${esc(fmtDate(u.last_login))}</div>` +
    `</div>` +
    `<div class="sec"><div class="t">基本信息</div>` +
      `<div class="cur" style="margin-bottom:8px">当前用户名 <b>${esc(u.username)}</b> · 邮箱 <b>${esc(u.email || '未绑定')}</b></div>` +
      `<div class="row nowrap" style="margin-bottom:8px">` +
        `<input class="input" id="muName" placeholder="新用户名（留空 = 不改）">` +
        `<input class="input" id="muEmail" placeholder="新邮箱（留空 = 不改）">` +
        `<button class="btn" data-act="um.saveBasic" style="flex:0 0 auto">保存</button>` +
      `</div>` +
      `<div class="row nowrap">` +
        `<select class="select" id="muRole">` +
          opt('user', '角色：普通用户', u.role) +
          `<option value="admin" data-min-rank="3"${u.role === 'admin' ? ' selected' : ''}>角色：管理员</option>` +
          `<option value="developer" data-min-rank="3"${u.role === 'developer' ? ' selected' : ''}>角色：开发者</option>` +
        `</select>` +
        `<select class="select" id="muStatus">` +
          opt('active', '状态：正常', u.status) + opt('banned', '状态：禁用', u.status) +
        `</select>` +
        `<button class="btn" data-act="um.saveRole" style="flex:0 0 auto">应用</button>` +
      `</div>` +
    `</div>` +
    `<div class="sec"><div class="t">修改用户 ID（级联迁移密钥 / 日志 / 反馈）</div>` +
      `<div class="row nowrap"><input class="input" id="muNewId" type="number" min="1" placeholder="新 ID（正整数）">` +
      `<button class="btn" data-act="um.changeId" style="flex:0 0 auto">修改 ID</button></div>` +
    `</div>` +
    `<div class="sec"><div class="t">重置密码</div>` +
      `<div class="row nowrap"><input class="input" id="muNewPwd" type="text" placeholder="新密码（至少 6 位）">` +
      `<button class="btn" data-act="um.resetPwd" style="flex:0 0 auto">重置密码</button></div>` +
    `</div>` +
    `</div>`;
}

function openUserModal(id) {
  const u = USERS.find((x) => Number(x.id) === Number(id));
  if (!u) return toast('用户不存在', 'err');
  MODAL_USER = u;
  if (userModalApi) userModalApi.close();
  const m = openModal({
    title: '用户管理 · ' + u.username,
    width: 'wide',
    body: userModalBody(u),
    footer: `<span class="small muted">同级或更高等级账号为只读，服务端会再挡一次。</span><div class="sp"></div>` +
      `<button class="btn" data-close>关闭</button>`,
  });
  userModalApi = m;
  m.footer.querySelector('[data-close]').addEventListener('click', () => m.close());
  /* 弹窗里的角色下拉同样按等级禁用越级选项（管理员不能把谁提成开发者）。 */
  m.body.querySelectorAll('option[data-min-rank]').forEach((o) => {
    const need = parseInt(o.dataset.minRank, 10) || 0;
    if (myRank() < need) {
      o.disabled = true;
      if (o.selected) {
        const sel = o.parentElement;
        const first = [...sel.options].find((x) => !x.disabled);
        if (first) sel.value = first.value;
      }
    }
  });
}

async function refreshModal() {
  if (!MODAL_USER) return;
  const id = MODAL_USER.id;
  await loadUsers();
  const again = USERS.find((x) => Number(x.id) === Number(id));
  if (again) openUserModal(again.id);
}

/* ══════════════ 新增用户 ══════════════ */
function openAddUser() {
  const m = openModal({
    title: '新增用户',
    width: 'narrow',
    body:
      `<div class="field"><label for="auName">用户名 <span class="opt">2-24 位，不能是纯数字</span></label>` +
        `<input class="input" id="auName" maxlength="24" placeholder="新用户名" autocomplete="off"></div>` +
      `<div class="field"><label for="auPass">初始密码 <span class="opt">至少 6 位</span></label>` +
        `<input class="input" id="auPass" type="text" placeholder="初始密码" autocomplete="off"></div>` +
      `<div class="field mb-0"><label for="auRole">角色</label>` +
        `<select class="select" id="auRole">` +
          `<option value="user">普通用户</option>` +
          `<option value="admin" data-min-rank="3">管理员（仅开发者可分配）</option>` +
          `<option value="developer" data-min-rank="3">开发者（仅开发者可分配）</option>` +
        `</select></div>`,
    footer: `<div class="sp"></div><button class="btn ghost" data-close>取消</button>` +
      `<button class="btn pri" data-ok>创建用户</button>`,
  });
  m.footer.querySelector('[data-close]').addEventListener('click', () => m.close());
  m.footer.querySelector('[data-ok]').addEventListener('click', async (e) => {
    const btn = e.currentTarget;
    const u = $('auName').value.trim();
    const p = $('auPass').value;
    if (!u || !p) return toast('请填写用户名和密码', 'err');
    const ue = usernameErr(u);
    if (ue) return toast(ue, 'err');
    busy(btn, true, '创建中…');
    try {
      const j = await api('/users', 'POST', { username: u, password: p, role: $('auRole').value });
      toast(j.message || '已创建', 'ok');
      m.close();
      loadUsers();
    } catch (err) {
      toast(err.message, 'err');
    } finally {
      busy(btn, false, '创建用户');
    }
  });
  m.body.querySelectorAll('option[data-min-rank]').forEach((o) => {
    if (myRank() < (parseInt(o.dataset.minRank, 10) || 0)) o.disabled = true;
  });
}

/* ══════════════ 反馈管理 ══════════════ */
const FB_CAT = { bug: '问题反馈', suggestion: '功能建议', account: '账号问题', other: '其他' };
const FB_STATUS = { open: ['待处理', ''], replied: ['已回复', 'info'], closed: ['已关闭', 'ok'] };

async function loadFb() {
  if (myRank() < 2) return;
  const box = $('admFb');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/feedback?limit=100');
    if ($('admFbOpen')) $('admFbOpen').textContent = '待处理 ' + (j.openCount || 0);
    const list = j.data || [];
    box.innerHTML = list.length
      ? list.map((f) => {
        const st = FB_STATUS[f.status] || [f.status, ''];
        return `<div class="item">` +
          `<div class="item-h"><b>#${Number(f.id)} ${esc(f.title)}</b><span class="badge ${st[1]}">${esc(st[0])}</span>` +
            `<span class="item-meta">${esc(f.username || '—')}<span class="sep">·</span>${esc(FB_CAT[f.category] || f.category)}<span class="sep">·</span>${esc(fmtDate(f.created_at))}</span>` +
            `<span class="sp"></span>` +
            `<button class="btn sm danger" data-act="fb.del" data-id="${Number(f.id)}">删除</button></div>` +
          `<div class="item-b">${esc(f.content)}</div>` +
          (f.contact ? `<div class="item-foot">联系方式：${esc(f.contact)}</div>` : '') +
          `<div class="item-actions">` +
            `<select class="select" id="fs-${Number(f.id)}" style="width:110px">` +
              `<option value="open"${f.status === 'open' ? ' selected' : ''}>待处理</option>` +
              `<option value="replied"${f.status === 'replied' ? ' selected' : ''}>已回复</option>` +
              `<option value="closed"${f.status === 'closed' ? ' selected' : ''}>已关闭</option>` +
            `</select>` +
            `<input class="input" id="fr-${Number(f.id)}" placeholder="回复内容（用户可在「我的反馈」看到）" value="${esc(f.reply || '')}" style="flex:1;min-width:200px">` +
            `<button class="btn sm pri" data-act="fb.save" data-id="${Number(f.id)}">保存</button>` +
          `</div>` +
        `</div>`;
      }).join('')
      : empty('暂无反馈', '', 'inbox');
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ══════════════ 登录日志 ══════════════ */
async function loadLogs() {
  if (myRank() < 2) return;
  const box = $('admLogs');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/logs?limit=100');
    const logs = j.data || [];
    const withIp = logs.length > 0 && Object.prototype.hasOwnProperty.call(logs[0], 'ip_address');
    const withApp = logs.length > 0 && Object.prototype.hasOwnProperty.call(logs[0], 'login_app');
    const stKind = (s) => (s === 'success' ? 'ok' : (s === '2fa_pending' ? 'info' : 'bad'));
    const stText = (s) => (s === 'success' ? '成功' : (s === '2fa_pending' ? '待验证' : '失败'));
    box.innerHTML = logs.length
      ? `<div class="tbl-wrap"><table class="tbl compact"><thead><tr>` +
          `<th style="width:160px">时间</th><th>用户名</th>` +
          (withApp ? `<th style="width:120px">来源</th>` : '') +
          (withIp ? `<th style="width:160px">登录 IP</th>` : '') +
          `<th style="width:100px">状态</th></tr></thead><tbody>` +
          logs.map((x) => `<tr>` +
            `<td class="muted nowrap">${esc(fmtDate(x.login_time))}</td>` +
            `<td>${esc(x.username || '—')}</td>` +
            (withApp ? `<td>${esc(x.login_app || '—')}</td>` : '') +
            (withIp ? `<td class="mono">${esc(x.ip_address || '—')}</td>` : '') +
            `<td><span class="badge ${stKind(x.status)}">${stText(x.status)}</span></td>` +
          `</tr>`).join('') + `</tbody></table></div>`
      : empty('暂无登录日志', '', 'clock');
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ══════════════ 视图 ══════════════ */
export default {
  label: '用户',
  group: '管理',
  rank: 2,
  async render({ el }) {
    const admin = myRank() >= 2;
    const roleSel = admin
      ? `<select class="select" id="usrRoleSel" data-change="au.filter" style="width:130px">` +
          `<option value="">全部角色</option><option value="user">普通用户</option>` +
          `<option value="admin">管理员</option><option value="developer">开发者</option></select>`
      : '';

    el.innerHTML =
      pageHead({
        title: '用户',
        sub: '账号状态与角色。同级或更高等级的账号只读，只有开发者能管理全部。',
        actions: `<button class="btn sm ghost" data-act="au.refresh">${icon('refresh', 13)}刷新</button>` +
          `<button class="btn sm pri" data-act="au.add">${icon('plus', 13)}新增用户</button>`,
      }) +
      card({
        title: '用户管理',
        flush: true,
        body:
          `<div style="padding:12px 16px 0"><div class="toolbar">` +
            `<input class="input grow" id="usrSearch" placeholder="搜索用户名 / 邮箱 / ID…" data-input="au.filter">` +
            `<select class="select" id="usrStatusSel" data-change="au.filter" style="width:160px">` +
              `<option value="">全部状态</option><option value="active">正常</option>` +
              `<option value="banned-env">环境检测封禁</option><option value="banned-manual">管理员封禁</option>` +
              `<option value="disabled">禁用</option></select>` +
            roleSel +
          `</div></div>` +
          `<div id="admUsers">${loading()}</div>`,
      }) +
      (admin
        ? `<div class="mt-16">${card({
            title: '反馈管理 <span class="badge" id="admFbOpen" style="margin-left:6px"></span>',
            actions: `<button class="btn sm ghost" data-act="fb.reload">${icon('refresh', 13)}刷新</button>`,
            flush: true,
            body: `<div id="admFb">${loading()}</div>`,
          })}</div>`
        : '') +
      (admin
        ? `<div class="mt-16">${card({
            title: '登录日志',
            desc: '最近 100 条登录记录（含失败）。登录 IP 仅开发者可见。',
            actions: `<button class="btn sm ghost" data-act="logs.refresh">${icon('refresh', 13)}刷新</button>`,
            flush: true,
            body: `<div id="admLogs">${loading()}</div>`,
          })}</div>`
        : '');

    loadUsers();
    if (admin) loadFb();
    if (admin) loadLogs();
  },
};

/* ══════════════ 动作 ══════════════ */
registerActions({
  'au.refresh': () => loadUsers(),
  'au.filter': () => renderUsers(),

  'au.add': () => openAddUser(),

  'au.manage': (b) => openUserModal(b.dataset.id),

  'au.kick': async (b) => {
    const u = USERS.find((x) => Number(x.id) === Number(b.dataset.id));
    const name = u ? u.username : '#' + b.dataset.id;
    const hint = u && u.online ? '' : '\n\n（该账号当前显示为离线，可能是客户端没在运行；被踢状态仍会在下次请求时生效）';
    const ok = await confirmDialog({
      title: '强制下线',
      message: '强制下线「' + name + '」？\n\n该账号所有已登录客户端会在下一次请求时退出登录（约 30 秒内）。用户重新登录后即可继续使用。' + hint,
      okText: '强制下线',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/users/' + b.dataset.id + '/kick', 'POST', {});
      toast(j.message || '已强制下线', 'ok');
      loadUsers();
    } catch (e) { toast(e.message, 'err'); }
  },

  'au.unban': async (b) => {
    const u = USERS.find((x) => Number(x.id) === Number(b.dataset.id));
    const name = u ? u.username : '#' + b.dataset.id;
    const ok = await confirmDialog({
      title: '解除环境检测封禁',
      message: '解除「' + name + '」的环境检测封禁？\n\n该账号可以立即重新登录。只解除时限封禁，不影响管理员手工永久封禁。',
      okText: '解封',
    });
    if (!ok) return;
    try {
      const j = await api('/admin/users/' + b.dataset.id + '/unban', 'POST', {});
      toast(j.message || '已解封', 'ok');
      loadUsers();
    } catch (e) { toast(e.message, 'err'); }
  },

  'au.del': async (b) => {
    const ok = await confirmDialog({
      title: '删除用户',
      message: '确定删除用户 #' + b.dataset.id + '？其密钥与数据将一并删除，不可恢复。',
      okText: '删除',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/users/' + b.dataset.id, 'DELETE');
      toast(j.message || '已删除', 'ok');
      loadUsers();
    } catch (e) { toast(e.message, 'err'); }
  },

  /* ── 用户弹窗 ── */
  'um.saveBasic': async (btn) => {
    if (!MODAL_USER) return;
    const body = {};
    const un = $('muName').value.trim();
    const em = $('muEmail').value.trim();
    if (un) body.username = un;
    if (em) body.email = em;
    if (!Object.keys(body).length) return toast('没有需要保存的修改', 'err');
    busy(btn, true, '保存中…');
    try {
      const j = await api('/users/' + MODAL_USER.id, 'PUT', body);
      toast(j.message || '已保存', 'ok');
      await refreshModal();
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '保存');
    }
  },

  'um.saveRole': async (btn) => {
    if (!MODAL_USER) return;
    busy(btn, true, '应用中…');
    try {
      const j = await api('/users/' + MODAL_USER.id, 'PUT', {
        role: $('muRole').value, status: $('muStatus').value,
      });
      toast(j.message || '已应用', 'ok');
      await refreshModal();
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '应用');
    }
  },

  'um.changeId': async (btn) => {
    if (!MODAL_USER) return;
    const nid = parseInt($('muNewId').value, 10);
    if (!nid) return toast('请输入新 ID', 'err');
    busy(btn, true, '修改中…');
    try {
      const j = await api('/users/' + MODAL_USER.id + '/change-id', 'PUT', { newId: nid });
      toast(j.message || '已修改', 'ok');
      MODAL_USER = null;
      if (userModalApi) { userModalApi.close(); userModalApi = null; }
      loadUsers();
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '修改 ID');
    }
  },

  'um.resetPwd': async (btn) => {
    if (!MODAL_USER) return;
    const np = $('muNewPwd').value;
    if (!np || np.length < 6) return toast('密码至少 6 位', 'err');
    busy(btn, true, '重置中…');
    try {
      const j = await api('/users/' + MODAL_USER.id + '/reset-password', 'POST', { newPassword: np });
      toast(j.message || '已重置', 'ok');
      $('muNewPwd').value = '';
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(btn, false, '重置密码');
    }
  },

  /* ── 反馈 ── */
  'fb.reload': () => loadFb(),
  'fb.save': async (b) => {
    const id = b.dataset.id;
    try {
      const j = await api('/feedback/' + id, 'PUT', {
        status: $('fs-' + id).value,
        reply: $('fr-' + id).value,
      });
      toast(j.message || '已保存', 'ok');
      loadFb();
    } catch (e) { toast(e.message, 'err'); }
  },
  'fb.del': async (b) => {
    const ok = await confirmDialog({
      title: '删除反馈',
      message: '确定删除这条反馈？删除后用户也看不到。',
      okText: '删除',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/feedback/' + b.dataset.id, 'DELETE');
      toast(j.message || '已删除', 'ok');
      loadFb();
    } catch (e) { toast(e.message, 'err'); }
  },

  'logs.refresh': () => loadLogs(),
});
