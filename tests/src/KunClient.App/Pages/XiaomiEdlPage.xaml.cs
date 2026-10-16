using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using KunFlashTool.App.Dialogs;
using KunFlashTool.App.Services;
using KunFlashTool.Core.Configuration;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.Core.Services;
using CoreDevices = KunFlashTool.Core.Modules.Common;

namespace KunFlashTool.App.Pages
{
    /// <summary>
    /// 小米 EDL 通道 —— 流程照「FDL 计算端」：
    ///
    ///   fastboot getvar unlocked
    ///     ├─ yes → 直接 fastboot oem edl
    ///     └─ no  → fastboot getvar token 取 blob
    ///              → FDL Auth 窗（blob 只读 + 复制 / sig 空框 + 粘贴 → 写入）
    ///              → hex 清洗转二进制，落临时 bin（**只含 sig**）
    ///              → fastboot stage &lt;文件&gt;
    ///              → 删临时文件
    ///              → fastboot oem edl
    ///   最后检测 9008 端口有没有出现：出现 = 绿勾成功，没出现 = 红叉失败。
    ///
    /// 🔴 本工具**不做** blob → sig 的计算：9008 的鉴权是设备发 token、主机用**厂商私钥**签名，
    ///    sig 只能由用户在别处拿到后粘贴进来。这里只负责把 sig 落成 bin、stage、进 EDL。
    ///
    /// 🔴 **文件里只有 sig，不拼 blob**（2026-09-29 实机确认）：设备自己持有刚发出的 token，
    ///    多垫 36 字节会让它验签失败（`Edl Token Verify Failed`）。blob 只用来换 sig。
    ///
    /// 界面上**没有** blob 框 / 打包顺序 / 命令输入框（2026-09-29 按要求删掉两张卡）：
    /// 这些值仍从 <c>settings.json</c> 读（见 <see cref="LoadCommands"/>），老配置继续生效。
    /// sig 输入集中在 <see cref="FdlAuthDialog"/>，与页面共用 <see cref="SignCodec"/> 一套解析口径。
    /// </summary>
    public partial class XiaomiEdlPage : BasePage
    {
        /// <summary>
        /// 工作目录放在 **KunClientState** 而不是 KunClient：
        /// %LOCALAPPDATA%\KunClient 整个目录会在关闭工具时被删掉，日志会跟着没。
        /// </summary>
        private static string WorkDir => Path.Combine(AppStatePaths.Root, "edl");
        private static string BlobRawPath => Path.Combine(WorkDir, "blob.raw");

        /// <summary>
        /// 交给 `fastboot stage` 的签名文件（**原始 bin：只有 sig**，没有头、没有长度前缀、
        /// **不含 blob** —— 和 FDL 计算端丢给 `stage` 的 `Sign.data` 是同一种东西）。
        /// **用完就删**，所以放 %TEMP%\KunClient 而不是工作目录（%TEMP%\KunClient 在退出客户端时
        /// 本来就会被整体清掉，算双保险）。文件名刻意用 ASCII —— 中文路径喂给 fastboot 有被
        /// ANSI 解码搞坏的历史。
        /// </summary>
        private static string TempSignPath =>
            Path.Combine(Path.GetTempPath(), "KunClient", "sign.bin");

        /// <summary>等下发的 oem edl 命令返回的上限。设备进 EDL 会立刻断 USB，超时很正常。</summary>
        private const int EdlCmdTimeoutMs = 15000;

        /// <summary>等 9008 端口出现的最长时间。设备重枚举 + 驱动加载一般 5~20 秒。</summary>
        private const int EdlWaitMs = 30000;

        private PlatformToolsManager? _tools;
        private string _toolsDir = "";
        private bool _toolsReady;
        private bool _busy;

        // ── 界面已删除，但值仍然从 settings.json 读的三个配置 ──
        private string _readCmd = "getvar token";
        private string _writeCmd = "stage";
        private string _edlCmd = "oem edl";

        /// <summary>最近一次读到的 blob（= 设备 token），一键流程与 FDL Auth 窗共用。</summary>
        private string _blob = "";

