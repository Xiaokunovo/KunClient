/* 管理 › 封禁中心（仅开发者）—— 封禁相关的**唯一处置入口**。
 *
 * 归类原则（2026-10-24 重排）：
 *   · 「检测」留在安全页：环境检测阈值 / 总开关 / 环境告警流水 / 客户端日志；
 *   · 「处置」全部收到这里：账号封禁、来源 IP 封禁、自动封禁记录、用户申诉、通知邮件。
 * 之前这些散在「安全」和「用户」两页：阈值旁边摆着 IP 封禁、申诉夹在告警列表下面、
 * 自动封禁记录又在另一张卡 —— 出了问题要先想「这功能在哪一页」。
 *
 * 边界（刻意划清）：
 *   · 账号只做**有时限**封禁；永久停用（status='banned'）留在「用户管理」，
 *     那是「账号停用」，与「临时限制」语义不同，混进来迟早有人点错。
 */
import { api } from '../../core/api.js';
import { state, myRank, friendlySignalReason } from '../../core/state.js';
import { registerActions } from '../../core/actions.js';
import { icon, esc, fmtDate, toast, busy, empty, loading, confirmDialog, switchRow } from '../../core/ui.js';
import { pageHead, card, stats } from '../../core/layout.js';

const $ = (id) => document.getElementById(id);

/* 封禁类型 → 徽标。manual = 管理员手动；high/mid = 环境检测自动封禁的级别。 */
const TYPE = { manual: ['手动', 'warn'], high: ['自动 · HIGH', 'bad'], mid: ['自动 · MID', 'warn'] };

const APPEAL_ST = { pending: ['待处理', 'warn'], approved: ['已通过', 'ok'], rejected: ['已驳回', ''] };

let SELF_IP = '';   // 服务端回传的本机出口 IP：封到自己时给二次确认

function daysLeft(until) {
  if (!until) return '永久';
  const ms = new Date(until).getTime() - Date.now();
  if (!Number.isFinite(ms) || ms <= 0) return '已到期';
  const d = Math.ceil(ms / 86400000);
  return d <= 1 ? '<1 天' : d + ' 天';
}

function statCell(key, label) {
  return `<div class="stat"><div class="k">${label}</div><div class="v" id="${key}">—</div></div>`;
}

/* ── 账号封禁：生效中 ─────────────────────────────────────────────────── */
function renderAccounts() {
  const box = $('bnAccounts');
  if (!box) return;
  const list = state.banAccounts || [];
  if (!list.length) {
    box.innerHTML = empty('当前没有被封禁的账号', '自动封禁与手动封禁都会出现在这里。', 'shieldCheck');
    return;
  }
  box.innerHTML = list.map((u) => {
    const permanent = u.status === 'banned';
    const t = permanent ? ['永久停用', 'bad'] : (TYPE[u.ban_level] || ['临时限制', 'warn']);
    const auto90 = Number(u.auto90 || 0);
    return `<div class="item">` +
      `<div class="item-h"><b>${esc(u.username || ('#' + u.id))}</b>` +
        `<span class="badge ${t[1]}">${esc(t[0])}</span>` +
        (auto90 >= 2 ? `<span class="badge warn" title="90 天内被自动封禁 ${auto90} 次，天数已按倍数升级">90 天 ${auto90} 次</span>` : '') +
        `<span class="sp"></span>` +
        `<span class="item-meta">${permanent ? '需在用户管理解除' : '剩余 ' + daysLeft(u.banned_until)}</span></div>` +
      `<div class="item-b">${esc(friendlySignalReason(u.ban_reason || '') || '—')}</div>` +
      `<div class="item-foot">${esc(u.email || '未填邮箱')}` +
        (permanent ? '' : ' · 到期 ' + esc(String(u.banned_until || '').slice(0, 10))) + `</div>` +
      `<div class="item-actions">` +
        (permanent
          ? `<span class="small muted">永久停用请到「用户管理 → 编辑 → 状态」处理</span>`
          : `<button class="btn sm" data-act="bn.unban" data-id="${Number(u.id)}" data-name="${esc(u.username || '')}">解除封禁</button>`) +
      `</div>` +
    `</div>`;
  }).join('');
}

