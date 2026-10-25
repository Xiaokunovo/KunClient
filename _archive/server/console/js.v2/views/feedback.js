/* 问题反馈：左侧提交、右侧我的记录。提交后服务端会发邮件给管理员，
   回复会出现在「我的反馈」里。 */
import { api } from '../core/api.js';
import { registerActions } from '../core/actions.js';
import { icon, esc, fmtDate, toast, busy, empty, loading } from '../core/ui.js';
import { pageHead, card } from '../core/layout.js';

const $ = (id) => document.getElementById(id);

const CAT = { bug: '问题反馈', suggestion: '功能建议', account: '账号问题', other: '其他' };
const STATUS = {
  open: ['待处理', ''],
  replied: ['已回复', 'info'],
  closed: ['已关闭', 'ok'],
};

async function loadMine() {
  const box = $('fbMine');
  if (!box) return;
  box.innerHTML = loading();
  try {
    const j = await api('/feedback/mine');
    const list = j.data || [];
    box.innerHTML = list.length
      ? list.map((f) => {
        const st = STATUS[f.status] || [f.status, ''];
        return `<div class="item">` +
          `<div class="item-h"><b>#${Number(f.id)} ${esc(f.title)}</b>` +
            `<span class="badge ${st[1]}">${esc(st[0])}</span>` +
            `<span class="item-meta">${esc(CAT[f.category] || f.category)}<span class="sep">·</span>${esc(fmtDate(f.created_at))}</span>` +
          `</div>` +
          `<div class="item-b">${esc(f.content)}</div>` +
          (f.reply ? `<div class="item-reply"><span class="muted">管理员回复</span><br>${esc(f.reply)}</div>` : '') +
        `</div>`;
      }).join('')
      : empty('还没有反馈记录', '提交后这里会显示处理进度与管理员的回复。', 'message');
  } catch (e) {
    box.innerHTML = empty('加载失败', e.message, 'alert');
  }
}

async function submit(btn) {
  const category = $('fbCat').value;
  const title = $('fbTitle').value.trim();
  const content = $('fbContent').value.trim();
  const contact = $('fbContact').value.trim();
  if (!title) return toast('请填写反馈标题', 'err');
  if (content.length < 5) return toast('请把问题描述得再详细一点（至少 5 个字）', 'err');
  busy(btn, true, '提交中…');
  try {
    const j = await api('/feedback', 'POST', { category, title, content, contact });
    toast(j.message || '已提交', 'ok');
    $('fbTitle').value = '';
    $('fbContent').value = '';
    $('fbContact').value = '';
    loadMine();
  } catch (e) {
    toast(e.message, 'err');
  } finally {
    busy(btn, false, '发送给管理员');
  }
}

export default {
  label: '反馈',
  group: '工作台',
  rank: 0,
  async render({ el }) {
    el.innerHTML =
      pageHead({
        title: '问题反馈',
        sub: '描述越具体，定位越快：现象、复现步骤、期望结果，能附版本号更好。',
        actions: `<button class="btn sm ghost" data-act="fb.reload">${icon('refresh', 13)}刷新记录</button>`,
      }) +
      `<div class="split even">` +
        `<div>${card({
          title: '提交反馈',
          desc: '提交后会发送到管理员邮箱，处理结果会回复在右侧列表里。',
          body:
            `<div class="field"><label for="fbCat">类型</label>` +
              `<select class="select" id="fbCat">` +
                `<option value="bug">问题反馈</option>` +
                `<option value="suggestion">功能建议</option>` +
                `<option value="account">账号问题</option>` +
                `<option value="other">其他</option>` +
              `</select></div>` +
            `<div class="field"><label for="fbTitle">标题 <span class="opt">最多 60 字</span></label>` +
              `<input class="input" id="fbTitle" maxlength="60" placeholder="一句话概括问题"></div>` +
            `<div class="field"><label for="fbContent">详细描述 <span class="opt">5 - 2000 字</span></label>` +
              `<textarea class="textarea" id="fbContent" maxlength="2000" placeholder="问题现象、复现步骤、期望结果…"></textarea></div>` +
            `<div class="field"><label for="fbContact">联系方式 <span class="opt">选填</span></label>` +
              `<input class="input" id="fbContact" maxlength="120" placeholder="QQ / 邮箱 / 微信"></div>` +
            `<div class="row"><button class="btn pri" data-act="fb.submit">发送给管理员</button>` +
              `<button class="btn ghost" data-act="fb.clear">清空</button></div>`,
        })}</div>` +
        `<div>${card({
          title: '我的反馈',
          desc: '按提交时间倒序，最多显示最近 100 条。',
          flush: true,
          body: `<div id="fbMine">${loading()}</div>`,
        })}</div>` +
      `</div>`;
    loadMine();
  },
};

registerActions({
  'fb.reload': () => loadMine(),
  'fb.submit': (b) => submit(b),
  'fb.clear': () => {
    $('fbTitle').value = '';
    $('fbContent').value = '';
    $('fbContact').value = '';
    $('fbTitle').focus();
  },
});
