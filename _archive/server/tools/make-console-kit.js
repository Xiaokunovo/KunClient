/* 从 console/index.html 抽出 <style> 段，生成一张「组件标本页」用于截图核对。
   只读生产文件，不改动任何东西。 */
const fs = require('fs');
const path = require('path');

const src = path.join(__dirname, '..', 'console', 'index.html');
const html = fs.readFileSync(src, 'utf8');
const m = html.match(/<style>([\s\S]*?)<\/style>/);
if (!m) throw new Error('未找到 <style> 段');
const css = m[1];

const icon = (d) => `<svg viewBox="0 0 24 24" width="19" height="19" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${d}</svg>`;

const nav = (d, label, on) => `<button class="${on ? 'on' : ''}">${icon(d)}<span class="lbl">${label}</span></button>`;

const rail = (wide) => `
<aside class="rail" ${wide ? 'data-wide="1"' : ''}>
  <svg class="logo-fb" viewBox="0 0 24 24" width="30" height="30" fill="none" stroke="var(--pri)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" style="display:flex"><path d="M13.5 2 4 13.5h6L10.5 22 20 10.5h-6z"/></svg>
  <nav class="rnav">
    ${nav('<path d="M3 10.5L12 3l9 7.5M5.5 9.5V21h5v-5.5h3V21h5V9.5"/>', '概览', true)}
    ${nav('<circle cx="8" cy="15" r="4.2"/><path d="M11 12L20.5 2.5M16.5 6.5l3 3M19 4l2.5 2.5"/>', '我的密钥')}
    ${nav('<path d="M4 4h16v12H9l-5 4z"/>', '问题反馈')}
    ${nav('<circle cx="12" cy="8" r="4"/><path d="M4.5 21c.5-4.4 3.8-6.5 7.5-6.5s7 2.1 7.5 6.5"/>', '账号设置')}
    ${nav('<path d="M3 8l4.5 4L12 5l4.5 7L21 8l-1.6 11H4.6z"/>', '会员中心')}
  </nav>
  <div class="rfoot">
    <div class="rsep"></div>
    <button id="railToggle">${icon('<path d="M9 5l7 7-7 7"/>')}<span class="lbl">侧栏文字</span></button>
    <button>${icon('<path d="M20 13.5A8.5 8.5 0 1 1 10.5 4a7 7 0 0 0 9.5 9.5z"/>')}<span class="lbl">切换主题</span></button>
    <button>${icon('<path d="M12 3v8M6.3 6.5a8 8 0 1 0 11.4 0"/>')}<span class="lbl">退出登录</span></button>
  </div>
</aside>`;

const stats = `
<div class="grid">
  <div class="stat"><div class="k">账号状态</div><div class="v">正常</div></div>
  <div class="stat"><div class="k">会员到期</div><div class="v">2027-03-14</div></div>
  <div class="stat"><div class="k">剩余天数</div><div class="v">147 天</div></div>
  <div class="stat"><div class="k">密钥数量</div><div class="v">6</div></div>
  <div class="stat"><div class="k">累计线刷</div><div class="v">1,284</div></div>
  <div class="stat"><div class="k">注册时间</div><div class="v">2025-11-02</div></div>
</div>`;

const table = `
<table>
  <thead><tr><th>密钥</th><th>创建时间</th><th>状态</th><th>备注</th><th style="text-align:right">操作</th></tr></thead>
  <tbody>
    <tr><td class="mono">KC-8F2A-71D3-4B9C</td><td>2026-10-18 11:02</td><td><span class="badge b-ok">可用</span></td><td>小米 14 维修</td><td style="text-align:right"><button class="btn sm danger">删除</button></td></tr>
    <tr><td class="mono">KC-1D77-90AE-2C55</td><td>2026-10-16 20:41</td><td><span class="badge b-off">已失效</span></td><td>—</td><td style="text-align:right"><button class="btn sm danger">删除</button></td></tr>
    <tr><td class="mono">KC-4C09-B3E8-7A12</td><td>2026-10-12 09:15</td><td><span class="badge b-open">待处理</span></td><td>Redmi K70</td><td style="text-align:right"><button class="btn sm danger">删除</button></td></tr>
    <tr><td class="mono">KC-77E1-2A4F-88B0</td><td>2026-10-08 14:30</td><td><span class="badge b-dev">开发者</span></td><td>内部测试</td><td style="text-align:right"><button class="btn sm danger">删除</button></td></tr>
  </tbody>
</table>`;