        /// <summary>
        /// fastboot 逐行回包 → 日志框的批量投递。
        /// 原来是**每行一次同步 Dispatcher.Invoke**：读线程阻塞等 UI 线程，
        /// fastboot 连续吐回包（oem edl / getvar 一次回很多行）时界面越忙读得越慢。
        /// </summary>
        private readonly LogLineQueue<string> _fastbootOut;

        public XiaomiEdlPage()
        {
            InitializeComponent();

            _fastbootOut = new LogLineQueue<string>(Dispatcher, lines =>
            {
                foreach (var line in lines) LogInfo(line);
            });

            _tools = new PlatformToolsManager(msg => LogInfo("· " + msg));
            _toolsDir = Path.GetDirectoryName(_tools.GetAdbPath()) ?? "";
            LoadCommands();

            Loaded += async (_, __) =>
            {
                // 页内左栏「设备信息」的任务/进度/状态三格 ← ShellStatus（本页探测的
                // Fastboot / 机型·BL 两格由 RefreshDeviceAsync 直接写，不重复走广播）。
                // 静态事件必须在 Unloaded 摘除：页面销毁后不摘会一直被回调（照 DeviceLogRail 的写法）。
                ShellStatus.Changed += SyncRailStatus;
                SyncRailStatus();

                await EnsureToolsAsync(silent: true);
                await RefreshDeviceAsync();
            };
            Unloaded += (_, _) =>
            {
                ShellStatus.Changed -= SyncRailStatus;
            };
        }

        /// <summary>ShellStatus → 左栏「设备信息」的 任务 / 进度 / 状态 三格（与外壳左栏同源）。</summary>
        private void SyncRailStatus()
        {
            try
            {
                TxtRailTask.Text = ShellStatus.Task;
                TxtRailPercent.Text = $"{ShellStatus.Percent:F0}%";
                TxtRailHint.Text = ShellStatus.Hint;
            }
            catch
            {
                // 读数刷新失败不影响任何功能路径：控件卸载后回调仍可能到（事件晚一步），忽略
            }
        }

        // ───────────────────────── 配置 ─────────────────────────

        /// <summary>
        /// 读 blob / 上传 / 进 EDL 三条命令 + 打包顺序。**这些已经没有界面了**（用户要求删掉
        /// 「签名数据」「命令」两张卡），但设置里存着的值继续生效 —— 谁以前在界面上把读 blob
        /// 改成了 `oem read_blob` 之类，那份 settings.json 不该因为删了输入框就失效。
        /// 以后要改只能手改 <c>%LOCALAPPDATA%\KunClientState\settings.json</c>（或删掉该键回落默认）。
        /// </summary>
        private void LoadCommands()
        {
            var s = AppSettings.Current;
            if (!string.IsNullOrWhiteSpace(s.XiaomiEdlReadCommand)) _readCmd = s.XiaomiEdlReadCommand.Trim();
            if (!string.IsNullOrWhiteSpace(s.XiaomiEdlWriteCommand)) _writeCmd = s.XiaomiEdlWriteCommand.Trim();
            if (!string.IsNullOrWhiteSpace(s.XiaomiEdlEdlCommand)) _edlCmd = s.XiaomiEdlEdlCommand.Trim();

            // ⚠️ AppSettings.XiaomiEdlPackOrder 仍在（不删配置），但**已经不起作用**：
            //    2026-09-29 实机确认「文件里只有 sig」，没有 blob 参与打包，自然也没有顺序可言。
            //    留着这个键只是为了让老 settings.json 少一个「找不到的字段」。

            LogInfo($"流程：{_readCmd} → （FDL Auth 填 sig）→ {_writeCmd} → {_edlCmd}；"
                    + "stage 的文件 = 仅 sig（不含 blob）");
        }

        // ───────────────────────── 工具 / 设备 ─────────────────────────

        private async Task EnsureToolsAsync(bool silent)
        {
            if (_tools == null) return;

            if (_tools.CheckToolsExist())
            {
                _toolsReady = true;
                if (!silent) ShowSuccess("Fastboot 已就绪");
                return;
            }

            LogInfo("正在准备 fastboot…");
            _toolsReady = await _tools.EnsureToolsExistAsync();
            if (_toolsReady)
            {
                LogSuccess("fastboot 就绪");
                if (!silent) ShowSuccess("Fastboot 就绪");
            }
            else
            {
                LogError("fastboot 获取失败，请检查网络后点「刷新设备」重试");
                if (!silent) ShowError("fastboot 获取失败，请检查网络");
            }
        }

