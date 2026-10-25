/* 管理 › 运营：站点统计、到期与版本闸门、官网展示口径、公告。
   所有设置都走 PUT /admin/settings 的「单键写入」—— 一次只提交改动的那一项，
   避免整页表单回写把别的管理员刚改的值覆盖掉。 */
import { api, publicJson } from '../../core/api.js';
import { registerActions } from '../../core/actions.js';
import { icon, esc, toast, busy, statCell, switchRow, confirmDialog, loading } from '../../core/ui.js';
import { pageHead, card } from '../../core/layout.js';

const $ = (id) => document.getElementById(id);
const num = (v, d) => (v === undefined || v === null || v === '' ? d : v);

let MODULES = null;
let FEATURES = [];

/* ── 站点统计 ─────────────────────────────────────────────────────────── */
async function loadStats() {
  const box = $('admStats');
  if (!box) return;
  try {
    const j = await api('/stats');
    const d = j.data || {};
    box.innerHTML = [
      statCell('总用户数', d.totalUsers ?? '—'),
      statCell('活跃用户', d.activeUsers ?? '—'),
      statCell('密钥总数', d.totalKeys ?? '—'),
      statCell('有效密钥', d.activeKeys ?? '—'),
      statCell('24h 登录成功', d.loginsToday ?? '—', { cls: 'ok' }),
      statCell('24h 登录失败', d.failedLogins ?? '—', { cls: d.failedLogins ? 'bad' : '' }),
      statCell('功能使用总次数', d.totalUsage ?? '—'),
    ].join('');
  } catch (e) {
    box.innerHTML = statCell('站点统计', '读取失败：' + e.message, { cls: 'sm' });
  }
}

/* ── 设置读写 ─────────────────────────────────────────────────────────── */
async function saveSetting(payload, okMsg) {
  const j = await api('/admin/settings', 'PUT', payload);
  toast(j.message || okMsg || '已保存', 'ok');
  return j;
}

async function loadSettings() {
  try {
    const j = await api('/admin/settings');
    const d = j.data || {};
    if ($('annText')) $('annText').value = d.announcement || '';
    if ($('toolExp')) $('toolExp').value = d.tool_expire || '';
    if ($('minVer')) $('minVer').value = d.min_client_version || '';
    /* 缺键按默认值处理，与服务端 seed 一致：
       官网下载默认开、内测默认关、云端更新默认开。 */
    if ($('tgDlOpen')) $('tgDlOpen').classList.toggle('on', d.site_download_enabled !== '0');
    if ($('tgInternal')) $('tgInternal').classList.toggle('on', d.internal_stage === '1');
    if ($('tgCloudUpdate')) $('tgCloudUpdate').classList.toggle('on', d.cloud_update_enabled !== '0');
  } catch (e) {
    toast(e.message, 'err');
  }
}


