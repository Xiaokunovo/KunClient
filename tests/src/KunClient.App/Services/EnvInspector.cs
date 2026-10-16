// ============================================================================
// EnvInspector —— 运行环境检测（2026-09-30 建立 / 2026-10-01 增强）
//
// 用户要求：客户端做环境检测，检测到问题就自动封禁账户若干天，**按信号强度分级**。
//
// 🔴 分级的核心决策（这不是保守，是这条功能能不能活下来的关键）：
//    LOW（只记录，永不自动封禁）：调试器 / 虚拟机 / 已知工具进程 / 可疑模块 / 远程会话。
//      理由：这是**刷机工具**。刷机用户的机器上装 Visual Studio、x64dbg、Wireshark、
//      在虚拟机里跑工具，都是**极其常见且完全正当**的用法。拿这些自动封号 =
//      成批封掉正常用户。上一轮我们刚把反调试的「防误杀」定为最关键的回归判据，
//      这里不能反过来用它当封号依据。
//    MID（累积到阈值才封）：客户端自述的完整性/反调试绕过/签名失败/内联 Hook/注入模块。
//    HIGH（服务端自证）：防伪校验、nonce 重放、多开 —— 这些不用客户端自述。
//
// ⚠️ 说清楚能力边界，别自欺：这些检测**全部可以被有经验的逆向者绕过**。
//    它们的价值是抬高门槛 + 给服务端提供「查证流水」，不是挡住专业攻击者。
//    真正的攻击者会直接把这段代码 NOP 掉。所以别拿它去承诺「防破解」。
//
// ── 信号清单（signal 名必须与服务端 ENV_SIGNAL_LEVEL 键逐字一致）：
//    LOW : debugger / vm / suspicious_module / remote_session
//    MID : integrity_fail / debug_bypassed / bad_hmac / inline_hook / injected_module
//          / intercept_tool / proxy_intercept / root_ca_injected   ← 2026-10-09 新增三条
//          / known_tool                                            ← 2026-10-09 由 LOW 提级
//    HIGH: challenge_fail / nonce_invalid / multi_instance（由服务端自判）
//          + 服务端的**组合判据**：同账号 24h 内命中 ≥2 类中间人特征 → 按 HIGH 处置
//
// ── 2026-10-09 增强：抓包 / 中间人专项（用户要求「不是只针对一点，要多加、而且针对抓包」）──
//    单点检测不够狠，所以做的是**成套特征**：
//      ① intercept_tool   抓包工具进程（Charles / Fiddler / Wireshark / mitmproxy / Reqable…）
//      ② proxy_intercept  系统代理被指向环回或中间人常用端口
//      ③ root_ca_injected 信任库里有中间人自己的根证书（HTTPS 抓包的硬证据）
//    要抓 HTTPS，这三件事**必须同时做到**；做到两件以上就不再是「恰好装了软件」，
//    所以服务端对组合做了升级（见 server.js 的 mitmComboHit）。
// ============================================================================
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Management;
using System.Net.NetworkInformation;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace KunFlashTool.App.Services
{
    /// <summary>一条环境信号。signal 取值必须与服务端 ENV_SIGNAL_LEVEL 的键**完全一致**，
    /// 否则服务端会把它当未知信号降级成 low（只记录）—— 那等于这条检测白做了。</summary>
    internal sealed class EnvSignal
    {
        public string Signal { get; set; }
        public string Detail { get; set; }
        public EnvSignal(string signal, string detail = "")
        {
            Signal = signal;
            Detail = detail ?? "";
        }
    }

    internal static class EnvInspector
    {
        // ── P/Invoke ────────────────────────────────────────────────────────
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        private static extern bool CheckRemoteDebuggerPresent(IntPtr hProcess, ref bool isDebuggerPresent);

        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        private static extern bool IsDebuggerPresent();

        [DllImport("ntdll.dll", SetLastError = true)]
        private static extern int NtQueryInformationProcess(IntPtr hProcess, int infoClass,
            IntPtr info, uint infoLength, out uint retLength);

        [DllImport("user32.dll")]
        private static extern int GetSystemMetrics(int nIndex);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibrary(string lpFileName);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

        private const int SmRemoteSession = 0x1000;   // SM_REMOTESESSION
        private const int ProcessDebugPort = 7;       // ProcessInformationClass

        /// <summary>已知的逆向 / 内存 / PE 分析 / 沙箱工具进程名（小写，子串匹配）。
        /// <para>🔴 **抓包类刻意不在这份名单里** —— 它们已拆到 <see cref="InterceptTools"/>。
        /// 两边都放的话，同一个进程会在一次采集里同时报 known_tool 与 intercept_tool 两条，
        /// 而这两条现在**都是 MID** → 服务端按自然日累积 strike 会一次算两次，
        /// 等于把「阈值 3 次」悄悄腰斩成「1.5 次」。这份名单里的每一条都参与 MID 累积，
        /// 加之前先想清楚它会不会与抓包名单重叠。</para>
        /// <para>2026-10-09：随 known_tool 提级到 MID，本名单同步收紧为「纯逆向/内存/沙箱」，
        /// **并删掉了过宽的条目** —— 子串匹配下 `ida` 会命中 **AIDA64**（刷机用户常装的硬件检测工具）、
        /// `stream` 会命中直播软件。在 LOW 时这些只是噪声，提到 MID 就变成「正常工具把用户封了」，
        /// 所以提级的同一批必须把名单按「具体进程名」收一遍。</para></summary>
        private static readonly string[] KnownTools =
        {
            "x64dbg", "x32dbg", "x96dbg", "ollydbg", "ida64", "idaq", "idaq64",
            "cheatengine", "processhacker", "systeminformer",
            "dnspy", "ilspy", "de4dot", "de4dotmod", "extremedumper",
            // 2026-10-01 补充：更多逆向 / 反汇编 / PE 分析 / 沙箱
            "pestudio", "pebear", "binaryninja", "ghidra", "radare2", "cutter",
            "procmon", "procexp", "autoruns", "apimonitor",
            "pydbg", "immunity", "windbg", "rexed", "megadumper", "scylla",
        };

        /// <summary>VM 特征：进程名。同样是 LOW 级，只记录。</summary>
        private static readonly string[] VmProcesses =
        {
            "vmwaretray", "vmwareuser", "vmtoolsd", "vboxtray", "vboxservice",
            "qemu-ga", "qemu-ga-x86_64", "xenservice", "prl_tools", "parallels",
            "vmcompute", "hvhost", "vmmem", "vmsrvc", "vmicsvc",
        };

        /// <summary>WMI / BIOS / 主板 厂商型号里的虚拟机特征词（小写匹配）。</summary>
        private static readonly string[] VmWmiMarkers =
        {
            "vmware", "virtualbox", "virtual machine", "vbox",
            "kvm", "qemu", "xen", "parallels", "hyper-v", "hyperv", "bochs",
            "innotek", "oracle vm", "hvm domu", "virtio",
            "openstack", "red hat", "xen hvm",
        };

        /// <summary>已知虚拟机厂商的 MAC 地址 OUI（取前 3 字节，大写十六进制无分隔）。</summary>
        private static readonly string[] VmMacOuis =
        {
            "005056", "000C29", "000569", "001C14",   // VMware
            "080027",                                  // VirtualBox
            "00155D",                                  // Hyper-V
            "001C42",                                  // Parallels
            "525400", "00163E",                        // QEMU / KVM / Xen
        };

        // ════════════════════════════════════════════════════════════════════
        // 抓包 / 中间人（MITM）检测（2026-10-09 新增）
        //
        // 🔴 为什么单独做一组、而不是塞进 KnownTools：
        //    KnownTools 是 LOW（只记录不封），因为刷机用户机器上装 x64dbg / Wireshark
        //    是常见的正当用法。但**「全套中间人环境」是另一回事**：要抓 HTTPS 必须同时
        //    ① 跑抓包工具 ② 改系统代理 ③ 装它自己的根证书 —— 三件事都做全了，
        //    性质就从「机器上恰好装了这个软件」变成「正在对我们的通信做中间人」。
        //    所以这三条各自是 MID（累积阈值才封），而**任意两条同时命中就在服务端升级为 HIGH**。
        //    这就是「不是只针对一点」的落点：判据是组合，不是单点。
        //
        // ⚠️ 能力边界照旧：这些都能被绕过（改进程名、用别的代理端口、自己签证书）。
        //    它抬高的是门槛与留痕，不是绝对防线。
        // ════════════════════════════════════════════════════════════════════

        /// <summary>抓包 / 中间人工具进程名（小写，子串匹配）。
        /// ⚠️ 只放**具体进程名**：子串匹配下越短的词越危险 —— 原来的 `stream` 会命中
        /// 直播/推流软件（streamlabs 等），`http toolkit` 带空格则永远匹配不到（进程名不含空格）。
        /// 这类条目在 LOW 时只是噪声，在 MID 就是误封，所以一律收紧成能对得上的名字。</summary>
        private static readonly string[] InterceptTools =
        {
            "fiddler", "fiddlereverywhere", "fiddlercore", "charles", "wireshark",
            "tshark", "dumpcap", "mitmproxy", "mitmdump", "mitmweb", "httptoolkit",
            "proxifier", "proxyman", "reqable", "whistle", "anyproxy",
            "burpsuite", "tcpdump", "ettercap", "bettercap", "netmon",
            "justniffer", "httpdebugger", "smartsniff", "networkminer",
            "packetmon", "netsniffer", "sniffpass", "intercepter-ng",
        };

        /// <summary>中间人工具自签根证书的特征词（小写，出现在 Subject 或 Issuer 里）。
        /// 🔴 这一条是**最硬**的抓包证据：HTTPS 中间人必须把 CA 装进系统信任库才能解密，
        ///    不装就只会得到证书错误。装了 = 本机正在（或曾经）做中间人。</summary>
        private static readonly string[] MitmCaMarkers =
        {
            "mitmproxy", "charles", "fiddler", "do_not_trust", "do not trust",
            "portswigger", "burp", "httptoolkit", "http toolkit", "anyproxy",
            "whistle", "reqable", "proxyman", "opensslproxy", "caido",
        };

        /// <summary>中间人工具常用的监听端口（系统代理指向这些端口时视为抓包特征）。</summary>
        private static readonly int[] MitmPorts = { 8888, 8889, 8080, 8081, 9090, 3128, 8000, 1080, 9000, 888, 7777 };

        /// <summary>采集全部信号。**每一步都必须吞异常** ——
        /// 环境检测是「附加功能」，任何权限不足 / WMI 不可用 / API 变动都不许把主程序带崩。
        /// （历史上 WMI 在部分机器上会抛，这里一律降级为「没查到」而不是崩溃。）</summary>
        public static List<EnvSignal> Collect()
        {
            var list = new List<EnvSignal>();

            // ── LOW：调试器（托管 + 原生多路） ──────────────────────────
            try
            {
                bool attached = false;
                try { attached = Debugger.IsAttached; } catch { }
                if (!attached) attached = NativeDebuggerPresent();
                if (attached) list.Add(new EnvSignal("debugger", "检测到调试器附加"));
            }
            catch { }

            // ── MID：走了反调试后门（放行） ─────────────────────────────
            // 允许调试是给开发者自己的逃生通道，但**开着它跑**应当留痕：
            // 它同时意味着「反调试那道关没生效」。
            // 🔴 后门名**不以明文字面量出现**（与壳 ShellMain.BackdoorName() 同款字符码拼法）——
            //    旧版这里直接写了明文，等于在产物里公开"怎么把保护关掉"。
            //    上报文案同样不再回显开关名。
            try
            {
                var allow = Environment.GetEnvironmentVariable(BackdoorName());
                if (allow == "1" || string.Equals(allow, "true", StringComparison.OrdinalIgnoreCase))
                    list.Add(new EnvSignal("debug_bypassed", "以后门模式启动，反调试已放行"));
            }
            catch { }

            // ── LOW：虚拟机（进程名 → 注册表 → WMI/BIOS → MAC OUI） ─────
            try
            {
                var hit = RunningNames().FirstOrDefault(n => VmProcesses.Any(v => n.Contains(v)));
                if (hit != null) list.Add(new EnvSignal("vm", "虚拟机相关进程: " + hit));
                else if (VmRegistryHit()) list.Add(new EnvSignal("vm", "注册表存在虚拟机特征"));
                else if (VmWmiHit(out var wmiDetail)) list.Add(new EnvSignal("vm", wmiDetail));
                else if (VmMacOuiHit(out var macDetail)) list.Add(new EnvSignal("vm", macDetail));
            }
            catch { }

            // ── LOW：已知工具进程 ────────────────────────────────────────
            // ── MID：抓包 / 中间人工具进程（比 KnownTools 强一级，理由见上方长注释）──
            try
            {
                var names = RunningNames();
                foreach (var t in KnownTools)
                {
                    var hit = names.FirstOrDefault(n => n.Contains(t));
                    if (hit != null) list.Add(new EnvSignal("known_tool", "进程: " + hit));
                }
                foreach (var t in InterceptTools)
                {
                    var hit = names.FirstOrDefault(n => n.Contains(t));
                    if (hit != null) list.Add(new EnvSignal("intercept_tool", "抓包/中间人工具进程: " + hit));
                }
            }
            catch { }

            // ── MID：系统代理被指向本机 / 中间人常用端口 ─────────────────
            try
            {
                if (ProxyInterceptHit(out var proxyDetail))
                    list.Add(new EnvSignal("proxy_intercept", proxyDetail));
            }
            catch { }

            // ── MID：信任库里存在中间人根证书（HTTPS 抓包的**硬证据**）───
            try
            {
                if (MitmRootCaHit(out var caDetail))
                    list.Add(new EnvSignal("root_ca_injected", caDetail));
            }
            catch { }

            // ── LOW：远程桌面 / 终端服务会话 ─────────────────────────────
            // 刷机用户把工具跑在云主机 / 远程桌面上是**完全正当**的用法，所以只记录不封。
            try
            {
                if (RemoteSessionHit(out var why)) list.Add(new EnvSignal("remote_session", why));
            }
            catch { }

            // ── LOW：本进程内被注入的可疑模块（按**名字**判定） ──────────
            try
            {
                foreach (ProcessModule m in Process.GetCurrentProcess().Modules)
                {
                    var f = (m.ModuleName ?? "").ToLowerInvariant();
                    if (f.Contains("hook") || f.Contains("inject") || f.Contains("detour") || f.Contains("dumper"))
                        list.Add(new EnvSignal("suspicious_module", "模块: " + m.ModuleName));
                }
            }
            catch { }

            // ── MID：模块从「松散可疑路径」加载（按**路径**判定） ────────
            // 与 suspicious_module 互补：那条看名字，这条看它从哪加载。
            try
            {
                if (InjectedModuleHit(out var injDetail))
                    list.Add(new EnvSignal("injected_module", injDetail));
            }
            catch { }

            // ── MID：关键系统 API 入口被改写（内联 Hook） ─────────────────
            // 惰性基线：本进程首次采集时把入口字节存为基线，之后每次复检比对。
            // 首次采集不报（只建基线），因此不会在登录那一刻误报。
            try
            {
                if (InlineHookHit(out var hookDetail))
                    list.Add(new EnvSignal("inline_hook", hookDetail));
            }
            catch { }

            // ── MID：主程序集不是「内存加载」的 ──────────────────────────
            // 正常发布下 KunClient.dll 由壳解密后 Assembly.Load(byte[]) 载入，
            // **Location 必然是空字符串**。若这里拿到真实路径，说明跑的是一份
            // 被 dump 到磁盘、再直接加载的副本 —— 壳已经被绕过。
            // 这条比「扫进程名」硬得多：它不是在猜，是在看自己是怎么被加载进来的。
            try
            {
#pragma warning disable IL3000
                // 🔴 故意读 Location：单文件发布下被壳内存加载的 KunClient.dll 此处必然为空串，
                //    空串 = 正常；非空 = 磁盘副本（壳被绕过）→ 这才是要捕的 integrity_fail。
                //    所以 IL3000（"单文件下 Location 返回空串"）在这里是预期行为，不是缺陷，压掉。
                var asm = typeof(EnvInspector).Assembly;
                var loc = asm.Location;
#pragma warning restore IL3000
                if (!string.IsNullOrEmpty(loc))
                    list.Add(new EnvSignal("integrity_fail", "主程序集从磁盘加载（非内存加载）: " + loc));
            }
            catch { }

            return list;
        }

        private static List<string> RunningNames()
        {
            var names = new List<string>();
            foreach (var p in Process.GetProcesses())
            {
                try
                {
                    var n = p.ProcessName;
                    if (!string.IsNullOrEmpty(n)) names.Add(n.ToLowerInvariant());
                }
                catch { }
            }
            return names;
        }

        /// <summary>
        /// 后门环境变量名，**运行期拼出**（源码与二进制里都不含明文，grep / strings 都搜不到）。
        /// 🔴 必须与壳 <c>KunClient.Shell/ShellMain.cs</c> 的 <c>BackdoorName()</c> 逐字一致 ——
        ///    壳与 App 是两份独立程序集（壳不引用 App），常量无法共享，只能各持一份、改一改两处。
        /// </summary>
        private static string BackdoorName()
        {
            var codes = new[] { 75, 85, 78, 95, 65, 76, 76, 79, 87, 95, 68, 69, 66, 85, 71 };
            return string.Concat(Array.ConvertAll(codes, c => (char)c));
        }

        /// <summary>原生调试器检测：PEB 的 BeingDebugged / NtQueryInformationProcess(ProcessDebugPort)。
        /// 托管侧 Debugger.IsAttached 只看托管调试器，这里补上原生附加的那一半。</summary>
        private static bool NativeDebuggerPresent()
        {
            // 1) PEB.BeingDebugged（等价 IsDebuggerPresent）
            try { if (IsDebuggerPresent()) return true; } catch { }

            // 2) NtQueryInformationProcess(ProcessDebugPort)：被调试时返回非 0 端口
            try
            {
                var size = IntPtr.Size;
                var buf = Marshal.AllocHGlobal(size);
                try
                {
                    using var p = Process.GetCurrentProcess();
                    var st = NtQueryInformationProcess(p.Handle, ProcessDebugPort, buf, (uint)size, out _);
                    if (st == 0 && Marshal.ReadIntPtr(buf) != IntPtr.Zero) return true;
                }
                finally { Marshal.FreeHGlobal(buf); }
            }
            catch { }

            // 3) 远程调试器
            try
            {
                var remote = false;
                using var p = Process.GetCurrentProcess();
                if (CheckRemoteDebuggerPresent(p.Handle, ref remote) && remote) return true;
            }
            catch { }

            return false;
        }

        private static bool VmRegistryHit()
        {
            // 只读 HKLM 下的几个 VM 特征键；读不到就当没有（权限不足是常态）
            string[] keys =
            {
                @"HKEY_LOCAL_MACHINE\SOFTWARE\VMware, Inc.\VMware Tools",
                @"HKEY_LOCAL_MACHINE\SOFTWARE\Oracle\VirtualBox Guest Additions",
                @"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Virtual Machine\Guest\Parameters",
                @"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\BIOS",
            };
            foreach (var k in keys)
            {
                try { if (Microsoft.Win32.Registry.GetValue(k, "InstallPath", null) != null) return true; }
                catch { }
            }
            return false;
        }

        /// <summary>WMI 查主板/BIOS 厂商型号里的虚拟机特征。</summary>
        private static bool VmWmiHit(out string detail)
        {
            detail = null!;
            try
            {
                var text = new StringBuilder();
                try
                {
                    using var s = new ManagementObjectSearcher(
                        "SELECT Manufacturer, Model FROM Win32_ComputerSystem");
                    foreach (ManagementBaseObject o in s.Get())
                    {
                        try { text.Append(o["Manufacturer"]).Append(' ').Append(o["Model"]).Append(' '); } catch { }
                        try { o.Dispose(); } catch { }
                    }
                }
                catch { }
                try
                {
                    using var s = new ManagementObjectSearcher(
                        "SELECT Manufacturer, Version, SerialNumber FROM Win32_BIOS");
                    foreach (ManagementBaseObject o in s.Get())
                    {
                        try { text.Append(o["Manufacturer"]).Append(' ').Append(o["Version"]).Append(' ').Append(o["SerialNumber"]); } catch { }
                        try { o.Dispose(); } catch { }
                    }
                }
                catch { }

                var t = text.ToString().ToLowerInvariant();
                var hit = VmWmiMarkers.FirstOrDefault(m => t.Contains(m));
                if (hit != null) { detail = "WMI/BIOS 特征: " + hit; return true; }
            }
            catch { }
            return false;
        }

        /// <summary>MAC 地址 OUI 比对（虚拟机厂商前缀）。</summary>
        private static bool VmMacOuiHit(out string detail)
        {
            detail = null!;
            try
            {
                foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
                {
                    try
                    {
                        var mac = nic.GetPhysicalAddress()?.ToString();
                        if (string.IsNullOrEmpty(mac) || mac.Length < 6) continue;
                        var oui = mac.Substring(0, 6).ToUpperInvariant();
                        if (VmMacOuis.Contains(oui)) { detail = "网卡 OUI: " + oui; return true; }
                    }
                    catch { }
                }
            }
            catch { }
            return false;
        }

        /// <summary>远程桌面 / 终端服务会话检测。第三种是环境变量兜底。</summary>
        private static bool RemoteSessionHit(out string why)
        {
            why = null!;
            try { if (GetSystemMetrics(SmRemoteSession) != 0) { why = "GetSystemMetrics(SM_REMOTESESSION) 命中"; return true; } }
            catch { }
            try
            {
                var session = Environment.GetEnvironmentVariable("SESSIONNAME") ?? "";
                if (session.StartsWith("RDP", StringComparison.OrdinalIgnoreCase))
                { why = "会话变量 SESSIONNAME=" + session; return true; }
                var client = Environment.GetEnvironmentVariable("CLIENTNAME") ?? "";
                if (!string.IsNullOrEmpty(client)) { why = "远程客户端 CLIENTNAME=" + client; return true; }
            }
            catch { }
            return false;
        }

        /// <summary>
        /// 系统代理是否被指向**本机或中间人常用端口**（WinINET 设置 —— IE/Chrome 以及大多数
        /// .NET 程序走的那一份代理）。
        ///
        /// 判据刻意收得很紧，只有两种命中方式：
        ///   ① 代理主机是环回（127.0.0.1 / localhost / ::1）—— 正规企业代理不会指向本机；
        ///   ② 端口落在中间人工具默认监听集合里（8888 Fiddler/Charles、8080 等）。
        /// 🔴 不做「只要开了代理就报」：公司/学校的正常 HTTP 代理会让大批正常用户误命中。
        /// </summary>
        private static bool ProxyInterceptHit(out string detail)
        {
            detail = null!;
            try
            {
                using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                    @"Software\Microsoft\Windows\CurrentVersion\Internet Settings");
                if (key == null) return false;

                var enabled = key.GetValue("ProxyEnable");
                if (enabled == null || Convert.ToInt32(enabled) == 0) return false;

                var server = (key.GetValue("ProxyServer") as string ?? "").Trim();
                if (string.IsNullOrEmpty(server)) return false;

                // 形如 "127.0.0.1:8888"，也可能是 "http=127.0.0.1:8888;https=127.0.0.1:8888"
                foreach (var part in server.Split(';', ','))
                {
                    var seg = part;
                    var eq = seg.IndexOf('=');                  // 丢掉 "http=" 这类协议前缀
                    if (eq >= 0) seg = seg.Substring(eq + 1);
                    seg = seg.Trim();
                    if (seg.Length == 0) continue;

                    var slashes = seg.IndexOf("//", StringComparison.Ordinal);
                    if (slashes >= 0) seg = seg.Substring(slashes + 2);   // 去 "http://"

                    var colon = seg.LastIndexOf(':');           // ⚠️ LastIndexOf：IPv6 字面量里也带冒号
                    var host = (colon > 0 ? seg.Substring(0, colon) : seg).Trim().Trim('[', ']').ToLowerInvariant();
                    var portStr = colon > 0 ? seg.Substring(colon + 1) : "";

                    if (host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "0.0.0.0")
                    {
                        detail = "系统代理指向本机: " + seg;
                        return true;
                    }
                    if (int.TryParse(portStr, out var port) && MitmPorts.Contains(port))
                    {
                        detail = "系统代理端口疑似中间人: " + seg;
                        return true;
                    }
                }
            }
            catch { }
            return false;
        }

        /// <summary>
        /// 信任库里是否装了中间人工具的根证书（CurrentUser 与 LocalMachine 两处 Root 存储）。
        ///
        /// 这是整套中间人环境里**最难伪装、也最能说明问题**的一条：不装根证书，抓 HTTPS
        /// 只会得到一串证书错误；装了它，等于用户**主动**接受了「让这台机器上的某个程序
        /// 冒充任意网站」。人工复核时这条几乎是决定性的。
        /// ⚠️ 只读 Subject/Issuer 做子串匹配，不读私钥、不做任何写入。
        /// </summary>
        private static bool MitmRootCaHit(out string detail)
        {
            detail = null!;
            foreach (var loc in new[] { StoreLocation.CurrentUser, StoreLocation.LocalMachine })
            {
                try
                {
                    using var store = new X509Store(StoreName.Root, loc);
                    store.Open(OpenFlags.ReadOnly | OpenFlags.OpenExistingOnly);
                    foreach (var cert in store.Certificates)
                    {
                        try
                        {
                            var text = ((cert.Subject ?? "") + " " + (cert.Issuer ?? "")).ToLowerInvariant();
                            var hit = MitmCaMarkers.FirstOrDefault(m => text.Contains(m));
                            if (hit != null)
                            {
                                detail = "信任库存在中间人根证书（" + hit + "）: " + TrimSubject(cert.Subject ?? "");
                                return true;
                            }
                        }
                        catch { }
                    }
                }
                catch { /* 权限不足 / LocalMachine 不可读是常态，跳过 */ }
            }
            return false;
        }

        /// <summary>证书 Subject 截断（服务端 detail 会截到 200，这里先收一下）。</summary>
        private static string TrimSubject(string subject)
        {
            var s = (subject ?? "").Replace("\r", "").Replace("\n", " ").Trim();
            return s.Length > 80 ? s.Substring(0, 80) : s;
        }

        /// <summary>模块是否从「松散可疑路径」加载（temp / 下载 / 桌面 / 文档）。
        /// 🔴 关键防误伤：单文件发布的程序**自身会被解压到 %TEMP%\.net\...** 再加载，
        ///    所以必须排除「本进程自己所在目录」这棵子树，否则全体用户都会命中。</summary>
        private static bool InjectedModuleHit(out string detail)
        {
            detail = null!;
            var exeDir = "";
            var appBase = "";
            try { appBase = (AppContext.BaseDirectory ?? "").TrimEnd('\\').ToLowerInvariant(); } catch { }
            try
            {
                var exe = Process.GetCurrentProcess().MainModule?.FileName ?? "";
                exeDir = (System.IO.Path.GetDirectoryName(exe) ?? "").TrimEnd('\\').ToLowerInvariant();
            }
            catch { }

            string[] redRoots;
            try
            {
                var temp = (System.IO.Path.GetTempPath() ?? "").TrimEnd('\\').ToLowerInvariant();
                var userProfile = (Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) ?? "").TrimEnd('\\').ToLowerInvariant();
                redRoots = new[]
                {
                    temp,
                    System.IO.Path.Combine(userProfile, "downloads").TrimEnd('\\'),
                    System.IO.Path.Combine(userProfile, "desktop").TrimEnd('\\'),
                    System.IO.Path.Combine(userProfile, "documents").TrimEnd('\\'),
                };
            }
            catch { redRoots = new string[0]; }

            try
            {
                foreach (ProcessModule m in Process.GetCurrentProcess().Modules)
                {
                    try
                    {
                        var path = m.FileName;
                        if (string.IsNullOrEmpty(path)) continue;
                        var lf = System.IO.Path.GetFullPath(path).ToLowerInvariant();

                        // 排除本进程自己的解压/运行目录（单文件发布的核心防误伤）
                        if (!string.IsNullOrEmpty(appBase) && lf.StartsWith(appBase)) continue;
                        if (!string.IsNullOrEmpty(exeDir) && lf.StartsWith(exeDir)) continue;

                        foreach (var r in redRoots)
                        {
                            if (!string.IsNullOrEmpty(r) && lf.StartsWith(r))
                            {
                                detail = "模块从可疑路径加载: " + path;
                                return true;
                            }
                        }
                    }
                    catch { }
                }
            }
            catch { }
            return false;
        }

        // 内联 Hook 基线（进程内一次性捕获，惰性）
        private static byte[] _hookBaseline = null!;
        private static readonly object _hookLock = new object();
        private static readonly string[] HookProbeTargets = { "CreateFileW", "GetModuleHandleW", "LoadLibraryW" };

        /// <summary>内联 Hook 检测：读关键系统 API 的入口字节，与首次采集的基线比对。
        /// 首次采集只建基线不报；之后（运行期复检）若入口字节被改写（典型的内联 hook 垫片）即命中。
        /// ⚠️ 只覆盖「本进程运行期被改写」这一场景；启动前就存在的持久化全局 Hook 不在覆盖范围内
        ///    （那需要一份可信的干净基线，代价与误伤风险都不划算）。</summary>
        private static bool InlineHookHit(out string detail)
        {
            detail = null!;
            try
            {
                var h = LoadLibrary("kernel32.dll");
                if (h == IntPtr.Zero) return false;

                var cur = new List<byte>();
                foreach (var name in HookProbeTargets)
                {
                    var addr = GetProcAddress(h, name);
                    if (addr == IntPtr.Zero) continue;
                    for (var i = 0; i < 12; i++)
                    {
                        try { cur.Add(Marshal.ReadByte(addr, i)); } catch { break; }
                    }
                }
                if (cur.Count == 0) return false;

                var snapshot = cur.ToArray();
                lock (_hookLock)
                {
                    if (_hookBaseline == null)
                    {
                        _hookBaseline = snapshot;   // 惰性建基线，本次不报
                        return false;
                    }
                    if (_hookBaseline.Length != snapshot.Length)
                    {
                        detail = "关键 API 入口字节长度异变（疑似内联 Hook）";
                        return true;
                    }
                    for (var i = 0; i < snapshot.Length; i++)
                    {
                        if (_hookBaseline[i] != snapshot[i])
                        {
                            detail = "关键 API 入口字节被改写（疑似内联 Hook）";
                            return true;
                        }
                    }
                }
            }
            catch { }
            return false;
        }

        /// <summary>客户端实例标识：服务端用它做「多开」判定。
        /// 只用**本机持久化**的一个随机串（不是 MAC / 不是硬盘号 —— 那些属于设备指纹，
        /// 采集面越大合规风险越高，而多开判定只需要「同一账号下出现了几个不同的我」）。</summary>
        public static string InstanceId()
        {
            try
            {
                var dir = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                // ⚠️ 必须落在 KunClientState —— %LOCALAPPDATA%\KunClient 会被退出清理**整目录删掉**
                var d = System.IO.Path.Combine(dir, "KunClientState");
                System.IO.Directory.CreateDirectory(d);
                var f = System.IO.Path.Combine(d, "instance.id");
                var id = System.IO.File.Exists(f) ? System.IO.File.ReadAllText(f).Trim() : "";
                if (string.IsNullOrEmpty(id))
                {
                    id = Guid.NewGuid().ToString("N");
                    System.IO.File.WriteAllText(f, id);
                }
                return id;
            }
            catch
            {
                return Guid.NewGuid().ToString("N");
            }
        }
    }
}
