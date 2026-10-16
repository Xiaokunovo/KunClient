using System;
using System.IO;
using System.Reflection;
using System.Text.Json;

namespace KunFlashTool.Core.Configuration
{
    /// <summary>
    /// 客户端软配置（只读，Core 层与 App 层共用）：
    /// 优先级 = 环境变量 KUNCLIENT_SERVER_URL
    ///          > %LOCALAPPDATA%\KunClientState\KunClient.config.json（登录框「服务器设置」写入，优先级最高）
    ///          > %LOCALAPPDATA%\KunClient\KunClient.config.json（历史位置，仅迁移读取）
    ///          > exe 同目录 KunClient.config.local.json（本机覆盖，可选）
    ///          > exe 同目录 KunClient.config.json（手工放置，可选）
    ///          > 程序集内嵌的 KunClient.config.json（编译时烧进去的默认值）
    ///          > 内置兜底 DefaultServerUrl。
    /// 打包产物里不再出现松散的 json 配置文件。模板见 KunClient.config.example.json。
    /// </summary>
    public static class ClientConfig
    {
        /// <summary>最高优先级的环境变量名</summary>
        public const string EnvVarName = "KUNCLIENT_SERVER_URL";

        /// <summary>内置兜底地址（内嵌配置解析失败时才会用到）。
        ///
        /// 🔴 <b>本仓库是开源版，这里已抹成占位符</b>：自行部署时请替换成你自己的服务端地址，
        /// 或改用环境变量 / 配置文件（优先级见类型注释）。占位符状态下所有**在线**接口都连不上，
        /// 本地刷写类功能不受影响。
        ///
        /// ⚠️ 别改回 <c>http://127.0.0.1:3000</c>：那只是个"什么都没配"的信号，
        /// 一旦内嵌资源读不到（如入口程序集不是 KunClient.App），用户会**静默**落到
        /// localhost → 表现为「所有请求都失败」，而不是报配置错误。指向真实域名至少能连上。
        ///
        /// ⚠️ 选域名时避开**被 CDN 人机校验（Managed Challenge）拦下**的域：客户端不执行 JS，
        /// 所有接口会拿到 403 挑战页，表现与「服务器宕机」完全一样，极难排查。
        ///
        /// 🔴 不能用 <c>const</c>：const 的值进元数据 Constant 表，HideStrings 加密不到
        /// （详见 <c>FdlApiClient</c>）。填回真实域名时用 <c>static readonly</c>，
        /// 才会以 ldstr 形式被字符串加密覆盖。
        /// </summary>
        public static readonly string DefaultServerUrl = "https://your-domain.com";

        public const string ConfigFileName = "KunClient.config.json";
        public const string LocalConfigFileName = "KunClient.config.local.json";

        /// <summary>随程序发布的默认配置文件路径（可选；现在默认不随包分发）</summary>
        public static string ConfigFilePath => Path.Combine(AppContext.BaseDirectory, ConfigFileName);

        /// <summary>本机覆盖配置文件路径（可选，优先级更高）</summary>
        public static string LocalConfigFilePath => Path.Combine(AppContext.BaseDirectory, LocalConfigFileName);

        /// <summary>
        /// 用户级配置：登录框改服务器地址写这里（<c>%LOCALAPPDATA%\KunClientState\KunClient.config.json</c>）。
        /// <para>
        /// 注意目录是 **KunClientState** 而不是 KunClient：后者整个目录会在关闭工具时被删掉
        /// （见 <see cref="AppStatePaths"/>），写那里 = 用户改过的服务器地址每次启动丢失、静默回落内置默认地址。
        /// </para>
        /// </summary>
        public static string UserConfigFilePath => AppStatePaths.Combine(ConfigFileName);

        /// <summary>历史位置：<c>%LOCALAPPDATA%\KunClient\KunClient.config.json</c>，仅用于迁移读取。</summary>
        public static string LegacyUserConfigFilePath => AppStatePaths.Legacy(ConfigFileName);

        /// <summary>当前生效的服务器地址（结尾无 /）</summary>
        public static string ServerUrl => ResolveServerUrl() ?? DefaultServerUrl;

        /// <summary>按优先级解析服务器地址；全部缺失时返回 null</summary>
        public static string? ResolveServerUrl()
        {
            var v = Accept(Environment.GetEnvironmentVariable(EnvVarName));
            if (v != null) return v;

            // UserConfigFilePath（新状态目录）→ LegacyUserConfigFilePath（旧的、会被退出清理删掉的目录）
            // → 程序同目录 → 内嵌。旧目录那条只为老用户升级后不丢服务器地址。
            string[] files = { UserConfigFilePath, LegacyUserConfigFilePath, LocalConfigFilePath, ConfigFilePath };
            foreach (var file in files)
            {
                v = Accept(ReadServerUrl(file));
                if (v != null) return v;
            }

            return Accept(ReadServerUrlFromEmbedded());
        }

