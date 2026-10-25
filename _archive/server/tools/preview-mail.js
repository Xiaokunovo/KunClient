// 邮件模板预览：把 server.js 里的邮件视觉模块原样抠出来渲染成 HTML，用浏览器看效果。
// 用法：node tools/preview-mail.js [输出目录]
// 🔴 只读 server.js、不改任何东西；渲染用的都是模板真实代码，不是复刻。
const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', 'server.js'), 'utf8');
const start = src.indexOf('const MAIL_BRAND');
const end = src.indexOf('/** 字节数');
if (start < 0 || end < 0) {
  console.error('在 server.js 里找不到邮件视觉模块（MAIL_BRAND .. 字节数）');
  process.exit(1);
}
const mod = src.slice(start, end);

// 模板函数只依赖 MAIL_BRAND / MAIL_FONT / MAIL_MONO，在一个函数作用域里 eval 即可。
const api = new Function(mod + '\nreturn { MAIL_BRAND, mailDocument, mailButton, mailPanel, mailInfoTable, mailMetaRows, mailBulletPanel };')();
const { MAIL_BRAND: b, mailDocument, mailButton, mailPanel, mailInfoTable,
  mailMetaRows, mailBulletPanel } = api;

const outDir = process.argv[2] || path.join(__dirname, '..', 'tmp', 'mail-preview');
fs.mkdirSync(outDir, { recursive: true });

const wrap = (title, html) => `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<title>${title}</title><style>body{margin:0;background:#dfe6f0}</style></head>
<body>${html}</body></html>`;

// ① 验证码
const code = mailDocument({
  kicker: '安全验证',
  title: '你的验证码',
  subtitle: '请把下面这串数字填回 KunClient，以完成本次身份验证。',
  body:
    `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0"`
    + ` style="margin:0 0 18px"><tr>`
    + `<td align="center" style="background:${b.accentWash};border:1px dashed ${b.accentLine};`
    + `border-radius:12px;padding:22px 12px 18px">`
    + `<div style="font-family:'SF Mono',SFMono-Regular,Consolas,'Cascadia Mono','Courier New',monospace;font-size:34px;font-weight:700;letter-spacing:9px;`
    + `color:${b.accentInk};line-height:1.15;text-indent:9px">482913</div>`
    + `<div style="margin-top:10px;font-size:11.5px;letter-spacing:.6px;color:${b.accentDeep}">`
    + `10 分钟内有效</div>`
    + `</td></tr></table>`
    + `<div style="font-size:13px;line-height:1.8;color:${b.ink2}">`
    + `请勿把验证码告诉任何人 —— KunClient 的客服不会向你索取它。<br>`
    + `如果这不是你本人的操作，直接忽略本邮件即可，账号不会有任何变化。</div>`,
  footNote: '这封邮件发给了 xiaokun@example.com。',
});

// ② 登录通知
const login = mailDocument({
  kicker: '登录通知',
  title: '你的账号刚刚登录',
  subtitle: '下面是这次登录的匹配信息，花两秒核对一下。',
  body:
    mailInfoTable([
      ['账号', 'Xiaokunovo'],
      ['应用', 'KunClient'],
      ['时间', '2026-10-18 10:12:31'],
      ['地点', '中国 广东 深圳'],
      ['IP', '113.87.***.***'],
      ['方式', '密码'],
    ])
    + `<div style="margin:2px 0 0">`
    + mailButton('https://kunawa.cn/console/', '去控制台改密码', 'ghost')
    + `</div>`
    + `<div style="margin-top:4px;font-size:12.5px;line-height:1.8;color:${b.ink2}">`
    + `如果这确实是你本人，不用做任何事。</div>`,
  footNote: '如非本人操作，请立即修改密码并检查账号安全。',
});

// ③ 版本更新
const update = mailDocument({
  kicker: '版本更新',
  title: 'v2.0.2 已发布',
  subtitle: 'Xiaokunovo，您好 —— 建议更新到最新版本，以获得这一轮的改进与修复。',
  body:
    mailMetaRows([['版本', 'v2.0.2'], ['发布', '2026-10-18'], ['体积', '12.0 MB']])
    + mailBulletPanel(['灵动岛展开态改为页面切换器', '主屏图标统一成半透明玻璃方块', '控制中心搬到主屏左侧，横滑进入'])
    + `<div style="margin:2px 0 0">`
    + mailButton('https://kunawa.cn/assets/KunClient.exe', '下载安装版 (.exe)', 'solid')
    + mailButton('https://kunawa.cn/assets/KunClient.7z', '免安装版 (.7z)', 'ghost')
    + `</div>`
    + `<div style="margin-top:6px;font-size:12px;line-height:1.8;color:${b.ink2}">`
    + `.exe 为安装版；.7z 为免安装绿色包，解压即用，适合没有安装权限或想随身携带的场景。两条链接任一可用即可。</div>`,
  footNote: '你收到这封邮件，是因为该邮箱绑定在 KunClient 账号上。',
});

// ④ 用户反馈
const feedback = mailDocument({
  kicker: '用户反馈',
  title: '#128 刷机到 80% 就断了',
  subtitle: '功能异常 · 2026/10/18 10:20:11',
  body:
    mailInfoTable([
      ['类型', '功能异常'],
      ['用户', 'Xiaokunovo <span style="color:' + b.ink3 + '">（ID 3）</span>'],
      ['邮箱', 'xiaokun@example.com'],
      ['联系方式', 'QQ 12345678'],
    ])
    + `<div style="font-size:12px;font-weight:700;letter-spacing:.4px;color:${b.ink3};margin:4px 0 8px">反馈正文</div>`
    + `<div style="padding:14px 16px;background:${b.lineSoft};border-radius:12px;font-size:13.5px;line-height:1.8;white-space:pre-wrap">`
    + `用的是小米线刷，进度到 80% 左右直接断开。\n重试了两次都一样，日志已经导出。</div>`,
  footNote: '直接回复本邮件即可与用户沟通（回复地址已设为用户绑定邮箱）。',
});

// ⑤ 环境告警
const alert = mailDocument({
  kicker: '环境告警',
  title: '未登录客户端触发了防护',
  subtitle: '该请求已被拦截并按 IP 处置，以下是上报明细。',
  body:
    mailPanel('<div style="font-size:12.5px;line-height:1.75;color:#c0392b">'
      + '这是<b>开发侧</b>的告警邮件，普通用户看不到。若要放行，到控制台「IP 封禁」里删掉对应条目即可。'
      + '</div>', 'danger')
    + `<table role="presentation" width="100%" cellpadding="0" cellspacing="0" border="0">`
    + ['IP　113.87.12.9（中国 广东 深圳）', '信号　bad_hmac', '版本　2.0.1+8f3c2a1', '时间　2026-10-18 10:22:04']
        .map((l, i) => `<tr><td style="padding:8px 0;font-size:13px;line-height:1.75;${i ? 'border-top:1px solid #eff3f9;' : ''}">${l}</td></tr>`).join('')
    + `</table>`,
});

const files = [
  ['01-验证码.html', code],
  ['02-登录通知.html', login],
  ['03-版本更新.html', update],
  ['04-用户反馈.html', feedback],
  ['05-环境告警.html', alert],
];
for (const [name, html] of files) {
  fs.writeFileSync(path.join(outDir, name), wrap(name, html), 'utf8');
}
console.log('已渲染 ' + files.length + ' 封邮件 → ' + outDir);
