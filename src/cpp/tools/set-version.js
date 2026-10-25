#!/usr/bin/env node
/**
 * set-version.js — KunClient C++ 版本号统一落点更新
 *
 * 版本号红线（.NET 时代就是 5 处，C++ 工程继承）：
 *   1. src/cpp/CMakeLists.txt        → project(KunClient VERSION x.y.z)
 *   2. src/cpp/kc/version.h          → kVersionMajor/Minor/Patch + kVersionString + kUserAgent
 *      （kVersionBuild 保留为 0 的占位常量，不再参与对外版本号）
 *   3. src/cpp/resources/app.rc      → FILEVERSION / PRODUCTVERSION / "FileVersion"
 *      （exe 的 Windows 版本信息资源；🔴 2026-10-11 前是**手改**的漏网点，
 *        已纳入本脚本——资源里的版本号对不上时，客户端「关于」页显示与
 *        资源管理器属性页会给出两个不同的版本）
 *   4. website/src/lib/data.ts       → FALLBACK_VERSION.version
 *   5. 线上 app_settings.version     → 运行时数据库，本脚本不碰（上线时用 set_setting.js）
 *   6. UA 头                          → 派生自 version.h 的 kUserAgent，不单独改
 *
 * 用法：
 *   node tools/set-version.js 2.1.0                # 实际写入全部落点
 *   node tools/set-version.js 2.1.0 --dry          # 只打印将要做的修改
 *   node tools/set-version.js 2.1.0 --no-website   # 只改 C++ 两处（迁移期用：
 *              官网 FALLBACK_VERSION 跟「线上已发布版本」走，C++ 新版本线
 *              未发布前两者本就不同步，别让官网提前宣传一个不存在的版本）
 *
 * 版本格式：major.minor.patch（x.x.x 三段式，不再使用第四段 build）。
 */
'use strict';

const fs = require('fs');
const path = require('path');

const CPP_ROOT = path.join(__dirname, '..');
const REPO_ROOT = path.join(CPP_ROOT, '..', '..');

const FILES = {
  cmake: path.join(CPP_ROOT, 'CMakeLists.txt'),
  versionH: path.join(CPP_ROOT, 'kc', 'version.h'),
  appRc: path.join(CPP_ROOT, 'resources', 'app.rc'),
  dataTs: path.join(REPO_ROOT, 'website', 'src', 'lib', 'data.ts'),
};

function fail(msg) {
  console.error(`[set-version] 错误：${msg}`);
  process.exit(1);
}

// ── 参数解析 ─────────────────────────────────────────────────────────────────
const argv = process.argv.slice(2);
const dry = argv.includes('--dry');
const noWebsite = argv.includes('--no-website');
const versionArg = argv.find((a) => !a.startsWith('--'));
if (!versionArg) fail('用法：node tools/set-version.js <major.minor.patch> [--dry]');

const parts = versionArg.split('.').map((s) => {
  const n = Number(s);
  if (!Number.isInteger(n) || n < 0) fail(`版本段 "${s}" 不是非负整数`);
  return n;
});
if (parts.length !== 3) fail('版本格式：major.minor.patch（x.x.x 三段式）');

const [major, minor, patch] = parts;
const build = 0; // 仅用于 app.rc 的 FILEVERSION（Windows 资源固定 4 个 16-bit 数）
const versionString = `${major}.${minor}.${patch}`; // 用户可见版本号：三段式
const cmakeVersion = `${major}.${minor}.${patch}`;

console.log(`[set-version] 目标版本：${versionString}（CMake 落点：${cmakeVersion}）${dry ? '（dry-run）' : ''}`);

let changed = 0;

// ── 1. CMakeLists.txt ────────────────────────────────────────────────────────
{
  const file = FILES.cmake;
  const src = fs.readFileSync(file, 'utf8');
  const re = /(project\(KunClient VERSION )([0-9.]+)( LANGUAGES CXX\))/;
  if (!re.test(src)) fail(`CMakeLists.txt 里找不到 project(KunClient VERSION ...) 落点`);
  const out = src.replace(re, `$1${cmakeVersion}$3`);
  if (out !== src) {
    console.log(`  [1/4] ${path.relative(REPO_ROOT, file)}：VERSION → ${cmakeVersion}`);
    if (!dry) fs.writeFileSync(file, out);
    changed++;
  } else {
    console.log(`  [1/4] ${path.relative(REPO_ROOT, file)}：已是 ${cmakeVersion}，跳过`);
  }
}

