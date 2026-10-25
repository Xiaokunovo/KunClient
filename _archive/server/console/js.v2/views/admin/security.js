/* 管理 › 安全：**只管检测** —— 环境检测阈值 / 总开关 / 告警流水 / 客户端日志。
   第一张卡（阈值 / 总开关）管理员（2）可用；告警流水与客户端日志仅开发者（3），
   服务端也会独立拒绝 —— 前端的隐藏从来不是门槛。
   注意：封禁处置（账号 / IP / 自动记录 / 申诉 / 通知邮件）在「封禁中心」（admin/bans.js）：
      以前这些混在本页，阈值旁边摆着 IP 封禁、申诉夹在告警列表下面 —— 找功能先要猜在哪一页。 */
import { api, apiBlob } from '../../core/api.js';
import { state, myRank, ENV_SIGNAL_CN, friendlySignalReason } from '../../core/state.js';
import { registerActions } from '../../core/actions.js';
import {
  icon, esc, fmtDate, fmtSize, toast, busy, empty, loading, switchRow,
  confirmDialog, openModal,
} from '../../core/ui.js';
import { pageHead, card } from '../../core/layout.js';

const $ = (id) => document.getElementById(id);

/* ── 设置 ─────────────────────────────────────────────────────────────── */
async function loadSettings() {
  try {
    const j = await api('/admin/settings');
    const d = j.data || {};
    const set = (id, v) => { if ($(id)) $(id).value = v; };
    if ($('tgEnvBan')) $('tgEnvBan').classList.toggle('on', d.env_auto_ban_enabled === '1');
    set('envHighDays', d.env_ban_high_days || '7');
    set('envMidDays', d.env_ban_mid_days || '3');
    set('envMidThr', d.env_mid_strike_threshold || '3');
    set('envMultiThr', d.env_multi_instance_threshold || '3');
    set('envBreaker', d.env_breaker_accounts || '5');
    set('envBanIp', d.env_ban_ip_hours || '6');
    set('devAlertMail', d.dev_alert_email || '');
  } catch (e) {
    toast(e.message, 'err');
  }
}

/* ── 环境告警 ─────────────────────────────────────────────────────────── */
/* 「配置类」信号：服务端**不单独封禁**它们（见 evaluateEnvSignal 的 MID_NEEDS_COMPANY），
   必须配合硬信号才计数。列表里标出来，省得看到一条 MID 就以为「这人被封了」。
   两端的清单必须一致：客户端 EnvInspector + 服务端 MID_NEEDS_COMPANY。 */
const MID_NEEDS_COMPANY = ['known_tool', 'proxy_intercept'];

// 信号码 → 人话：与「封禁中心」共用同一份实现（core/state.js），别再各写一套
const friendlyReason = friendlySignalReason;

function renderEnvReports() {
  const box = $('envReports');
  if (!box) return;
  const q = ($('envRepSearch') ? $('envRepSearch').value : '').trim().toLowerCase();
  const list = (state.envReports || []).filter((r) => {
    if (state.envLevel && (r.level || '') !== state.envLevel) return false;
    if (q) {
      const hay = ((r.username || '') + ' ' + (r.signal || '') + ' ' +
        (ENV_SIGNAL_CN[r.signal] || '') + ' ' + (r.ip || '') + ' ' + (r.detail || '')).toLowerCase();
      if (!hay.includes(q)) return false;
    }
    return true;
  });
  if (!list.length) {
    box.innerHTML = empty(
      (state.envReports || []).length ? '没有符合筛选条件的告警' : '暂无环境告警',
      '', 'shieldCheck');
    return;
  }
  const LV = { high: 'bad', mid: 'warn', low: '' };
  box.innerHTML = list.map((r) => `<div class="item">` +
    `<div class="item-h"><b>${esc(r.username || '—')}</b>` +
      `<span class="badge ${LV[r.level] || ''}">${esc(String(r.level || '').toUpperCase())}</span>` +
      `<span class="small muted">${esc(ENV_SIGNAL_CN[r.signal] || r.signal || '')}</span>` +
      // 单独出现不会封号的信号，明确标出来（否则一条 MID 会被误读成「已封禁」）
      (MID_NEEDS_COMPANY.includes(r.signal)
        ? `<span class="badge" title="单独出现只记录，需配合硬信号才计入封禁">不单独封禁</span>` : '') +
      `<span class="sp"></span><span class="item-meta">${esc(fmtDate(r.created_at))}</span></div>` +
    `<div class="item-b">${esc(r.detail || '')}</div>` +
    `<div class="item-foot">build ${esc(r.build || '—')} · ${esc(r.ip || '—')}</div>` +
  `</div>`).join('');
}

