/**
 * 在服务器上直接改 DB 里的版本信息（sql.js 库，需在服务器上运行，或把 dbPath 指到已同步的库）。
 *
 * 注意：服务端 config.json / config.local.json 里 admin.password 为空 → 未播种管理员账号，
 * 因此**不能**走 /api/admin/settings 推送版本（tools/push-version.js 会 login fail）。
 * 线上实际做法见 kunclient-deploy-verify skill：停服 → node 脚本改 app_settings → 启服。
 *
 * force_update 默认写 '0'（不强制）；写成 '1' 会让所有旧客户端被强制更新，改前需确认。
 */
const path = require('path');
const Database = require('better-sqlite3');
const DEPLOY = require('./deploy-config');

// 数据库路径可外部配置：命令行参数 > 环境变量 REMOTE_DB_PATH > 由部署配置 remoteBase 派生
const dbPath = process.argv[2] || process.env.REMOTE_DB_PATH
  || path.join(DEPLOY.remoteBase, 'data', 'kunclient.db').replace(/\\/g, '/');
const db = new Database(dbPath);

db.prepare("UPDATE app_settings SET value='1.0.4' WHERE key='version'").run();
db.prepare("UPDATE app_settings SET value='0' WHERE key='force_update'").run();
db.prepare("UPDATE app_settings SET value='2026-09-22' WHERE key='build_date'").run();

const changelog = [
  'v1.0.4 更新内容：',
  '• 修复云端更新：下载完成后现在会正确替换主程序并重启（此前会停在旧版本）',
  '• 更新包新增完整性校验，避免把服务器错误页当成安装包导致程序打不开',
  '• 更新失败不再删除旧程序：会自动重试并留下失败日志',
  '• 「本地引导 / 下载引导」现在无需连接设备即可选择（原需先插线）',
  '• 修复切换深色主题后卡片与输入框不透明度失效的问题',
  '• 修复选择框无法跟随卡片一起透明的问题',
  '• Wallpaper Engine 场景 / 网页类壁纸改用官方预览素材，现在也能用作动态背景',
  '• 移除界面残留的图形 logo，统一为彩色文字品牌',
  '• 多项界面细节修复'
].join('\n');

db.prepare("UPDATE app_settings SET value=? WHERE key='changelog'").run(changelog);

console.log('Updated to v1.0.4');
db.close();
