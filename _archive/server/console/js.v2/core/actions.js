/* 事件委托注册表。
   页面里只写 data-act="keys.gen" / data-id="12"，不写 onclick ——
   服务端字符串永远不进 JS 字面量，含引号的用户名/密钥不会再把 handler 截断。 */

const handlers = new Map();

export function registerActions(map) {
  Object.keys(map).forEach((k) => handlers.set(k, map[k]));
}

export function hasAction(name) {
  return handlers.has(name);
}

function run(el, e, name) {
  const fn = handlers.get(name);
  if (!fn) {
    console.warn('[console] 未注册的动作：' + name);
    return;
  }
  try {
    fn(el, e);
  } catch (err) {
    console.error('[console] 动作执行失败：' + name, err);
  }
}

export function bindActions(root) {
  const scope = root || document;

  scope.addEventListener('click', (e) => {
    const el = e.target.closest('[data-act]');
    if (!el || el.disabled) return;
    if (el.dataset.act === 'noop') { e.preventDefault(); return; }
    e.preventDefault();
    run(el, e, el.dataset.act);
  });

  scope.addEventListener('change', (e) => {
    const el = e.target.closest('[data-change]');
    if (!el) return;
    run(el, e, el.dataset.change);
  });

  scope.addEventListener('input', (e) => {
    const el = e.target.closest('[data-input]');
    if (!el) return;
    run(el, e, el.dataset.input);
  });

  scope.addEventListener('submit', (e) => {
    const el = e.target.closest('[data-submit]');
    if (!el) return;
    e.preventDefault();
    run(el, e, el.dataset.submit);
  });
}
