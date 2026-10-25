/* 概览：公告 + 账号摘要 + 最近使用 + 客户端下载 + 个性签名。 */
import { api, publicJson } from '../core/api.js';
import { state, setMe, ROLE_LABEL } from '../core/state.js';
import { registerActions } from '../core/actions.js';
import { icon, esc, fmtDate, toast, statCell, empty, busy } from '../core/ui.js';
import { pageHead, card, stats } from '../core/layout.js';

const $ = (id) => document.getElementById(id);
let annText = '';

async function loadAnnouncement() {
  const box = $('annBar');
  if (!box) return;
  try {
    const j = await api('/announcement');
    const txt = String((j.data && j.data.content) || '').trim();
    annText = txt;
    if (!txt) { box.innerHTML = ''; return; }
    if (localStorage.getItem('annDismissed') === txt) { box.innerHTML = ''; return; }
    box.innerHTML = `<div class="callout" style="margin-bottom:14px">` +
      `<span style="color:var(--accent);flex:0 0 auto">${icon('zap', 16)}</span>` +
      `<div style="flex:1;min-width:0"><b>公告</b>` +
      `<div style="white-space:pre-wrap;margin-top:3px">${esc(txt)}</div></div>` +
      `<button class="btn sm ghost" data-act="ov.dismissAnn">知道了</button></div>`;
  } catch (e) {
    box.innerHTML = '';
  }
}

async function loadDownload() {
  const box = $('dlCard');
  if (!box) return;
  const site = await publicJson('/api/site/info');
  /* 内测阶段 / 官网下载关闭：整张卡收起，连版本号一起 —— 不留半个空壳。 */
  if (site && site.success && site.data && site.data.downloadEnabled === false) {
    box.hidden = true;
    return;
  }
  const j = await publicJson('/api/app/version');
  if (!j || !j.success || !j.data) { box.hidden = true; return; }
  const d = j.data;
  const meta = [d.sizeText, d.buildDate ? '发布于 ' + d.buildDate : ''].filter(Boolean).join(' · ') || '发布时间以服务器为准';
  const exe = String(d.downloadUrl || '');
  const zip = String(d.zipUrl || (exe ? exe.replace(/\.exe(\?|$)/i, '.7z$1') : ''));
  box.hidden = false;
  box.innerHTML = card({
    title: '下载客户端',
    desc: '安装版双击即用；免安装版解压即用，适合没有安装权限或想随身携带的场景。',
    body:
      `<div class="row" style="align-items:flex-start;gap:14px">` +
        `<div style="flex:1;min-width:150px">` +
          `<div style="font:600 17px/1.2 var(--mono)">v${esc(d.version || '-')}</div>` +
          `<div class="hint" style="margin-top:5px">${esc(meta)}</div>` +
        `</div>` +
        `<div class="row" style="gap:6px">` +
          (exe ? `<a class="btn pri" href="${esc(exe)}" rel="noopener">下载 .exe</a>` : '') +
          (zip && zip !== exe ? `<a class="btn" href="${esc(zip)}" rel="noopener">免安装 .7z</a>` : '') +
        `</div>` +
      `</div>` +
      (d.changelog
        ? `<details style="margin-top:12px"><summary class="small muted" style="cursor:pointer">更新说明</summary>` +
          `<pre class="code-block" style="margin-top:8px">${esc(d.changelog)}</pre></details>`
        : ''),
  });
}

