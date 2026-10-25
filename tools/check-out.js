// 校验 next 静态导出产物是否包含重写后的内容与正确链接
const fs = require('fs');
const path = require('path');

const OUT = 'e:/flashtool/website/out';
const html = fs.readFileSync(path.join(OUT, 'index.html'), 'utf8');

// 2026-10-03 官网重写后同步：下面两句标题被改写（去模板腔），护栏要跟着改，
// 否则就是「预期内的失败」—— 护栏的意义是拦**意外**丢失，不是拦有意改动。
// 2026-10-10 整站重做（3D）：同口径再同步 3 条，并把新增的 3D 结构纳入护栏。
//   ① 界面一览区标题  '工具界面一览' → '深色专业界面'（区块还在，只是换了说法）
//   ② 首屏背景层      'hero-grid'（CSS 网格视差） → 'hero-canvas'（Canvas 粒子星座）
//   ③ 能力速览        'hero-chips'（静态 chip 行） → 'orbit-item'（3D 圆环公转）
//   ④ 新增：'deck-panel'（首屏 3D 分层甲板）、'orbit-ring'（能力圆环容器）
//      —— 这两个是本次 3D 的主结构，丢了就等于 3D 整块没了，必须有人看着。
// 2026-10-10 二轮（跨页 3D + 人员）：再纳 4 条 ——
//   ⑤ 'scene-bridge' —— 区块接缝带，是「首页 3D 连成一条路径」的唯一结构，
//      它一没，三处场景就又退回各画各的。
//   ⑥ 'legal-scene'  —— 法务页背景场景，跨页视觉贯穿的载体。
//   ⑦ 'page-enter' / 'page-leave' —— 跨页转场的两个 body 类名，
//      只存在于 CSS 里；这两个类丢了转场就成了「白等 340ms 再硬切」，比不做还糟。
//   ⑧ 鸣谢/反馈人员：'清辞'、'反馈人员'、'河南省省长-青墨'。
//      人名容易在重构数据数组时被整段覆盖掉，而这属于用户点名的内容。
// 2026-10-12 C++/Qt6 原生单文件上线后同步：
//   ⑨ '.NET 10 桌面运行时' → '无需安装，原生单文件'
//      （2.0.0 起客户端不再依赖 .NET 运行时，旧断言与事实相悖必须换掉；
//        顺带把「带版本号的下载文件名」纳入护栏——/assets/KunClient.exe
//       被 CF 缓存死，复用无版本号 URL 等于官网导回旧包）
// 2026-10-12 二轮（官网/控制台 IA 重做）：MTK 上线 + 能力面重排后同步 4 条 ——
//   ⑩ '三种刷机通道，一个窗口' → '三块模块，覆盖全部功能'
//      原「核心能力」三卡换成了与服务端 MODULE_CATALOG 同源的三块模块；
//      同时原「能力速览」（圆环）与「核心能力」说的是同一件事，已合并为「速览 → 全景」一条线。
//   ⑪ '深色专业界面' → '两条芯片线，同一套界面'
//      界面一览的第三个 mockup 由「编出来的 EDL 工具清单」换成 MTK 刷写页（原清单里
//      「设置槽位 A/B」「开启 OEM 解锁」「读取字库品牌」这些按钮客户端并不存在）。
//   ⑫ '无需安装，原生单文件' → '无需额外运行时'
//      技术规格表删掉了「运行环境」行（并进「程序大小 · 免安装单文件」），
//      但「不依赖 .NET 运行时」这个事实断言要继续守着，改挂到首屏那句上。
//   ⑬ '高通 EDL 刷机工具' → '高通 / 联发科刷机工具'
//      站点标题也去掉了「只支持高通」的口径。
//   另新增：三块模块名（'刷写与擦除' '读取与备份' '专项与工具'）、'能力全景'、
//   'MTK BROM'、'云端驱动'、'命令行' —— 这几项正是「官网不符合现在的工具」的缺口，丢了要报警。
const MUST_HAVE = [
  // 2026-10-19 v6「工程制图」全站重写后同步：
  //   ① 首屏从「居中大字号 + 堆叠窗口」改成「左文案 + 右 3D 作业台 + 协议条」，
  //      新结构标记：hero-floor（透视网格）、deck（Z 轴分层）、proto-row（协议条）。
  //   ② 能力速览从 3×2 文字格改成带说明的表格格（caps-grid），
  //      能力全景从三张图标卡改成三列清单（mods）。
  //   ③ 章节号 01~08 进入正文，段序：速览 → 全景 → 流程 → 界面 → 下载 → 会员 → 问题 → 关于。
  //   ④ 旧 3D（orbit / scene-bridge / deck-panel）整组删除，护栏改为守新结构。
  //   ⑤ 首屏标题的「砖」带一层实心色块下划线，DOM 里被 <span> 切开，
  //      '从砖里' 不再是连续字符串 —— 断言改成后半句 '救回来'。
  '把设备', '救回来',
  '十七个页面，收进一个窗口', '三块模块，覆盖全部功能',
  '四步走完一次刷机', '两条芯片线，同一套界面',
  '常见问题', '关于', '控制台', '无需额外运行时',
  '高通 / 联发科刷机工具', '下载安装',
  '能力全景', '刷写与擦除', '读取与备份', '专项与工具',
  'MTK BROM', '云端驱动', '命令行',
  'hero-floor', 'deck', 'proto-row', 'caps-grid', 'mods',
  '清辞', '反馈人员', '河南省省长-青墨', '皓皓的小月',
  'sec-title', 'btn-primary', 'faq-item',
];

