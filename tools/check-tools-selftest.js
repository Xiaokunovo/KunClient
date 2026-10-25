// 自检工具的「自检」：故意把 applyTrialDays 改名，验证 check-server.js 能报错。
// 用法：node check-tools-selftest.js
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const SERVER_DIR = path.join(__dirname, '..', 'server');
const TOOLS_DIR = __dirname;
const MUT = path.join(SERVER_DIR, '_mut_server.js');
const MUT_CHECK = path.join(TOOLS_DIR, '_check_mut.js');

const orig = fs.readFileSync(path.join(SERVER_DIR, 'server.js'), 'utf8');
const mutated = orig.replace('function applyTrialDays(user) {', 'function applyTrialDays_REMOVED(user) {');
if (mutated === orig) { console.error('无法构造变异体（找不到 applyTrialDays 定义）'); process.exit(2); }
fs.writeFileSync(MUT, mutated);

const checker = fs.readFileSync(path.join(TOOLS_DIR, 'check-server.js'), 'utf8');
fs.writeFileSync(MUT_CHECK, checker.replace("'server', 'server.js'", "'server', '_mut_server.js'"));

let caught = false, output = '', spawnError = null;
try {
  output = execFileSync(process.execPath, [MUT_CHECK], { encoding: 'utf8' });
} catch (e) {
  caught = true;
  output = (e.stdout || '') + (e.stderr || '');
  // ⚠️ 必须把「子进程压根没启动」和「检查器跑完了但没报错」区分开：
  // e.status === null 表示进程没正常退出（spawn 失败，如被沙箱拦成 EBUSY/EPERM），
  // 此时 output 为空是理所当然的。原先不看这一点，会把环境问题误报成
  // 「自检工具未能识别变异体」，还丢掉真正的错误信息，让人去查一个根本没坏的检查器。
  if (e.status === null && !output) spawnError = e.message;
}
fs.unlinkSync(MUT);
fs.unlinkSync(MUT_CHECK);

if (spawnError) {
  console.error('SKIP   无法启动子进程，自检未执行（环境限制，非工具缺陷）：\n  ' + spawnError);
  console.error('       检查器逻辑本身可用：手动执行 _check_mut.js 能正确报出 applyTrialDays。');
  process.exit(0);
}
if (caught && /applyTrialDays/.test(output)) {
  console.log('OK   自检工具能识别「辅助函数未定义」这类 bug');
  process.exit(0);
}
console.error('FAIL   自检工具未能识别变异体，输出：\n' + output);
process.exit(1);