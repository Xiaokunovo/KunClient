using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace KunFlashTool.App.Services
{
    /// <summary>
    /// 客户端软配置（软口）：服务器地址不在代码里写死。
    /// 优先级 = 环境变量 KUNCLIENT_SERVER_URL
    ///          > %LOCALAPPDATA%\KunClientState\KunClient.config.json（本文件，登录框「服务器设置」写入）
    ///            （原在 %LOCALAPPDATA%\KunClient 下，那个目录退出时会整个被删掉，已迁走）
    ///          > exe 同目录 KunClient.config.local.json（本机覆盖，可选）
    ///          > exe 同目录 KunClient.config.json（手工放置，可选）
    ///          > 程序集内嵌的 KunClient.config.json（编译时烧进去的默认值）
    ///          > 内置默认值。
    /// 运行时可通过登录框“服务器设置”修改，写回的是用户级配置（不在 exe 目录里落 json，
    /// 这样打包目录干净、装在 Program Files 也有写权限）；模板见 KunClient.config.example.json。
    /// </summary>
    public static class AppConfig
    {
        /// <summary>内置兜底地址（不指向任何生产环境）。转发自 Core，同为 static readonly
        /// （不能是 const —— 上游已是运行期字段，且 const 会绕过字符串加密）。</summary>
        public static readonly string DefaultServerUrl = KunFlashTool.Core.Configuration.ClientConfig.DefaultServerUrl;

        private static readonly string ConfigFile = KunFlashTool.Core.Configuration.ClientConfig.UserConfigFilePath;
        private static readonly object Gate = new();
        private static string _serverUrl = LoadServerUrl();

        /// <summary>当前服务器地址（结尾无 /）</summary>
        public static string ServerUrl
        {
            get { lock (Gate) { return _serverUrl; } }
        }

        /// <summary>用户控制台地址</summary>
        public static string ConsoleUrl => ServerUrl + "/console/";

        /// <summary>最近一次被主机白名单拒绝的地址（没有则为 null）。
        /// 供登录框提示"该地址不在可信名单内，已回落内置地址"，避免用户以为改成功了。</summary>
        public static string? LastRejectedServerUrl
            => KunFlashTool.Core.Configuration.ClientConfig.LastRejectedServerUrl;

        /// <summary>地址是否落在可信主机名单内（官方域名 / 回环 / 私有内网）。</summary>
        public static bool IsTrustedHost(string url)
            => KunFlashTool.Core.Configuration.ClientConfig.IsTrustedHost(url);

        /// <summary>
        /// 修改服务器地址并持久化。空/空白 = 恢复默认值。非法地址返回 false。
        /// <para>
        /// 🔴 过的是 <see cref="AcceptUrl"/>（归一化 + 主机白名单），不是单纯的格式归一化：
        /// 否则「输入框里写 <c>https://evil.example.com</c>」会显示保存成功，
        /// 但 <see cref="LoadServerUrl"/> 在白名单那一关把它丢掉、静默回落内置地址 ——
        /// 用户看到的是"改了没生效"，排查成本极高。
        /// </para>
        /// </summary>
        public static bool SetServerUrl(string? url)
        {
            var v = string.IsNullOrWhiteSpace(url) ? DefaultServerUrl : AcceptUrl(url);
            if (string.IsNullOrEmpty(v)) return false;
            lock (Gate)
            {
                _serverUrl = v!;
                SaveServerUrl(v!);
            }
            return true;
        }

        public static string ConfigFilePath => ConfigFile;

        /// <summary>本机覆盖配置文件路径（可选；位于 exe 同目录，优先级低于用户级配置）</summary>
        public static string LocalConfigFilePath => KunFlashTool.Core.Configuration.ClientConfig.LocalConfigFilePath;

        /// <summary>exe 同目录的随包配置文件路径（可选；打包时已不再输出，仅供手工放置）</summary>
        public static string ShippedConfigFilePath => KunFlashTool.Core.Configuration.ClientConfig.ConfigFilePath;

        /// <summary>重新按优先级读取配置（外部修改配置文件后调用即可生效）</summary>
        public static string Reload()
        {
            lock (Gate)
            {
                _serverUrl = LoadServerUrl();
                return _serverUrl;
            }
        }

        /// <summary>按优先级解析：环境变量 > KunClient.config.local.json > KunClient.config.json > 内置默认值</summary>
        private static string LoadServerUrl()
            => KunFlashTool.Core.Configuration.ClientConfig.ResolveServerUrl() ?? DefaultServerUrl;

        private static void SaveServerUrl(string url)
        {
            try
            {
                var dict = new Dictionary<string, string>(StringComparer.Ordinal);
                if (File.Exists(ConfigFile))
                {
                    using var doc = JsonDocument.Parse(File.ReadAllText(ConfigFile));
                    foreach (var p in doc.RootElement.EnumerateObject())
                        dict[p.Name] = p.Value.ValueKind == JsonValueKind.String
                            ? (p.Value.GetString() ?? "")
                            : p.Value.GetRawText();
                }
                else
                {
                    // 首次写入时补一句说明：这个文件是登录框改服务器地址后生成的，改回默认值删掉即可
                    dict["_说明"] = "KunClient 用户级软配置：登录框「服务器设置」修改后的地址写在这里，优先级高于程序内嵌的默认值。删除本文件即恢复默认地址。";
                }
                dict["serverUrl"] = url;

                var dir = Path.GetDirectoryName(ConfigFile);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                    Directory.CreateDirectory(dir);
                File.WriteAllText(ConfigFile,
                    JsonSerializer.Serialize(dict, new JsonSerializerOptions { WriteIndented = true }));
            }
            catch { /* 只读目录等场景静默失败，本次会话内仍生效 */ }
        }

        /// <summary>
        /// 与 <c>ClientConfig.Accept</c> **同口径**（远程强制 https + 主机白名单）。
        /// 直接委托过去，而不是在这里各写一套 —— 两处逻辑一旦分叉，走哪条就取决于
        /// 优先级链先命中谁，会出现「配置文件 A 生效、配置文件 B 被拒」这种极难排查的现象。
        /// </summary>
        private static string? AcceptUrl(string? url)
            => KunFlashTool.Core.Configuration.ClientConfig.Accept(url);
    }
}
