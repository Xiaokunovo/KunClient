using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using KunFlashTool.App.Services;
using KunFlashTool.Core.PlatformTools;

namespace KunFlashTool.App.Pages
{
    /// <summary>
    /// 命令窗口：把用户敲的命令交给 **Windows 自带的 cmd.exe** 执行。
    /// 关键点只有一个 —— 进程的 PATH 前面拼上本工具自带的 platform-tools 目录，
    /// 这样 adb / fastboot 不需要用户自己装、也不需要全局配置环境变量。
    /// </summary>
    public partial class CommandConsolePage : BasePage
    {
        /// <summary>控制台保留的最大行数，防止 adb logcat 之类长时间输出把内存堆爆。</summary>
        private const int MaxConsoleLines = 3000;

        private PlatformToolsManager? _tools;
        private string _toolsDir = "";
        private bool _toolsReady;

        private readonly List<string> _history = new();
        private int _historyIndex = -1;

        private Process? _running;

        /// <summary>
        /// 进程输出行 → 输出区的批量投递。
        /// 原来是**每行一次同步 Dispatcher.Invoke + 每行一次 blocks.Add/ScrollToEnd**：
        /// 读线程阻塞等 UI，UI 线程一行一次调度、一行重排一次整篇文档 —— `dir /s`、
        /// `adb logcat` 这类几百上千行的命令直接掉帧。现在读线程只入队，UI 侧一次回调渲染一批。
        /// </summary>
        private readonly LogLineQueue<(string Text, string Color)> _outQueue;

        public CommandConsolePage()
        {
            InitializeComponent();
            _outQueue = new LogLineQueue<(string Text, string Color)>(Dispatcher, FlushOutputLines);
            _tools = new PlatformToolsManager(AppendToolLog);
            _toolsDir = Path.GetDirectoryName(_tools.GetAdbPath()) ?? "";
            Loaded += async (_, __) => await EnsureToolsAsync(silent: true);
        }

        /// <summary>一批输出行落地：逐行进文档，**整批只 ScrollToEnd 一次**。</summary>
        private void FlushOutputLines(List<(string Text, string Color)> lines)
        {
            foreach (var (text, color) in lines)
                AppendLine(text, color, scroll: false);
            if (lines.Count > 0)
                TxtConsole.ScrollToEnd();
        }

        // ───────────────────────── 工具就绪 ─────────────────────────

        private async Task EnsureToolsAsync(bool silent)
        {
            if (_tools == null) return;

            if (_tools.CheckToolsExist())
            {
                _toolsReady = true;
                SetHint($"已就绪 · 工具目录：{_toolsDir}");
                if (!silent) ShowSuccess("ADB / Fastboot 已就绪");
                return;
            }

            SetHint("正在从服务器获取 ADB / Fastboot…");
            var ok = await _tools.EnsureToolsExistAsync();
            _toolsReady = ok;
            if (ok)
            {
                SetHint($"已就绪 · 工具目录：{_toolsDir}");
                if (!silent) ShowSuccess("ADB / Fastboot 就绪");
            }
            else
            {
                SetHint("ADB / Fastboot 不可用：请检查网络后点「检测工具」重试");
                if (!silent) ShowError("ADB / Fastboot 获取失败，请检查网络后重试");
            }
        }

        private void BtnCheckTools_Click(object sender, RoutedEventArgs e) => _ = EnsureToolsAsync(silent: false);

        // ───────────────────────── 执行 ─────────────────────────

        private void BtnRun_Click(object sender, RoutedEventArgs e) => _ = RunAsync(TxtCommand.Text);

        private void TxtCommand_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter)
            {
                e.Handled = true;
                _ = RunAsync(TxtCommand.Text);
                return;
            }

