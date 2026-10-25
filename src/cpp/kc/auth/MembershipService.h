#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MembershipService —— 【停更离线版（2.0.6）已清空】
//
//  原实现是「会员等级 + 每日额度」的服务端裁决层：每次动作前向
//  POST /api/v2/app/check|consume 要一个结论，计数落在服务端 feature_usage 表。
//
//  🔴 2026-10-20 付费体系整体下线；2026-10-24 服务器关停，本类彻底退化为
//     **纯本地空壳**：所有闸门一律放行、不再有任何网络调用、不再有额度概念。
//
//  为什么还留着这个类而不是连文件一起删：22 处 guardAndConsume / 11 处
//  ensureAllowed / 9 处 consume 散布在 9 个功能页里，逐个删调用点纯属制造
//  编译风险，而保留一个「永远放行」的空壳在语义上等价（功能已无限制）。
//  日后若要彻底移除，把调用点删干净即可。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>

namespace kc {

class MembershipService
{
public:
    /// 会员名（服务器关停后恒为空 —— 不再有账号体系）。
    static QString userName() { return QString(); }

    /// 登录后同步会员身份 —— 已无意义，保留为空实现以免调用点报错。
    static void setUser(const QString&, const QString&, const QString& = QString()) {}

    /// 功能准入检查：**一律放行**（付费体系已下线，全部功能免费）。
    static bool ensureAllowed(const QString&, const QString&) { return true; }

    /// 准入 + 扣费：**一律放行**（无额度概念）。
    static bool guardAndConsume(const QString&, const QString&) { return true; }

    /// 扣费：**一律放行**。
    static bool consume(const QString&) { return true; }
};

} // namespace kc
