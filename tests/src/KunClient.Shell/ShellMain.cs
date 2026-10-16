// ============================================================================
// 内存加载壳的入口（#97）
//
// 职责：把混淆后的 KunClient.dll / KunClient.Core.dll 从**内嵌密文**解密到内存，
//       Assembly.Load 起来，然后把 WPF 应用正常跑起来。
//
// 本文件里的每一条「为什么这么做」都是实测得来的（探针 `.workbuddy/tmp/shell_probe/`），
// 不是推理。删任何一条都会在运行期炸，且**编译期毫无征兆**。
// ============================================================================
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows;

namespace KunFlashTool.Shell
{
    internal static class ShellMain
    {
        /// <summary>
        /// 载荷密钥的**散落片段**（2026-09-30 二次增强：片段**故意乱序存放**，
        /// 真实拼合顺序由下面的 FragOrder 索引表给出）。
        /// 光是拆成片段还不够 —— 顺序若是自然顺序，反编译时照着数组从上往下抄一遍就拼出来了；
        /// 打乱后还得先读懂索引表。成本为零，多一层麻烦是一层。
        /// 必须与 tools/obfuscate-build.js 的 FRAG_STORE / FRAG_ORDER **逐项一致**，改一处就改两处。
        /// </summary>
        static readonly string[] FragStore =
            { "ell", "-20", "Kun", "-Pay", "Sh", "Cli", "26-", "ent", "load", "-v2", "Edl" };
        static readonly int[] FragOrder = { 2, 5, 7, 1, 6, 10, 4, 0, 3, 8, 9 };

        /// <summary>
        /// 🔴 **为什么不直接写一句明文密钥**（2026-09-30 订正，这是本壳最大的一处弱点）：
        ///     上一版是 `Encoding.UTF8.GetBytes("KunClient-2026-EdlShell-Payload-Key-v1")` +
        ///     逐字节 XOR。问题是：① 壳项目**不走 Obfuscar**（只有 App / Core 被混淆），
        ///     这句会**原样**留在 KunClientLoader.exe 里，`strings` 一次就拿到；
        ///     ② XOR 是可逆流密码，拿到密钥等于没加密 —— 再把密文 carve 出来，
        ///     `Buffer ^ key` 然后 inflate 就是完整的 KunClient.dll，一行脚本的事。
        ///     所以那版「加密」挡住的只有**不会写脚本**的人。
        ///
        /// 现在改成：**散落片段 → 拼合 → SHA-256 → 32 字节 key / 16 字节 iv**，
        /// 算法换成 AES-256-CBC。效果是把门槛从「strings」抬到「必须读 IL 还原派生逻辑」。
        ///
        /// ⚠️ 别高估这层保护：**客户端程序必须自带密钥，这在密码学上无解** ——
        ///    有调试能力的人照样能从内存里 dump 出解过的程序集。
        ///    本节的目标是抬高静态提取的成本，不是绝对安全。写下 Pacific 是为了别让后人
        ///    拿着「我们有壳」的错觉去做更激进的事。
        /// </summary>
        static byte[] _key, _iv;
        static void EnsureKey()
        {
            if (_key != null) return;
            var sb = new StringBuilder();
            foreach (var i in FragOrder) sb.Append(FragStore[i]);   // 按索引表拼，不是按存放顺序
            var seed = Encoding.UTF8.GetBytes(sb.ToString());
            _key = SHA256.HashData(seed);                       // 32 字节
            _iv = SHA256.HashData(_key).AsSpan(0, 16).ToArray(); // IV 由 key 再派生一次，不必存进载荷
            // 用完立刻抹掉拼出来的种子：它是最值钱的那几个字节（顺手，成本为零）
            Array.Clear(seed, 0, seed.Length);
        }

        /// <summary>密文清单：逻辑资源名 → 程序集简单名。</summary>
        static readonly string[] PayloadNames = { "KunClient.dll", "KunClient.Core.dll" };

        /// <summary>
        /// ⭐ 已加载程序集的缓存 —— 这不是优化，是**正确性**要求。
        ///
        /// Assembly.Load(byte[]) **不会**登记进「按名解析」的缓存。没有这个缓存时：
        ///   Assembly.Load(byte[]) 加载了一份 KunClient
        ///   → 随后 Assembly.Load("KunClient")（pack URI 就是这么找类型的）没命中它
        ///   → 触发解析钩子 → **又解密加载了第二份**
        ///   → 同进程两份同名程序集 → 类型标识分裂 → 资源字典查不到 CardBorder、x:Class 解析不到
        ///   → 25 个视图全 FAIL，看起来像「壳把整个 UI 毁了」。
        /// 实测结论：`ReferenceEquals` 必须是 True。
        /// </summary>
        static readonly Dictionary<string, Assembly> Loaded = new Dictionary<string, Assembly>(StringComparer.OrdinalIgnoreCase);