        // ══════════════════════════════════════════════════════════════════════
        // 主机白名单（2026-10-09 逆向报告 D6 / §8.3 之后加）
        //
        // 原状：只要写得出来（环境变量 / %LOCALAPPDATA% 下的 JSON / exe 同目录 json），
        //       任意 https 主机都会被照单全收。而 /api/login 提交的是**明文密码**，
        //       之后每个请求都带 Authorization: Bearer <token>。
        //       ⇒ 任何以当前用户身份运行的进程改一个 JSON，就能把凭据静默导向自己的服务器
        //         （自己持域名 + Let's Encrypt 证书即可，https 拦不住这件事）。
        // 现在：先归一化（远程必须 https），再过一遍可信主机判定；不在名单里的**直接丢弃**，
        //       回落到内置默认地址，而不是"照用户写的来"。
        // ══════════════════════════════════════════════════════════════════════

        /// <summary>允许"自定义远程服务器"的显式开关；不设时非白名单主机一律拒绝。</summary>
        public const string AllowCustomServerVarName = "KUNCLIENT_ALLOW_CUSTOM_SERVER";

        /// <summary>最近一次被拒的地址（供 UI / 日志提示"已回落内置地址"；正常情况为空）。</summary>
        public static string? LastRejectedServerUrl { get; private set; }

        /// <summary>归一化 + 白名单；任一关不过就返回 null（调用方回落到内置默认地址）。</summary>
        public static string? Accept(string? url)
        {
            var v = Normalize(url);
            if (v == null) return null;
            if (IsTrustedHost(v)) return v;

            // 🔴 不写死"只认自家域名"以外的判断进常量：白名单靠 IsTrustedHost 一处维护。
            if (CustomServerAllowed()) return v;

            LastRejectedServerUrl = v;
            return null;
        }

        /// <summary>用户是否显式放开自定义服务器（=1 / =true）。</summary>
        private static bool CustomServerAllowed()
        {
            try
            {
                var v = Environment.GetEnvironmentVariable(AllowCustomServerVarName);
                return v == "1" || string.Equals(v, "true", StringComparison.OrdinalIgnoreCase);
            }
            catch { return false; }
        }

        /// <summary>
        /// 主机是否可信：官方域名（含子域）、回环、私有内网。其余一律 false。
        /// <para>
        /// 官方域名用「精确匹配或子域」判定（<c>your-domain.com</c>、<c>www.your-domain.com</c> 都算，
        /// <c>your-domain.com.evil.com</c> 不算）—— 别只写 <c>EndsWith</c>，那样后缀伪造能过。
        /// </para>
        /// </summary>
        public static bool IsTrustedHost(string url)
        {
            try
            {
                var host = new Uri(url).Host.ToLowerInvariant().Trim('[', ']');
                if (host.Length == 0) return false;

                foreach (var d in OfficialDomains)
                    if (host == d || host.EndsWith("." + d, StringComparison.Ordinal)) return true;

                return IsLocalHost(host);
            }
            catch { return false; }
        }

        /// <summary>
        /// 官方域名（开源版已抹成占位符，部署时改成你自己的域）。
        /// 🔴 不能是 <c>const</c>（值会进元数据 Constant 表，字符串加密碰不到）；
        /// 用 <c>static readonly</c> 才会以 ldstr 形式被加密覆盖。
        /// <para>
        /// 只登记真正可用的主域：任何**被 CDN 人机校验拦下**的备用域都**故意不进名单** ——
        /// 放进名单等于允许把用户配置指向一个必然连不上的地址，那正是当年
        /// 「注册全线失败」的原样复现（详见 <c>ServerUrlGuardTests</c>）。
        /// </para>
        /// </summary>
        private static readonly string[] OfficialDomains =
        {
            "your-domain.com",
        };

        /// <summary>回环 / 私有内网 / 常见内网域名后缀：本机调试与自建内网服务要用，放行。</summary>
        private static bool IsLocalHost(string host)
        {
            if (host is "localhost" or "127.0.0.1" or "::1" or "0.0.0.0") return true;
            if (host.EndsWith(".local") || host.EndsWith(".lan") || host.EndsWith(".internal")) return true;

            var parts = host.Split('.');
            if (parts.Length == 4 && byte.TryParse(parts[0], out var a) && byte.TryParse(parts[1], out var b))
            {
                if (a == 10) return true;                        // 10.0.0.0/8
                if (a == 192 && b == 168) return true;           // 192.168.0.0/16
                if (a == 172 && b >= 16 && b <= 31) return true; // 172.16.0.0/12
                if (a == 169 && b == 254) return true;           // 169.254.0.0/16
            }
            return false;
        }

