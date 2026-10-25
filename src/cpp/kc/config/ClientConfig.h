#pragma once

#include <QString>
#include <QStringList>

namespace kc {

// ─────────────────────────────────────────────────────────────────────────────
//  客户端软配置（只读，Core 层与 App 层共用）：
//  优先级 = 环境变量 KUNCLIENT_SERVER_URL
//           > %LOCALAPPDATA%\KunClientState\KunClient.config.json（登录框写入，优先级最高）
//           > %LOCALAPPDATA%\KunClient\KunClient.config.json（历史位置，仅迁移读取）
//           > exe 同目录 KunClient.config.local.json（本机覆盖，可选）
//           > exe 同目录 KunClient.config.json（手工放置，可选）
//           > 程序内嵌的 KunClient.config.json（编译时烧进去的默认值）
//           > 内置兜底 DefaultServerUrl。
//  打包产物里不再出现松散的 json 配置文件。模板见 KunClient.config.example.json。
// ─────────────────────────────────────────────────────────────────────────────
class ClientConfig
{
public:
    // 最高优先级的环境变量名
    static const QString EnvVarName;

    // 允许「自定义远程服务器」的显式开关；不设时非白名单主机一律拒绝。
    static const QString AllowCustomServerVarName;

    static const QString ConfigFileName;
    static const QString LocalConfigFileName;

    // 内置兜底地址（内嵌配置解析失败时才会用到）。
    //
    // 别改回 http://127.0.0.1:3000：那只是个「什么都没配」的信号，
    // 一旦内嵌资源读不到，用户会**静默**落到 localhost → 表现为「所有请求都失败」。
    // 域名选择：不能用 kunawa.xyz —— 它被 Cloudflare 挂了 Managed Challenge，
    // 客户端不执行 JS，所有接口会被 403 拦掉。kunawa.cn 实测干净可用。
    // 🔴 不做成常量字面量：字符串加密碰不到元数据常量表，编译期折叠的片段拼接更容易被覆盖。
    static QString DefaultServerUrl();

    // 随程序发布的默认配置文件路径（exe 同目录）。
    static QString ConfigFilePath();
    // 本机覆盖配置文件路径（优先级更高）。
    static QString LocalConfigFilePath();
    // 用户级配置：登录框改服务器地址写这里。
    // 注意目录是 KunClientState 而非 KunClient —— 后者整个目录关闭时会被删掉。
    static QString UserConfigFilePath();
    // 历史位置：%LOCALAPPDATA%\KunClient\KunClient.config.json，仅用于迁移读取。
    static QString LegacyUserConfigFilePath();

    // 当前生效的服务器地址（结尾无 /）
    static QString ServerUrl();
    // 按优先级解析服务器地址；全部缺失时返回空字符串
    static QString ResolveServerUrl();

    // ── 主机白名单 ───────────────────────────────────────────────────────────
    // 原状：只要写得出来（环境变量 / %LOCALAPPDATA% 下的 JSON / exe 同目录 json），
    //       任意 https 主机都会被照单全收。而 /api/login 提交的是**明文密码**，
    //       之后每个请求都带 Authorization: Bearer <token>。
    //       ⇒ 任何以当前用户身份运行的进程改一个 JSON，就能把凭据静默导向自己的服务器。
    // 现在：先归一化（远程必须 https），再过一遍可信主机判定；不在名单里的**直接丢弃**，
    //       回落到内置默认地址，而不是「照用户写的来」。

    // 归一化 + 白名单；任一关不过就返回空（调用方回落到内置默认地址）。
    static QString Accept(const QString& url);

    // 补全协议、去掉结尾斜杠；空值返回空；非法值返回空。
    // 远程地址必须是 https，只有本地/内网允许 http。
    static QString Normalize(const QString& url);

    // 主机是否可信：官方域名（含子域）、回环、私有内网。其余一律 false。
    static bool IsTrustedHost(const QString& url);

    // 回环 / 私有内网 / 常见内网域名后缀：本机调试与自建内网服务要用，放行。
    static bool IsLocalHost(const QString& host);

    // 是不是「明文 http 且指向非本地主机」—— 这种组合会把凭据暴露在网络上。
    static bool IsInsecureRemote(const QString& url);

    // 用户是否显式放开自定义服务器（=1 / =true）。
    static bool CustomServerAllowed();

    // 最近一次被拒的地址（供 UI / 日志提示「已回落内置地址」；正常情况为空）。
    static QString LastRejectedServerUrl();

    // 官方域名。🔴 只有 kunawa.cn：另一个官方域 kunawa.xyz **故意不进名单** ——
    // 它被 Cloudflare 挂了 Managed Challenge，客户端不执行 JS，接口会被 403 拦掉。
    // 用「精确匹配或子域」判定（kunawa.cn、www.kunawa.cn 都算，kunawa.cn.evil.com 不算）。
    static QStringList OfficialDomains();

    // 读取程序内嵌的默认配置（Qt 资源 :/KunClient.config.json）。
    // 取不到时返回空，交给内置兜底。
    static QString ReadServerUrlFromEmbedded();

    // 从指定配置文件读取 serverUrl 字段；文件缺失/损坏/字段无效时返回空。
    static QString ReadServerUrl(const QString& file);
};

} // namespace kc