        static readonly Dictionary<string, string> PayloadOf = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            { "KunClient",      "KunClient.dll.enc" },
            { "KunClient.Core", "KunClient.Core.dll.enc" },
        };

        [STAThread]
        static int Main()
        {
            try
            {
                // 反分析必须在**解密之前**：等 Assembly.Load 完了再查，人家早就 dump 完了。
                GuardAgainstDebugger();           // 多路调试器检测（托管 + 原生 + 调试端口）
                RejectAnalysisEnvironment();      // 同机已知 dump / 逆向工具检测
                HideCurrentThreadFromDebugger();  // 线程对调试器隐藏，抬高附加成本
                ArmPeriodicGuard();               // 启动后定时复检（补掉「启动后再附加」这个洞）

                // 解析钩子必须先挂：KunClient.Core 是在 KunClient 被真正用到时才按需解析的。
                AssemblyLoadContext.Default.Resolving += (ctx, name) => Resolve(ctx, name.Name);
                AppDomain.CurrentDomain.AssemblyResolve += (_, e) => Resolve(AssemblyLoadContext.Default, SafeSimpleName(e.Name));

                var asm = Resolve(AssemblyLoadContext.Default, "KunClient");
                if (asm == null) throw new InvalidOperationException("解密/加载 KunClient 失败（内嵌载荷缺失或损坏）");

                // 必须早于任何 Application/资源字典访问：WPF 在 Application 的静态构造里
                // 已把 ResourceAssembly 固定成「入口程序集」（= 壳，壳里没有 BAML）。
                ForceResourceAssembly(asm);

                var appType = asm.GetType("KunFlashTool.App.App", throwOnError: true);
                var app = (Application)Activator.CreateInstance(appType);

                var init = appType.GetMethod("InitializeComponent", Type.EmptyTypes);
                if (init != null) init.Invoke(app, null);   // 等价于 WPF 为 App.xaml 生成的 Main

                return app.Run();
            }
            catch (Exception ex)
            {
                Report(ex);
                return 1;
            }
        }

        static string SafeSimpleName(string fullName)
        {
            try { return new AssemblyName(fullName).Name; } catch { return null; }
        }

        /// <summary>
        /// 反调试 / 反分析加固（2026-10-01 第三轮：多路检测 + 线程隐藏 + 运行期复检 + 工具检测）。
        ///
        /// 壳能挡的**最后一件事**就是「程序跑起来以后被人附加调试器 / 一键 dump 内存」——
        /// 只要程序能运行，解开的程序集就一定在进程里，这是绕不过去的。所以只能：
        ///   ① 让「附加」这个动作本身尽量失败（多路检测 + NtSetInformationThread 线程隐藏）；
        ///   ② 启动后**定时复检** —— 旧实现只在 Main 开头查一次，**启动后再附加完全不受影响**，
        ///      这是本轮补掉的最大一个洞；
        ///   ③ 检测常见 dump / 调试工具正在同机运行（这类工具正常用户不会开着刷机）。
        ///
        /// 多路检测（任何一路命中即判定）：托管 `Debugger.IsAttached` / `Debugger.IsLogging`、
        /// 原生 `CheckRemoteDebuggerPresent`、`NtQueryInformationProcess(ProcessDebugPort)`。
        /// 多路是必要的 —— 单看 `IsAttached` 挡不住不通过 CLR 调试接口的 native 工具。
        ///
        /// ⚠️ 仍然**必须留后门**：① 自己日后排查总得能跑；② 沙箱 / 安全软件 / 远程会话误判时，
        ///    用户要有一条立刻自救的路。但 2026-10-01 起有两处收紧：
        ///    · 后门名**不再以明文字面量**出现在源码/二进制里（运行期用字符码拼出，"strings" 搜不到）；
        ///    · **不再把后门名印在报错文案上** —— 旧做法等于主动教「怎么关掉保护」，是本案的缺陷之一。
        ///      误判时改为引导用户走官网反馈，而非泄露绕过方式。
        /// </summary>
        static void GuardAgainstDebugger()
        {
            if (BackdoorAllowed()) return;

            if (DebuggerPresent())
                throw new InvalidOperationException(
                    "检测到调试器正在附加到本进程，已拒绝解密并启动。\n\n"
                    + "这是客户端的自我保护（防止程序被内存 dump / 动态调试）。"
                    + "如果你确认这是误判，请通过官网反馈渠道联系我们。");
        }

        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        static extern bool CheckRemoteDebuggerPresent(IntPtr hProcess, ref bool isDebuggerPresent);