const body = `
<div class="login-page" style="display:none"></div>
<div id="app" style="display:block">
  ${rail(false)}
  <div class="content">
    <div class="top">
      <h2>概览</h2>
      <div class="who"><div class="av">K</div><div><div class="nm">小坤ovo</div><div class="rl">管理员 · 开发者</div></div></div>
    </div>
    <div class="pages">
      <section class="page on">
        <div class="card">
          <h3>欢迎回来，小坤ovo</h3>
          ${stats}
          <div style="margin-top:14px">
            <label>个性签名（最多 100 字）</label>
            <div class="row"><input style="flex:1;min-width:220px" value="专注高通 EDL 线刷"><button class="btn sm">保存签名</button></div>
          </div>
        </div>
        <div class="card">
          <h3>我的密钥</h3>
          <div class="desc">密钥用于客户端「密钥登录」，是<b>一次性凭证</b>：登录成功后立即失效。</div>
          <div class="row" style="margin-bottom:12px">
            <button class="btn sm">生成密钥</button>
            <button class="btn sm ghost">刷新</button>
            <span class="sp"></span>
            <button class="btn sm danger">删除选中</button>
          </div>
          <div class="k-new"><span class="mono">KC-3A9F-60C1-EE42-7D18</span><span class="sp"></span><button class="btn sm">复制</button></div>
          ${table}
        </div>
        <div class="card">
          <h3>账号设置</h3>
          <div class="setline"><div><div class="t">登录双重验证</div><div class="d">登录时额外校验邮箱验证码</div></div><span class="sp"></span><div class="toggle on"></div></div>
          <div class="setline"><div><div class="t">邮件通知</div><div class="d">版本更新与安全提醒</div></div><span class="sp"></span><div class="toggle"></div></div>
          <div class="setline"><div><div class="t">环境检测</div><div class="d">异常环境自动封禁</div></div><span class="sp"></span><div class="toggle on"></div></div>
        </div>
        <div class="card">
          <h3>问题反馈</h3>
          <div class="fb-item">
            <div class="h"><span class="badge b-open">待处理</span><b>刷机到 80% 卡住不动</b><span class="sp"></span><span class="desc" style="margin:0">2026-10-17</span></div>
            <div class="b">Redmi Note 12 刷到 super 分区时停在 80%，等了十分钟没有反应，拔线重试也是一样。</div>
            <div class="reply">已定位：super 分区镜像超过 4G，需要走 sparse 分包。下个版本修复。</div>
          </div>
          <div class="fb-item">
            <div class="h"><span class="badge b-closed">已关闭</span><b>希望支持批量导入分区表</b><span class="sp"></span><span class="desc" style="margin:0">2026-10-09</span></div>
            <div class="b">每次换机型都要重新选一遍分区表，希望能记住上次用的。</div>
          </div>
        </div>
        <!-- 管理页：筛选条 + 用户表 + 增值模块（标本用，覆盖 .filters/.chip/.mm-* 这套只出现在管理页的组件） -->
        <div class="card">
          <h3>用户管理</h3>
          <div class="filters">
            <input placeholder="搜索用户名 / 邮箱" style="max-width:240px">
            <select style="max-width:150px"><option>全部状态</option><option>仅封禁</option></select>
            <span class="chip on">全部 128</span>
            <span class="chip">正常 124</span>
            <span class="chip">封禁 3</span>
            <span class="chip">开发者 1</span>
          </div>
          <table>
            <thead><tr><th><input type="checkbox" class="chk"></th><th>用户</th><th>角色</th><th>状态</th><th>注册时间</th><th style="text-align:right">操作</th></tr></thead>
            <tbody>
              <tr><td><input type="checkbox" class="chk"></td><td><div class="role-line"><span class="dot bad"></span><b>阿伟的店</b></div></td><td><span class="badge b-dev">开发者</span></td><td><span class="badge b-bad">已封禁</span></td><td>2026-09-30 08:12</td><td style="text-align:right"><button class="btn sm">详情</button></td></tr>
              <tr><td><input type="checkbox" class="chk"></td><td><div class="role-line"><span class="dot"></span><b>维修老张</b></div></td><td>普通用户</td><td><span class="badge b-ok">正常</span></td><td>2026-08-14 19:03</td><td style="text-align:right"><button class="btn sm">详情</button></td></tr>
              <tr><td><input type="checkbox" class="chk"></td><td><div class="role-line"><span class="dot gray"></span><b>K70pro</b></div></td><td>普通用户</td><td><span class="badge b-off">未验证</span></td><td>2026-10-01 11:47</td><td style="text-align:right"><button class="btn sm">详情</button></td></tr>
            </tbody>
          </table>
        </div>
        <div class="card">
          <h3>增值模块</h3>
          <div class="mm-wrap">
            <div class="mm-card">
              <div class="mm-top"><span class="mm-id">模块 1 · 基础线刷</span><span class="badge b-ok">已启用</span><span class="sp"></span><span class="mm-tip" title="按功能授权">?</span></div>
              <div class="mm-incrow"><label class="mm-inc"><input type="checkbox" class="chk" checked>包含功能</label><span>8 个功能</span></div>
              <div class="mm-feats">edl-flash · edl-read · edl-erase · edl-backup · partition-edit · fdl-sign · mtk-brom · qc-9008</div>
            </div>
            <div class="mm-card">
              <div class="mm-top"><span class="mm-id">模块 2 · 高级诊断</span><span class="badge b-off">未启用</span><span class="sp"></span></div>
              <div class="mm-feats">内存检测 · 分区校验 · 坏块扫描</div>
            </div>
          </div>
        </div>
      </section>
    </div>
  </div>
</div>
<!-- 弹窗与 Toast：用相对定位容器包住，避免 fixed 铺满整张长图 -->
<div style="position:relative;height:420px;margin:0 28px 40px;border-radius:14px;overflow:hidden;border:1px solid var(--line)">
  <div class="modal-mask" style="position:absolute">
    <div class="modal-box">
      <div class="modal-head"><h3>单用户权限</h3><button class="modal-x">×</button></div>
      <div class="modal-body">
        <div class="modal-sec"><div class="t">账号信息</div><div class="cur">维修老张 · 注册于 2026-08-14 · 普通用户</div>
          <div class="row"><button class="btn sm">重置密码</button><button class="btn sm ghost">发送通知</button><button class="btn sm danger">封禁账号</button></div>
        </div>
        <div class="ent-sec"><b>模块授权</b>
          <div class="ent-mods">
            <div class="ent-mod">
              <div class="ent-mhead"><span class="mm-id">模块 1</span><span class="ent-mtitle">基础线刷</span><span class="sp"></span><span class="badge b-ok">已授权</span></div>
              <div class="ent-mrow"><label class="ent-chk"><input type="checkbox" class="chk" checked>启用</label><label class="ent-days-lab">有效期 <input value="30"> 天</label><span class="ent-hint">留空 = 永久</span></div>
              <div class="ent-feats">edl-flash · edl-read · edl-erase · edl-backup</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>
<div style="position:relative;height:70px;margin:0 28px 40px">
  <div id="toast" class="show ok" style="position:absolute;top:8px">密钥已生成，请立即复制保存</div>
</div>`;