        private async void BtnRefresh_Click(object sender, RoutedEventArgs e)
        {
            // 2026-10-02 修复：一键流程进行中不允许并发刷新 —— 刷新会对同一台设备另起
            // fastboot 探测，可能把 token/stage 握手序列搅乱。
            if (_busy) { LogWarning("一键流程进行中，暂不能刷新设备"); return; }
            await EnsureToolsAsync(silent: true);
            await RefreshDeviceAsync();
        }

        private async Task RefreshDeviceAsync()
        {
            if (!_toolsReady) await EnsureToolsAsync(silent: true);

            var devices = await RunFastbootAsync("devices");
            var serial = FirstFastbootSerial(devices.Out);

            if (serial.Length == 0)
            {
                TxtDevice.Text = "未检测到 Fastboot 设备";
                TxtProduct.Text = "-";
                LogWarning("没有检测到 Fastboot 设备，请把手机进 Fastboot 模式后重试");
                LogInfo("`fastboot devices` 原始回包：" + Visible(devices.Out)
                        + "（\\t=Tab \\r=回车；若这里已经有设备行却仍报未检测到，就是解析口径的问题）");
                // 探测结论广播给外壳（左栏「设备信息」卡 + 状态栏设备段都照它显示）
                DeviceStateHub.Report(null, "未连接");
                return;
            }

            TxtDevice.Text = serial + "  (fastboot)";
            var product = await RunFastbootAsync("getvar product");
            var unlocked = await RunFastbootAsync("getvar unlocked");
            var name = GrepVar(product.Out, "product");
            var lockState = GrepVar(unlocked.Out, "unlocked");
            var lockText = lockState.Equals("yes", StringComparison.OrdinalIgnoreCase) ? "BL 已解锁"
                : lockState.Equals("no", StringComparison.OrdinalIgnoreCase) ? "BL 已锁定"
                : "未知";
            TxtProduct.Text = $"{name} · {lockText}";
            LogInfo($"Fastboot 设备：{serial} · {name} · {lockText}");
            DeviceStateHub.Report(serial, "Fastboot 模式",
                new DeviceStateHub.Row("机型", name),
                new DeviceStateHub.Row("Bootloader", lockText));
        }

        /// <summary>
        /// 从 `fastboot devices` 的回包里取第一台设备的序列号。
        /// 解析口径统一收在 <see cref="FastbootOutput.FirstFastbootSerial"/>（Core，有单测护栏）——
        /// 这里只做转发，别再在本页写第四份解析。
        /// </summary>
        private static string FirstFastbootSerial(string? output)
            => FastbootOutput.FirstFastbootSerial(output);

        /// <summary>回包转可见转义，供日志排查「到底是分隔符不对还是真没设备」。</summary>
        private static string Visible(string? text) => FastbootOutput.Escaped(text);