        [DllImport("ntdll.dll")]
        static extern int NtQueryInformationProcess(IntPtr hProcess, int infoClass, ref IntPtr info, int infoLen, out int retLen);

        [DllImport("ntdll.dll")]
        static extern int NtSetInformationThread(IntPtr hThread, int infoClass, IntPtr info, int infoLen);

        [DllImport("kernel32.dll")]
        static extern IntPtr GetCurrentThread();

        /// <summary>`ThreadHideFromDebugger` 的信息类号（未公开，NDK 常量）。</summary>
        const int ThreadHideFromDebugger = 0x11;
        /// <summary>`ProcessDebugPort` 的信息类号；被调试时返回非 0 端口的句柄值。</summary>
        const int ProcessDebugPort = 7;

        /// <summary>多路判定「当前进程是否被调试」。任一路命中即 True。</summary>
        static bool DebuggerPresent()
        {
            try { if (Debugger.IsAttached) return true; } catch { }
            try { if (Debugger.IsLogging()) return true; } catch { }

            bool present = false;
            try { using var p = Process.GetCurrentProcess(); CheckRemoteDebuggerPresent(p.Handle, ref present); } catch { }
            if (present) return true;

            try
            {
                var port = IntPtr.Zero;
                using var p = Process.GetCurrentProcess();
                int rc = NtQueryInformationProcess(p.Handle, ProcessDebugPort, ref port, IntPtr.Size, out _);
                if (rc >= 0 && port != IntPtr.Zero) return true;   // 有调试端口 = 被调试
            }
            catch { }

            return false;
        }

        /// <summary>
        /// 把当前线程对调试器隐藏（`ThreadHideFromDebugger`）。
        /// 效果：即便事后附加调试器，本线程也不再上报调试事件，抬高「挂上去看一眼」的成本。
        /// 失败（老系统 / 被安全软件拦）不致命，静默忽略 —— 它只是纵深防御的一层。
        /// </summary>
        static void HideCurrentThreadFromDebugger()
        {
            try { NtSetInformationThread(GetCurrentThread(), ThreadHideFromDebugger, IntPtr.Zero, 0); } catch { }
        }

        /// <summary>已知的「内存 dump / 反编译 / 逆向」工具进程名（小写，前缀匹配）。</summary>
        static readonly string[] DumpToolProcesses =
        {
            "megadumper", "extremedumper", "dotnet-dump", "scylla",
            "x64dbg", "x32dbg", "ollydbg", "windbg", "windbgx",
            "ida64", "idaq64", "ghidra", "dnspy", "ilspy", "de4dot",
            "cheatengine",
        };

        /// <summary>扫描同机运行的进程；命中已知 dump / 逆向工具时返回其进程名（小写），否则 null。</summary>
        static string FindRunningDumpTool()
        {
            try
            {
                foreach (var p in Process.GetProcesses())
                {
                    try
                    {
                        var n = p.ProcessName;
                        if (!string.IsNullOrEmpty(n))
                        {
                            n = n.ToLowerInvariant();
                            foreach (var b in DumpToolProcesses)
                                if (n.StartsWith(b, StringComparison.Ordinal)) return n;
                        }
                    }
                    catch { }
                    finally { try { p.Dispose(); } catch { } }
                }
            }
            catch { }
            return null;
        }

        /// <summary>
        /// 启动前的环境闸门：同机若在运行已知 dump / 逆向工具，拒绝解密启动（后门可跳过）。
        /// 为什么敢在这里硬拦：这类工具与「正常刷机」没有交集 —— 一个用户不会一边开着
        /// MegaDumper / x64dbg 一边给手机刷机。误判时后门可自救，且在报错里给了反馈渠道。
        /// </summary>
        static void RejectAnalysisEnvironment()
        {
            if (BackdoorAllowed()) return;
            var tool = FindRunningDumpTool();
            if (tool != null)
                throw new InvalidOperationException(
                    "检测到本机正在运行调试 / 内存分析工具（" + tool + "），已拒绝启动。\n\n"
                    + "请关闭该类工具后重试。这是客户端的自我保护。");
        }