// ⚠️ 下面三项**只存在于 CSS / 别的 HTML 里**，不能塞进 MUST_HAVE 查 index.html：
//   · 'legal-scene'            只在三个法务页的 HTML 与 CSS 里（首页没有它）
//   · 'page-enter'/'page-leave' 是运行时挂到 <body> 的类名，只在 CSS 里
//   2026-10-10 我先按同口径塞进 MUST_HAVE，结果三条全 MISS —— 那是**护栏写错了
//   检查位置**，不是产物真的丢了。护栏报错必须能一眼分清「产物坏了」还是「判据错了」，
//   所以分开成独立的检查段。
const MUST_HAVE_IN_CSS = ['page-enter', 'page-leave', 'hero-floor', 'footer-mark'];
const MUST_HAVE_IN_LEGAL = ['legal-top', 'legal-body'];
// 旧强调色回归护栏（**在构建产物里查，不是只查源码**）：
//   v1 蓝  #2D7AF0 / #0067c0 / #2f8bff
//   v2 墨绿 #0f6b4f / #1e8f63 / #2e9e73 / #45b183 / #6fcfa4 / #8fdcb9
//   v2 成功绿 #16a34a（新版成功色是 #34a06a，与天空蓝强调色刻意分开）
// 换色是整站级的，漏一处就会在某个角落里留一块旧色补丁 —— 只靠肉眼翻页面必漏。
const OLD_ACCENTS = ['#2D7AF0', '#0067c0', '#0067C0', '#2f8bff', '#2F8BFF', '#7ab8f3', '#7AB8F3',
  '#0f6b4f', '#1e8f63', '#2e9e73', '#45b183', '#6fcfa4', '#8fdcb9', '#16a34a'];
const MUST_NOT_HAVE = ['/user', '免费开源', '全机型', '通常已自带', 'LoginModal',
  'orbit-ring', 'scene-bridge', 'deck-panel'].concat(OLD_ACCENTS);

let bad = 0;
console.log('--- 必须包含 ---');
for (const s of MUST_HAVE) {
  const ok = html.includes(s);
  if (!ok) bad++;
  console.log((ok ? 'OK   ' : 'MISS ') + s);
}
console.log('--- 必须不存在 ---');
for (const s of MUST_NOT_HAVE) {
  const found = html.includes(s);
  if (found) bad++;
  console.log((found ? 'BAD  ' : 'OK   ') + s);
}

