using System;
using System.Diagnostics;
using System.Text.Json;
using System.Threading;
using System.Windows;

namespace KunFlashTool.App.Services
{
    /// <summary>
    /// 「云端强制下线」的客户端落点。
    ///
    /// 管理员在控制台点「强制下线」后，服务端把该账号的 `session_epoch` 写成当前时间；
    /// 该账号**已签发**的 JWT 的 `iat` 小于它，于是下一次带 token 的请求会收到
    /// <c>401 { code: "KICKED", message: "账号已被管理员强制下线，请重新登录" }</c>。
    ///
    /// 🔴 必须与「登录态自然过期」区分开：
    ///    普通 401（token 过期 / 无效）走原来的静默路径 —— 用户下次操作时自己会看到登录提示；
    ///    只有 <c>code == KICKED</c> 才清登录态并退出程序。混在一起处理的话，服务端一次抖动
    ///    或 token 自然过期就会把用户直接踢回登录窗，是明显的误伤。
    /// </summary>
    public static class ForceLogout
    {
        /// <summary>0 = 还没处理过。轮询链路与功能放行链路可能同时命中，用它保证只处理一次。</summary>
        private static int _handling;

        /// <summary>
        /// 拼进 cmd 命令行之前必须排除的字符（详见 <see cref="Trigger"/> 里的说明）。
        /// Windows 文件名不允许 <c>"</c>，但 <c>%</c>（变量展开）、<c>!</c>（延迟展开）、
        /// <c>^</c>、<c>&amp;</c>、<c>|</c>、<c>&lt;</c>、<c>&gt;</c> 都是合法字符。
        /// </summary>
        private static readonly char[] CmdUnsafeChars = { '"', '%', '!', '^', '&', '|', '<', '>', '\r', '\n' };

        /// <summary>
        /// 判断一个失败响应是不是「被强制下线」，是则带回服务端给的说明文案。
        /// body 允许为 null —— 读不到就当不是（宁可漏踢一次，也不要靠猜去误踢）。
        /// </summary>
        public static bool IsKicked(int statusCode, string? body, out string message)
        {
            message = "";
            if (statusCode != 401 || string.IsNullOrEmpty(body)) return false;
            try
            {
                using var doc = JsonDocument.Parse(body);
                var root = doc.RootElement;
                if (!root.TryGetProperty("code", out var c) || c.ValueKind != JsonValueKind.String) return false;
                if (!string.Equals(c.GetString(), "KICKED", StringComparison.Ordinal)) return false;
                if (root.TryGetProperty("message", out var m) && m.ValueKind == JsonValueKind.String)
                    message = m.GetString() ?? "";
                return true;
            }
            catch { return false; }
        }

        /// <summary>
        /// 执行强制下线：清掉本地登录态 → 说明原因 → 重启工具回到登录窗。
        /// 可从任意线程调用（内部切回 UI 线程），且只生效一次。
        /// </summary>
        public static void Trigger(string? message)
        {
            if (Interlocked.CompareExchange(ref _handling, 1, 0) != 0) return;

            var app = Application.Current;
            if (app == null) return;

            var text = string.IsNullOrEmpty(message) ? "账号已被管理员强制下线，请重新登录。" : message;

            // 用 BeginInvoke 而不是 Invoke：调用方（MembershipService.RequestGate）是在 UI 线程上
            // 同步阻塞取结果的，这里若同步回投会立刻死锁。
            app.Dispatcher.BeginInvoke(new Action(() =>
            {
                // 先清登录态：无论后面的弹窗 / 重启是否成功，本地都不能再拿这份已失效的 token 自动登录。
                // 内存（AuthSession）与磁盘（login.dat）必须一起清 —— 只清磁盘的话，
                // 本进程内 ResolveToken() 仍会命中内存 token，闸门继续用已失效的 token 请求。
                try { AuthSession.Clear(); } catch { }
                try { LoginStorage.ClearCredentials(); } catch { }
                try { MembershipService.SetUser(null, null, null); } catch { }

                try
                {
                    MessageBox.Show(
                        text + "\n\n工具会重新启动，请重新登录。",
                        "已强制下线", MessageBoxButton.OK, MessageBoxImage.Warning);
                }
                catch { }

                // 重启而不是直接退出：直接关掉用户会以为工具崩了。
                // 用 cmd 延迟 3 秒拉起 —— 等本进程 OnExit 把 %TEMP%\KunClient 与
                // %LOCALAPPDATA%\KunClient 两个缓存目录清完再启动，否则新进程刚下载的
                // platform-tools 会被旧进程的退出清理删掉。
                // 用 ping 而非 timeout 做延时：timeout 在无控制台的进程里会因为
                // 「输入重定向不受支持」立刻失败，延时就白加了。
                //
                // 🔴 路径要拼进 cmd 命令行，所以先做字符检查（2026-10-09 加固）：
                //    含 cmd 元字符的安装路径一旦拼进来就是命令注入汇聚点（含网络/UNC 路径尤为可疑）。
                //    这类路径极少见；遇到时**放弃 shell 拼接**，退化成直接启动 ——
                //    代价只是少了 3 秒延迟，新进程可能与旧进程的缓存清理抢一下，远好过注入。
                var restarted = false;
                try
                {
                    var exe = Environment.ProcessPath;
                    if (!string.IsNullOrEmpty(exe))
                    {
                        if (exe.IndexOfAny(CmdUnsafeChars) < 0)
                        {
                            Process.Start(new ProcessStartInfo("cmd.exe",
                                "/c ping -n 4 127.0.0.1 >nul & start \"\" \"" + exe + "\"")
                            {
                                CreateNoWindow = true,
                                UseShellExecute = false,
                            });
                        }
                        else
                        {
                            Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
                        }
                        restarted = true;
                    }
                }
                catch { }

                if (!restarted)
                {
                    try
                    {
                        MessageBox.Show("请手动重新启动 KunClient 并重新登录。", "已强制下线",
                            MessageBoxButton.OK, MessageBoxImage.Information);
                    }
                    catch { }
                }

                try { app.Shutdown(); } catch { }
            }));
        }
    }
}
