/* 账号设置：拆成三个子页 —— 资料 / 偏好 / 登录与安全。
   老控制台把 7 张卡塞在一页里，靠 tab 过滤；这里直接变成三条路由，
   侧栏可直达，滚动条也不会拉不到底。 */
import { api } from '../core/api.js';
import { state, setMe, usernameErr } from '../core/state.js';
import { registerActions } from '../core/actions.js';
import {
  icon, esc, fmtDate, toast, busy, empty, loading, countdown,
  statCell, confirmDialog,
} from '../core/ui.js';
import { pageHead, card } from '../core/layout.js';
import { paintWho, renderAvatar, forceLogout } from '../app.js';

const $ = (id) => document.getElementById(id);

/* ══════════════ 资料 ══════════════ */
function profileHtml() {
  return pageHead({
    title: '资料',
    sub: '基本信息、头像、用户名与密码。改用户名或密码后需要重新登录。',
    actions: `<button class="btn sm ghost" data-act="account.reloadProfile">${icon('refresh', 13)}刷新</button>`,
  }) +
    card({
      title: '基本信息',
      flush: true,
      body: `<div class="stats" id="accStats" style="border:0;border-radius:0">` +
        `${statCell('用户 ID', '…')}${statCell('用户名', '…')}${statCell('角色', '…')}` +
        `${statCell('邮箱', '…')}${statCell('注册时间', '…')}${statCell('最近登录', '…')}</div>`,
      footer:
        `<span class="avatar lg" id="accAv" title="点击更换头像" data-act="account.pickAvatar" style="cursor:pointer">?</span>` +
        `<button class="btn sm" data-act="account.pickAvatar">更换头像</button>` +
        `<button class="btn sm ghost" data-act="account.resetAvatar">恢复默认</button>` +
        `<span class="sp"></span><span class="hint" style="margin:0">png / jpg / webp，自动居中裁剪为 128×128</span>`,
    }) +
    `<div class="split even mt-16">` +
      `<div>${card({
        title: '修改用户名',
        desc: '用户 ID 不可修改。修改用户名需验证当前密码，成功后需用新用户名重新登录。',
        body:
          `<div class="field"><label for="unNew">新用户名 <span class="opt">2-24 位，不能是纯数字</span></label>` +
            `<input class="input" id="unNew" maxlength="24" placeholder="新用户名" autocomplete="off"></div>` +
          `<div class="field"><label for="unPass">当前密码</label>` +
            `<input class="input" id="unPass" type="password" placeholder="验证当前密码" autocomplete="current-password"></div>` +
          `<button class="btn" id="unBtn" data-act="account.changeUsername">确认修改</button>`,
      })}</div>` +
      `<div>${card({
        title: '修改密码',
        desc: '修改成功后所有已登录设备都需要重新登录。',
        body:
          `<div class="field"><label for="pwOld">当前密码</label>` +
            `<input class="input" id="pwOld" type="password" autocomplete="current-password"></div>` +
          `<div class="field"><label for="pwNew">新密码 <span class="opt">至少 6 位</span></label>` +
            `<input class="input" id="pwNew" type="password" autocomplete="new-password"></div>` +
          `<div class="field"><label for="pwNew2">确认新密码</label>` +
            `<input class="input" id="pwNew2" type="password" autocomplete="new-password"></div>` +
          `<button class="btn" id="pwBtn" data-act="account.changePassword">确认修改</button>`,
      })}</div>` +
    `</div>` +
    `<div class="mt-16">${card({
      title: '邮箱绑定',
      desc: '绑定后可用邮箱验证码登录、接收登录提醒与反馈回复。<span id="emailFlowDesc"></span>',
      body:
        `<div class="row" style="margin-bottom:10px"><span class="muted small">当前邮箱</span><b id="curEmail" class="mono">—</b></div>` +
        `<div id="bindSection" hidden>` +
          `<div class="field"><label for="bindEmail">新邮箱</label>` +
            `<div class="row nowrap"><input class="input" id="bindEmail" type="email" placeholder="new@example.com" autocomplete="email">` +
            `<button class="btn" id="bindSend" data-act="account.sendBind" style="flex:0 0 auto;width:118px">发送验证码</button></div></div>` +
          `<div class="field"><label for="bindCode">邮箱验证码</label>` +
            `<input class="input" id="bindCode" placeholder="6 位验证码" inputmode="numeric" style="max-width:200px"></div>` +
          `<button class="btn pri" data-act="account.bindEmail">确认绑定</button>` +
        `</div>` +
        `<div id="changeSection" hidden>` +
          `<div class="field"><label for="oldCode">原邮箱验证码</label>` +
            `<div class="row nowrap"><input class="input" id="oldCode" placeholder="原邮箱收到的验证码" inputmode="numeric" style="max-width:200px">` +
            `<button class="btn" id="oldSendBtn" data-act="account.sendOldCode" style="flex:0 0 auto">发送到原邮箱</button></div></div>` +
          `<div class="field"><label for="newEmail">新邮箱</label>` +
            `<input class="input" id="newEmail" type="email" placeholder="new@example.com" autocomplete="email" style="max-width:280px"></div>` +
          `<div class="field"><label for="newCode">新邮箱验证码</label>` +
            `<div class="row nowrap"><input class="input" id="newCode" placeholder="新邮箱收到的验证码" inputmode="numeric" style="max-width:200px">` +
            `<button class="btn" id="newSendBtn" data-act="account.sendNewCode" style="flex:0 0 auto">发送到新邮箱</button></div></div>` +
          `<button class="btn pri" data-act="account.changeEmail">确认换绑</button>` +
        `</div>`,
    })}</div>`;
}

