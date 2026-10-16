using System;

namespace KunFlashTool.App.Services
{
    /// <summary>登录态的内存持有者。token 无论是否勾选「记住密码」都进内存，
    /// 只在勾选时才把密码落盘。内存优先、磁盘兜底。</summary>
    public static class AuthSession
    {
        private static readonly object Gate = new();
        private static string _token = "";
        private static string _username = "";
        private static DateTime _expiry = DateTime.MinValue;

        public static string Username { get { lock (Gate) { return _username; } } }
        public static DateTime Expiry  { get { lock (Gate) { return _expiry;  } } }

        /// <summary>
        /// 本地是否持有一枚 token。
        ///
        /// 🔴 **刻意不在这里判过期**（2026-10-09 明确写下，免得后人当成漏掉的校验）：
        ///    token 的有效性**权威在服务端** —— 失效会返回 401，客户端按 401 走重新登录。
        ///    如果这里再拿本地 <see cref="Expiry"/> 比一次，客户端时钟偏一点就会把
        ///    本来有效的登录态判死，凭空多出一类"登录莫名其妙失效"的报障。
        /// </summary>
        public static bool IsAuthenticated => ResolveToken() != null;

        /// <summary>
        /// 本地「还算已登录」的**加强口径**：有 token **且**本地记录的到期时间还没到。
        ///
        /// 与 <see cref="IsAuthenticated"/> 的分工（两者都**不是**授权依据，授权在服务端）：
        ///   · <see cref="IsAuthenticated"/>      —— 只问「有没有 token」，不看时间，最宽松；
        ///   · 本属性 —— 本地快速口径，适合「要不要先把登录框藏起来 / 直接显示已登录态」这类 UI 决策。
        ///
        /// ⚠️ 使用注意：本地时钟可能不准，<see cref="Expiry"/> 也只是签发时按 24h 估的值，
        ///    与 JWT 真实有效期未必逐秒一致。所以「本地说过期」只能用来**提前引导重新登录**，
        ///    不能用来拒绝一次本来会成功的操作（那种判断交给服务端的 401）。
        /// </summary>
        public static bool IsAuthenticatedAndFresh
        {
            get
            {
                if (ResolveToken() == null) return false;

                var exp = Expiry;
                if (exp == DateTime.MinValue)
                {
                    // 本次会话还没登录过（token 来自磁盘）→ 借磁盘记录的到期时间判
                    try { exp = LoginStorage.LoadCredentials()?.Expiry ?? DateTime.MinValue; }
                    catch { exp = DateTime.MinValue; }
                }

                // 从未记录过到期时间（老版本写入的 / 非密码登录路径）→ 不拦，交给服务端判
                return exp == DateTime.MinValue || exp > DateTime.Now;
            }
        }

        public static void Set(string username, string token, DateTime expiry)
        {
            lock (Gate) { _username = username ?? ""; _token = token ?? ""; _expiry = expiry; }
        }

        public static void Clear()
        {
            lock (Gate) { _username = ""; _token = ""; _expiry = DateTime.MinValue; }
        }

        /// <summary>
        /// 服务端明确判定这枚 token 无效（401，且不是"被管理员踢下线"）时**彻底丢掉它**。
        /// <para>
        /// 🔴 为什么需要它（2026-10-09 逆向报告 D5 之后加）：原来 401 只返回一个 <c>noauth</c>，
        /// 内存与磁盘里的 token **都还在**。而 <see cref="ResolveToken"/> 会回落到磁盘，
        /// 于是程序永远停留在"已登录"、却持有一枚死 token —— 用户看到的是
        /// 「点了功能 → 提示请重新登录 → 界面又显示已登录」的死循环。
        /// </para>
        /// <para>
        /// ⚠️ 只清**凭据**，不做重启、不动用户名显示之外的状态：
        /// 强制重启是 <c>ForceLogout</c> 的职责（那条路径会打断正在进行的刷机，必须分开）。
        /// </para>
        /// </summary>
        public static void Drop()
        {
            Clear();
            try { LoginStorage.ClearCredentials(); } catch { }
        }

        /// <summary>内存优先；内存没有就回落磁盘（老用户升级 / 其它入口写入）。</summary>
        public static string? ResolveToken()
        {
            lock (Gate) { if (!string.IsNullOrEmpty(_token)) return _token; }
            try { return LoginStorage.LoadCredentials()?.Token; }
            catch { return null; }
        }
    }
}