async function loadEnvReports() {
  if (myRank() < 3) return;
  const box = $('envReports');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/admin/env-reports?limit=100');
    if ($('envAutoBanState')) {
      $('envAutoBanState').textContent = j.autoBanEnabled ? '自动封禁：开' : '自动封禁：关（已止血）';
      $('envAutoBanState').className = 'small ' + (j.autoBanEnabled ? 'muted' : '');
    }
    state.envReports = j.data || [];
    renderEnvReports();
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

/* ── 申诉 ─────────────────────────────────────────────────────────────── */
let logModalApi = null;   // 日志详情弹窗（客户端日志用）

/* ── 客户端日志 ───────────────────────────────────────────────────────── */
function renderClientLogs() {
  const box = $('clientLogs');
  if (!box) return;
  const q = ($('clSearch') ? $('clSearch').value : '').trim().toLowerCase();
  const list = (state.clientLogs || []).filter((r) => {
    if (!q) return true;
    return ((r.username || '') + ' ' + (r.build || '') + ' ' + (r.note || '') + ' ' + (r.ip || ''))
      .toLowerCase().includes(q);
  });
  if ($('clCount')) {
    const nw = (state.clientLogs || []).filter((r) => r.status === 'new').length;
    $('clCount').textContent = '共 ' + (state.clientLogs || []).length + ' 份' +
      (nw ? '（' + nw + ' 份未看）' : '') + ' · 保留 ' + state.retention + ' 天';
  }
  if (!list.length) {
    box.innerHTML = empty(
      (state.clientLogs || []).length ? '没有符合筛选条件的日志' : '还没有用户上传日志',
      '', 'file');
    return;
  }
  box.innerHTML = list.map((r) => `<div class="item">` +
    `<div class="item-h"><b>#${Number(r.id)} ${esc(r.username || '—')}</b>` +
      (r.status === 'new' ? '<span class="badge info">NEW</span>' : '') +
      `<span class="badge">${esc(r.build || '未知版本')}</span>` +
      `<span class="small muted">${fmtSize(r.size)}</span>` +
      `<span class="sp"></span><span class="item-meta">${esc(fmtDate(r.created_at))}</span></div>` +
    (r.note ? `<div class="item-b">${esc(r.note)}</div>` : `<div class="item-b muted">（用户未填写问题描述）</div>`) +
    `<div class="item-foot">来源 IP ${esc(r.ip || '—')}</div>` +
    `<div class="item-actions">` +
      `<button class="btn sm" data-act="sec.logView" data-id="${Number(r.id)}">查看</button>` +
      `<button class="btn sm ghost" data-act="sec.logDownload" data-id="${Number(r.id)}">下载</button>` +
      `<button class="btn sm danger" data-act="sec.logDel" data-id="${Number(r.id)}">删除</button>` +
    `</div>` +
  `</div>`).join('');
}

async function loadClientLogs() {
  if (myRank() < 3) return;
  const box = $('clientLogs');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/admin/client-logs?limit=200');
    state.clientLogs = j.data || [];
    state.retention = j.retentionDays || 30;
    if ($('clRetention')) $('clRetention').value = state.retention;
    renderClientLogs();
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

async function viewLog(id) {
  const m = openModal({ title: '日志详情 #' + id, width: 'wide', body: loading() });
  logModalApi = m;
  try {
    const j = await api('/admin/client-logs/' + id);
    const d = j.data || {};
    const meta = d.meta || {};
    const kv = [
      ['上传账号', (d.username || '—') + (meta.loggedIn === false ? '（未登录）' : '')],
      ['客户端版本', d.build || meta.clientVersion || '—'],
      ['EDL 引擎', meta.engineVersion || '—'],
      ['操作系统', meta.os || '—'],
      ['架构 / Qt', (meta.arch || '—') + ' · Qt ' + (meta.qt || '—')],
      ['实例标识', meta.instanceId || '—'],
      ['来源 IP', d.ip || '—'],
      ['上传时间', fmtDate(d.createdAt)],
    ];
    const files = d.files || [];
    m.body.innerHTML =
      `<div class="sec"><div class="t">上传信息</div><dl class="kv">` +
        kv.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join('') +
      `</dl></div>` +
      (d.note ? `<div class="sec"><div class="t">用户的问题描述</div><div class="item-b">${esc(d.note)}</div></div>` : '') +
      `<div class="sec"><div class="t">日志文件（${files.length}）</div>` +
        (files.length ? files.map((f) => `<div style="margin-bottom:10px">` +
          `<div class="small muted" style="margin-bottom:4px">${esc(f.name)} · ${fmtSize(f.size)}` +
            (f.binary ? ' · <b>二进制 / 超大内容，请下载查看</b>' : '') + `</div>` +
          (f.binary ? '' : `<pre class="code-block" style="max-height:260px">${esc(f.text || '（空）')}</pre>`) +
        `</div>`).join('') : '<div class="muted small">没有可预览的文件。</div>') +
      `</div>` +
      `<div class="row"><button class="btn" data-act="sec.logDownload" data-id="${Number(id)}">下载原始包</button>` +
        `<button class="btn danger" data-act="sec.logDel" data-id="${Number(id)}">删除这份日志</button></div>`;
    /* 打开详情即已读（服务端把 new 置 viewed）→ 本地也同步，NEW 徽标立刻消失。 */
    const row = (state.clientLogs || []).find((r) => Number(r.id) === Number(id));
    if (row && row.status === 'new') { row.status = 'viewed'; renderClientLogs(); }
  } catch (e) {
    m.body.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

async function downloadLog(id) {
  try {
    const blob = await apiBlob('/admin/client-logs/' + id + '/download');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'kunclient-log-' + id + '.json';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  } catch (e) {
    toast('下载失败：' + (e.message || e), 'err');
  }
}

/* ── 视图 ─────────────────────────────────────────────────────────────── */
export default {
  label: '安全',
  group: '管理',
  rank: 2,
  async render({ el }) {
    const dev = myRank() >= 3;
    const numRow = (label, id, key, hint) =>
      `<div class="field"><label for="${id}">${label}</label>` +
        `<div class="row nowrap"><input class="input" id="${id}" type="number" min="1" style="width:110px">` +
          `<button class="btn" data-act="sec.saveNum" data-key="${key}" data-input="${id}" data-label="${esc(label)}">保存</button></div>` +
        (hint ? `<div class="hint">${hint}</div>` : '') +
      `</div>`;

    el.innerHTML =
      pageHead({
        title: '安全',
        sub: '这一页只管**检测**：环境检测阈值、告警流水、客户端日志。' +
          '封禁处置（账号 / IP / 自动记录 / 申诉 / 通知邮件）都在「封禁中心」。阈值改动即时生效，不需要发版。',
        actions: `<button class="btn sm ghost" data-act="sec.reload">${icon('refresh', 13)}刷新</button>`,
      }) +
      card({
        title: '环境检测与自动封禁',
        desc: '客户端<b>启动即检测</b>抓包 / 中间人 / 逆向工具，按信号强度分级处置。' +
          '误伤集中爆发时先关总开关止血。所有阈值改动即时生效，不需要发版。',
        body:
          switchRow('自动封禁总开关', '关闭后只记录流水、一律不自动封禁（紧急刹车）', false, 'sec.toggle', 'data-key="env_auto_ban_enabled" id="tgEnvBan"') +
          `<div class="hr"></div>` +
          `<div class="grid-form">` +
            numRow('HIGH / 中间人组合：封号天数', 'envHighDays', 'env_ban_high_days',
              '多开按 MID 天数封（账号共享 ≠ 客户端被改）') +
            numRow('MID 累积命中：封号天数', 'envMidDays', 'env_ban_mid_days',
              '90 天内重复违规自动 ×2 / ×3，封顶 30 天') +
            numRow('MID 累积阈值（同一自然日几次）', 'envMidThr', 'env_mid_strike_threshold',
              '「配置类」信号（逆向工具 / 系统代理）单独出现不计入') +
            numRow('多开阈值（60 秒内几个实例）', 'envMultiThr', 'env_multi_instance_threshold') +
            numRow('熔断账号数（1 小时内）', 'envBreaker', 'env_breaker_accounts', '同一版本出现这么多账号异常 → 暂停自动封禁') +
            numRow('未登录违规：封 IP 小时数', 'envBanIp', 'env_ban_ip_hours',
              '0 = 不封。只对「服务端自证 / 客户端被改 / 中间人组合」生效，单条抓包或逆向工具只记录') +
          `</div>` +
          `<div class="field mt-12 mb-0"><label for="devAlertMail">环境告警收件邮箱 <span class="opt">留空 = 用管理员邮箱</span></label>` +
            `<div class="row nowrap"><input class="input" id="devAlertMail" placeholder="dev@example.com" style="max-width:300px">` +
              `<button class="btn" data-act="sec.saveAlert">保存告警邮箱</button></div>` +
            `<div class="hint">未登录客户端命中「服务端自证 / 客户端被改 / 中间人组合」时，按来源 IP 封禁并向该邮箱发送告警（含 IP、信号、详情、时间）。同一 IP 在封禁期内不重复发信。</div></div>`,
      }) +
      (dev
        ? `<div class="mt-16">${card({
            title: '环境告警流水 <span class="badge warn" style="margin-left:6px">仅开发者</span>',
            desc: '自动封禁按信号强度分级：弱信号只记录；中信号按日累积到阈值才封；强信号直接封。' +
              '<b>「配置类」信号（逆向工具进程 / 系统代理指向本机）单独出现不封号</b>，' +
              '必须配合硬信号（Hook / 注入 / 完整性 / 抓包工具）才计数 —— 这两条是刷机用户的常态。' +
              '误伤暴涨时，回到上方关掉自动封禁总开关止血。' +
              '封禁结果（谁被封了、还剩几天）在「封禁中心」。',
            actions: `<span class="small muted" id="envAutoBanState"></span>` +
              `<button class="btn sm ghost" data-act="sec.envReload">${icon('refresh', 13)}刷新</button>`,
            body:
              `<div class="toolbar">` +
                `<input class="input grow" id="envRepSearch" placeholder="搜索账号 / 信号 / IP…" data-input="sec.envSearch">` +
                `<div class="chips" id="envRepChips">` +
                  `<button class="chip on" data-act="sec.envLevel" data-lv="">全部</button>` +
                  `<button class="chip" data-act="sec.envLevel" data-lv="high">HIGH</button>` +
                  `<button class="chip" data-act="sec.envLevel" data-lv="mid">MID</button>` +
                  `<button class="chip" data-act="sec.envLevel" data-lv="low">LOW</button>` +
                `</div>` +
              `</div>` +
              `<div id="envReports">${loading()}</div>`,
          })}</div>` +
          `<div class="mt-16">${card({
            title: '客户端日志上传 <span class="badge warn" style="margin-left:6px">仅开发者</span>',
            desc: '用户在客户端「设置 → 诊断与日志」主动上传的日志包。内容 = 最近几天的日志 + 版本/系统环境 + 上传者账号；' +
              '<b>不采集密码、设备序列号等敏感信息</b>。',
            body:
              `<div class="toolbar">` +
                `<label class="small muted">保留天数</label>` +
                `<input class="input" id="clRetention" type="number" min="1" max="3650" value="30" style="width:100px">` +
                `<button class="btn sm" data-act="sec.saveRetention">保存</button>` +
                `<span class="sp"></span>` +
                `<span class="small muted" id="clCount"></span>` +
              `</div>` +
              `<div class="toolbar">` +
                `<input class="input grow" id="clSearch" placeholder="搜索账号 / 版本 / 描述…" data-input="sec.logSearch">` +
                `<button class="btn sm ghost" data-act="sec.logsReload">${icon('refresh', 13)}刷新</button>` +
              `</div>` +
              `<div id="clientLogs">${loading()}</div>`,
          })}</div>`
        : `<div class="mt-16"><div class="notice">` +
            `环境告警流水与客户端日志<b>仅开发者可见</b>。管理员可以调整上方阈值与自动封禁开关。` +
          `</div></div>`);

    loadSettings();
    if (dev) {
      loadEnvReports();
      loadClientLogs();
    }
  },
};

/* ── 动作 ─────────────────────────────────────────────────────────────── */
registerActions({
  'sec.reload': () => {
    loadSettings();
    if (myRank() >= 3) { loadEnvReports(); loadClientLogs(); }
  },

  'sec.toggle': async (b) => {
    const on = !b.classList.contains('on');
    b.classList.toggle('on', on);
    b.setAttribute('aria-checked', on ? 'true' : 'false');
    try {
      const j = await api('/admin/settings', 'PUT', { [b.dataset.key]: on ? '1' : '0' });
      toast(j.message || '已保存', 'ok');
    } catch (e) {
      b.classList.toggle('on', !on);
      toast(e.message, 'err');
    }
  },

  'sec.saveNum': async (b) => {
    const el = $(b.dataset.input);
    const v = String((el && el.value) || '').trim();
    if (v !== '' && !/^\d+$/.test(v)) return toast('请输入非负整数', 'err');
    busy(b, true, '保存中…');
    try {
      const j = await api('/admin/settings', 'PUT', { [b.dataset.key]: v === '' ? '0' : v });
      toast((b.dataset.label || '设置') + '已保存：' + (v === '' ? 0 : v), 'ok');
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(b, false, '保存');
    }
  },

  'sec.saveAlert': async (b) => {
    const v = String($('devAlertMail').value || '').trim();
    if (v && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(v)) return toast('邮箱格式不正确', 'err');
    busy(b, true, '保存中…');
    try {
      const j = await api('/admin/settings', 'PUT', { dev_alert_email: v });
      toast(v ? '告警邮箱已保存' : '已清空（将改用管理员邮箱）', 'ok');
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(b, false, '保存告警邮箱');
    }
  },

  'sec.saveRetention': async (b) => {
    const v = String($('clRetention').value || '').trim();
    if (!/^\d+$/.test(v) || parseInt(v, 10) < 1) return toast('请输入正整数天数', 'err');
    busy(b, true, '保存中…');
    try {
      const j = await api('/admin/settings', 'PUT', { client_log_retention_days: v });
      toast('日志保留天数已保存：' + v, 'ok');
      loadClientLogs();
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(b, false, '保存');
    }
  },

  'sec.envReload': () => loadEnvReports(),
  'sec.envLevel': (b) => {
    state.envLevel = b.dataset.lv || '';
    b.parentElement.querySelectorAll('.chip').forEach((c) => c.classList.toggle('on', c === b));
    renderEnvReports();
  },
  'sec.envSearch': () => renderEnvReports(),
  'sec.logsReload': () => loadClientLogs(),
  'sec.logSearch': () => renderClientLogs(),
  'sec.logView': (b) => viewLog(b.dataset.id),
  'sec.logDownload': (b) => downloadLog(b.dataset.id),
  'sec.logDel': async (b) => {
    const ok = await confirmDialog({
      title: '删除日志',
      message: '删除日志 #' + b.dataset.id + '？\n\n文件与记录会一起删掉，不可恢复。',
      okText: '删除',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/admin/client-logs/' + b.dataset.id, 'DELETE', {});
      toast(j.message || '已删除', 'ok');
      if (logModalApi) { logModalApi.close(); logModalApi = null; }
      loadClientLogs();
    } catch (e) { toast(e.message, 'err'); }
  },
});