        static Timer _guardTimer;
        static int _guardTicks;

        /// <summary>
        /// 启动后**定时复检**（补掉「启动后再附加」这个洞）。
        /// 每 5 秒查一次调试器；每 12 个 tick（≈60 秒）顺带扫一次 dump 工具。
        ///
        /// 🔴 2026-10-09 变更：命中时**只留痕，不再终止进程**（原来是 Kill + FailFast）。
        ///    理由：此刻用户**很可能正在刷机**（EDL 正在写分区），中途杀进程有变砖风险 ——
        ///    「绝不把用户置于变砖风险」是硬红线（与「绝不真实踢人」同源）。
        ///    第一道关没被削弱：启动时的 <see cref="GuardAgainstDebugger"/> 与
        ///    <see cref="RejectAnalysisEnvironment"/> 仍在**做任何事之前**拒绝启动。
        ///    运行期复检的定位从此是"取证"而非"处决"。
        ///
        /// ⚠️ 这里刻意**只用确定性的调试器信号 + 精确的工具名单**，不做任何启发式判断 ——
        ///    误判正常用户（尤其刷机中途）的代价远大于收益，这条是老账，别再踩。
        /// </summary>
        static void ArmPeriodicGuard()
        {
            if (_guardTimer != null) return;
            _guardTimer = new Timer(_ =>
            {
                try
                {
                    if (BackdoorAllowed()) return;
                    int tick = Interlocked.Increment(ref _guardTicks);
                    bool bad = DebuggerPresent() || (tick % 12 == 0 && FindRunningDumpTool() != null);
                    if (bad)
                    {
                        LogGuardHit(DebuggerPresent() ? "运行期检测到调试器" : "运行期检测到调试 / 内存分析工具");
                        // 只记一次就停：否则每 5 秒往日志里刷一行
                        try { _guardTimer?.Dispose(); } catch { }
                    }
                }
                catch { /* 定时器里绝不能抛出去 */ }
            }, null, 5000, 5000);
            GC.KeepAlive(_guardTimer);
        }

