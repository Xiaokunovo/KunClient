/* 入口：注册路由、绑定全局委托、恢复会话。
   所有视图模块在被 import 时就会注册自己的 data-act 动作，
   所以这里只需要把它们引入一次（顺序无关）。 */
import { state } from './core/state.js';
import { bindActions } from './core/actions.js';
import {
  defineRoutes, initShell, setHooks, enterApp, showLoginView,
} from './app.js';
import { initAuth, resetAuth } from './auth.js';

import overview from './views/overview.js';
import keys from './views/keys.js';
import feedback from './views/feedback.js';
import { profileView, prefsView, securityView, initAccountFileInputs } from './views/account.js';
import ops from './views/admin/ops.js';
import adminUsers from './views/admin/users.js';
import adminSecurity from './views/admin/security.js';
import adminBans from './views/admin/bans.js';

defineRoutes({
  overview,
  keys,
  feedback,
  'account/profile': profileView,
  'account/prefs': prefsView,
  'account/security': securityView,
  'admin/ops': ops,
  'admin/users': adminUsers,
  'admin/security': adminSecurity,
  'admin/bans': adminBans,
});

initShell();
bindActions(document);
initAccountFileInputs();
initAuth({ onAuthed: () => enterApp() });
setHooks({ onLogout: resetAuth });

(async function boot() {
  if (state.token) {
    const ok = await enterApp();
    if (ok) return;
  }
  showLoginView();
})();