        /// <summary>fastboot 的 getvar 回包形如 "(bootloader) token: xxxx"，取冒号后面的值。</summary>
        private static string GrepVar(string? output, string key)
        {
            foreach (var raw in (output ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var line = raw.Trim();
                var idx = line.IndexOf(key + ":", StringComparison.Ordinal);
                if (idx < 0) continue;
                return line.Substring(idx + key.Length + 1).Trim();
            }
            return "";
        }

        /// <summary>
        /// 从读 blob 的回包里抠出 blob 本体。
        ///
        /// 为什么不能只认 `token:` 一个键：不同机型/命令回的形态不一样 ——
        /// `getvar token` 回 `(bootloader) token: &lt;hex&gt;`，而 `oem read_blob` 有的回 `blob: …`、
        /// 有的干脆只吐一行十六进制。所以按「token → blob → 输出里最长的一段 hex → 原文」依次退让。
        /// </summary>
        private static string ExtractBlob(string? output)
        {
            var text = output ?? "";

            var v = GrepVar(text, "token");
            if (v.Length == 0) v = GrepVar(text, "blob");
            if (v.Length > 0) return v;

            var best = "";
            foreach (var raw in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var line = raw.Trim();
                if (line.StartsWith("<", StringComparison.Ordinal)) continue;

                var colon = line.LastIndexOf(':');
                var hex = SignCodec.CleanHex(colon >= 0 ? line.Substring(colon + 1) : line);
                if (hex.Length >= 32 && hex.Length > best.Length) best = hex;
            }
            return best.Length >= 32 ? best : text.Trim();
        }

        // ───────────────────────── 读 token（= FDL 的 blob）─────────────────────────

        /// <summary>读设备 token（blob）。读不到返回 false。</summary>
        private async Task<bool> ReadTokenCoreAsync()
        {
            Directory.CreateDirectory(WorkDir);
            var r = await RunFastbootAsync(_readCmd);
            File.WriteAllText(BlobRawPath, r.Out ?? "", new UTF8Encoding(false));

            var blob = ExtractBlob(r.Out);
            if (blob.Length == 0)
            {
                LogError($"没有读到 blob（`{_readCmd}` 没有回包）：设备可能不在 Fastboot 模式，或该机型不支持这条命令");
                ShowError("没有读到 blob");
                return false;
            }

            _blob = blob;
            var bytes = SignCodec.TryParseBytes(blob);
            LogSuccess($"blob 已读回：{blob.Length} 字符"
                       + (bytes != null ? $"（{bytes.Length} 字节）" : "（不是 hex，打包时按原文处理）"));
            return true;
        }

        // ───────────────────────── FDL Auth 窗口 ─────────────────────────

        /// <summary>单独打开一次 FDL Auth（不跑全流程时手动用）。</summary>
        private void BtnAuth_Click(object sender, RoutedEventArgs e)
        {
            if (_blob.Length == 0)
                LogWarning("还没读过 blob —— 先点「一键进入 EDL」会连读代填；这里空着也能只填 sig");

            if (!PromptSig(_blob, out var sigText)) return;

            var bytes = SignCodec.TryParseBytes(sigText);
            LogSuccess($"已收下 sig：{sigText.Length} 字符"
                       + (bytes != null ? $"（{bytes.Length} 字节）" : "（格式未识别）"));
        }

        /// <summary>
        /// 弹出 FDL Auth 窗让用户粘贴 sig。点「写入」返回 true 并给出 sig 原文；
        /// 取消 / 没填返回 false。<b>本窗不计算签名</b> —— sig 是用户从别处拿到的，这里只收下它。
        /// sig 框每次都是空的（照 FDL 计算端的样子），不在页面留副本来回填。
        /// </summary>
        private bool PromptSig(string blob, out string sigText)
        {
            sigText = "";

            var dlg = new FdlAuthDialog(blob);
            var owner = Window.GetWindow(this);
            if (owner != null) dlg.Owner = owner;

            if (dlg.ShowDialog() != true) return false;
            sigText = dlg.SigText ?? "";
            return true;
        }

        // ───────────────────────── 一键流程 ─────────────────────────

        private async void BtnRun_Click(object sender, RoutedEventArgs e)
        {
            await WithBusy(async () =>
            {
                HideResult();
                _blob = "";
                // 日志三明治的「头」：Section 画区段标题，同时把外壳状态栏「任务」段设成同一个名字。
                // 「尾」统一在 WithBusy 的 finally 里补（成功/失败/中途 return 都不会漏）。
                PageLog.Section(LogTarget, "一键进入 EDL");
                LogInfo("===== 开始：一键进入 EDL =====");

                if (!_toolsReady) await EnsureToolsAsync(silent: true);

                // ① Fastboot 端口
                SetStep("检查 Fastboot 端口…");
                var dev = await RunFastbootAsync("devices");
                var serial = FirstFastbootSerial(dev.Out);
                if (serial.Length == 0)
                {
                    LogError("未检测到 Fastboot 设备：请确认手机已进入 Fastboot 模式、驱动正常");
                    LogInfo("`fastboot devices` 原始回包：" + Visible(dev.Out));
                    ShowError("没有检测到 Fastboot 设备");
                    SetStep("失败：没有 Fastboot 设备");
                    ShowResult(false, "失败：没有检测到 Fastboot 设备",
                        "请让手机进入 Fastboot 模式并连上电脑（设备管理器里应出现 Android Bootloader Interface）。");
                    return;
                }
                TxtDevice.Text = serial + "  (fastboot)";
                LogSuccess("Fastboot 端口正常：" + serial);

                // ② BL 锁状态 —— yes 就直接进 EDL，整段签名流程全省掉
                SetStep("读取 BL 解锁状态…");
                var un = await RunFastbootAsync("getvar unlocked");
                var lockState = GrepVar(un.Out, "unlocked");
                LogInfo("unlocked = " + (lockState.Length == 0 ? "（没有回包）" : lockState));

                if (lockState.Equals("yes", StringComparison.OrdinalIgnoreCase))
                {
                    LogSuccess("BL 已解锁 —— 不需要签名，直接进 EDL");
                    await SwitchToEdlAsync();
                    LogInfo("===== 流程结束 =====");
                    return;
                }

                if (lockState.Length == 0)
                    LogWarning("没读到 unlocked 状态，按「未解锁」继续走读 blob + 填 sig");

                // ③ 读 blob
                SetStep("读取设备 blob…");
                if (!await ReadTokenCoreAsync())
                {
                    SetStep("失败：没有读到 blob");
                    ShowResult(false, "失败：没有读到 blob",
                        "设备没有回 blob。确认手机在 Fastboot 模式；若你的机型不认这条命令，"
                        + "改 %LOCALAPPDATA%\\KunClientState\\settings.json 里的 XiaomiEdlReadCommand。");
                    return;
                }

                // ④ FDL Auth 窗：blob 只读 + 复制；sig 空框 + 粘贴；点「写入」才继续
                SetStep("等待填写 sig…");
                LogInfo("打开「FDL Auth」窗口，等待粘贴 sig…");
                if (!PromptSig(_blob, out var sigText))
                {
                    LogWarning("已取消「FDL Auth」——没有 sig 就没法进 EDL");
                    SetStep("已取消");
                    ShowError("已取消：没有填写 sig");
                    return;
                }

                var sigBytes = SignCodec.TryParseBytes(sigText);
                if (sigBytes == null || sigBytes.Length == 0)
                {
                    SetStep("失败：sig 格式不认识");
                    ShowResult(false, "失败：sig 格式不认识",
                        "sig 支持十六进制（可带空格 / 冒号 / 0x 前缀）与 Base64 两种写法。");
                    return;
                }

                // ⑤ 落临时 bin：**文件里只有 sig**
                //    🔴 不要把 blob 拼进去。2026-09-29 实机：`blob + sig`（292 字节）→ 设备回
                //       `FAILED (remote: 'Edl Token Verify Failed')`。设备自己就持有它刚发出去的 token，
                //       它要的只是**这枚 token 的签名**；多垫 36 字节等于把所有字节错位，必然验签失败。
                //       blob 的作用只剩「拿给签名端换 sig」，不进文件。
                SetStep("打包签名文件…");
                var payload = XiaomiEdlAuthFile.Build(sigBytes);

                var tmp = TempSignPath;
                Directory.CreateDirectory(Path.GetDirectoryName(tmp)!);
                File.WriteAllBytes(tmp, payload);

                LogSuccess($"已打包：{Path.GetFileName(tmp)}（{payload.Length} 字节 = 仅 sig，"
                           + "不含 blob、无附加头）");
                LogInfo("前 32 字节：" + SignCodec.ToHex(payload, 32));
                LogInfo("路径：" + tmp + "（stage 后立即删除）");
                if (XiaomiEdlAuthFile.SizeWarning(sigBytes.Length) is { } sizeWarn)
                    LogWarning(sizeWarn);

                try
                {
                    // ⑥ stage：把签名文件送进设备暂存区（不写任何分区）
                    SetStep("上传签名到设备暂存区…");
                    var wr = await RunFastbootAsync($"{_writeCmd} {Quote(tmp)}");
                    if (wr.Code != 0)
                    {
                        LogError($"上传失败（退出码 {wr.Code}）：{FirstMeaningfulLine(wr.Out)}");
                        SetStep("失败：签名上传失败");
                        ShowResult(false, "失败：签名上传失败",
                            "设备没有接受这份签名文件 —— 确认 sig 与这台设备的 blob 是配套的。");
                        return;
                    }
                    LogSuccess("签名已送进设备暂存区");
                }
                finally
                {
                    // ⑦ 无论成败，临时文件立刻删掉
                    TryDeleteTemp(tmp);
                }

                // ⑧ oem edl → ⑨ 检测 9008
                await SwitchToEdlAsync();
                LogInfo("===== 流程结束 =====");
            });
        }

        /// <summary>
        /// 下发进 EDL 的命令，然后**以 9008 端口有没有出现**作为唯一判据。
        ///
        /// 为什么不看 fastboot 的退出码：`oem edl` 一旦成功，设备当场从 USB 上摘掉重枚举，
        /// fastboot 常常来不及回 OKAY（甚至进程被杀），退出码反而是非 0。
        /// 拿退出码当判据会把成功判成失败 —— 用户要的判据本来就是「有没有 9008」。
        /// </summary>
        private async Task SwitchToEdlAsync()
        {
            SetStep("下发 " + _edlCmd + " …");
            LogInfo("切换到高通 9008…");

            var er = await RunFastbootAsync(_edlCmd, EdlCmdTimeoutMs);
            LogInfo($"`{_edlCmd}` 已下发（退出码 {er.Code}）—— 后面以 9008 端口是否出现为准");
            var first = FirstMeaningfulLine(er.Out);
            if (first != "（无输出）") LogInfo("设备回包：" + first);

            SetStep("等待设备进入 9008…");
            LogInfo($"等待 9008 端口出现（最多 {EdlWaitMs / 1000} 秒）…");
            var device = await WaitFor9008Async();

            if (device != null)
            {
                TxtDevice.Text = $"{device.PortName}  (9008)";
                LogSuccess($"检测到 9008：{device.PortName} · {device.Description}");
                SetStep("完成：设备已进入 EDL");
                ShowSuccess("已进入 EDL（9008）");
                ShowResult(true, "成功：设备已进入 EDL",
                    $"已检测到 9008 端口 {device.PortName} · {device.Description}");
                // 进了 EDL 就是「设备模式」变了：广播给外壳（与 EdlFlashPage 同一口径）
                DeviceStateHub.Report(device.PortName, "EDL 模式 (9008)");
                return;
            }

            // 端口没等到，再按 PnP 的 DeviceID 兜一次：驱动没装时设备也会出现，
            // 只是没有 COM 端口（DeviceWatcher 只收有端口的条目，会漏掉它）。
            var driverless = await Task.Run(Find9008WithoutPort);
            if (driverless != null)
            {
                TxtDevice.Text = $"{driverless}  (9008)";
                LogSuccess($"检测到 9008 设备：{driverless}（没有 COM 端口，多半是驱动没装）");
                SetStep("完成：设备已进入 EDL（驱动缺失）");
                ShowSuccess("已进入 EDL（9008）");
                ShowResult(true, "成功：设备已进入 EDL",
                    $"检测到 9008 设备「{driverless}」，但没拿到 COM 端口 —— 多半是 9008 驱动没装，"
                    + "设备管理器里会带感叹号，装上驱动再刷写。");
                DeviceStateHub.Report(driverless, "EDL 模式 (9008)");
                return;
            }

            LogError($"等了 {EdlWaitMs / 1000} 秒，没有出现 9008 端口");
            SetStep("失败：没有检测到 9008");
            ShowError("失败：没有检测到 9008 端口");
            ShowResult(false, "失败：没有检测到 9008 端口",
                "设备没进 EDL。常见原因：sig 与这台设备的 blob 不配套、该机型封锁了 oem edl、"
                + "或 9008 驱动没装（设备管理器里会带感叹号）。");
        }

        /// <summary>
        /// 轮询等 9008 端口（Qualcomm HS-USB QDLoader 9008）。
        ///
        /// 复用 Core 的 DeviceWatcher（WMI 查询，跨厂商 VID/PID 表已内置），
        /// 但必须丢到后台线程：它内部是同步 WMI 查询，直接在 UI 线程上跑会把界面冻住几十秒。
        /// </summary>
        private async Task<CoreDevices.DeviceInfo?> WaitFor9008Async()
        {
            try
            {
                return await Task.Run(async () =>
                {
                    using var watcher = new CoreDevices.DeviceWatcher();
                    return await watcher.WaitForDeviceAsync(CoreDevices.DeviceType.Qualcomm9008, EdlWaitMs);
                });
            }
            catch (Exception ex)
            {
                LogWarning("检测 9008 出错：" + ex.Message);
                return null;
            }
        }

        /// <summary>
        /// 兜底：直接问 WMI 有没有 `PID_9008` 的 PnP 设备节点。
        ///
        /// 为什么需要它：<c>DeviceWatcher.GetAllDevices()</c> 只保留**有 COM 端口名**的条目，
        /// 而 9008 在**驱动没装**时是个没有端口的未知设备 —— 那一刻设备其实已经进了 EDL，
        /// 却会被判成「没出现」。用户要的判据是「9008 有没有出现」，不是「端口能不能打开」。
        /// </summary>
        private static string? Find9008WithoutPort()
        {
            try
            {
                // `_` 在 SQL LIKE 里是单字符通配符，用 [_] 转义，免得匹配到别的 PID_90xx
                const string query =
                    "SELECT DeviceID, Caption, Name FROM Win32_PnPEntity " +
                    "WHERE DeviceID LIKE '%PID[_]9008%'";
                using var searcher = new System.Management.ManagementObjectSearcher(query);
                searcher.Options.Timeout = TimeSpan.FromSeconds(3);

                foreach (var obj in searcher.Get())
                {
                    var caption = obj["Caption"]?.ToString() ?? "";
                    if (caption.Length > 0) return caption;
                    var name = obj["Name"]?.ToString() ?? "";
                    if (name.Length > 0) return name;
                    var id = obj["DeviceID"]?.ToString() ?? "";
                    if (id.Length > 0) return id;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine("9008 兜底检测失败: " + ex.Message);
            }
            return null;
        }

        // ───────────────────────── 打包 / 临时文件 ─────────────────────────

        // 🔴 原来这里有个 PackBytes(blob, sig, sigFirst) 做「blob + sig」拼接 —— **已删除，别加回来**。
        //    2026-09-29 实机：拼出来的 292 字节被设备打回 `Edl Token Verify Failed`。
        //    设备自己持有刚发出的 token，要的只是那枚 token 的签名 → 文件里**只有 sig**。
        //    构造点在 Core 的 XiaomiEdlAuthFile.Build（有单测护栏）。

        private static string Quote(string path) => "\"" + path + "\"";

        private void TryDeleteTemp(string path)
        {
            try
            {
                if (File.Exists(path))
                {
                    File.Delete(path);
                    LogInfo("临时签名文件已删除");
                }

                // 目录空了就顺手带走，别在 %TEMP% 里留个空壳
                var dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)
                    && Directory.GetFileSystemEntries(dir).Length == 0)
                {
                    Directory.Delete(dir);
                }
            }
            catch (Exception ex)
            {
                LogWarning("删除临时文件失败：" + ex.Message);
            }
        }