async function loadAccounts() {
  const box = $('bnAccounts');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/admin/bans/accounts?limit=200');
    state.banAccounts = j.data || [];
    renderAccounts();
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ── 来源 IP 封禁 ─────────────────────────────────────────────────────── */
function renderIps() {
  const box = $('bnIps');
  if (!box) return;
  const list = state.bannedIps || [];
  if (!list.length) {
    box.innerHTML = empty('当前没有封禁的 IP', '未登录客户端触发自动封禁时，默认封 6 小时。', 'shield');
    return;
  }
  box.innerHTML = `<div class="tbl-wrap"><table class="tbl compact"><thead><tr>` +
    `<th>IP</th><th>原因</th><th style="width:150px">封禁时间</th><th style="width:150px">到期</th><th style="width:80px"></th>` +
    `</tr></thead><tbody>` +
    list.map((b) => {
      const until = b.permanent ? '<span class="badge bad">永久</span>'
        : (b.expired ? '<span class="badge">已失效</span>' : esc(fmtDate(b.expireAt)));
      return `<tr>` +
        `<td class="mono">${esc(b.ip)}` +
          (b.ip === SELF_IP ? ' <span class="badge">本机</span>' : '') + `</td>` +
        `<td>${b.reason ? esc(friendlySignalReason(b.reason)) : '<span class="muted">—</span>'}</td>` +
        `<td class="muted nowrap">${esc(fmtDate(b.createdAt))}</td>` +
        `<td>${until}</td>` +
        `<td><div class="cell-actions"><button class="btn sm ghost" data-act="bn.ipDel" data-ip="${esc(b.ip)}">解封</button></div></td>` +
      `</tr>`;
    }).join('') + `</tbody></table></div>`;
}

async function loadIps() {
  const box = $('bnIps');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/admin/banned-ips');
    SELF_IP = j.selfIp || '';
    state.bannedIps = j.data || [];
    renderIps();
  } catch (e) {
    box.innerHTML = empty('读取失败', e.message, 'alert');
  }
}

/* SELF_BAN：要封的正好是自己 → 二次确认后带 force=1 重试（否则这条请求之后直接失联）。 */
async function addIpBan() {
  const ip = ($('bnIp').value || '').trim();
  if (!ip) return toast('请输入要封禁的 IP', 'err');
  const payload = { ip };
  const reason = ($('bnIpReason').value || '').trim();
  if (reason) payload.reason = reason;
  const days = parseInt($('bnIpDays').value, 10);
  if (Number.isFinite(days) && days > 0) payload.days = days;

  const post = async (force) => {
    if (force) payload.force = '1';
    try {
      const j = await api('/admin/banned-ips', 'POST', payload);
      toast(j.message || '已封禁', 'ok');
      $('bnIp').value = ''; $('bnIpReason').value = ''; $('bnIpDays').value = '';
      loadSummary(); loadIps();
    } catch (e) {
      if (e && e.code === 'SELF_BAN') {
        const ok = await confirmDialog({
          title: '封禁自己的 IP',
          message: e.message + '\n\n确认要封禁自己的 IP 吗？提交后你将无法继续访问本站。',
          okText: '仍然封禁',
          danger: true,
        });
        if (ok) return post(true);
        return;
      }
      toast(e.message, 'err');
    }
  };
  await post(false);
}

/* ── 自动封禁记录（env_bans）──────────────────────────────────────────── */
function renderEnvBans() {
  const box = $('bnAutoList');
  if (!box) return;
  const list = state.envBans || [];
  if (!list.length) {
    box.innerHTML = empty('暂无自动封禁记录',
      '这里只记环境检测触发的时限封禁；管理员手工封禁与 IP 封禁各有自己的列表。', 'shieldCheck');
    return;
  }
  const LV = { high: 'bad', mid: 'warn' };
  const counts = state.envBanCounts || {};
  box.innerHTML = list.map((b) => {
    const n = Number(counts[String(b.user_id)] || 0);
    return `<div class="item">` +
      `<div class="item-h"><b>${esc(b.username || ('#' + b.user_id))}</b>` +
        `<span class="badge ${LV[b.level] || ''}">${esc(String(b.level || '').toUpperCase())}</span>` +
        `<span class="small muted">封 ${Number(b.days) || 0} 天</span>` +
        // 90 天内 ≥2 次 = 天数被升级过，标出来，免得以为「怎么封了 6 天」
        (n >= 2 ? `<span class="badge warn" title="90 天内第 ${n} 次，天数已按倍数升级">第 ${n} 次</span>` : '') +
        `<span class="sp"></span><span class="item-meta">${esc(fmtDate(b.created_at))}</span></div>` +
      `<div class="item-b">${esc(friendlySignalReason(b.reason || ''))}</div>` +
    `</div>`;
  }).join('');
}