async function loadData() {
  const statBox = $('ovStats');
  const logBox = $('ovLogs');
  if (!statBox || !logBox) return;
  try {
    const [p, k, l] = await Promise.all([
      api('/profile'),
      api('/user/keys'),
      api('/usage/logs?limit=8'),
    ]);
    setMe(p.data);
    const u = p.data || {};
    const keys = k.data || [];
    const active = keys.filter((x) => x.status === 'active').length;

    statBox.innerHTML = [
      statCell('用户 ID', '#' + (u.id || '-'), { cls: 'mono' }),
      statCell('角色', ROLE_LABEL[u.role] || '普通用户', { cls: 'sm' }),
      statCell('密钥总数', keys.length),
      statCell('有效密钥', active, { cls: active ? 'ok' : '' }),
      statCell('注册时间', fmtDate(u.created_at), { cls: 'sm' }),
    ].join('');

    if ($('sigInput') && document.activeElement !== $('sigInput')) {
      $('sigInput').value = u.signature || '';
    }

    /* /usage/logs 返回的是 { logs, total }，不是数组 —— 老控制台在这里把对象当数组用过，
       结果「最近使用」永远空着。这里显式取 .logs。 */
    const logs = (l.data && l.data.logs) || [];
    logBox.innerHTML = logs.length
      ? `<div class="tbl-wrap"><table class="tbl compact"><thead><tr>` +
          `<th style="width:150px">时间</th><th style="width:180px">功能</th><th>详情</th></tr></thead><tbody>` +
          logs.map((x) => `<tr>` +
            `<td class="muted nowrap">${esc(fmtDate(x.created_at))}</td>` +
            `<td>${esc(x.feature || '—')}</td>` +
            `<td class="dim break">${esc(x.detail || '—')}</td>` +
          `</tr>`).join('') +
        `</tbody></table></div>`
      : empty('暂无使用记录', '客户端执行刷写、备份等操作后，这里会出现流水。', 'activity');
  } catch (e) {
    logBox.innerHTML = empty('加载失败', e.message, 'alert');
    toast(e.message, 'err');
  }
}

export default {
  label: '概览',
  group: '工作台',
  rank: 0,
  async render({ el }) {
    el.innerHTML =
      pageHead({
        title: '概览',
        sub: '账号、密钥与最近使用记录一屏看完。',
        actions: `<button class="btn sm" data-act="ov.refresh">${icon('refresh', 13)}刷新</button>`,
      }) +
      `<div id="annBar"></div>` +
      `<div class="stats" id="ovStats">${statCell('用户 ID', '…')}${statCell('角色', '…')}${statCell('密钥总数', '…')}${statCell('有效密钥', '…')}${statCell('注册时间', '…')}</div>` +
      `<div class="split mt-16">` +
        `<div>${card({
          title: '最近使用记录',
          desc: '客户端最近 8 条功能调用流水。',
          flush: true,
          body: `<div id="ovLogs">${empty('加载中…')}</div>`,
        })}</div>` +
        `<div>` +
          `<div id="dlCard" hidden></div>` +
          card({
            title: '个性签名',
            desc: '展示在控制台与客户端账号信息里，最多 100 字。',
            body:
              `<div class="row nowrap" style="align-items:flex-start">` +
                `<input class="input" id="sigInput" maxlength="100" placeholder="写点什么介绍自己…">` +
                `<button class="btn" data-act="ov.saveSig" style="flex:0 0 auto">保存</button>` +
              `</div>`,
          }) +
        `</div>` +
      `</div>`;

    loadAnnouncement();
    loadDownload();
    loadData();
  },
};

registerActions({
  'ov.refresh': () => { loadAnnouncement(); loadDownload(); loadData(); },
  'ov.dismissAnn': () => {
    const box = $('annBar');
    try { localStorage.setItem('annDismissed', annText); } catch (e) { }
    if (box) box.innerHTML = '';
  },
  'ov.saveSig': async (btn) => {
    const v = $('sigInput').value.trim();
    if (v.length > 100) return toast('签名最多 100 字', 'err');
    busy(btn, true, '保存中…');
    try {
      await api('/user/signature', 'PUT', { signature: v });
      if (state.me) state.me.signature = v;
      toast('签名已保存', 'ok');
    } catch (e) {
      toast(e.message, 'err');
    } finally {
      busy(btn, false, '保存');
    }
  },
});
