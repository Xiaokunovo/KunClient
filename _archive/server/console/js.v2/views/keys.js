/* 我的密钥：一次性凭证，登录即失效。生成入口必须显眼 ——
   老版本控制台只有「刷新 / 删除」，用户点遍整页都拿不到密钥。 */
import { api } from '../core/api.js';
import { state } from '../core/state.js';
import { registerActions } from '../core/actions.js';
import { icon, esc, fmtDate, toast, busy, empty, loading, copyText, confirmDialog } from '../core/ui.js';
import { pageHead, card } from '../core/layout.js';

const $ = (id) => document.getElementById(id);

function renderNewKey() {
  const box = $('newKeyBox');
  if (!box) return;
  if (!state.newKey) { box.innerHTML = ''; box.hidden = true; return; }
  box.hidden = false;
  box.innerHTML = `<div class="callout" style="margin-bottom:14px">` +
    `<span style="color:var(--accent);flex:0 0 auto">${icon('key', 16)}</span>` +
    `<div style="flex:1;min-width:0"><b>刚生成的密钥</b>` +
      `<div class="mono" style="margin-top:3px;word-break:break-all;user-select:all">${esc(state.newKey)}</div>` +
      `<div class="hint" style="margin-top:4px">到客户端「密钥登录」使用；登录成功后立即失效，不能重复使用。</div>` +
    `</div>` +
    `<button class="btn sm" data-act="keys.copy" data-code="${esc(state.newKey)}">复制</button></div>`;
}

function rows(list) {
  return list.map((k) => {
    const active = k.status === 'active';
    return `<tr>` +
      `<td style="width:34px"><input type="checkbox" class="k-chk" data-id="${Number(k.id)}" aria-label="选择密钥"></td>` +
      `<td class="mono break">${esc(k.key_code)}</td>` +
      `<td>${active ? '<span class="badge ok"><span class="dot"></span>有效</span>' : '<span class="badge"><span class="dot"></span>已停用</span>'}</td>` +
      `<td class="muted nowrap">${esc(fmtDate(k.created_at))}</td>` +
      `<td class="muted nowrap">${k.expires_at ? esc(fmtDate(k.expires_at)) : '永久'}</td>` +
      `<td><div class="cell-actions">` +
        `<button class="btn sm ghost" data-act="keys.copy" data-code="${esc(k.key_code)}">复制</button>` +
        `<button class="btn sm ${active ? 'danger' : ''}" data-act="keys.toggle" data-id="${Number(k.id)}" data-status="${active ? 'disabled' : 'active'}">${active ? '停用' : '启用'}</button>` +
        `<button class="btn sm danger" data-act="keys.del" data-id="${Number(k.id)}">删除</button>` +
      `</div></td>` +
    `</tr>`;
  }).join('');
}

async function load() {
  const box = $('keyList');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/user/keys');
    state.keys = j.data || [];
    const list = state.keys;
    box.innerHTML = list.length
      ? `<div class="tbl-wrap"><table class="tbl"><thead><tr>` +
          `<th style="width:34px"><input type="checkbox" id="chkAll" data-change="keys.checkAll" aria-label="全选"></th>` +
          `<th>密钥</th><th style="width:110px">状态</th><th style="width:150px">创建时间</th>` +
          `<th style="width:150px">过期时间</th><th style="width:210px"></th></tr></thead>` +
          `<tbody>${rows(list)}</tbody></table></div>`
      : empty('还没有密钥', '点右上角「生成密钥」创建一枚，用于客户端密钥登录。', 'key');
    const count = $('keyCount');
    if (count) count.textContent = list.length ? `共 ${list.length} 枚` : '';
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
    toast(e.message, 'err');
  }
}

export default {
  label: '密钥',
  group: '工作台',
  rank: 0,
  async render({ el }) {
    el.innerHTML =
      pageHead({
        title: '我的密钥',
        sub: '密钥是<b>一次性凭证</b>：登录成功后立即失效，不能重复使用。只属于你自己的账号，可随时停用或删除。',
        actions:
          `<button class="btn sm ghost" data-act="keys.refresh">${icon('refresh', 13)}刷新</button>` +
          `<button class="btn sm pri" data-act="keys.gen">${icon('plus', 13)}生成密钥</button>`,
      }) +
      `<div id="newKeyBox" hidden></div>` +
      card({
        title: '密钥列表',
        actions: `<span class="small muted" id="keyCount"></span>` +
          `<button class="btn sm danger" data-act="keys.batchDel">删除选中</button>`,
        flush: true,
        body: `<div id="keyList">${loading()}</div>`,
      });
    renderNewKey();
    load();
  },
};

registerActions({
  'keys.refresh': () => load(),
  'keys.gen': async (btn) => {
    busy(btn, true, '生成中…');
    try {
      const j = await api('/user/generate-key', 'POST', {});
      state.newKey = (j.data && j.data.key) || '';
      if (!state.newKey) throw new Error('服务端没有返回密钥');
      renderNewKey();
      toast('密钥已生成', 'ok');
      load();
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(btn, false, '生成密钥');
    }
  },
  'keys.copy': (b) => copyText(b.dataset.code),
  'keys.toggle': async (b) => {
    try {
      const j = await api('/user/keys/' + b.dataset.id, 'PUT', { status: b.dataset.status });
      toast(j.message || '已更新', 'ok');
      load();
    } catch (e) { toast(e.message, 'err'); }
  },
  'keys.del': async (b) => {
    const ok = await confirmDialog({
      title: '删除密钥',
      message: '删除后不可恢复。如果这枚密钥还没用过，删除后也无法再用于登录。',
      okText: '删除',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/user/keys/' + b.dataset.id, 'DELETE');
      toast(j.message || '已删除', 'ok');
      load();
    } catch (e) { toast(e.message, 'err'); }
  },
  'keys.checkAll': (b) => {
    document.querySelectorAll('.k-chk').forEach((c) => { c.checked = b.checked; });
  },
  'keys.batchDel': async () => {
    const ids = [...document.querySelectorAll('.k-chk:checked')].map((c) => parseInt(c.dataset.id, 10));
    if (!ids.length) return toast('请先勾选要删除的密钥', 'err');
    const ok = await confirmDialog({
      title: '批量删除密钥',
      message: `确定删除选中的 ${ids.length} 枚密钥？删除后不可恢复。`,
      okText: '删除 ' + ids.length + ' 枚',
      danger: true,
    });
    if (!ok) return;
    try {
      const j = await api('/user/keys/batch-delete', 'POST', { ids });
      toast(j.message || '已删除', 'ok');
      load();
    } catch (e) { toast(e.message, 'err'); }
  },
});