const out = `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>kit</title><style>${css}
/* 标本页专用：把三栏并排看两态侧栏 */
body{padding:0}
</style></head><body>${body}</body></html>`;

/* 弹窗与 Toast 单独出一页：它们本来是 fixed 铺满视口的，混在长图里既看不清也不好裁。 */
const overlay = `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><title>kit-overlay</title><style>${css}
body{padding:32px;background:var(--bg)}
.wrap{position:relative;height:640px;border-radius:var(--r-lg);overflow:hidden;border:1px solid var(--line)}
.wrap .modal-mask{position:absolute}
.toast-wrap{position:relative;height:80px;margin-top:24px}
.toast-wrap #toast{position:absolute;top:0}
</style></head><body>
<div class="wrap">
  <div class="modal-mask">
    <div class="modal-box">
      <div class="modal-head"><h3>单用户权限</h3><button class="modal-x">&times;</button></div>
      <div class="modal-body">
        <div class="modal-sec"><div class="t">账号信息</div><div class="cur">维修老张 · 注册于 2026-08-14 · 普通用户</div>
          <div class="row"><button class="btn sm">重置密码</button><button class="btn sm ghost">发送通知</button><button class="btn sm danger">封禁账号</button></div>
        </div>
        <div class="ent-sec"><b>模块授权</b>
          <div class="ent-mods">
            <div class="ent-mod">
              <div class="ent-mhead"><span class="mm-id">模块 1</span><span class="ent-mtitle">基础线刷</span><span class="sp"></span><span class="badge b-ok">已授权</span></div>
              <div class="ent-mrow"><label class="ent-chk"><input type="checkbox" class="chk" checked>启用</label><label class="ent-days-lab">有效期 <input value="30"> 天</label><span class="ent-hint">留空 = 永久</span></div>
              <div class="ent-feats">edl-flash · edl-read · edl-erase · edl-backup</div>
            </div>
            <div class="ent-mod">
              <div class="ent-mhead"><span class="mm-id">模块 2</span><span class="ent-mtitle">高级诊断</span><span class="sp"></span><span class="badge b-off">未授权</span></div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>
<div class="toast-wrap"><div id="toast" class="show ok">密钥已生成，请立即复制保存</div></div>
</body></html>`;
const tmp = path.join(__dirname, '..', 'tmp');
fs.writeFileSync(path.join(tmp, 'console-overlay.html'), overlay, 'utf8');
fs.writeFileSync(path.join(tmp, 'console-overlay-dark.html'), overlay.replace('<body>', '<body class="dark">'), 'utf8');
const light = path.join(tmp, 'console-kit.html');
const dark = path.join(tmp, 'console-kit-dark.html');
fs.writeFileSync(light, out, 'utf8');
fs.writeFileSync(dark, out.replace('<body>', '<body class="dark">'), 'utf8');
/* 管理页专用变体：整页上移，让视口直接落在「用户管理 / 增值模块」两卡上（近距离核对 chip 与 mm-*）。 */
const adm = out.replace('<body>', '<body style="margin-top:-1240px">');
fs.writeFileSync(path.join(tmp, 'console-kit-adm.html'), adm, 'utf8');
fs.writeFileSync(path.join(tmp, 'console-kit-adm-dark.html'), adm.replace('<body ', '<body class="dark" '), 'utf8');
console.log('written', light, dark);