// ── 2. kc/version.h ──────────────────────────────────────────────────────────
{
  const file = FILES.versionH;
  let src = fs.readFileSync(file, 'utf8');
  const before = src;

  const subs = [
    [/inline constexpr int kVersionMajor = \d+;/, `inline constexpr int kVersionMajor = ${major};`],
    [/inline constexpr int kVersionMinor = \d+;/, `inline constexpr int kVersionMinor = ${minor};`],
    [/inline constexpr int kVersionPatch = \d+;/, `inline constexpr int kVersionPatch = ${patch};`],
    [/inline constexpr int kVersionBuild = \d+;/, `inline constexpr int kVersionBuild = ${build};`],
    [/inline constexpr const char\* kVersionString = "[0-9.]+";/, `inline constexpr const char* kVersionString = "${versionString}";`],
    [/inline constexpr const char\* kUserAgent = "KunClient\/[0-9.]+";/, `inline constexpr const char* kUserAgent = "KunClient/${versionString}";`],
  ];
  for (const [re, rep] of subs) {
    if (!re.test(src)) fail(`version.h 里找不到落点：${re}`);
    src = src.replace(re, rep);
  }
  if (src !== before) {
    console.log(`  [2/4] ${path.relative(REPO_ROOT, file)}：6 个常量 → ${versionString}`);
    if (!dry) fs.writeFileSync(file, src);
    changed++;
  } else {
    console.log(`  [2/4] ${path.relative(REPO_ROOT, file)}：已是 ${versionString}，跳过`);
  }
}

// ── 3. resources/app.rc（exe 的 Windows 版本信息资源）───────────────────────
{
  const file = FILES.appRc;
  if (!fs.existsSync(file)) {
    console.warn(`  [3/4] 警告：${file} 不存在，跳过`);
  } else {
    let src = fs.readFileSync(file, 'utf8');
    const before = src;
    // rc 里 FILEVERSION 用逗号分隔（2,0,0,0），字符串值用点分隔（"2.0.0.0"）。
    const commaVersion = `${major},${minor},${patch},${build}`;
    const subs = [
      [/FILEVERSION [0-9,]+/, `FILEVERSION ${commaVersion}`],
      [/PRODUCTVERSION [0-9,]+/, `PRODUCTVERSION ${commaVersion}`],
      [/VALUE "FileVersion", "[0-9.]+"/, `VALUE "FileVersion", "${versionString}"`],
    ];
    for (const [re, rep] of subs) {
      if (!re.test(src)) fail(`app.rc 里找不到落点：${re}`);
      src = src.replace(re, rep);
    }
    if (src !== before) {
      console.log(`  [3/4] ${path.relative(REPO_ROOT, file)}：FILEVERSION/PRODUCTVERSION/FileVersion → ${versionString}`);
      if (!dry) fs.writeFileSync(file, src);
      changed++;
    } else {
      console.log(`  [3/4] ${path.relative(REPO_ROOT, file)}：已是 ${versionString}，跳过`);
    }
  }
}

// ── 3. website/src/lib/data.ts ───────────────────────────────────────────────
{
  const file = FILES.dataTs;
  if (noWebsite) {
    console.log('  [4/4] --no-website：跳过官网 FALLBACK_VERSION');
  } else if (!fs.existsSync(file)) {
    console.warn(`  [4/4] 警告：${file} 不存在，跳过（官网仓库不在？）`);
  } else {
    const src = fs.readFileSync(file, 'utf8');
    const re = /(export const FALLBACK_VERSION: VersionInfo = \{\s*\r?\n\s*version: )"[0-9.]+(")/;
    if (!re.test(src)) fail(`data.ts 里找不到 FALLBACK_VERSION.version 落点`);
    const out = src.replace(re, `$1"${versionString}$2`);
    if (out !== src) {
      console.log(`  [4/4] ${path.relative(REPO_ROOT, file)}：FALLBACK_VERSION → ${versionString}`);
      if (!dry) fs.writeFileSync(file, out);
      changed++;
    } else {
      console.log(`  [4/4] ${path.relative(REPO_ROOT, file)}：已是 ${versionString}，跳过`);
    }
  }
}

console.log(`[set-version] 完成：${changed} 个文件${dry ? '（dry-run，未实际写入）' : ' 已写入'}`);
if (!dry && changed > 0) {
  console.log('[set-version] 提醒：改完记得重新构建；线上开闸用 .workbuddy/tmp/set_setting.js min_client_version <ver>');
}