        private static string FirstMeaningfulLine(string? output)
        {
            foreach (var raw in (output ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var line = raw.Trim();
                if (line.Length > 0 && !line.StartsWith("$", StringComparison.Ordinal)) return line;
            }
            return "（无输出）";
        }

        // ───────────────────────── 结果 / 步骤 ─────────────────────────

        /// <summary>出现 9008 → 绿勾；没出现 → 红叉。</summary>
        private void ShowResult(bool ok, string title, string sub)
        {
            if (ResultBox == null || TxtResultIcon == null) return;

            var accent = ThemeBrush(ok ? "Theme.OkGreen" : "Theme.Danger");
            var back = ThemeBrush(ok ? "Theme.OkGreenBg" : "Theme.DangerBg");

            TxtResultIcon.Text = ok ? "\u2714" : "\u2718";   // ✔ / ✘
            TxtResultIcon.Foreground = accent;

            if (TxtResult != null)
            {
                TxtResult.Text = title;
                TxtResult.Foreground = accent;
            }
            if (TxtResultSub != null) TxtResultSub.Text = sub;

            ResultBox.Background = back;
            ResultBox.BorderBrush = accent;
            ResultBox.Visibility = Visibility.Visible;
        }

        private void HideResult()
        {
            if (ResultBox != null) ResultBox.Visibility = Visibility.Collapsed;
        }

        private static Brush ThemeBrush(string key)
            => Application.Current?.TryFindResource(key) as Brush ?? Brushes.Gray;

        private void SetStep(string text)
        {
            if (TxtStep != null) TxtStep.Text = text;
        }

        // ───────────────────────── 执行 ─────────────────────────

        private async Task<(int Code, string Out)> RunFastbootAsync(string arguments, int timeoutMs = 60000)
        {
            LogInfo("$ fastboot " + arguments);

            if (_tools == null) return (-1, "工具未初始化");
            var exe = _tools.GetFastbootPath();
            if (!File.Exists(exe))
            {
                LogError("找不到 fastboot.exe");
                return (-1, "找不到 fastboot.exe");
            }

            var psi = new ProcessStartInfo(exe)
            {
                Arguments = arguments,
                WorkingDirectory = Directory.Exists(WorkDir) ? WorkDir : _toolsDir,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };

            var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
            var buffer = new StringBuilder();
            void Sink(string? data)
            {
                if (data == null) return;
                buffer.AppendLine(data);
                _fastbootOut.Enqueue(data);
            }
            process.OutputDataReceived += (_, a) => Sink(a.Data);
            // fastboot 的正常回包（含 OKAY）大多走 stderr，不能当错误染色
            process.ErrorDataReceived += (_, a) => Sink(a.Data);

            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                LogError("启动 fastboot 失败：" + ex.Message);
                return (-1, ex.Message);
            }

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            try
            {
                var wait = process.WaitForExitAsync();
                if (await Task.WhenAny(wait, Task.Delay(timeoutMs)) != wait)
                {
                    // 设备进 EDL 后 USB 会立刻断开，fastboot 可能永远等不到回包
                    LogWarning($"fastboot 超过 {timeoutMs / 1000} 秒没有返回，强制结束");
                    try { process.Kill(entireProcessTree: true); } catch { }
                    try { await process.WaitForExitAsync(); } catch { }
                }
            }
            catch (Exception ex)
            {
                LogError("等待 fastboot 结束出错：" + ex.Message);
            }

            // 回包行先落地，后面页脚 / 步骤读数才不会插到它们前面
            _fastbootOut.FlushNow();

            var code = -1;
            try { code = process.ExitCode; } catch { }
            process.Dispose();
            return (code, buffer.ToString());
        }

