/* 登录 / 注册 / 找回密码 / 双重验证。
   五种模式共用一张卡：pwd、email、reg 是顶部三个 tab；forgot、2fa 是从密码页进去的二级页，
   进去时把 tab 条收起来（它们不是并列入口）。 */
import { api, publicJson } from './core/api.js';
import { LEGAL_VERSION, saveSession, usernameErr, state } from './core/state.js';
import { registerActions } from './core/actions.js';
import { toast, busy, countdown, esc } from './core/ui.js';

const $ = (id) => document.getElementById(id);
let onAuthed = null;
let tfaTicket = '';

function setMsg(msg, ok) {
  const b = $('authMsg');
  if (!b) return;
  if (!msg) { b.className = 'form-error'; b.textContent = ''; return; }
  b.className = 'form-error on' + (ok ? ' ok' : '');
  b.textContent = msg;
}

const HEAD = {
  pwd: ['登录控制台', '使用 KunClient 账号继续'],
  email: ['邮箱登录', '用绑定邮箱收到的验证码登录'],
  reg: ['创建账户', '注册后可在客户端与控制台使用同一账号'],
  forgot: ['找回密码', '验证注册邮箱后设置新密码'],
  '2fa': ['双重验证', '再输入一次邮箱验证码'],
};

function switchMode(mode) {
  document.querySelectorAll('#authTabs .chip').forEach((b) => {
    b.classList.toggle('on', b.dataset.mode === mode);
  });
  const tabs = document.querySelector('#authTabs');
  if (tabs) tabs.parentElement.style.display = (mode === 'forgot' || mode === '2fa') ? 'none' : '';
  const h = HEAD[mode];
  if (h && $('authTitle')) {
    $('authTitle').textContent = h[0];
    $('authSub').textContent = h[1];
  }
  document.querySelectorAll('[data-pane]').forEach((p) => {
    p.hidden = p.dataset.pane !== mode;
  });
  setMsg('');
  const first = document.querySelector(`[data-pane="${mode}"] input`);
  if (first) setTimeout(() => first.focus(), 30);
}

function finishLogin(j) {
  saveSession(j.data);
  setMsg('登录成功，正在进入…', true);
  setTimeout(() => { if (onAuthed) onAuthed(); }, 220);
}

/* ── 密码登录 ─────────────────────────────────────────────────────────── */
async function doLogin() {
  const u = $('lUser').value.trim();
  const p = $('lPass').value;
  if (!u || !p) return setMsg('请输入用户名和密码');
  const btn = $('lBtn');
  busy(btn, true, '登录中…');
  try {
    const j = await api('/login', 'POST', { username: u, password: p, app: 'console' });
    finishLogin(j);
  } catch (e) {
    if (e.code === '2FA_REQUIRED' && e.data && e.data.ticket) {
      enter2FA(e.data.ticket, e.data.maskedEmail);
    } else {
      setMsg(e.message);
    }
  } finally {
    busy(btn, false, '登录');
  }
}

/* ── 双重验证第二步 ───────────────────────────────────────────────────── */
function enter2FA(ticket, masked) {
  tfaTicket = ticket || '';
  switchMode('2fa');
  $('tTip').textContent = (masked ? '验证码已发送至 ' + masked + '，' : '验证码已发送至你的绑定邮箱，') +
    '10 分钟内有效。请勿把验证码告诉任何人。';
  $('tCode').value = '';
  countdown($('tSend'), '重新发送');
  $('tCode').focus();
}

function back2FA() {
  tfaTicket = '';
  switchMode('pwd');
  $('lPass').focus();
}

async function resend2FA() {
  if (!tfaTicket) return back2FA();
  const btn = $('tSend');
  busy(btn, true, '发送中…');
  try {
    const j = await api('/auth/2fa/resend', 'POST', { ticket: tfaTicket });
    setMsg(j.message || '验证码已发送', true);
    countdown(btn, '重新发送');
  } catch (e) {
    if (e.code === '2FA_TICKET_INVALID' || e.code === '2FA_CHANGED') back2FA();
    setMsg(e.message);
  } finally {
    busy(btn, false, '重新发送');
  }
}

async function verify2FA() {
  const c = $('tCode').value.trim();
  if (!c) return setMsg('请输入邮箱收到的验证码');
  if (!tfaTicket) return back2FA();
  const btn = $('tBtn');
  busy(btn, true, '验证中…');
  try {
    const j = await api('/auth/2fa/verify', 'POST', { ticket: tfaTicket, code: c });
    finishLogin(j);
  } catch (e) {
    if (e.code === '2FA_TICKET_INVALID' || e.code === '2FA_CHANGED') back2FA();
    setMsg(e.message);
  } finally {
    busy(btn, false, '验证并登录');
  }
}

/* ── 邮箱验证码 ───────────────────────────────────────────────────────── */
const CODE_EMAIL = { login: 'lEmail', register: 'rEmail', reset: 'fEmail' };
const CODE_BTN = { login: 'eSend', register: 'rSend', reset: 'fSend' };

async function sendCode(kind) {
  const em = $(CODE_EMAIL[kind]).value.trim();
  if (!em) return setMsg('请先填写邮箱');
  const btn = $(CODE_BTN[kind]);
  busy(btn, true, '发送中…');
  try {
    const j = await api('/auth/send-code', 'POST', { email: em, type: kind });
    setMsg(j.message || '验证码已发送', true);
    countdown(btn, '发送验证码');
  } catch (e) {
    setMsg(e.message);
    busy(btn, false, '发送验证码');
  }
}