// 检查 css/chunk 文件
const css = fs.readdirSync(path.join(OUT, '_next/static/chunks')).filter((f) => f.endsWith('.css'));
console.log('--- css chunks ---', css.join(', '));
const cssText = css.map((f) => fs.readFileSync(path.join(OUT, '_next/static/chunks', f), 'utf8')).join('\n');
console.log('css 含 --accent 变量: ' + /--accent:\s*#[0-9a-fA-F]{3,6}/.test(cssText));
console.log('css 含 .dl: ' + cssText.includes('.dl'));

// 🔴 旧色回归护栏必须**查 CSS**：强调色是写在 CSS 里的，只查 HTML 等于没查
//    （HTML 里根本没有 hex）。大小写归一后再比，否则 #0F6B4F 会被漏掉。
console.log('--- css 旧色残留 ---');
const cssLower = cssText.toLowerCase();
for (const c of OLD_ACCENTS) {
  const found = cssLower.includes(c.toLowerCase());
  if (found) bad++;
  console.log((found ? 'BAD  ' : 'OK   ') + c);
}

// 只在 CSS 里存在的结构类（转场 / 法务页场景）—— 检查位置与 MUST_HAVE 不同，
// 详见文件顶部 MUST_HAVE_IN_CSS 的注释。
console.log('--- css 专属结构类 ---');
for (const s of MUST_HAVE_IN_CSS) {
  const ok = cssText.includes(s);
  if (!ok) bad++;
  console.log((ok ? 'OK   ' : 'MISS ') + s);
}

// 三个法务页：必须有背景 3D 场景容器 + Reveal3D 入场节点
console.log('--- 法务页 3D ---');
for (const page of ['privacy', 'terms', 'disclaimer']) {
  const p = fs.readFileSync(path.join(OUT, page, 'index.html'), 'utf8');
  for (const s of MUST_HAVE_IN_LEGAL) {
    const ok = p.includes(s);
    if (!ok) bad++;
    console.log((ok ? 'OK   ' : 'MISS ') + page + ' ← ' + s);
  }
}

// 检查下载链接是否指向 assets
const m = html.match(/href="([^"]*assets[^"]*)"/g);
console.log('--- assets 链接 ---', m ? m.join(' | ') : '(无)');
console.log('console 链接: ' + (html.match(/href="[^"]*console[^"]*"/g) || []).join(' | '));

// 🔴 下载 URL 必须带版本号（2026-10-12 纳入护栏）：
//   /assets/KunClient.exe 被 Cloudflare 缓存成旧 .NET 包且手里 token 无 purge 权限，
//   官网一旦导回这个裸 URL，用户点下载拿到的就是旧包——2026-10-11 就是这么翻车的。
//
// 2026-10-14 改状态感知：官网新增「官网下载」开关（控制台 运营 › 官网展示 › 官网下载），
//   关闭态（内测 / 暂不发版）下静态产物**故意**不渲染「三步开始使用」与任何下载链接，
//   只留一句内测说明 —— 再无条件断言「必须有带版本号的 exe/7z 链接」就是把有意改动
//   判成故障（护栏的本意是拦**意外**丢失）。核心隐患（裸 /assets/KunClient.exe、
//   以及任何不带版本号的下载链接）在**任何状态下**都继续守着。
console.log('--- 官网下载开关（状态感知） ---');
{
  const DL_CLOSED_LABEL = '内测中';
  const closed = html.includes(DL_CLOSED_LABEL);

  const bare = html.includes('href="/assets/KunClient.exe"');
  if (bare) bad++;
  console.log((bare ? 'BAD  ' : 'OK   ') + '裸 URL href="/assets/KunClient.exe" 不存在');

  const exeLinks = html.match(/href="\/assets\/KunClient-[0-9.]+\.exe"/g) || [];
  const zipLinks = html.match(/href="\/assets\/KunClient-[0-9.]+\.7z"/g) || [];
  // 任何指向 assets 的 .exe 链接都不许不带版本号（版本号段允许 x.y.z.w 多段）
  const allExeHrefs = html.match(/href="[^"]*assets\/KunClient[^"]*\.exe"/g) || [];
  const unversioned = allExeHrefs.filter((h) => !/KunClient-\d[\d.]*\.exe/.test(h));

  if (closed) {
    console.log('OK   关闭态：出现「内测中」说明');
    if (!html.includes('下载安装')) { bad++; console.log('MISS 关闭态仍应保留「下载安装」区块标题'); }
    else console.log('OK   关闭态仍保留「下载安装」区块');
    if (exeLinks.length || zipLinks.length) {
      bad++; console.log('BAD  关闭态却仍渲染下载链接：' + exeLinks.concat(zipLinks).join(' | '));
    } else {
      console.log('OK   关闭态未渲染任何下载链接');
    }
  } else {
    if (!exeLinks.length) { bad++; console.log('MISS href="/assets/KunClient-<ver>.exe"'); }
    else console.log('OK   href="/assets/KunClient-<ver>.exe" x' + exeLinks.length);
    // 7z 备选包（2026-10-12 双轨分发）：必须与 exe 同版本号，漏传 assets 上的 .7z 就是 404。
    if (!zipLinks.length) { bad++; console.log('MISS 7z 备选包 href="/assets/KunClient-<ver>.7z"'); }
    else console.log('OK   7z 备选包 href="' + zipLinks[0] + '"');
    if (!html.includes('三步开始使用')) { bad++; console.log('MISS 三步开始使用'); }
    else console.log('OK   三步开始使用');
  }

  if (unversioned.length) { bad++; console.log('BAD  存在不带版本号的下载链接：' + unversioned.join(' | ')); }
  else console.log('OK   所有下载链接均带版本号');
}

console.log(bad === 0 ? '\nALL CHECKS PASSED' : `\n${bad} CHECK(S) FAILED`);
process.exit(bad === 0 ? 0 : 1);