        private async Task WithBusy(Func<Task> body)
        {
            if (_busy)
            {
                ShowError("上一次操作还没结束");
                return;
            }

            _busy = true;
            BtnRun.IsEnabled = false;
            BtnRefresh.IsEnabled = false;
            BtnAuth.IsEnabled = false;
            // 一键流程的耗时与收尾统计（Section/Footer 三明治的「尾」）。
            // 成败判断复用页面上已有的步骤读数：只有走到 9008 的那两条路径才会把 TxtStep 置成「完成：…」。
            var sw = Stopwatch.StartNew();
            try
            {
                await body();
            }
            catch (Exception ex)
            {
                LogError("执行出错：" + ex.Message);
                ShowError("执行出错：" + ex.Message);
            }
            finally
            {
                _busy = false;
                BtnRun.IsEnabled = true;
                BtnRefresh.IsEnabled = true;
                BtnAuth.IsEnabled = true;

                var ok = TxtStep != null && TxtStep.Text.StartsWith("完成", StringComparison.Ordinal);
                if (ok) ShellStatus.EndTask();
                else ShellStatus.Report(task: "未进入 EDL");
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        // ───────────────────────── 杂项 ─────────────────────────

        private void BtnOpenDir_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                Directory.CreateDirectory(WorkDir);
                Process.Start(new ProcessStartInfo("explorer.exe")
                {
                    Arguments = WorkDir,
                    UseShellExecute = true
                });
            }
            catch (Exception ex)
            {
                ShowError("打开目录失败：" + ex.Message);
            }
        }

        private void BtnClearLog_Click(object sender, RoutedEventArgs e) => PageLog.Clear(LogTarget);
    }
}