async function loadEnvBans() {
  const box = $('bnAutoList');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/admin/env-bans?limit=100');
    state.envBans = j.data || [];
    state.envBanCounts = j.counts90d || {};
    renderEnvBans();
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ── 用户申诉 ─────────────────────────────────────────────────────────── */
function renderAppeals() {
  const box = $('bnAppeals');
  if (!box) return;
  const q = ($('bnAppealSearch') ? $('bnAppealSearch').value : '').trim().toLowerCase();
  const list = (state.appeals || []).filter((a) => {
    if (state.appealStatus && a.status !== state.appealStatus) return false;
    if (q) {
      const hay = ((a.username || '') + ' ' + (a.content || '') + ' ' + (a.ban_reason || '')).toLowerCase();
      if (!hay.includes(q)) return false;
    }
    return true;
  });
  if (!list.length) {
    box.innerHTML = empty((state.appeals || []).length ? '没有符合筛选条件的申诉' : '暂无申诉', '', 'message');
    return;
  }
  box.innerHTML = list.map((a) => {
    const handled = a.status !== 'pending';
    const st = APPEAL_ST[a.status] || [a.status, ''];
    return `<div class="item">` +
      `<div class="item-h"><b>#${Number(a.id)} ${esc(a.username || '—')}</b>` +
        `<span class="badge ${st[1]}">${esc(st[0])}</span>` +
        `<span class="sp"></span><span class="item-meta">${esc(fmtDate(a.created_at))}</span></div>` +
      `<div class="item-b" style="color:var(--ink-3)">封禁原因：${esc(friendlySignalReason(a.ban_reason || '—'))}</div>` +
      `<div class="item-b">${esc(a.content || '')}</div>` +
      (a.contact ? `<div class="item-foot">联系方式：${esc(a.contact)}</div>` : '') +
      (a.reply ? `<div class="item-reply">处理回复：${esc(a.reply)}</div>` : '') +
      `<div class="item-actions">` +
        (handled ? '' :
          `<input class="input" id="ar-${Number(a.id)}" placeholder="处理回复（可选）" style="flex:1;min-width:180px">` +
          `<button class="btn sm pri" data-act="bn.appealResolve" data-id="${Number(a.id)}" data-action="approve">通过并解封</button>` +
          `<button class="btn sm danger" data-act="bn.appealResolve" data-id="${Number(a.id)}" data-action="reject">驳回</button>`) +
        `<span class="sp"></span>` +
        `<button class="btn sm ghost" data-act="bn.appealDel" data-id="${Number(a.id)}" title="只删除这条记录，不影响账号封禁状态">删除记录</button>` +
      `</div>` +
    `</div>`;
  }).join('');
}

async function loadAppeals() {
  const box = $('bnAppeals');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/admin/appeals');
    state.appeals = j.data || [];
    renderAppeals();
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ── 统计 ─────────────────────────────────────────────────────────────── */
async function loadSummary() {
  try {
    const j = await api('/admin/bans/summary');
    const d = j.data || {};
    const put = (id, v, cls) => { const el = $(id); if (el) { el.textContent = v; if (cls) el.className = 'v ' + cls; } };
    put('bnStatTimed', Number(d.timed || 0), d.timed ? 'warn' : 'ok');
    put('bnStatPerm', Number(d.permanent || 0), d.permanent ? 'bad' : 'ok');
    put('bnStatIp', Number(d.ips || 0), d.ips ? 'warn' : 'ok');
    put('bnStatAuto', Number(d.auto7d || 0), '');
    put('bnStatAppeal', Number(d.appeals || 0), d.appeals ? 'warn' : 'ok');
    if ($('bnMailSwitch')) {
      const on = d.mailEnabled !== false;
      $('bnMailSwitch').classList.toggle('on', on);
      $('bnMailSwitch').setAttribute('aria-checked', on ? 'true' : 'false');
    }
    if ($('bnIpHours')) $('bnIpHours').textContent = String(d.ipHours || 6);
  } catch (e) {
    toast(e.message, 'err');
  }
}

/* ── 视图 ─────────────────────────────────────────────────────────────── */
export default {
  label: '封禁',
  group: '管理',
  rank: 3,
  async render({ el }) {
    if (myRank() < 3) {
      el.innerHTML = pageHead({ title: '封禁中心' }) +
        `<div class="notice">封禁中心<b>仅开发者可见</b>（涉及账号处置与来源 IP，权限收在最高一级）。</div>`;
      return;
    }

    el.innerHTML =
      pageHead({
        title: '封禁中心',
        sub: '封禁相关的一切都在这一页：账号、来源 IP、自动封禁记录、申诉、通知邮件。' +
          '「安全」页只负责检测（阈值 / 告警流水 / 客户端日志）。',
        actions: `<button class="btn sm ghost" data-act="bn.reload">${icon('refresh', 13)}刷新</button>`,
      }) +
      stats([
        statCell('bnStatTimed', '时限封禁'),
        statCell('bnStatPerm', '永久停用'),
        statCell('bnStatIp', '封禁 IP'),
        statCell('bnStatAuto', '近 7 天自动封禁'),
        statCell('bnStatAppeal', '待处理申诉'),
      ]) +

      /* ── 一、账号 ── */
      `<div class="label mt-16">账号封禁</div>` +
      card({
        title: '手动封禁账号',
        desc: '只做<b>有时限</b>的封禁（到期自动恢复）。永久停用请到「用户管理 → 编辑 → 状态」。' +
          '封禁会立即踢下线，并给该账号绑定的邮箱发一封说明邮件。',
        body:
          `<div class="row nowrap">` +
            `<input class="input" id="bnUser" placeholder="账号名或用户 ID">` +
            `<input class="input" id="bnDays" type="number" min="1" max="36500" placeholder="天数" style="max-width:110px">` +
            `<input class="input grow" id="bnReason" placeholder="原因（会写进邮件，建议说清楚：例如「多次在抓包环境登录」）">` +
            `<button class="btn danger" data-act="bn.ban" style="flex:0 0 auto">封禁</button>` +
          `</div>` +
          `<div class="hint">工作人员账号（开发者 / 管理员）不能被封；也不能封自己。</div>`,
      }) +
      `<div class="mt-16">${card({
        title: '生效中的账号封禁',
        desc: '按到期时间排序，永久停用排在前面。',
        actions: `<button class="btn sm ghost" data-act="bn.accountsReload">${icon('refresh', 13)}刷新</button>`,
        body: `<div id="bnAccounts">${loading()}</div>`,
      })}</div>` +

      /* ── 二、来源 IP ── */
      `<div class="label mt-16">来源 IP 封禁</div>` +
      card({
        title: '封禁 IP',
        desc: '被封 IP 的<b>任何接口请求</b>都会被拒绝（含登录、注册、发验证码）。支持单个 IPv4 / IPv6，' +
          '<b>不支持网段</b>；天数留空 = 永久。封的是来源 IP —— 同一出口下的其他人会一起被挡。' +
          '未登录客户端触发的自动封禁默认封 <b><span id="bnIpHours">6</span> 小时</b>。',
        actions: `<button class="btn sm ghost" data-act="bn.ipReload">${icon('refresh', 13)}刷新</button>`,
        body:
          `<div class="row nowrap" style="margin-bottom:12px">` +
            `<input class="input" id="bnIp" placeholder="IPv4 / IPv6，如 1.2.3.4 或 240e:390:1:2::1">` +
            `<input class="input" id="bnIpReason" placeholder="原因（可选）" style="max-width:220px">` +
            `<input class="input" id="bnIpDays" type="number" min="0" max="36500" placeholder="天数（留空=永久）" style="max-width:180px">` +
            `<button class="btn danger" data-act="bn.ipAdd" style="flex:0 0 auto">封禁</button>` +
          `</div>` +
          `<div id="bnIps">${loading()}</div>`,
      }) +

      /* ── 三、记录 ── */
      `<div class="label mt-16">自动封禁记录</div>` +
      card({
        title: '自动封禁记录',
        desc: '环境检测触发的时限封禁流水。<b>解封不会删掉记录</b>：90 天内第 2~3 次自动 ×2、' +
          '第 4 次起 ×3（封顶 30 天）。',
        actions: `<button class="btn sm ghost" data-act="bn.banLogReload">${icon('refresh', 13)}刷新</button>`,
        body: `<div id="bnAutoList">${loading()}</div>`,
      }) +

      /* ── 四、申诉 ── */
      `<div class="label mt-16">用户申诉</div>` +
      card({
        title: '用户申诉',
        desc: '用户在客户端提交的申诉。「通过并解封」会同时解除时限封禁并回写处理结果。',
        actions: `<button class="btn sm ghost" data-act="bn.appealReload">${icon('refresh', 13)}刷新</button>`,
        body:
          `<div class="toolbar">` +
            `<input class="input grow" id="bnAppealSearch" placeholder="搜索账号 / 内容…" data-input="bn.appealSearch">` +
            `<div class="chips">` +
              `<button class="chip on" data-act="bn.appealStatus" data-st="">全部</button>` +
              `<button class="chip" data-act="bn.appealStatus" data-st="pending">待处理</button>` +
              `<button class="chip" data-act="bn.appealStatus" data-st="approved">已通过</button>` +
              `<button class="chip" data-act="bn.appealStatus" data-st="rejected">已驳回</button>` +
            `</div>` +
          `</div>` +
          `<div id="bnAppeals">${loading()}</div>`,
      }) +

      /* ── 五、通知 ── */
      `<div class="label mt-16">通知</div>` +
      card({
        title: '账号状态邮件',
        desc: '账号处置与申诉的每一步都会给用户绑定的邮箱发信：' +
          '<b>封禁</b>（原因 + 到期 + 怎么申诉）、<b>解封</b>、<b>申诉回执</b>、<b>申诉通过</b>、<b>申诉驳回</b>。' +
          '没填邮箱的账号跳过（不报错）；关掉后这些操作照常生效，只是不发信。<br>' +
          '<b>安全类邮件不受这个开关影响</b>：密码重置、邮箱变更、双重验证开关会照发 —— ' +
          '它们是账号被盗时唯一的警报渠道，不能因为「封禁邮件太吵」被一起关掉。',
        body: switchRow('发送封禁 / 申诉邮件', '关闭后封禁、解封、申诉都只记流水，不发信（安全类邮件照发）', true, 'bn.mailToggle', 'id="bnMailSwitch"') +
          `<div class="row" style="margin-top:10px">` +
            `<select class="input" id="bnMailKind" style="max-width:220px">` +
              `<optgroup label="封禁 / 申诉">` +
                `<option value="ban">封禁通知</option>` +
                `<option value="unban">解封通知</option>` +
                `<option value="appeal_ack">申诉回执</option>` +
                `<option value="appeal_ok">申诉通过</option>` +
                `<option value="appeal_reject">申诉驳回</option>` +
              `</optgroup>` +
              `<optgroup label="账号安全（始终发送）">` +
                `<option value="password_changed">密码已重置</option>` +
                `<option value="password_reset_admin">密码被管理员重置</option>` +
                `<option value="email_changed">邮箱已变更</option>` +
                `<option value="email_bound">邮箱绑定成功</option>` +
                `<option value="twofa">双重验证开关</option>` +
              `</optgroup>` +
            `</select>` +
            `<button class="btn sm ghost" data-act="bn.testMail">发送测试邮件</button>` +
            `<span class="small muted">只发到你自己的告警邮箱，不会打扰用户</span>` +
          `</div>`,
      });

    loadSummary();
    loadAccounts();
    loadIps();
    loadEnvBans();
    loadAppeals();
  },
};

/* ── 动作 ─────────────────────────────────────────────────────────────── */
registerActions({
  'bn.reload': () => { loadSummary(); loadAccounts(); loadIps(); loadEnvBans(); loadAppeals(); },
  'bn.accountsReload': () => loadAccounts(),
  'bn.ipReload': () => loadIps(),
  'bn.banLogReload': () => loadEnvBans(),
  'bn.appealReload': () => loadAppeals(),

  'bn.ban': async () => {
    const user = ($('bnUser').value || '').trim();
    const days = parseInt($('bnDays').value, 10);
    const reason = ($('bnReason').value || '').trim();
    if (!user) return toast('请填账号名或用户 ID', 'err');
    if (!Number.isFinite(days) || days < 1) return toast('天数需 ≥ 1', 'err');
    const ok = await confirmDialog({
      title: '确认封禁',
      message: `封禁「${user}」${days} 天？\n\n原因：${reason || '管理员手动封禁'}\n\n` +
        '封禁会立即踢下线，并给该账号的邮箱发一封说明邮件。',
      okText: '封禁',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/admin/bans/account', 'POST', { user, days, reason });
      toast(j.message || '已封禁', 'ok');
      $('bnUser').value = ''; $('bnDays').value = ''; $('bnReason').value = '';
      loadSummary(); loadAccounts();
    } catch (e) {
      toast(e.message, 'err');
    }
  },

  'bn.unban': async (b) => {
    const id = Number(b.dataset.id);
    const name = b.dataset.name || ('#' + id);
    const ok = await confirmDialog({
      title: '解除封禁',
      message: `解除「${name}」的封禁？\n\n该账号可以立即重新登录。只解除时限封禁，不影响管理员手工永久停用。`,
      okText: '解除',
    });
    if (!ok) return;
    try {
      const j = await api('/admin/users/' + id + '/unban', 'POST', {});
      toast(j.message || '已解封', 'ok');
      loadSummary(); loadAccounts();
    } catch (e) {
      toast(e.message, 'err');
    }
  },

  'bn.ipAdd': () => addIpBan(),

  'bn.ipDel': async (b) => {
    const ip = b.dataset.ip;
    const ok = await confirmDialog({
      title: '解除 IP 封禁',
      message: `解除对 ${ip} 的封禁？解除后该 IP 可以立即访问所有接口。`,
      okText: '解除',
    });
    if (!ok) return;
    try {
      const j = await api('/admin/banned-ips/' + encodeURIComponent(ip), 'DELETE', {});
      toast(j.message || '已解封', 'ok');
      loadSummary(); loadIps();
    } catch (e) {
      toast(e.message, 'err');
    }
  },

  'bn.appealStatus': (b) => {
    state.appealStatus = b.dataset.st || '';
    const box = b.parentElement;
    if (box) box.querySelectorAll('.chip').forEach((c) => c.classList.toggle('on', c === b));
    renderAppeals();
  },

  'bn.appealSearch': () => renderAppeals(),

  'bn.appealResolve': async (b) => {
    const id = Number(b.dataset.id);
    const act = b.dataset.action;
    const reply = (($('ar-' + id) || {}).value || '').trim();
    const ok = await confirmDialog({
      title: act === 'approve' ? '通过申诉并解封' : '驳回申诉',
      message: act === 'approve'
        ? '通过后会立即解除该账号的时限封禁，并把处理回复发给用户。'
        : '驳回不会解除封禁，只把处理回复发给用户。',
      okText: act === 'approve' ? '通过并解封' : '驳回',
      danger: act !== 'approve',
    });
    if (!ok) return;
    try {
      const j = await api('/admin/appeals/' + id + '/resolve', 'POST', { action: act, reply });
      toast(j.message || '已处理', 'ok');
      loadAppeals(); loadSummary(); loadAccounts();
    } catch (e) {
      toast(e.message, 'err');
    }
  },

  'bn.appealDel': async (b) => {
    const id = Number(b.dataset.id);
    const ok = await confirmDialog({
      title: '删除申诉记录',
      message: '只删除这条申诉记录，不影响账号当前的封禁状态。',
      okText: '删除',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/admin/appeals/' + id, 'DELETE', {});
      toast(j.message || '已删除', 'ok');
      loadAppeals(); loadSummary();
    } catch (e) {
      toast(e.message, 'err');
    }
  },

  // 模板自测：只发给开发者自己的告警邮箱，绝不碰用户
  'bn.testMail': async (b) => {
    const kind = ($('bnMailKind') || {}).value || 'ban';
    busy(b, true, '发送中…');
    try {
      const j = await api('/admin/bans/test-mail', 'POST', { kind });
      toast(j.message || '已发送', 'ok');
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(b, false);
    }
  },

  'bn.mailToggle': async (b) => {
    const on = !b.classList.contains('on');
    b.classList.toggle('on', on);
    b.setAttribute('aria-checked', on ? 'true' : 'false');
    try {
      const j = await api('/admin/settings', 'PUT', { env_ban_mail_enabled: on ? '1' : '0' });
      toast(j.message || '已保存', 'ok');
    } catch (e) {
      b.classList.toggle('on', !on);
      toast(e.message, 'err');
    }
  },
});
