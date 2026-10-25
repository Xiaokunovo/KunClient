/* 页面骨架片段：页头与卡片。视图里拼字符串时用它，避免每页重复写同一层 div。 */

export function pageHead(o) {
  const opt = o || {};
  return `<div class="page-head">` +
    `<div style="min-width:0"><h1>${opt.title || ''}</h1>` +
    (opt.sub ? `<div class="sub">${opt.sub}</div>` : '') + `</div>` +
    (opt.actions ? `<div class="actions">${opt.actions}</div>` : '') +
    `</div>`;
}

export function card(o) {
  const opt = o || {};
  const cls = 'card-b' + (opt.flush ? ' flush' : '') + (opt.tight ? ' tight' : '');
  return `<section class="card"${opt.id ? ` id="${opt.id}"` : ''}>` +
    (opt.title
      ? `<div class="card-h"><div style="flex:1;min-width:0">` +
          `<div class="t">${opt.title}</div>` +
          (opt.desc ? `<div class="d">${opt.desc}</div>` : '') +
        `</div>${opt.actions ? `<div class="h-actions">${opt.actions}</div>` : ''}</div>`
      : '') +
    `<div class="${cls}">${opt.body || ''}</div>` +
    (opt.footer ? `<div class="card-f">${opt.footer}</div>` : '') +
    `</section>`;
}

export function stats(cells) {
  return `<div class="stats">${cells.join('')}</div>`;
}