        /// <summary>
        /// 运行期守卫命中时留痕。写到 <c>%LOCALAPPDATA%\KunClientState\logs</c> ——
        /// 与 <see cref="Report"/> 同目录（那边是启动失败，这边是运行期命中）。
        /// 🔴 路径必须落在 KunClientState：<c>%LOCALAPPDATA%\KunClient</c> 是 App 的
        /// **退出清理目标**，写那边会在正常退出时被整目录删掉，报修时永远拿不到现场。
        /// 任何异常一律吞掉 —— 这不是主流程。
        /// </summary>
        static void LogGuardHit(string detail)
        {
            try
            {
                var dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "KunClientState", "logs");
                Directory.CreateDirectory(dir);
                File.AppendAllText(Path.Combine(dir, "shell-guard.log"),
                    DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + detail + Environment.NewLine);
            }
            catch { }
        }

        /// <summary>
        /// 后门环境变量名（**运行期拼出**，源码与二进制里都不含明文字面量，
        /// 故 "strings" / grep 都搜不到这个开关名）。
        /// 名字由下方 15 个字符码拼成（语义：KUN + 下划线 + 允许 + 下划线 + DEBUG）。
        /// 改这里就等于换后门名，只需改这一处。
        /// </summary>
        static string BackdoorName()
        {
#if KUNCLIENT_DEV
            var codes = new[]
            {
                75, 85, 78, 95, 65, 76, 76, 79, 87, 95, 68, 69, 66, 85, 71
            };
            var sb = new StringBuilder(codes.Length);
            foreach (var c in codes) sb.Append((char)c);
            return sb.ToString();
#else
            // 发行包里没有后门，自然也没有"后门名"这回事 —— 返回空串，环境变量永不匹配。
            return string.Empty;
#endif
        }

        /// <summary>
        /// 后门是否放行（=1 / =true）。读不到环境变量时按「不放行」走。
        /// <para>
        /// 🔴🔴 **发行构建里这个方法恒为 false**（2026-10-09 逆向报告 D1 之后改）。
        /// 原状：任何人在发行包上设一个环境变量就能关掉全部反调试 / 反 dump 检测 ——
        /// 保护形同虚设，因为开关握在攻击者手里。
        /// 现在只有带 <c>KUNCLIENT_DEV</c> 符号的构建（Debug）才编译进这段判定；
        /// Release 下这段代码根本不在程序里，设什么环境变量都没用。
        /// 需要临时放行请用 Debug 构建，或 <c>-p:DefineConstants=KUNCLIENT_DEV</c>。
        /// </para>
        /// </summary>
        static bool BackdoorAllowed()
        {
#if KUNCLIENT_DEV
            try
            {
                var v = Environment.GetEnvironmentVariable(BackdoorName());
                return v == "1" || string.Equals(v, "true", StringComparison.OrdinalIgnoreCase);
            }
            catch { return false; }
#else
            // 发行包：没有后门。别在这里"为了调试方便"重新打开 —— 上了线的 exe 会被任何人设环境变量绕过。
            return false;
#endif
        }

        /// <summary>统一的按名解析：缓存 → 内嵌密文 → exe 同目录兜底。</summary>
        static Assembly Resolve(AssemblyLoadContext ctx, string simpleName)
        {
            if (string.IsNullOrEmpty(simpleName)) return null;

            if (Loaded.TryGetValue(simpleName, out var cached)) return cached;

            if (PayloadOf.TryGetValue(simpleName, out var res))
            {
                var plain = Decrypt(res);
                if (plain == null) return null;
                try
                {
                    var a = Assembly.Load(plain);
                    Loaded[simpleName] = a;
                    return a;
                }
                finally
                {
                    // 明文字节用完即抹（Assembly.Load 已把内容复制进 CLR 的加载堆，这里清掉的是
                    // **我们自己那份副本**）。旧实现只清了密文、漏了明文 —— 结果进程内存里长期躺着
                    // 一份可 carve 的完整 PE，一次快照就能拿走，等于把壳的收益抵消掉一半。
                    Array.Clear(plain, 0, plain.Length);
                }
            }

            // 非加密依赖的兜底。默认 ALC 只认发布清单（deps.json）里的 TPA 条目，
            // 而内存加载的程序集不在其中 → 它的依赖必须自己探。
            // 单文件包内这些依赖有 deps.json 登记，正常走不到这里；留着是为了「万一」。
            var path = Path.Combine(AppContext.BaseDirectory, simpleName + ".dll");
            if (File.Exists(path))
            {
                var a = ctx.LoadFromAssemblyPath(path);
                Loaded[simpleName] = a;
                return a;
            }
            return null;
        }

        /// <summary>
        /// 内嵌载荷 → 原始程序集字节。三步必须与 tools/obfuscate-build.js 的 payloadBytes() 严格对应：
        ///   JS:  deflateRawSync(明文) → AES-256-CBC(key,iv) 加密
        ///   C#:  AES-256-CBC 解密 → DeflateStream 解压
        /// 顺序不能反（先压缩再加密；反过来压不动随机数据）。两边必须一起改。
        ///
        /// 解压后多做一道**PE 魔数校验**：密钥不对 / 载荷损坏时，这里立刻给出人话报错，
        /// 而不是等到 `Assembly.Load` 抛一个指不出方向的 BadImageFormatException。
        /// （2026-09-30 加的，起因正是换算法时最可能出错的地方就在这一步。）
        /// </summary>
        static byte[] Decrypt(string logicalName)
        {
            EnsureKey();
            using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(logicalName);
            if (s == null) return null;
            var buf = new byte[s.Length];
            int off = 0;
            while (off < buf.Length)
            {
                int n = s.Read(buf, off, buf.Length - off);
                if (n <= 0) break;
                off += n;
            }

            byte[] plain;
            try
            {
                using var aes = Aes.Create();
                aes.Key = _key;
                aes.IV = _iv;
                aes.Mode = CipherMode.CBC;
                aes.Padding = PaddingMode.PKCS7;
                using var dec = aes.CreateDecryptor();
                using var src = new MemoryStream(buf, writable: false);
                using var cs = new CryptoStream(src, dec, CryptoStreamMode.Read);
                using var inflate = new DeflateStream(cs, CompressionMode.Decompress);
                using var dst = new MemoryStream(buf.Length * 2);
                inflate.CopyTo(dst);
                plain = dst.ToArray();
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException(
                    logicalName + " 载荷解密失败（不是有效密文，或密钥/算法与构建端不一致）: " + ex.Message, ex);
            }
            finally
            {
                // 密文缓冲用完**立即抹掉**（2026-09-30 增强）：不抹的话进程内存里会同时躺着
                // 完整密文和完整明文，dump 出来两份对照着看，等于把密钥的验证样本一起送出去。
                // 这一步挡不住有内存快照能力的人（明文最终还是要在进程里），但成本为零、
                // 收益明确：让「一次性 dump」拿不到完整素材。
                Array.Clear(buf, 0, buf.Length);
            }

            // PE 映像一定以 'MZ' 开头 —— 既校验完整性，也校验「密钥是否真的对上了」
            if (plain.Length < 2 || plain[0] != 0x4D || plain[1] != 0x5A)
                throw new InvalidOperationException(
                    logicalName + " 载荷解密后不是合法的 PE 映像（解出 " + plain.Length
                    + " 字节，头两字节不是 'MZ'）→ 密钥不匹配或载荷已损坏");

            // ── 摘要校验（2026-09-30 增强）──
            // 'MZ' 只有两个字节，误判空间太大；换成整份明文的 SHA-256 后才能把
            // 「密钥算错」「载荷传坏/被替换」和「程序本身有 bug」三类故障分开。
            var want = ReadDigest(logicalName);
            if (want == null)
                throw new InvalidOperationException(
                    logicalName + " 缺少摘要资源（*.sha）—— 载荷与摘要必须由同一版 obfuscate-build.js 一起生成");
            if (!SHA256.HashData(plain).AsSpan().SequenceEqual(want))
                throw new InvalidOperationException(
                    logicalName + " 解密结果的 SHA-256 与内嵌摘要不一致 → 载荷已被替换/损坏，或密钥不匹配");

            return plain;
        }

        /// <summary>读内嵌的明文摘要（Xxx.dll.enc → Xxx.dll.sha）。</summary>
        static byte[] ReadDigest(string encName)
        {
            var name = encName.Substring(0, encName.Length - ".enc".Length) + ".sha";
            using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(name);
            if (s == null) return null;
            var buf = new byte[s.Length];
            int off = 0;
            while (off < buf.Length)
            {
                int n = s.Read(buf, off, buf.Length - off);
                if (n <= 0) break;
                off += n;
            }
            return buf;
        }

        /// <summary>
        /// 把 Application.ResourceAssembly 指向内存里的 KunClient。
        /// 公开 setter 必定抛 InvalidOperationException（WPF 已固定该值），只能直写私有静态字段。
        /// </summary>
        static void ForceResourceAssembly(Assembly asm)
        {
            try { Application.ResourceAssembly = asm; return; }
            catch { /* 预期失败，走反射 */ }

            foreach (var f in typeof(Application).GetFields(BindingFlags.Static | BindingFlags.NonPublic))
            {
                if (f.FieldType != typeof(Assembly)) continue;
                try
                {
                    f.SetValue(null, asm);
                    if (ReferenceEquals((Assembly)f.GetValue(null), asm)) return;
                }
                catch { /* 试下一个字段 */ }
            }
            throw new InvalidOperationException("无法设置 Application.ResourceAssembly（WPF 内部结构变了？）");
        }

        /// <summary>
        /// 壳这一层失败必须**可见**。默认什么都不做的话，用户看到的是「双击没反应」，
        /// 而这类故障（载荷缺失、WPF 内部结构变化）只能靠现场信息定位。
        /// </summary>
        static void Report(Exception ex)
        {
            // ⚠️ 日志**必须**落在 %LOCALAPPDATA%\KunClientState\logs —— 不能写 %LOCALAPPDATA%\KunClient。
            // 后者是 App 的**退出清理目标**（正常退出时整目录删除，用户要求关掉工具不留痕迹），
            // 壳层失败日志写在那边，用户只要成功跑过哪怕一次就被清掉，报修时永远拿不到现场
            // （EDL 原生日志踩过同一个坑，见 Core/Configuration/AppStatePaths.cs）。
            // 目录名与 AppStatePaths.DirName 必须一致；这里**不能**引用那个类型 ——
            // KunClient.Core 对本壳是密文载荷，编译期不可见，只能自己拼。
            string logPath = null;
            try
            {
                var dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "KunClientState", "logs");
                Directory.CreateDirectory(dir);
                logPath = Path.Combine(dir, "shell-error.log");
                File.AppendAllText(logPath,
                    DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + ex + Environment.NewLine);
            }
            catch { /* 连日志都写不了就不再纠结 */ }

            try
            {
                MessageBox.Show(
                    "程序启动失败。\n\n" + ex.GetType().Name + ": " + ex.Message
                        + (logPath != null
                            ? "\n\n诊断日志：\n" + logPath
                            : "\n\n（诊断日志也写入失败）"),
                    "KunClient", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            catch { }
        }
    }
}