function fillEmailFlow() {
  const has = !!(state.me && state.me.email);
  if ($('bindSection')) $('bindSection').hidden = has;
  if ($('changeSection')) $('changeSection').hidden = !has;
  const d = $('emailFlowDesc');
  if (d) d.textContent = has ? '换绑需先验证原邮箱。' : '尚未绑定。';
}

async function loadProfile() {
  try {
    const j = await api('/profile');
    const u = j.data || {};
    setMe(u);
    const box = $('accStats');
    if (box) {
      box.innerHTML = [
        statCell('用户 ID', '#' + u.id, { cls: 'mono' }),
        statCell('用户名', u.username || '—', { cls: 'sm' }),
        statCell('角色', ({ user: '普通用户', admin: '管理员', developer: '开发者' })[u.role] || '普通用户', { cls: 'sm' }),
        statCell('邮箱', u.email || '未绑定', { cls: 'sm' }),
        statCell('注册时间', fmtDate(u.created_at), { cls: 'sm' }),
        statCell('最近登录', fmtDate(u.last_login), { cls: 'sm' }),
      ].join('');
    }
    if ($('curEmail')) $('curEmail').textContent = u.email || '未绑定';
    fillEmailFlow();
    const av = $('accAv');
    if (av) renderAvatar(av, 46);
    paintWho();
  } catch (e) {
    toast(e.message, 'err');
  }
}

/* ══════════════ 偏好 ══════════════ */
async function loadPrefs() {
  try {
    const j = await api('/profile');
    setMe(j.data);
    $('tgEmailLogin').classList.toggle('on', !!j.data.email_login_enabled);
    $('tgNotify').classList.toggle('on', !!j.data.login_notify_enabled);
  } catch (e) { toast(e.message, 'err'); }
}

/* ══════════════ 登录与安全 ══════════════ */
async function loadSecurity() {
  try {
    const j = await api('/profile');
    setMe(j.data);
    $('tg2fa').classList.toggle('on', !!j.data.two_factor_enabled);
    $('tg2fa').setAttribute('aria-checked', j.data.two_factor_enabled ? 'true' : 'false');
    if ($('tfaState')) {
      $('tfaState').textContent = j.data.two_factor_enabled
        ? '已开启：密码 / 密钥登录需要邮箱验证码'
        : '未开启：仅用密码即可登录';
    }
  } catch (e) { toast(e.message, 'err'); }
  loadLoginLogs();
}