/* ── 视图 ─────────────────────────────────────────────────────────────── */
export default {
  label: '运营',
  group: '管理',
  rank: 2,
  async render({ el }) {
    el.innerHTML =
      pageHead({
        title: '运营',
        sub: '站点统计、到期与版本闸门、官网展示与公告。改动即时生效，不需要重新部署。',
        actions: `<button class="btn sm ghost" data-act="ops.reload">${icon('refresh', 13)}刷新</button>`,
      }) +
      `<div class="stats" id="admStats">${statCell('站点统计', '加载中…', { cls: 'sm' })}</div>` +

      `<div class="split even mt-16">` +
        `<div>${card({
          title: '到期与版本闸门',
          desc: '整包到期时间与客户端版本底线（与收费无关，付费体系已于 2026-10-20 下线）。',
          body:
            `<div class="field mt-12"><label for="toolExp">工具到期时间</label>` +
              `<div class="row nowrap"><input class="input" id="toolExp" type="date" style="max-width:190px">` +
                `<button class="btn" data-act="ops.saveExpire">保存</button>` +
                `<button class="btn ghost" data-act="ops.clearExpire">清除</button></div>` +
              `<div class="hint">留空 = 永不过期；到期后所有客户端被拦截。</div></div>` +
            `<div class="field"><label for="minVer">客户端版本底线</label>` +
              `<div class="row nowrap"><input class="input mono" id="minVer" placeholder="如 1.1.4.0" style="max-width:190px">` +
                `<button class="btn" data-act="ops.saveMinVer">保存</button>` +
                `<button class="btn ghost" data-act="ops.clearMinVer">关闭闸门</button></div>` +
              `<div class="hint">按 User-Agent 识别版本；低于底线的客户端所有业务请求返回 426。留空 = 不限制。</div></div>` +
              `</div>`,
        })}</div>` +

        `<div>${card({
          title: '官网展示',
          desc: '决定官网首页的对外口径。官网实时读取该设置，切换后刷新页面即生效。',
          body:
            `<div class="label">对外口径</div>` +
            `<div class="hint" style="margin-top:8px">全部功能免费（无会员、无卡密、无次数限制）。` +
            `阶段选择已随付费体系于 2026-10-20 下线，官网固定显示「免费使用」。</div>` +
            `<div class="hr"></div>` +
            switchRow('官网下载', '只关官网：收起下载区、首屏下载按钮与版本号', true, 'ops.toggle', 'data-key="site_download_enabled" id="tgDlOpen"') +
            switchRow('内测阶段', '全站总闸：官网<b>和</b>控制台一起收起下载入口，<span class="mono">/assets</span> 直链返回 403。开启后「官网下载」不再起作用。', false, 'ops.toggle', 'data-key="internal_stage" id="tgInternal"') +
            switchRow('云端更新', '客户端自更新通道。关闭后只停自更新，不影响手动下载，与「内测阶段」正交。', true, 'ops.toggle', 'data-key="cloud_update_enabled" id="tgCloudUpdate"'),
        })}</div>` +
      `</div>` +

      `<div class="mt-16">${card({
        title: '公告设置',
        desc: '发布后显示在所有用户控制台概览顶部；清空内容并保存即撤销公告。',
        body:
          `<textarea class="textarea" id="annText" maxlength="1000" style="min-height:90px" placeholder="公告内容（留空 = 撤销公告）"></textarea>` +
          `<button class="btn pri mt-8" id="annSaveBtn" data-act="ops.saveAnn">发布公告</button>`,
      })}</div>`;

    loadStats();
    loadSettings();
  },
};

/* ── 动作 ─────────────────────────────────────────────────────────────── */
registerActions({
  'ops.reload': () => { loadStats(); loadSettings(); },

  'ops.toggle': async (b) => {
    const key = b.dataset.key;
    const on = !b.classList.contains('on');
    b.classList.toggle('on', on);
    b.setAttribute('aria-checked', on ? 'true' : 'false');
    try {
      await saveSetting({ [key]: on ? '1' : '0' }, '已保存');
    } catch (e) {
      b.classList.toggle('on', !on);
      toast(e.message, 'err');
    }
  },

  'ops.saveExpire': async (btn) => {
    busy(btn, true, '保存中…');
    try {
      await saveSetting({ tool_expire: $('toolExp').value || '' });
    } catch (e) { toast(e.message, 'err'); }
    finally { busy(btn, false, '保存'); }
  },
  'ops.clearExpire': async () => {
    try {
      await saveSetting({ tool_expire: '' }, '已设为永不过期');
      if ($('toolExp')) $('toolExp').value = '';
    } catch (e) { toast(e.message, 'err'); }
  },
  'ops.saveMinVer': async (btn) => {
    const v = String($('minVer').value || '').trim();
    if (v && !/^\d+(\.\d+)*$/.test(v)) return toast('版本号格式不正确（如 1.1.4.0）', 'err');
    busy(btn, true, '保存中…');
    try { await saveSetting({ min_client_version: v }, v ? '版本底线已保存：' + v : '版本闸门已关闭'); }
    catch (e) { toast(e.message, 'err'); }
    finally { busy(btn, false, '保存'); }
  },
  'ops.clearMinVer': async () => {
    try {
      await saveSetting({ min_client_version: '' }, '版本闸门已关闭');
      if ($('minVer')) $('minVer').value = '';
    } catch (e) { toast(e.message, 'err'); }
  },
  'ops.saveAnn': async (btn) => {
    busy(btn, true, '保存中…');
    try {
      await saveSetting({ announcement: $('annText').value }, '公告已更新');
    } catch (e) { toast(e.message, 'err'); }
    finally { busy(btn, false, '发布公告'); }
  },
});