            if (e.Key == Key.Up)
            {
                e.Handled = true;
                NavigateHistory(-1);
            }
            else if (e.Key == Key.Down)
            {
                e.Handled = true;
                NavigateHistory(1);
            }
        }

        private void NavigateHistory(int delta)
        {
            if (_history.Count == 0) return;
            _historyIndex = Math.Clamp(_historyIndex + delta, 0, _history.Count - 1);
            TxtCommand.Text = _history[_historyIndex];
            TxtCommand.CaretIndex = TxtCommand.Text.Length;
        }

        private async Task RunAsync(string? rawInput)
        {
            var input = (rawInput ?? "").Trim();
            if (string.IsNullOrEmpty(input)) return;

            if (_running != null)
            {
                ShowError("上一条命令还在执行，请先「停止」");
                return;
            }

            // 历史：连续重复的命令不重复记
            if (_history.Count == 0 || _history[^1] != input) _history.Add(input);
            _historyIndex = _history.Count;

            var commandLine = BuildCommandLine(input);
            AppendLine("$ " + commandLine, "Theme.Accent");
            TxtCommand.Clear();

            if (!_toolsReady) await EnsureToolsAsync(silent: true);

            var psi = new ProcessStartInfo("cmd.exe")
            {
                // chcp 65001 让中文输出不至于变成乱码；重定向编码同步设成 UTF-8。
                Arguments = "/c chcp 65001 >nul && " + commandLine,
                WorkingDirectory = string.IsNullOrEmpty(_toolsDir) ? null : _toolsDir,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            // 只影响这一个子进程，不改动系统/用户环境变量
            psi.EnvironmentVariables["PATH"] =
                _toolsDir + Path.PathSeparator + (Environment.GetEnvironmentVariable("PATH") ?? "");

            var process = new Process { StartInfo = psi, EnableRaisingEvents = true };
            process.OutputDataReceived += (_, args) =>
            {
                if (args.Data != null) _outQueue.Enqueue((args.Data, "Theme.TextPrimary"));
            };
            process.ErrorDataReceived += (_, args) =>
            {
                if (args.Data != null) _outQueue.Enqueue((args.Data, "Theme.LogError"));
            };

            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                AppendLine("启动 cmd.exe 失败：" + ex.Message, "Theme.LogError");
                ShowError("无法启动命令执行环境：" + ex.Message);
                return;
            }

            _running = process;
            BtnRun.IsEnabled = false;
            BtnStop.IsEnabled = true;

            // 外壳状态栏上报（工具箱配方第 3 节，纯加法）：命令可能一跑很久（logcat / 大文件推拉），
            // 期间外壳「任务」段显示这条命令（超长由 ToolStatusValue 的 TextTrimming 省略号截断），
            // 顺带清掉上一个任务可能残留的耗时读数（与 EDL 页 SetElapsed("") 同一做法）。
            // 页面自身不画状态栏/进度条 —— 输出仍然只落在上面的输出区里。
            ShellStatus.BeginTask("命令: " + commandLine);
            ShellStatus.SetElapsed("");

            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            try
            {
                await process.WaitForExitAsync();
                // 先把队列里剩下的输出行落地，再打退出码 —— 否则退出码会插到真正的输出前面
                _outQueue.FlushNow();
                AppendLine($"[退出码 {process.ExitCode}]", "Theme.TextTertiary");
            }
            catch (Exception ex)
            {
                AppendLine("等待命令结束出错：" + ex.Message, "Theme.LogError");
            }
            finally
            {
                _outQueue.FlushNow();   // 兜底：停止按钮杀进程的路径也把剩余行排空
                _running = null;
                BtnRun.IsEnabled = true;
                BtnStop.IsEnabled = false;
                // 收尾（含「停止」按钮杀进程的路径）：任务名保留、进度停在 100%，与其它页一致。
                ShellStatus.EndTask();
                process.Dispose();
            }
        }

        /// <summary>把输入框里的内容按前缀拼成完整命令行。</summary>
        private string BuildCommandLine(string input)
        {
            var prefix = (CboPrefix.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? "adb";
            return prefix switch
            {
                "adb" => "adb " + input,
                "fastboot" => "fastboot " + input,
                _ => input
            };
        }

        private void BtnStop_Click(object sender, RoutedEventArgs e)
        {
            var p = _running;
            if (p == null) return;
            try
            {
                if (!p.HasExited) p.Kill(entireProcessTree: true);
                AppendLine("[已停止]", "Theme.LogWarning");
            }
            catch (Exception ex)
            {
                AppendLine("停止失败：" + ex.Message, "Theme.LogError");
            }
        }

        private void BtnClear_Click(object sender, RoutedEventArgs e)
        {
            TxtConsole.Document.Blocks.Clear();
        }

        /// <summary>真正的 Windows cmd 窗口：PATH 已带 adb / fastboot，方便继续手动敲。</summary>
        private void BtnOpenCmd_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                var psi = new ProcessStartInfo("cmd.exe")
                {
                    Arguments = "/k set \"PATH=" + _toolsDir + ";%PATH%\"",
                    WorkingDirectory = string.IsNullOrEmpty(_toolsDir) ? null : _toolsDir,
                    UseShellExecute = false,
                    CreateNoWindow = false
                };
                Process.Start(psi);
                ShowSuccess("已打开系统命令提示符（可直接输入 adb / fastboot）");
            }
            catch (Exception ex)
            {
                ShowError("打开命令提示符失败：" + ex.Message);
            }
        }

        // ───────────────────────── 输出 ─────────────────────────

        /// <param name="scroll">
        /// 是否立刻滚到底。批量落地时传 false，一批只滚一次 ——
        /// ScrollToEnd 本身要重算整个可视区，一行一次是最主要的逐行渲染开销。
        /// </param>
        private void AppendLine(string text, string brushKey, bool scroll = true)
        {
            var brush = FindResource(brushKey) as Brush ?? FindResource("Theme.TextPrimary") as Brush;
            var paragraph = new Paragraph(new Run(text) { Foreground = brush })
            {
                Margin = new Thickness(0),
                LineHeight = 18
            };

            var blocks = TxtConsole.Document.Blocks;
            blocks.Add(paragraph);
            while (blocks.Count > MaxConsoleLines) blocks.Remove(blocks.FirstBlock);

            if (scroll) TxtConsole.ScrollToEnd();
        }

        private void AppendToolLog(string message)
        {
            if (TxtConsole == null) return;
            // 工具管理器的回调在**工作线程**上：入队即可，别再用同步 Invoke 去堵读线程
            _outQueue.Enqueue(("· " + message, "Theme.TextTertiary"));
        }

        private void SetHint(string text)
        {
            if (TxtHint != null) TxtHint.Text = text;
        }
    }
}