async function loadLoginLogs() {
  const box = $('loginLogs');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/auth/login-logs');
    const logs = j.data || [];
    /* 「谁能看 IP」只有一个事实来源：服务端对非开发者根本不 SELECT 这一列。
       这里只看返回对象里有没有这个键，不在前端比角色。 */
    const withIp = logs.length > 0 && Object.prototype.hasOwnProperty.call(logs[0], 'ip_address');
    const withApp = logs.length > 0 && Object.prototype.hasOwnProperty.call(logs[0], 'login_app');
    const stText = (s) => (s === 'success' ? '成功' : (s === '2fa_pending' ? '待验证' : '失败'));
    const stKind = (s) => (s === 'success' ? 'ok' : (s === '2fa_pending' ? 'info' : 'bad'));
    box.innerHTML = logs.length
      ? `<div class="tbl-wrap"><table class="tbl"><thead><tr>` +
          `<th style="width:160px">时间</th><th style="width:100px">结果</th>` +
          (withApp ? `<th style="width:120px">来源</th>` : '') +
          (withIp ? `<th>登录 IP</th>` : '') +
        `</tr></thead><tbody>` +
        logs.map((l) => `<tr>` +
          `<td class="muted nowrap">${esc(fmtDate(l.login_time))}</td>` +
          `<td><span class="badge ${stKind(l.status)}">${stText(l.status)}</span></td>` +
          (withApp ? `<td>${esc(l.login_app || '—')}</td>` : '') +
          (withIp ? `<td class="mono">${esc(l.ip_address || '—')}</td>` : '') +
        `</tr>`).join('') + `</tbody></table></div>`
      : empty('暂无登录记录', '', 'clock');
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ══════════════ 头像 / 壁纸文件选择 ══════════════ */
async function onAvatarFile(input) {
  const f = input.files && input.files[0];
  input.value = '';
  if (!f) return;
  if (!/^image\//.test(f.type)) return toast('请选择图片文件', 'err');
  const url = URL.createObjectURL(f);
  const img = new Image();
  img.onload = async () => {
    try {
      const S = 128;
      const c = document.createElement('canvas');
      c.width = S; c.height = S;
      const r = Math.min(img.width, img.height);
      c.getContext('2d').drawImage(img, (img.width - r) / 2, (img.height - r) / 2, r, r, 0, 0, S, S);
      const data = c.toDataURL('image/jpeg', 0.85);
      const j = await api('/user/avatar', 'PUT', { avatar: data });
      state.me.avatar = (j.data && j.data.avatar) || data;
      const av = $('accAv');
      if (av) renderAvatar(av, 46);
      paintWho();
      toast(j.message || '头像已更新', 'ok');
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      URL.revokeObjectURL(url);
    }
  };
  img.onerror = () => { URL.revokeObjectURL(url); toast('图片读取失败', 'err'); };
  img.src = url;
}

export function initAccountFileInputs() {
  const av = $('avFile');
  if (av) av.addEventListener('change', () => onAvatarFile(av));
}

/* ══════════════ 视图导出 ══════════════ */
export const profileView = {
  label: '资料', group: '账号', rank: 0,
  async render({ el }) {
    el.innerHTML = profileHtml();
    await loadProfile();
  },
};

export const prefsView = {
  label: '偏好', group: '账号', rank: 0,
  async render({ el }) {
    el.innerHTML =
      pageHead({ title: '偏好', sub: '登录方式、通知与云端共享背景。' }) +
      card({
        title: '登录偏好',
        body:
          `<div class="set-row"><div style="flex:1"><div class="t">邮箱验证码登录</div>` +
            `<div class="d">允许使用邮箱验证码登录客户端与控制台</div></div>` +
            `<button class="sw" id="tgEmailLogin" data-act="account.toggle" data-key="email_login_enabled" role="switch"></button></div>` +
          `<div class="set-row"><div style="flex:1"><div class="t">登录邮件提醒</div>` +
            `<div class="d">新设备登录时向绑定邮箱发送安全提醒</div></div>` +
            `<button class="sw" id="tgNotify" data-act="account.toggle" data-key="login_notify_enabled" role="switch"></button></div>`,
      }) +
    loadPrefs();
  },
};

export const securityView = {
  label: '登录与安全', group: '账号', rank: 0,
  async render({ el }) {
    el.innerHTML =
      pageHead({
        title: '登录与安全',
        sub: '双重验证、最近登录记录。发现陌生 IP 或非本人操作时，立即改密码。',
        actions: `<button class="btn sm ghost" data-act="account.logsRefresh">${icon('refresh', 13)}刷新</button>`,
      }) +
      card({
        title: '登录双重验证',
        desc: '开启后，密码 / 密钥登录需要额外输入邮箱验证码（邮箱验证码登录本身已证明邮箱所有权，不受影响）。<b>开与关都必须先验证绑定邮箱</b>。',
        body:
          `<div class="set-row"><div style="flex:1"><div class="t">双重验证</div>` +
            `<div class="d" id="tfaState">—</div></div>` +
            `<button class="sw" id="tg2fa" data-act="account.tfaOpen" role="switch"></button></div>` +
          `<div id="tfaPanel" hidden style="margin-top:12px;padding-top:12px;border-top:1px solid var(--line-2)">` +
            `<div class="hint" id="tfaPanelTip" style="margin-bottom:8px"></div>` +
            `<div class="row nowrap"><input class="input" id="tfaCode" placeholder="邮箱收到的 6 位验证码" inputmode="numeric" style="max-width:190px">` +
              `<button class="btn" id="tfaSend" data-act="account.tfaSend" style="flex:0 0 auto">发送验证码</button>` +
              `<button class="btn pri" id="tfaOk" data-act="account.tfaConfirm">确认</button>` +
              `<button class="btn ghost" data-act="account.tfaCancel">取消</button></div>` +
          `</div>`,
      }) +
      card({
        title: '最近登录记录',
        desc: '最近 30 次登录（含失败尝试）。登录 IP 仅开发者可见。',
        flush: true,
        body: `<div id="loginLogs">${loading()}</div>`,
      });
    loadSecurity();
  },
};

/* ══════════════ 动作 ══════════════ */
let tfaTargetOn = false;

registerActions({
  'account.reloadProfile': () => loadProfile(),
  'account.pickAvatar': () => $('avFile').click(),
  'account.resetAvatar': async () => {
    try {
      await api('/user/avatar', 'PUT', { avatar: '' });
      state.me.avatar = '';
      const av = $('accAv');
      if (av) renderAvatar(av, 46);
      paintWho();
      toast('头像已恢复默认', 'ok');
    } catch (e) { toast(e.message, 'err'); }
  },

  'account.changeUsername': async (btn) => {
    const nu = $('unNew').value.trim();
    const pw = $('unPass').value;
    if (!nu || !pw) return toast('请输入新用户名和当前密码', 'err');
    const ue = usernameErr(nu);
    if (ue) return toast(ue, 'err');
    busy(btn, true, '修改中…');
    try {
      const j = await api('/user/change-username', 'PUT', { newUsername: nu, password: pw });
      toast((j.message || '用户名已修改') + '，请用新用户名重新登录', 'ok');
      setTimeout(() => forceLogout(), 1500);
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '确认修改');
    }
  },

  'account.changePassword': async (btn) => {
    const o = $('pwOld').value;
    const n = $('pwNew').value;
    const n2 = $('pwNew2').value;
    if (!o || !n) return toast('请输入当前密码和新密码', 'err');
    if (n.length < 6) return toast('新密码至少 6 位', 'err');
    if (n !== n2) return toast('两次输入的新密码不一致', 'err');
    busy(btn, true, '修改中…');
    try {
      const j = await api('/change-password', 'POST', { oldPassword: o, newPassword: n });
      toast(j.message || '密码修改成功，请重新登录', 'ok');
      setTimeout(() => forceLogout(), 1500);
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '确认修改');
    }
  },

  'account.sendBind': async (btn) => {
    const em = $('bindEmail').value.trim();
    if (!em) return toast('请输入新邮箱', 'err');
    busy(btn, true, '发送中…');
    try {
      const j = await api('/user/send-bind-code', 'POST', { email: em });
      toast(j.message || '验证码已发送', 'ok');
      countdown(btn, '发送验证码');
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '发送验证码');
    }
  },

  'account.bindEmail': async (btn) => {
    const em = $('bindEmail').value.trim();
    const c = $('bindCode').value.trim();
    if (!em || !c) return toast('请输入新邮箱和验证码', 'err');
    busy(btn, true, '绑定中…');
    try {
      const j = await api('/user/bind-email', 'POST', { email: em, code: c });
      toast(j.message || '邮箱绑定成功', 'ok');
      await loadProfile();
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(btn, false, '确认绑定');
    }
  },

  'account.sendOldCode': async (btn) => {
    busy(btn, true, '发送中…');
    try {
      const j = await api('/user/send-change-email-code', 'POST', {});
      toast(j.message || '验证码已发送到原邮箱', 'ok');
      countdown(btn, '发送到原邮箱');
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '发送到原邮箱');
    }
  },

  'account.sendNewCode': async (btn) => {
    const em = $('newEmail').value.trim();
    if (!em) return toast('请先填写新邮箱地址', 'err');
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(em)) return toast('请输入有效的邮箱地址', 'err');
    busy(btn, true, '发送中…');
    try {
      const j = await api('/user/send-bind-code', 'POST', { email: em });
      toast(j.message || '验证码已发送到新邮箱', 'ok');
      countdown(btn, '发送到新邮箱');
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '发送到新邮箱');
    }
  },

  'account.changeEmail': async (btn) => {
    const oldCode = $('oldCode').value.trim();
    const newEmail = $('newEmail').value.trim();
    const newCode = $('newCode').value.trim();
    if (!oldCode) return toast('请输入原邮箱收到的验证码', 'err');
    if (!newEmail) return toast('请输入新邮箱地址', 'err');
    if (!newCode) return toast('请输入新邮箱收到的验证码', 'err');
    busy(btn, true, '换绑中…');
    try {
      const j = await api('/user/change-email', 'POST', { oldCode, newEmail, newCode });
      toast(j.message || '邮箱换绑成功', 'ok');
      $('oldCode').value = ''; $('newEmail').value = ''; $('newCode').value = '';
      await loadProfile();
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(btn, false, '确认换绑');
    }
  },

  'account.toggle': async (btn) => {
    const key = btn.dataset.key;
    const on = !btn.classList.contains('on');
    btn.classList.toggle('on', on);
    btn.setAttribute('aria-checked', on ? 'true' : 'false');
    try {
      await api('/user/settings', 'PUT', { [key]: on });
      if (state.me) state.me[key] = on ? 1 : 0;
      toast('设置已保存', 'ok');
    } catch (e) {
      btn.classList.toggle('on', !on);
      btn.setAttribute('aria-checked', !on ? 'true' : 'false');
      toast(e.message, 'err');
    }
  },

  'account.logsRefresh': () => loadLoginLogs(),

  'account.tfaOpen': () => {
    if (!state.me || !state.me.email) return toast('请先绑定邮箱再管理双重验证', 'err');
    tfaTargetOn = !$('tg2fa').classList.contains('on');
    $('tfaPanelTip').innerHTML = (tfaTargetOn
      ? '开启后，密码 / 密钥登录需额外输入邮箱验证码。'
      : '关闭后，密码 / 密钥登录将不再需要第二重验证。') +
      '请先验证绑定邮箱：<b>' + esc(state.me.email) + '</b>';
    $('tfaCode').value = '';
    $('tfaPanel').hidden = false;
  },

  'account.tfaCancel': () => { $('tfaPanel').hidden = true; },

  'account.tfaSend': async (btn) => {
    busy(btn, true, '发送中…');
    try {
      const j = await api('/auth/2fa/send-code', 'POST', {});
      toast(j.message || '验证码已发送', 'ok');
      countdown(btn, '发送验证码');
    } catch (e) {
      toast(e.message, 'err');
      busy(btn, false, '发送验证码');
    }
  },

  'account.tfaConfirm': async (btn) => {
    const c = $('tfaCode').value.trim();
    if (!c) return toast('请输入验证码', 'err');
    busy(btn, true, '验证中…');
    try {
      const j = await api('/user/2fa', 'POST', { enabled: tfaTargetOn, code: c });
      state.me.two_factor_enabled = tfaTargetOn ? 1 : 0;
      $('tg2fa').classList.toggle('on', tfaTargetOn);
      $('tg2fa').setAttribute('aria-checked', tfaTargetOn ? 'true' : 'false');
      $('tfaPanel').hidden = true;
      toast(j.message || '已保存', 'ok');
      loadSecurity();
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(btn, false, '确认');
    }
  },
});
