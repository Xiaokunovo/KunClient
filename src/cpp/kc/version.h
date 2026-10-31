#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  KunClient C++ 版本定义
//  版本号落点（5 处，同 .NET 时代的红线）：
//    1. CMakeLists.txt PROJECT VERSION
//    2. kc/version.h（本文件）
//    3. server/data.ts FALLBACK_VERSION
//    4. 线上 app_settings.version
//    5. UA 头（HttpClient 注入 KunClient/<ver>）
// ─────────────────────────────────────────────────────────────────────────────

namespace kc {

inline constexpr int kVersionMajor = 2;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 8;
inline constexpr int kVersionBuild = 0;

inline constexpr const char* kVersionString = "2.0.8";

// User-Agent 头值，服务端版本硬闸门按此识别客户端版本。
inline constexpr const char* kUserAgent = "KunClient/2.0.8";

// SWT-EDL 引擎版本（仅展示用，不影响功能）
inline constexpr const char* kEdlEngineVersion = "0.11.0";

} // namespace kc
