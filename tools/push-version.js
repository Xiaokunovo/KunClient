/** 通过管理 API 把版本设置更新为 1.0.3（更新版本号 / 构建日期 / 更新日志，不改强制更新开关） */
const fs = require('fs');
const http = require('http');
const cfg = require('../server/config');
// config.json 中 admin.password 为空（服务端使用内置默认），此处用已知的管理员密码
const ADMIN_USER = cfg.admin.username || 'admin';
const ADMIN_PASS = (cfg.admin.password && cfg.admin.password.length) ? cfg.admin.password : '';  // 开源版：不内置密码，请在 config.local.json 配置
// 旧机 your-old-server.example.com 已停用（2026-09-24 起），线上在 your-server.example.com。
// ⚠️ 另注意：线上 admin.password 为空（未播种管理员账号），本脚本走 /api/admin/settings
// 大概率 `login fail`。真要发版请走「停服 + 直接改 database.sqlite」，见 kunclient-deploy-verify 技能。
const BASE = { host: 'your-server.example.com', port: 3000 };

function req(method, path, token, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const r = http.request({ ...BASE, path, method,
      headers: { 'Content-Type': 'application/json',
        ...(token ? { Authorization: 'Bearer ' + token } : {}),
        ...(data ? { 'Content-Length': Buffer.byteLength(data) } : {}) } },
      res => { let d = ''; res.on('data', c => d += c);
        res.on('end', () => { try { resolve({ code: res.statusCode, json: JSON.parse(d) }); } catch { resolve({ code: res.statusCode, json: d }); } }); });
    r.on('error', reject);
    if (data) r.write(data);
    r.end();
  });
}

const changelog = 'v1.0.3 更新内容：\n' +
  '• 新增「忘记密码」：可在控制台登录页通过邮箱验证码重置密码\n' +
  '• 修复控制台注册验证码无法发送的问题，邮件失败原因会如实提示\n' +
  '• 控制台侧边栏固定，不再随页面内容滚动\n' +
  '• 自定义背景新增 Wallpaper Engine 动态壁纸导入（支持视频类壁纸）\n' +
  '• 设置页用户信息新增会员等级与到期时间\n' +
  '• 关于页面重写，补充合规承诺与功能范围说明\n' +
  '• 全面移除敏感信息采集与展示（IP、设备唯一标识、序列号）\n' +
  '• 修复日志面板不透明度不生效的问题（仅背景变化，字体不变）\n' +
  '• 多项界面细节与稳定性修复';

(async () => {
  const login = await req('POST', '/api/login', null, { username: ADMIN_USER, password: ADMIN_PASS });
  if (!login.json.success) { console.error('login fail:', login.json.message || login.json); process.exit(1); }
  const token = login.json.token || (login.json.data && login.json.data.token);
  console.log('login ok');

  const before = await req('GET', '/api/app/version');
  console.log('version before:', before.json.data && before.json.data.version, 'force=' + (before.json.data && before.json.data.forceUpdate));

  const put = await req('PUT', '/api/admin/settings', token, {
    version: '1.0.3',
    build_date: '2026-09-22',
    changelog
  });
  console.log('settings PUT:', put.code, JSON.stringify(put.json).slice(0, 120));

  const v = await req('GET', '/api/app/version');
  console.log('version now:', v.json.data && v.json.data.version, 'force=' + (v.json.data && v.json.data.forceUpdate));
  console.log('buildDate:', v.json.data && v.json.data.buildDate);
  console.log('changelog lines:', String((v.json.data && v.json.data.changelog) || '').split('\n').length);
})().catch(e => { console.error(e.message); process.exit(1); });