async function emailLogin() {
  const em = $('lEmail').value.trim();
  const c = $('lCode').value.trim();
  if (!em || !c) return setMsg('请输入邮箱和验证码');
  const btn = $('eBtn');
  busy(btn, true, '登录中…');
  try {
    const j = await api('/auth/code-login', 'POST', { email: em, code: c, app: 'console' });
    finishLogin(j);
  } catch (e) {
    setMsg(e.message);
  } finally {
    busy(btn, false, '邮箱登录');
  }
}

/* ── 注册 ─────────────────────────────────────────────────────────────── */
async function doRegister() {
  const u = $('rUser').value.trim();
  const p = $('rPass').value;
  const p2 = $('rPass2').value;
  const em = $('rEmail').value.trim();
  const c = $('rCode').value.trim();

  if (!u || !p) return setMsg('请输入用户名和密码');
  const ue = usernameErr(u);
  if (ue) return setMsg(ue);
  if (p !== p2) return setMsg('两次密码不一致');
  if (!em) return setMsg('请输入邮箱（用于找回密码与登录提醒）');
  if (!c) return setMsg('请输入邮箱验证码');
  if (!$('rAgree').checked) return setMsg('请先阅读并同意《用户协议》《隐私政策》《免责声明》');
  /* PIPL 第 39 条：个人信息出境的同意必须「单独」取得 —— 不能和上面的条款合成一个勾选，
     服务端也会独立校验（rAgree 可以前端绕过，crossBorderConsent 同样服务端兜底）。 */
  if (!$('rCross').checked) return setMsg('请单独同意个人信息出境后再注册');

  const btn = $('rBtn');
  busy(btn, true, '注册中…');
  try {
    /* 服务端注册成功即签发 token，必须存下来 —— 丢掉的话接下来的 /profile 会 401，
       用户就停在登录页，还看不到任何报错。 */
    const j = await api('/auth/code-register', 'POST', {
      username: u, password: p, email: em, code: c, agree: true, legalVersion: LEGAL_VERSION,
      crossBorderConsent: true,
    });
    finishLogin(j);
  } catch (e) {
    setMsg(e.message);
  } finally {
    busy(btn, false, '创建账户');
  }
}

/* ── 找回密码 ─────────────────────────────────────────────────────────── */
async function resetPassword() {
  const em = $('fEmail').value.trim();
  const c = $('fCode').value.trim();
  const p = $('fPass').value;
  const p2 = $('fPass2').value;
  if (!em || !c) return setMsg('请输入邮箱和验证码');
  if (!p) return setMsg('请输入新密码');
  if (p.length < 6) return setMsg('密码至少 6 个字符');
  if (p !== p2) return setMsg('两次输入的密码不一致');

  const btn = $('fBtn');
  busy(btn, true, '提交中…');
  try {
    const j = await api('/auth/reset-password', 'POST', { email: em, code: c, newPassword: p });
    $('fCode').value = '';
    $('fPass').value = '';
    $('fPass2').value = '';
    setMsg(j.message || '密码已重置，请用新密码登录', true);
    setTimeout(() => switchMode('pwd'), 1200);
  } catch (e) {
    setMsg(e.message);
  } finally {
    busy(btn, false, '重置密码');
  }
}

/* ── 页脚：Ver · by · 时钟（与客户端登录窗同一格式）──────────────────── */
async function initFooter() {
  const node = $('loginVer');
  if (!node) return;
  let ver = '';
  const j = await publicJson('/api/app/version');
  if (j && j.success && j.data) ver = j.data.version || '';
  const p2 = (n) => String(n).padStart(2, '0');
  const tick = () => {
    const d = new Date();
    node.textContent = 'Ver:' + (ver || '-') + ' · by 小坤ovo · ' +
      p2(d.getHours()) + ':' + p2(d.getMinutes()) + ':' + p2(d.getSeconds());
  };
  tick();
  clearInterval(initFooter._t);
  initFooter._t = setInterval(tick, 1000);
}

/* ── 对外 ─────────────────────────────────────────────────────────────── */
export function initAuth(opts) {
  onAuthed = (opts || {}).onAuthed || null;
  initFooter();

  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter') return;
    const login = $('loginView');
    if (!login || login.hidden) return;
    if (e.target && e.target.tagName === 'TEXTAREA') return;
    const pane = document.querySelector('[data-pane]:not([hidden])');
    if (!pane) return;
    const mode = pane.dataset.pane;
    if (mode === 'pwd') doLogin();
    else if (mode === 'email') emailLogin();
    else if (mode === 'reg') doRegister();
    else if (mode === 'forgot') resetPassword();
    else if (mode === '2fa') verify2FA();
  });
}

/** 退出 / 登录过期后重置表单：密码框清空、回到密码页、清掉上一次的报错。 */
export function resetAuth() {
  tfaTicket = '';
  ['lPass', 'lCode', 'rPass', 'rPass2', 'rCode', 'fCode', 'fPass', 'fPass2', 'tCode'].forEach((id) => {
    const n = $(id);
    if (n) n.value = '';
  });
  if ($('rAgree')) $('rAgree').checked = false;
  switchMode('pwd');
  setMsg('');
  if ($('lUser')) $('lUser').value = state.me ? state.me.username : $('lUser').value;
}

registerActions({
  'auth.mode': (b) => switchMode(b.dataset.mode),
  'auth.login': () => doLogin(),
  'auth.emailLogin': () => emailLogin(),
  'auth.register': () => doRegister(),
  'auth.reset': () => resetPassword(),
  'auth.sendCode': (b) => sendCode(b.dataset.kind),
  'auth.resend2fa': () => resend2FA(),
  'auth.verify2fa': () => verify2FA(),
  'auth.back2fa': () => back2FA(),
});