        /// <summary>
        /// 读取编译进程序集的默认配置（App 项目里以 EmbeddedResource 内嵌，LogicalName = KunClient.config.json）。
        /// 取不到时返回 null，交给内置兜底。
        /// </summary>
        public static string? ReadServerUrlFromEmbedded()
        {
            try
            {
                using var stream = OpenEmbeddedConfig();
                if (stream == null) return null;
                using var doc = JsonDocument.Parse(stream);
                if (doc.RootElement.TryGetProperty("serverUrl", out var el) && el.ValueKind == JsonValueKind.String)
                    return el.GetString();
            }
            catch { /* 内嵌资源缺失/损坏 → 回退内置默认值 */ }
            return null;
        }

        /// <summary>
        /// 找到真正内嵌了 <see cref="ConfigFileName"/> 的那个程序集并打开流。
        ///
        /// ⚠️ 为什么不能只用 <c>Assembly.GetEntryAssembly()</c>：
        /// 资源是内嵌在 **KunClient.App** 里的，而「入口程序集」和「持有资源的程序集」在两种情况下会分家：
        ///   1. 自建探针 / 单元测试宿主 —— 入口是宿主自己；
        ///   2. **内存加载壳（#97）** —— 入口是壳（KunClientLoader），真正的应用被解密后 Assembly.Load。
        /// 这两种情况 GetEntryAssembly() 都拿不到资源，会**静默**回落到 <see cref="DefaultServerUrl"/>
        /// （http://127.0.0.1:3000），表现为「所有请求都失败」而不是报配置错误 —— 看着像断网。
        /// 所以：先问入口程序集（正常路径、零开销），再遍历已加载程序集找一个真正持有该资源的。
        /// </summary>
        static System.IO.Stream? OpenEmbeddedConfig()
        {
            var entry = Assembly.GetEntryAssembly();
            var s = entry?.GetManifestResourceStream(ConfigFileName);
            if (s != null) return s;

            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                try
                {
                    s = asm.GetManifestResourceStream(ConfigFileName);
                    if (s != null) return s;
                }
                catch { /* 动态程序集等没有清单资源 → 跳过 */ }
            }
            return null;
        }

        /// <summary>从指定配置文件读取 serverUrl 字段；文件缺失/损坏/字段无效时返回 null</summary>
        public static string? ReadServerUrl(string file)
        {
            try
            {
                if (!File.Exists(file)) return null;
                using var doc = JsonDocument.Parse(File.ReadAllText(file));
                if (doc.RootElement.TryGetProperty("serverUrl", out var el) &&
                    el.ValueKind == JsonValueKind.String)
                    return el.GetString();
            }
            catch { /* 配置文件损坏时回退下一优先级 */ }
            return null;
        }

        /// <summary>补全协议、去掉结尾斜杠；空值返回 null；非法值返回 null</summary>
        /// <summary>
        /// 归一化服务器地址。**远程地址必须是 https**，只有本地/内网允许 http。
        ///
        /// 🔴 2026-10-09（源码审计后收紧）：这个地址决定 <c>/api/login</c> 的**明文密码**
        /// 与后续所有 <c>Authorization: Bearer</c> 往哪发。原来只要写 <c>http://任何域名</c>
        /// 就照单全收，配合「裸域名补 http://」的默认值 —— 等于默认把凭据摆在共享网络里。
        /// 现在：本地/内网（回环、10.x、192.168.x、172.16-31.x、169.254.x、*.local/*.lan）
        /// 仍允许 http（开发与自建内网要用），其余一律要求 https。
        /// 不满足时返回 <c>null</c>，调用方按"地址无效"处理。
        /// </summary>
        public static string? Normalize(string? url)
        {
            if (string.IsNullOrWhiteSpace(url)) return null;
            url = url.Trim().TrimEnd('/');
            // 只写了域名时补 **https**，不是 http —— 理由同下。
            if (!url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) &&
                !url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                url = "https://" + url;
            if (IsInsecureRemote(url)) return null;
            return url;
        }

        /// <summary>
        /// 是不是「明文 http 且指向非本地主机」—— 这种组合会把凭据暴露在网络上。
        /// 解析不出主机名时按不安全处理（宁可让用户改配置，也不要默默走明文）。
        /// </summary>
        private static bool IsInsecureRemote(string url)
        {
            if (!url.StartsWith("http://", StringComparison.OrdinalIgnoreCase)) return false;   // https 一律放行
            try
            {
                var host = new Uri(url).Host.ToLowerInvariant().Trim('[', ']');
                if (host.Length == 0) return true;
                // 复用同一份「本地主机」判定，避免两处规则各自漂移
                return !IsLocalHost(host);
            }
            catch { return true; }
        }
    }
}
