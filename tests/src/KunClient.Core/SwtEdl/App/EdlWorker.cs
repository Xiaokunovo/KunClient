using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Xml;
using KunFlashTool.Core.SwtEdl;
using KunFlashTool.Core.SwtEdl.Protocol;
using KunFlashTool.Core.SwtEdl.Storage;
using KunFlashTool.Core.SwtEdl.Transport;

namespace KunFlashTool.Core.SwtEdl.App;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/app/EdlWorker.{h,cpp}`。
///
/// <para>C# 差异说明：C++ 侧的 Qt signals 全部落成 C# <c>event</c>（同步 invoke，调用方自行决定切线程）；
/// <c>std::atomic_bool m_cancelled</c> 落到 <see cref="EdlCancel"/>；<c>QSaveFile</c> 用同目录临时文件 +
/// 原子替换复刻（见 <see cref="SaveFile"/>）。</para>
/// </summary>
public sealed class EdlWorker
{
    // C++: signals
    public event Action<string>? LogMessage;                       // C++: logMessage(QString)
    public event Action<SessionState, string>? StateChanged;       // C++: stateChanged(state, detail)
    public event Action<ulong, ulong>? ProgressChanged;            // C++: progressChanged(completed, total)
    public event Action<ulong, ulong>? OverallProgressChanged;     // C++: overallProgressChanged(completed, total)
    public event Action<DeviceInfo>? DeviceInfoChanged;            // C++: deviceInfoChanged(info)
    public event Action<List<PartitionInfo>>? PartitionsReady;     // C++: partitionsReady(partitions)
    public event Action<bool, string>? OperationFinished;          // C++: operationFinished(success, message)

    private readonly EdlCancel _cancelled = new();
    private bool _busy = false;
    private StorageType _storage = StorageType.Ufs;
    private DeviceInfo _deviceInfo = new();
    private SerialTransport? _transport;
    private FirehoseClient? _firehose;
    private readonly Dictionary<uint, ulong> _lunSectorCounts = new();
    private List<PartitionInfo> _currentPartitions = new();

    private const ulong XmlSliceBytes = 64UL * 1024UL * 1024UL;

    private static readonly Regex PatchToken = new(
        "NUM_DISK_SECTORS(?:\\s*([+-])\\s*([0-9]+)\\.?)?",
        RegexOptions.IgnoreCase);

    private static readonly Regex UnsafeFileName = new("[<>:\"/\\\\|?*]");

    private static readonly Regex RawProgramName = new(
        "^rawprogram([0-9]+)\\.xml$",
        RegexOptions.IgnoreCase);

    // C++: explicit EdlWorker(QObject* parent = nullptr)
    public EdlWorker()
    {
    }

    // ------------------------------------------------------------------ 匿名 namespace 的工具

    private static ulong SaturatingAdd(ulong left, ulong right)
    {
        return right > ulong.MaxValue - left ? ulong.MaxValue : left + right;
    }

    private static bool ResolveSectorExpression(string expression, ulong diskSectors, out ulong value)
    {
        value = 0;
        expression = expression.Trim();
        while (expression.EndsWith(".")) expression = expression.Substring(0, expression.Length - 1);
        const string token = "NUM_DISK_SECTORS";
        if (!expression.StartsWith(token, StringComparison.OrdinalIgnoreCase) || diskSectors == 0)
        {
            return false;
        }
        var remainder = expression.Substring(token.Length).Trim();
        if (remainder.Length == 0)
        {
            value = diskSectors;
            return true;
        }
        var operation = remainder[0];
        remainder = remainder.Substring(1);
        while (remainder.EndsWith(".")) remainder = remainder.Substring(0, remainder.Length - 1);
        if (!ulong.TryParse(remainder.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture,
                            out var operand))
        {
            return false;
        }
        if (operation == '-' && operand <= diskSectors)
        {
            value = diskSectors - operand;
            return true;
        }
        if (operation == '+' && operand <= ulong.MaxValue - diskSectors)
        {
            value = diskSectors + operand;
            return true;
        }
        return false;
    }

    private static bool ResolvePatchExpression(string expression, ulong diskSectors, out string resolved)
    {
        resolved = string.Empty;
        var match = PatchToken.Match(expression);
        while (match.Success)
        {
            var value = diskSectors;
            if (value == 0) return false;
            if (match.Groups[1].Value.Length != 0)
            {
                if (!ulong.TryParse(match.Groups[2].Value, NumberStyles.Integer,
                                    CultureInfo.InvariantCulture, out var operand))
                {
                    return false;
                }
                if (match.Groups[1].Value == "-")
                {
                    if (operand > value) return false;
                    value -= operand;
                }
                else
                {
                    if (operand > ulong.MaxValue - value) return false;
                    value += operand;
                }
            }
            expression = expression.Substring(0, match.Index) +
                         value.ToString(CultureInfo.InvariantCulture) +
                         expression.Substring(match.Index + match.Length);
            match = PatchToken.Match(expression);
        }
        resolved = expression.Trim();
        return !resolved.Contains("NUM_DISK_SECTORS", StringComparison.OrdinalIgnoreCase);
    }

    private static bool ParseUnsignedText(string? text, out ulong value)
    {
        value = 0;
        var trimmed = (text ?? string.Empty).Trim();
        while (trimmed.EndsWith(".")) trimmed = trimmed.Substring(0, trimmed.Length - 1);
        var radix = trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? 16 : 10;
        var digits = radix == 16 ? trimmed.Substring(2) : trimmed;
        return ulong.TryParse(digits,
                              radix == 16 ? NumberStyles.HexNumber : NumberStyles.Integer,
                              CultureInfo.InvariantCulture, out value);
    }

    /// <summary>
    /// 对应 C++ 匿名 namespace 里的 <c>class FillDevice final : public QIODevice</c>：
    /// 顺序产生 <paramref name="length"/> 个字节，内容是按绝对位置循环的 32 位小端填充值。
    /// </summary>
    private sealed class FillDevice : Stream
    {
        private readonly uint _value;
        private ulong _remaining;
        private ulong _position;

        public FillDevice(uint value, ulong length)
        {
            _value = value;
            _remaining = length;
        }

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => (long)(_remaining + _position);
        public override long Position
        {
            get => (long)_position;
            set => throw new NotSupportedException();
        }

        public override void Flush()
        {
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (count <= 0 || _remaining == 0) return 0;
            var amount = Math.Min((ulong)count, _remaining);
            for (ulong index = 0; index < amount; ++index)
            {
                var shift = (int)(((_position + index) & 3UL) * 8UL);
                buffer[offset + (int)index] = (byte)((_value >> shift) & 0xFFU);
            }
            _position += amount;
            _remaining -= amount;
            return (int)amount;
        }

        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }

    /// <summary>
    /// 对应 C++ 匿名 namespace 里的 <c>class DynamicImageDevice final : public QIODevice</c>：
    /// 把 super 动态分区的镜像（原始或 sparse）按 extent 展开成连续字节流，不足部分补零。
    /// </summary>
    private sealed class DynamicImageDevice : Stream
    {
        private readonly string _sourcePath;
        private readonly ulong _totalLength;
        private readonly ulong _sourceLength;
        private readonly bool _sparse;
        private FileStream? _file;
        private ulong _position;
        private SparseImageInfo _sparseInfo = new();
        private long _extentIndex;

        public DynamicImageDevice(DynamicSuperImage image)
        {
            _sourcePath = image.SourcePath;
            _totalLength = image.AllocatedSize;
            _sourceLength = image.ExpandedSize;
            _sparse = image.Sparse;
        }

        public bool Initialize(out string error)
        {
            error = string.Empty;
            try
            {
                _file = new FileStream(_sourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            }
            catch (Exception ex)
            {
                error = $"打开动态分区镜像失败：{ex.Message}";
                return false;
            }
            if (_sparse && !SparseImage.Inspect(_file!, out _sparseInfo, out error)) return false;
            return true;
        }

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => (long)_totalLength;
        public override long Position
        {
            get => (long)_position;
            set => throw new NotSupportedException();
        }

        public override void Flush()
        {
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (count <= 0 || _position >= _totalLength) return 0;
            var requested = Math.Min((ulong)count, _totalLength - _position);
            ulong produced = 0;
            while (produced < requested)
            {
                if (_position >= _sourceLength)
                {
                    var zeros = requested - produced;
                    Array.Clear(buffer, offset + (int)produced, (int)zeros);
                    produced += zeros;
                    _position += zeros;
                    continue;
                }
                if (!_sparse)
                {
                    var whole = Math.Min(requested - produced, _sourceLength - _position);
                    _file!.Seek((long)_position, SeekOrigin.Begin);
                    var read = _file.Read(buffer, offset + (int)produced, (int)whole);
                    if ((ulong)read != whole) return 0;
                    produced += whole;
                    _position += whole;
                    continue;
                }

                while (_extentIndex < _sparseInfo.Extents.Count)
                {
                    var probe = _sparseInfo.Extents[(int)_extentIndex];
                    var probeEnd = probe.OutputBlock * _sparseInfo.BlockSize +
                                   probe.BlockCount * _sparseInfo.BlockSize;
                    if (_position < probeEnd) break;
                    ++_extentIndex;
                }
                if (_extentIndex >= _sparseInfo.Extents.Count) return 0;
                var extent = _sparseInfo.Extents[(int)_extentIndex];
                var extentStart = extent.OutputBlock * _sparseInfo.BlockSize;
                var extentBytes = extent.BlockCount * _sparseInfo.BlockSize;
                if (_position < extentStart) return 0;
                var within = _position - extentStart;
                var piece = Math.Min(requested - produced, extentBytes - within);
                if (extent.Type == SparseExtentType.Raw)
                {
                    _file!.Seek((long)(extent.FileOffset + within), SeekOrigin.Begin);
                    var read = _file.Read(buffer, offset + (int)produced, (int)piece);
                    if ((ulong)read != piece) return 0;
                }
                else if (extent.Type == SparseExtentType.Fill)
                {
                    for (ulong index = 0; index < piece; ++index)
                    {
                        var shift = (int)(((within + index) & 3UL) * 8UL);
                        buffer[offset + (int)produced + (int)index] =
                            (byte)((extent.FillValue >> shift) & 0xFFU);
                    }
                }
                else
                {
                    Array.Clear(buffer, offset + (int)produced, (int)piece);
                }
                produced += piece;
                _position += piece;
            }
            return (int)produced;
        }

        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _file?.Dispose();
                _file = null;
            }
            base.Dispose(disposing);
        }
    }

    /// <summary>
    /// 对应 Qt 的 <c>QSaveFile</c>：写入同目录临时文件，<see cref="Commit"/> 时原子替换目标，
    /// <see cref="CancelWriting"/> 或未提交就释放时删除临时文件。
    /// </summary>
    private sealed class SaveFile : IDisposable
    {
        private readonly string _finalPath;
        private readonly string _tempPath;
        private FileStream? _stream;
        private bool _committed;

        public SaveFile(string path)
        {
            _finalPath = path;
            _tempPath = path + ".part";
        }

        public bool Open(out string error)
        {
            error = string.Empty;
            try
            {
                _stream = new FileStream(_tempPath, FileMode.Create, FileAccess.Write, FileShare.None);
                return true;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                return false;
            }
        }

        public Stream Stream => _stream!;

        public void CancelWriting()
        {
            _stream?.Dispose();
            _stream = null;
            TryDelete(_tempPath);
        }

        public bool Commit(out string error)
        {
            error = string.Empty;
            try
            {
                _stream?.Flush();
                _stream?.Dispose();
                _stream = null;
                if (File.Exists(_finalPath)) File.Delete(_finalPath);
                File.Move(_tempPath, _finalPath);
                _committed = true;
                return true;
            }
            catch (Exception ex)
            {
                // 对应 QSaveFile 析构时清理未提交的临时文件。
                TryDelete(_tempPath);
                error = ex.Message;
                return false;
            }
        }

        public void Dispose()
        {
            if (_committed) return;
            _stream?.Dispose();
            _stream = null;
            TryDelete(_tempPath);
        }

        private static void TryDelete(string path)
        {
            try
            {
                if (File.Exists(path)) File.Delete(path);
            }
            catch
            {
                // 清理失败不影响主流程。
            }
        }
    }

    // ------------------------------------------------------------------ 内部状态机

    private void SetState(SessionState state, string detail = "")
    {
        StateChanged?.Invoke(state, detail.Length == 0 ? EdlTypes.StateText(state) : detail);
    }

    private uint SectorSize()
    {
        return _storage == StorageType.Ufs ? 4096U : 512U;
    }

    private void Fail(string message, bool closeTransport = true)
    {
        RestoreProgressCallback();
        LogMessage?.Invoke($"错误：{message}");
        if (closeTransport)
        {
            _firehose = null;
            _transport?.Close();
            _deviceInfo = new DeviceInfo();
            _lunSectorCounts.Clear();
            _currentPartitions.Clear();
            DeviceInfoChanged?.Invoke(_deviceInfo);
            PartitionsReady?.Invoke(new List<PartitionInfo>());
            SetState(SessionState.Failed, message);
        }
        else
        {
            // Parameter, source-file and local-save failures leave the established
            // Firehose channel usable, so keep the UI in the connected Ready state.
            SetState(SessionState.Ready, message);
        }
        _busy = false;
        OperationFinished?.Invoke(false, message);
    }

    private void Finish(string message)
    {
        RestoreProgressCallback();
        LogMessage?.Invoke(message);
        SetState(SessionState.Ready);
        _busy = false;
        OperationFinished?.Invoke(true, message);
    }

    private void RestoreProgressCallback()
    {
        if (_firehose == null) return;
        _firehose.SetProgressCallback((current, total) => ProgressChanged?.Invoke(current, total));
    }

    private bool EnsureReady(string operation)
    {
        if (_busy)
        {
            OperationFinished?.Invoke(false, "当前已有任务正在执行");
            return false;
        }
        if (_transport == null || !_transport.IsOpen || _firehose == null)
        {
            OperationFinished?.Invoke(false, $"请先连接 EDL 设备再执行{operation}");
            return false;
        }
        _busy = true;
        _cancelled.Reset();
        ProgressChanged?.Invoke(0, 0);
        OverallProgressChanged?.Invoke(0, 0);
        return true;
    }

    // ------------------------------------------------------------------ 连接

    public void ConnectDevice(string portName, string programmerPath, StorageType storage,
                              OplusVipPackage vipPackage)
    {
        if (_busy) return;
        _busy = true;
        _cancelled.Reset();
        ProgressChanged?.Invoke(0, 0);
        OverallProgressChanged?.Invoke(0, 3);
        _storage = storage;
        _deviceInfo = new DeviceInfo();
        _deviceInfo.PortName = portName;
        _deviceInfo.Storage = EdlTypes.StorageName(storage).ToUpperInvariant();
        _deviceInfo.Programmer = Path.GetFileName(programmerPath);
        _deviceInfo.VipMode = vipPackage.Enabled ? "OPPO VIP" : "标准 EDL";
        if (vipPackage.Enabled) _deviceInfo.Brand = "OPLUS";
        var initialChip = DeviceIdentity.DetectProgrammerChip(
            programmerPath, new List<string> { vipPackage.PackageDirectory });
        if (initialChip.Length != 0)
        {
            _deviceInfo.Model = DeviceIdentity.QualcommChipDisplayName(initialChip);
        }

        if (portName.Length == 0)
        {
            Fail("请选择 Qualcomm 9008 串口");
            return;
        }

        _firehose = null;
        _transport = new SerialTransport(_cancelled);
        var error = string.Empty;

        SetState(SessionState.OpeningPort);
        LogMessage?.Invoke($"打开端口 {portName}");
        if (!_transport.Open(portName, out error))
        {
            Fail(error);
            return;
        }
        LogMessage?.Invoke($"串口配置：{_transport.OpenDiagnostics}");
        if (initialChip.Length != 0)
        {
            LogMessage?.Invoke($"Programmer 芯片标识：{initialChip}（本地元数据）");
        }

        LogCallback log = line => LogMessage?.Invoke(line);
        ProgressCallback progress = (current, total) => ProgressChanged?.Invoke(current, total);

        LogMessage?.Invoke("检测设备当前通信状态…");
        var saharaDetected = false;
        var pendingSaharaTransfer = false;
        // Most BootROMs either expose HELLO immediately or wait for a host kick.
        // Keep this passive window short; the active Sahara recovery below handles
        // delayed qcusbser delivery without adding several seconds to every connect.
        if (_transport.PeekExactly(8, out var probeHeader, 400, out _))
        {
            var command = BinaryPrimitives.ReadUInt32LittleEndian(probeHeader.AsSpan(0));
            var length = BinaryPrimitives.ReadUInt32LittleEndian(probeHeader.AsSpan(4));
            saharaDetected = command == 0x01 && length >= 48 && length <= 1024 * 1024;
            pendingSaharaTransfer = command == 0x04 && length >= 16 && length <= 1024 * 1024;
        }

        if (!saharaDetected && !pendingSaharaTransfer)
        {
            SetState(SessionState.WaitingForFirehose, "检测 Firehose 模式");
            var firehose = new FirehoseClient(_transport, _cancelled, log, progress);
            if (firehose.Ping(out var firehoseError, 1200))
            {
                LogMessage?.Invoke("状态检测：设备已处于 Firehose 模式，跳过引导传输");
                _firehose = firehose;
                if (vipPackage.Enabled) _firehose.ResumeOplusVip(vipPackage.DigestPath);

                if (!_firehose.GetStorageInfo(0, out var storageError))
                {
                    LogMessage?.Invoke("Firehose 存储尚未就绪，自动执行 Configure");
                    SetState(SessionState.Configuring);
                    if (!_firehose.Configure(storage, out error))
                    {
                        Fail(error);
                        return;
                    }
                    storageError = string.Empty;
                    if (!_firehose.GetStorageInfo(0, out storageError))
                    {
                        LogMessage?.Invoke($"存储信息查询跳过：{storageError}");
                    }
                }
                else
                {
                    LogMessage?.Invoke("Firehose 存储已配置，可直接读取分区表");
                }

                if (_deviceInfo.Programmer.Length == 0)
                {
                    _deviceInfo.Programmer = "已加载 Firehose";
                }
                _firehose.EnrichDeviceInfo(_deviceInfo);
                DeviceInfoChanged?.Invoke(_deviceInfo);
                OverallProgressChanged?.Invoke(3, 3);
                _busy = false;
                SetState(SessionState.Ready);
                LogMessage?.Invoke("EDL 通道接管完成");
                OperationFinished?.Invoke(true, "设备连接成功");
                return;
            }

            LogMessage?.Invoke("Firehose 探测未响应，切换为 Sahara 引导流程");
            _transport.Close();
            Thread.Sleep(220);
            if (!_transport.Open(portName, out error))
            {
                Fail(error);
                return;
            }
            LogMessage?.Invoke($"串口已按 Sahara 参数重新打开：{_transport.OpenDiagnostics}");
        }
        else if (saharaDetected)
        {
            LogMessage?.Invoke("状态检测：设备处于 Sahara 模式，需要发送引导");
        }

        var programmerUploaded = false;
        if (pendingSaharaTransfer)
        {
            LogMessage?.Invoke("状态检测：Programmer 已传输，正在完成 Sahara 交接");
            SetState(SessionState.SaharaHandshake, "确认 Sahara DONE");
            var sahara = new SaharaClient(_transport, _cancelled, log, progress);
            programmerUploaded = sahara.FinishPendingTransfer(out error);
        }
        else
        {
            if (programmerPath.Length == 0 || !File.Exists(programmerPath))
            {
                Fail("设备处于 Sahara 模式，请选择有效的 Firehose Programmer");
                return;
            }

            var programmerCandidates = new List<string> { programmerPath };
            if (vipPackage.Enabled && vipPackage.ProgrammerCandidates.Count != 0)
            {
                programmerCandidates = new List<string>(vipPackage.ProgrammerCandidates);
                if (!programmerCandidates.Contains(programmerPath, StringComparer.OrdinalIgnoreCase))
                {
                    programmerCandidates.Insert(0, programmerPath);
                }
            }

            var terminalSaharaFailure = false;
            for (var candidateIndex = 0;
                 candidateIndex < programmerCandidates.Count && !programmerUploaded &&
                 !terminalSaharaFailure;
                 ++candidateIndex)
            {
                var candidate = programmerCandidates[candidateIndex];
                _deviceInfo.Programmer = Path.GetFileName(candidate);
                var candidateChip = DeviceIdentity.DetectProgrammerChip(
                    candidate, new List<string> { vipPackage.PackageDirectory });
                if (candidateChip.Length != 0)
                {
                    _deviceInfo.Model = DeviceIdentity.QualcommChipDisplayName(candidateChip);
                }
                if (candidateIndex > 0)
                {
                    LogMessage?.Invoke($"Sahara 自动切换候选 Programmer：{_deviceInfo.Programmer}");
                }

                for (var attempt = 0; attempt < 2; ++attempt)
                {
                    SetState(SessionState.SaharaHandshake);
                    var sahara = new SaharaClient(_transport, _cancelled, log, progress);
                    SetState(SessionState.UploadingProgrammer);
                    error = string.Empty;
                    if (sahara.UploadProgrammer(candidate, _deviceInfo,
                                                !vipPackage.Enabled, out error,
                                                out var saharaStatus))
                    {
                        programmerUploaded = true;
                        DeviceInfoChanged?.Invoke(_deviceInfo);
                        break;
                    }

                    var authenticationNak = saharaStatus == 0x21 || saharaStatus == 0x22;
                    var hasNextCandidate = candidateIndex + 1 < programmerCandidates.Count;
                    var retrySameCandidate = attempt == 0 && !authenticationNak;
                    if ((retrySameCandidate || (authenticationNak && hasNextCandidate)) &&
                        !_cancelled.IsCancelled)
                    {
                        LogMessage?.Invoke(authenticationNak
                            ? "当前 Programmer 被 BootROM 校验拒绝，重新打开端口后尝试下一签名版本"
                            : "Sahara 首次握手未完成，重新打开端口再试一次");
                        _transport.Close();
                        Thread.Sleep(800);
                        if (!_transport.Open(portName, out error)) break;
                        LogMessage?.Invoke($"串口重连参数：{_transport.OpenDiagnostics}");
                    }
                    if (authenticationNak) break;
                    if (attempt == 1) terminalSaharaFailure = true;
                }
            }
        }
        if (!programmerUploaded)
        {
            Fail(error);
            return;
        }
        OverallProgressChanged?.Invoke(1, 3);

        SetState(SessionState.WaitingForFirehose);
        // VIP programmers start a short watchdog as soon as Sahara DONE completes.
        // Enter the Firehose receive/send path immediately instead of spending the
        // first watchdog window in a fixed hand-off delay.
        Thread.Sleep(vipPackage.Enabled ? 20 : 700);
        _firehose = new FirehoseClient(_transport, _cancelled, log, progress);

        if (vipPackage.Enabled)
        {
            SetState(SessionState.OplusVipVerification);
            if (!_firehose.InitializeOplusVip(vipPackage.DigestPath, vipPackage.SignaturePath,
                                              storage, out error))
            {
                Fail($"OPPO VIP 原生协议失败：{error}");
                return;
            }
        }
        else
        {
            if (!_firehose.Ping(out error))
            {
                Fail($"Firehose 启动检查失败：{error}");
                return;
            }
            SetState(SessionState.Configuring);
            if (!_firehose.Configure(storage, out error))
            {
                Fail(error);
                return;
            }
        }

        OverallProgressChanged?.Invoke(2, 3);
        if (!_firehose.GetStorageInfo(0, out var infoError))
        {
            LogMessage?.Invoke($"存储信息查询跳过：{infoError}");
        }
        OverallProgressChanged?.Invoke(3, 3);

        if (_deviceInfo.Programmer.Length == 0)
        {
            _deviceInfo.Programmer = "已加载 Firehose";
        }
        _firehose.EnrichDeviceInfo(_deviceInfo);
        var identity = _deviceInfo.Serial.Length != 0
            ? _deviceInfo.Serial : _deviceInfo.Hwid;
        LogMessage?.Invoke(
            $"设备信息：{(_deviceInfo.Model.Length == 0 ? "Qualcomm" : _deviceInfo.Model)}，" +
            $"CPUID {(identity.Length == 0 ? "—" : identity)}，" +
            $"存储 {(_deviceInfo.Storage.Length == 0 ? EdlTypes.StorageName(storage).ToUpperInvariant() : _deviceInfo.Storage)}" +
            $"{(_deviceInfo.StorageProduct.Length == 0 ? string.Empty : $" / {_deviceInfo.StorageProduct}")}");
        DeviceInfoChanged?.Invoke(_deviceInfo);

        _busy = false;
        SetState(SessionState.Ready);
        LogMessage?.Invoke("EDL 通道初始化完成");
        OperationFinished?.Invoke(true, "设备连接成功");
    }

    public void DisconnectDevice()
    {
        _cancelled.Cancel();
        _firehose = null;
        _transport?.Close();
        _busy = false;
        _deviceInfo = new DeviceInfo();
        _lunSectorCounts.Clear();
        _currentPartitions.Clear();
        DeviceInfoChanged?.Invoke(_deviceInfo);
        PartitionsReady?.Invoke(new List<PartitionInfo>());
        SetState(SessionState.Disconnected);
        LogMessage?.Invoke("EDL 连接已关闭");
        OperationFinished?.Invoke(true, "连接已断开");
    }

    // ------------------------------------------------------------------ 读取分区表

    public void ReadPartitionTable()
    {
        if (!EnsureReady("读取分区表")) return;
        SetState(SessionState.Reading, "读取 GPT 分区表");

        var allPartitions = new List<PartitionInfo>();
        _lunSectorCounts.Clear();
        var lunCount = _storage == StorageType.Ufs ? 6U : 1U;
        var gptSectors = _storage == StorageType.Ufs ? 6UL : 34UL;
        var lastError = string.Empty;
        OverallProgressChanged?.Invoke(0, lunCount);

        for (uint lun = 0; lun < lunCount; ++lun)
        {
            ProgressChanged?.Invoke(0, 0);
            OverallProgressChanged?.Invoke(lun, lunCount);
            if (_cancelled.IsCancelled)
            {
                Fail("操作已停止");
                return;
            }
            LogMessage?.Invoke($"读取 LUN {lun} 主 GPT");
            if (!_firehose!.ReadToByteArray(out var gpt, 0, gptSectors, lun, SectorSize(), out var error))
            {
                LogMessage?.Invoke($"LUN {lun}：{error}");
                lastError = error;
                if (error.Contains("串口读取超时") || error.Contains("等待协议帧超时"))
                {
                    LogMessage?.Invoke("RAW 数据流已失去同步，停止继续读取后续 LUN");
                    break;
                }
                continue;
            }
            if (gpt.Length >= (long)SectorSize() + 40)
            {
                var backupLba = BinaryPrimitives.ReadUInt64LittleEndian(gpt.AsSpan((int)SectorSize() + 32));
                if (backupLba > 0 && backupLba < ulong.MaxValue)
                {
                    _lunSectorCounts[lun] = backupLba + 1;
                }
            }
            var partitions = new List<PartitionInfo>();
            if (!GptParser.Parse(gpt, SectorSize(), lun, partitions, out error))
            {
                LogMessage?.Invoke($"LUN {lun} GPT：{error}");
                lastError = error;
                continue;
            }
            allPartitions.AddRange(partitions);
        }

        if (allPartitions.Count == 0)
        {
            Fail(lastError.Length == 0 ? "没有读取到有效 GPT" : lastError);
            return;
        }
        OverallProgressChanged?.Invoke(lunCount, lunCount);
        _currentPartitions = allPartitions;
        PartitionsReady?.Invoke(allPartitions);
        Finish($"分区表读取完成，共 {allPartitions.Count} 个分区");
    }

    // ------------------------------------------------------------------ 读取系统版本信息

    public void ReadSystemVersionInfo()
    {
        if (!EnsureReady("读取系统版本信息")) return;
        if (_currentPartitions.Count == 0)
        {
            Fail("请先读取 GPT 分区表", false);
            return;
        }
        SetState(SessionState.Reading, "读取系统版本信息");
        ProgressChanged?.Invoke(0, 0);
        OverallProgressChanged?.Invoke(0, 1);
        LogMessage?.Invoke("开始从设备分区读取系统版本信息…");

        PartitionInfo? FindPartition(List<string> names)
        {
            foreach (var name in names)
            {
                foreach (var partition in _currentPartitions)
                {
                    if (string.Equals(partition.Name, name, StringComparison.OrdinalIgnoreCase))
                    {
                        return partition;
                    }
                }
            }
            return null;
        }
        SystemVersionReader.PartitionByteReader ByteReader(PartitionInfo partition)
        {
            return (ulong offset, uint length, out string error) =>
            {
                error = string.Empty;
                if (length == 0 || offset > partition.SectorCount * SectorSize() ||
                    length > partition.SectorCount * SectorSize() - offset)
                {
                    return Array.Empty<byte>();
                }
                var alignedStart = offset / SectorSize() * SectorSize();
                var end = offset + length;
                var alignedEnd = ((end + SectorSize() - 1) / SectorSize()) * SectorSize();
                var count = (alignedEnd - alignedStart) / SectorSize();
                if (!_firehose!.ReadToByteArray(out var raw,
                        partition.FirstLba + alignedStart / SectorSize(), count,
                        partition.Lun, SectorSize(), out error, partition.Name,
                        partition.Name + ".img"))
                {
                    return Array.Empty<byte>();
                }
                return raw.AsSpan((int)(offset - alignedStart), (int)length).ToArray();
            };
        }

        var version = new SystemVersionInfo();
        var error = string.Empty;
        var found = false;
        var super = FindPartition(new List<string> { "super", "super_a" });
        if (super != null)
        {
            LogMessage?.Invoke("解析 super 动态分区元数据…");
            found = SystemVersionReader.ReadSuper(
                ByteReader(super), super.SectorCount * SectorSize(), version, out error,
                status => LogMessage?.Invoke(status));
        }
        if (!found)
        {
            var candidates = new List<string>
            {
                "my_manifest", "odm", "my_product", "vendor",
                "product", "system_ext", "system",
            };
            foreach (var baseName in candidates)
            {
                var partition = FindPartition(
                    new List<string> { baseName, baseName + "_a", baseName + "_b" });
                if (partition == null) continue;
                LogMessage?.Invoke($"解析 {partition.Name} 版本信息…");
                if (SystemVersionReader.ReadPhysical(
                        ByteReader(partition), partition.SectorCount * SectorSize(),
                        version, out _))
                {
                    found = true;
                }
            }
        }
        if (!found || version.IsEmpty)
        {
            Fail(error.Length == 0 ? "设备分区中未识别到系统版本属性" : error, false);
            return;
        }

        if (version.Brand.Length != 0) _deviceInfo.Brand = version.Brand.ToUpperInvariant();
        _deviceInfo.ProductName = version.MarketName.Length != 0
            ? version.MarketName : version.Model;
        _deviceInfo.AndroidVersion = version.AndroidVersion;
        _deviceInfo.SystemVersion = version.SystemVersion;
        _deviceInfo.SecurityPatch = version.SecurityPatch;
        _deviceInfo.BuildFingerprint = version.Fingerprint;
        DeviceInfoChanged?.Invoke(_deviceInfo);

        void LogField(string title, string value)
        {
            if (value.Length != 0) LogMessage?.Invoke($"{title}：{value}");
        }
        LogField("设备名称", _deviceInfo.ProductName);
        LogField("品牌", version.Brand);
        LogField("设备代号", version.Device);
        LogField("平台", version.Platform);
        LogField("Android", version.AndroidVersion);
        LogField("系统版本", version.SystemVersion);
        LogField("OTA 版本", version.OtaVersion);
        LogField("安全补丁", version.SecurityPatch);
        LogField("地区", version.Region);
        LogField("编译日期", version.BuildDate);
        LogField("系统指纹", version.Fingerprint);
        if (version.FileSystems.Count != 0)
        {
            LogMessage?.Invoke($"文件系统：{string.Join(" / ", version.FileSystems)}");
        }
        OverallProgressChanged?.Invoke(1, 1);
        Finish("系统版本信息读取完成");
    }

    // ------------------------------------------------------------------ 备份

    public void BackupPartition(PartitionInfo partition, string outputPath)
    {
        if (!EnsureReady("备份分区")) return;
        SetState(SessionState.Reading, $"备份 {partition.Name}");
        OverallProgressChanged?.Invoke(0, 1);

        var output = new SaveFile(outputPath);
        if (!output.Open(out var openError))
        {
            Fail($"创建备份文件失败：{openError}", false);
            return;
        }

        if (!_firehose!.ReadToDevice(output.Stream, partition.FirstLba, partition.SectorCount,
                                     partition.Lun, SectorSize(), out var error,
                                     partition.Name, partition.Name + ".bin"))
        {
            output.CancelWriting();
            Fail(error);
            return;
        }
        if (!output.Commit(out var commitError))
        {
            Fail($"保存备份失败：{commitError}", false);
            return;
        }
        OverallProgressChanged?.Invoke(1, 1);
        Finish($"分区 {partition.Name} 已备份到 {outputPath}");
    }

    public void BackupPartitions(IReadOnlyList<PartitionInfo> partitions, string outputDirectory,
                                 bool generateXml)
    {
        if (!EnsureReady("批量读取分区")) return;
        if (partitions.Count == 0 || outputDirectory.Length == 0)
        {
            Fail("批量读取参数为空", false);
            return;
        }
        if (!Directory.Exists(outputDirectory))
        {
            try
            {
                Directory.CreateDirectory(outputDirectory);
            }
            catch
            {
                Fail($"创建备份目录失败：{outputDirectory}", false);
                return;
            }
        }

        ulong totalBytes = 0;
        foreach (var partition in partitions)
        {
            totalBytes = SaturatingAdd(totalBytes, partition.SectorCount * SectorSize());
        }
        ulong completed = 0;
        var usedNames = new List<string>();
        var outputNames = new List<string>();
        SetState(SessionState.Reading, $"批量读取 {partitions.Count} 个分区");
        LogMessage?.Invoke($"回读任务：{partitions.Count} 个分区，总计 {totalBytes} 字节，保存到 {outputDirectory}");
        ProgressChanged?.Invoke(0, 0);
        OverallProgressChanged?.Invoke(0, totalBytes);
        var progressThrottle = new Stopwatch();

        for (long index = 0; index < partitions.Count; ++index)
        {
            var partition = partitions[(int)index];
            var outputName = UnsafeFileName.Replace(partition.Name, "_");
            outputName += ".img";
            if (usedNames.Contains(outputName, StringComparer.OrdinalIgnoreCase))
            {
                outputName = $"{partition.Name}_lun{partition.Lun}.img";
            }
            usedNames.Add(outputName);
            outputNames.Add(outputName);
            var outputPath = Path.Combine(outputDirectory, outputName);
            LogMessage?.Invoke($"读取 {index + 1}/{partitions.Count}：{partition.Name}（LUN {partition.Lun}）→ {outputName}");

            var output = new SaveFile(outputPath);
            if (!output.Open(out var openError))
            {
                Fail($"创建备份文件失败：{openError}", false);
                return;
            }
            var baseValue = completed;
            var partitionBytes = partition.SectorCount * SectorSize();
            ProgressChanged?.Invoke(0, partitionBytes);
            OverallProgressChanged?.Invoke(completed, totalBytes);
            progressThrottle.Reset();
            _firehose!.SetProgressCallback((current, currentTotal) =>
            {
                var finalUpdate = current >= currentTotal;
                if (progressThrottle.IsRunning && !finalUpdate &&
                    progressThrottle.ElapsedMilliseconds < 33)
                {
                    return;
                }
                if (progressThrottle.IsRunning) progressThrottle.Restart();
                else progressThrottle.Start();
                ProgressChanged?.Invoke(current, currentTotal);
                OverallProgressChanged?.Invoke(SaturatingAdd(baseValue, current), totalBytes);
            });
            if (!_firehose.ReadToDevice(output.Stream, partition.FirstLba, partition.SectorCount,
                                        partition.Lun, SectorSize(), out var error,
                                        partition.Name, outputName))
            {
                output.CancelWriting();
                Fail($"读取分区 {partition.Name} 失败：{error}");
                return;
            }
            if (!output.Commit(out var commitError))
            {
                Fail($"保存备份失败：{commitError}", false);
                return;
            }
            completed = SaturatingAdd(completed, partition.SectorCount * SectorSize());
            ProgressChanged?.Invoke(partitionBytes, partitionBytes);
            OverallProgressChanged?.Invoke(completed, totalBytes);
        }

        _firehose!.SetProgressCallback((current, total) => ProgressChanged?.Invoke(current, total));
        if (generateXml)
        {
            var generatedPaths = new List<string>();
            if (!RawProgramWriter.WriteBackup(outputDirectory, partitions, outputNames,
                                              SectorSize(), generatedPaths, out var xmlError))
            {
                Fail(xmlError, false);
                return;
            }
            foreach (var path in generatedPaths)
            {
                LogMessage?.Invoke($"已生成回读 XML：{path}");
            }
        }
        else
        {
            LogMessage?.Invoke("生成 XML 已关闭，本次仅保存分区镜像");
        }
        Finish($"批量读取完成：{partitions.Count} 个分区，保存到 {outputDirectory}");
    }

    // ------------------------------------------------------------------ 写入

    public void FlashPartition(PartitionInfo partition, string imagePath)
    {
        if (!EnsureReady("写入分区")) return;
        SetState(SessionState.Writing, $"写入 {partition.Name}");
        OverallProgressChanged?.Invoke(0, 1);

        FileStream input;
        try
        {
            input = new FileStream(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        }
        catch (Exception ex)
        {
            Fail($"打开镜像失败：{ex.Message}", false);
            return;
        }

        using (input)
        {
            if (!_firehose!.Program(input, (ulong)input.Length, partition.FirstLba,
                                    partition.SectorCount, partition.Lun, SectorSize(), out var error,
                                    partition.Name, Path.GetFileName(imagePath)))
            {
                Fail(error);
                return;
            }
        }
        OverallProgressChanged?.Invoke(1, 1);
        Finish($"分区 {partition.Name} 写入完成");
    }

    public void FlashPartitions(IReadOnlyList<PartitionInfo> partitions,
                                IReadOnlyList<string> imagePaths,
                                bool allowProtected = false)
    {
        if (!EnsureReady("批量写入分区")) return;
        var filtered = new List<PartitionInfo>();
        var filteredImages = new List<string>();
        for (long index = 0; index < partitions.Count &&
                            index < imagePaths.Count; ++index)
        {
            if (!allowProtected &&
                PartitionProtection.IsProtectedPartitionName(partitions[(int)index].Name))
            {
                LogMessage?.Invoke($"保护分区：跳过写入 {partitions[(int)index].Name}");
                continue;
            }
            filtered.Add(partitions[(int)index]);
            filteredImages.Add(imagePaths[(int)index]);
        }
        if (filtered.Count == 0 || filtered.Count != filteredImages.Count)
        {
            Fail("分区与镜像数量不匹配", false);
            return;
        }

        ulong totalBytes = 0;
        var sparseImages = new List<bool>();
        for (long index = 0; index < filteredImages.Count; ++index)
        {
            var imagePath = filteredImages[(int)index];
            var image = new FileInfo(imagePath);
            if (!image.Exists || image.Length <= 0)
            {
                Fail($"镜像文件错误：{imagePath}", false);
                return;
            }
            FileStream probe;
            try
            {
                probe = new FileStream(image.FullName, FileMode.Open, FileAccess.Read, FileShare.Read);
            }
            catch (Exception ex)
            {
                Fail($"打开镜像失败：{ex.Message}", false);
                return;
            }
            using (probe)
            {
                var sparse = SparseImage.HasSparseMagic(probe);
                sparseImages.Add(sparse);
                if (sparse)
                {
                    if (!SparseImage.Inspect(probe, out var sparseInfo, out var sparseError))
                    {
                        Fail($"{image.Name}：{sparseError}", false);
                        return;
                    }
                    if (sparseInfo.BlockSize % SectorSize() != 0 ||
                        sparseInfo.ExpandedSize > filtered[(int)index].SectorCount * SectorSize())
                    {
                        Fail($"Sparse 镜像 {image.Name} 展开后超过分区 {filtered[(int)index].Name} 容量", false);
                        return;
                    }
                    foreach (var extent in sparseInfo.Extents)
                    {
                        if (extent.Type != SparseExtentType.Skip)
                        {
                            totalBytes = SaturatingAdd(
                                totalBytes, extent.BlockCount * sparseInfo.BlockSize);
                        }
                    }
                }
                else
                {
                    var bytes = (ulong)image.Length;
                    if (bytes > filtered[(int)index].SectorCount * SectorSize())
                    {
                        Fail($"镜像 {image.Name} 超过分区 {filtered[(int)index].Name} 容量", false);
                        return;
                    }
                    totalBytes = SaturatingAdd(totalBytes, bytes);
                }
            }
        }
        if (totalBytes == 0)
        {
            Fail("镜像中没有可传输的数据", false);
            return;
        }

        ulong completed = 0;
        SetState(SessionState.Writing, $"批量写入 {filtered.Count} 个分区");
        ProgressChanged?.Invoke(0, 0);
        OverallProgressChanged?.Invoke(0, totalBytes);
        for (long index = 0; index < filtered.Count; ++index)
        {
            var partition = filtered[(int)index];
            var imagePath = filteredImages[(int)index];
            FileStream input;
            try
            {
                input = new FileStream(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read);
            }
            catch (Exception ex)
            {
                Fail($"打开镜像失败：{ex.Message}", false);
                return;
            }
            using (input)
            {
                LogMessage?.Invoke($"写入 {index + 1}/{filtered.Count}：{Path.GetFileName(imagePath)} → {partition.Name}（LUN {partition.Lun}）");
                var error = string.Empty;
                if (sparseImages[(int)index])
                {
                    if (!SparseImage.Inspect(input, out var sparseInfo, out error))
                    {
                        Fail($"{Path.GetFileName(imagePath)}：{error}", false);
                        return;
                    }
                    LogMessage?.Invoke($"Android Sparse：{sparseInfo.TotalBlocks} 个块，展开 {sparseInfo.ExpandedSize} 字节");
                    foreach (var extent in sparseInfo.Extents)
                    {
                        if (extent.Type == SparseExtentType.Skip) continue;
                        var extentBytes = extent.BlockCount * sparseInfo.BlockSize;
                        var targetSector = partition.FirstLba +
                            extent.OutputBlock * sparseInfo.BlockSize / SectorSize();
                        var ok = extent.Type == SparseExtentType.Raw
                            ? ProgramFileRange(input, extent.FileOffset, extentBytes, targetSector,
                                               partition.Lun, SectorSize(), partition.Name,
                                               Path.GetFileName(imagePath), ref completed,
                                               totalBytes, out error)
                            : ProgramFillRange(extent.FillValue, extentBytes, targetSector,
                                               partition.Lun, SectorSize(), partition.Name,
                                               Path.GetFileName(imagePath), ref completed,
                                               totalBytes, out error);
                        if (!ok)
                        {
                            Fail($"写入分区 {partition.Name} 失败：{error}");
                            return;
                        }
                    }
                }
                else
                {
                    var imageBytes = (ulong)input.Length;
                    var baseValue = completed;
                    _firehose!.SetProgressCallback((current, currentTotal) =>
                    {
                        ProgressChanged?.Invoke(current, currentTotal);
                        OverallProgressChanged?.Invoke(SaturatingAdd(baseValue, current), totalBytes);
                    });
                    if (!_firehose.Program(input, imageBytes, partition.FirstLba,
                                           partition.SectorCount, partition.Lun, SectorSize(), out error,
                                           partition.Name, Path.GetFileName(imagePath)))
                    {
                        Fail($"写入分区 {partition.Name} 失败：{error}");
                        return;
                    }
                    completed = SaturatingAdd(completed, imageBytes);
                    ProgressChanged?.Invoke(imageBytes, imageBytes);
                    OverallProgressChanged?.Invoke(completed, totalBytes);
                }
            }
        }

        _firehose!.SetProgressCallback((current, total) => ProgressChanged?.Invoke(current, total));
        Finish($"批量写入完成：{filtered.Count} 个分区");
    }

    private bool ProgramFileRange(Stream input, ulong fileOffset, ulong byteCount,
                                  ulong targetStartSector, uint lun, uint xmlSectorSize,
                                  string label, string filename, ref ulong completed,
                                  ulong total, out string error)
    {
        error = string.Empty;
        try
        {
            input.Seek((long)fileOffset, SeekOrigin.Begin);
        }
        catch (Exception ex)
        {
            error = $"定位镜像数据失败：{ex.Message}";
            return false;
        }
        return ProgramDeviceRange(input, byteCount, targetStartSector, lun, xmlSectorSize,
                                  label, filename, ref completed, total, out error);
    }

    private bool ProgramDeviceRange(Stream input, ulong byteCount, ulong targetStartSector,
                                    uint lun, uint xmlSectorSize, string label, string filename,
                                    ref ulong completed, ulong total, out string error)
    {
        error = string.Empty;
        ulong rangeCompleted = 0;
        while (rangeCompleted < byteCount)
        {
            if (_cancelled.IsCancelled)
            {
                error = "操作已停止";
                return false;
            }
            var chunkBytes = Math.Min(XmlSliceBytes, byteCount - rangeCompleted);
            var chunkSectors = (chunkBytes + xmlSectorSize - 1) / xmlSectorSize;
            var overallBase = completed;
            var taskBase = rangeCompleted;
            _firehose!.SetProgressCallback((current, _) =>
            {
                ProgressChanged?.Invoke(SaturatingAdd(taskBase, current), byteCount);
                OverallProgressChanged?.Invoke(SaturatingAdd(overallBase, current), total);
            });
            if (!_firehose.Program(input, chunkBytes,
                                   targetStartSector + rangeCompleted / xmlSectorSize,
                                   chunkSectors, lun, xmlSectorSize, out error, label, filename))
            {
                return false;
            }
            rangeCompleted += chunkBytes;
            completed = SaturatingAdd(completed, chunkBytes);
            ProgressChanged?.Invoke(rangeCompleted, byteCount);
            OverallProgressChanged?.Invoke(completed, total);
        }
        return true;
    }

    private bool ProgramFillRange(uint fillValue, ulong byteCount, ulong targetStartSector,
                                  uint lun, uint xmlSectorSize, string label, string filename,
                                  ref ulong completed, ulong total, out string error)
    {
        error = string.Empty;
        var fill = new FillDevice(fillValue, byteCount);
        return ProgramDeviceRange(fill, byteCount, targetStartSector, lun, xmlSectorSize,
                                  label, filename, ref completed, total, out error);
    }

    // ------------------------------------------------------------------ XML 刷机

    public void FlashXml(IReadOnlyList<string> xmlPaths, bool allowProtected = false)
    {
        if (!EnsureReady("XML 刷机")) return;

        var plan = new RawProgramPlan();
        if (!RawProgramParser.Parse(xmlPaths, plan, out var error))
        {
            Fail(error, false);
            return;
        }
        var protectedRanges = new List<RawProgramEntry>();
        var removedProtected = new Dictionary<string, int>();
        for (long index = plan.Entries.Count; index > 0; --index)
        {
            var entry = plan.Entries[(int)(index - 1)];
            if (!allowProtected &&
                PartitionProtection.IsProtectedPartitionMapping(entry.Label, entry.Filename))
            {
                protectedRanges.Add(entry);
                var key = PartitionProtection.NormalizedPartitionName(
                    entry.Label.Length != 0 ? entry.Label : entry.Filename);
                removedProtected[key] = removedProtected.TryGetValue(key, out var count) ? count + 1 : 1;
                plan.Entries.RemoveAt((int)(index - 1));
            }
        }
        if (!allowProtected)
        {
            foreach (var partition in _currentPartitions)
            {
                if (!PartitionProtection.IsProtectedPartitionName(partition.Name)) continue;
                var range = new RawProgramEntry
                {
                    Label = partition.Name,
                    Lun = partition.Lun,
                    StartSector = partition.FirstLba,
                    StartSectorResolved = true,
                    SectorCount = partition.SectorCount,
                    SectorSize = SectorSize(),
                };
                protectedRanges.Add(range);
            }
        }
        // ⚠️ C++ 这里是遍历 QHash（无序）；这里用 Dictionary，逐条日志互相独立，顺序差异对结果无影响。
        foreach (var pair in removedProtected)
        {
            LogMessage?.Invoke($"保护分区：已跳过 {pair.Key} 的 {pair.Value} 个 XML program 条目");
        }

        bool EntryStartSector(RawProgramEntry entry, out ulong startSector)
        {
            if (entry.StartSectorResolved)
            {
                startSector = entry.StartSector;
                return true;
            }
            var diskSectors = _lunSectorCounts.TryGetValue(entry.Lun, out var count) ? count : 0;
            return ResolveSectorExpression(entry.StartSectorExpression, diskSectors, out startSector);
        }

        ulong RangeEnd(ulong start, ulong count)
        {
            if (count == 0) return start;
            return count - 1 > ulong.MaxValue - start
                ? ulong.MaxValue : start + count - 1;
        }
        bool ProtectedConflict(RawProgramEntry entry, ulong start, out string protectedName)
        {
            protectedName = string.Empty;
            var end = RangeEnd(start, entry.SectorCount);
            foreach (var protectedEntry in protectedRanges)
            {
                if (entry.Lun != protectedEntry.Lun) continue;
                if (!EntryStartSector(protectedEntry, out var protectedStart)) continue;
                var protectedEnd = RangeEnd(protectedStart, protectedEntry.SectorCount);
                if (start <= protectedEnd && protectedStart <= end)
                {
                    protectedName = protectedEntry.Label;
                    return true;
                }
            }
            return false;
        }

        foreach (var protectedEntry in protectedRanges)
        {
            if (EntryStartSector(protectedEntry, out var start))
            {
                LogMessage?.Invoke($"保护范围已锁定：{protectedEntry.Label}，LUN {protectedEntry.Lun}，扇区 {start}-{RangeEnd(start, protectedEntry.SectorCount)}");
            }
        }

        if (!allowProtected)
        {
            for (long index = plan.Entries.Count; index > 0; --index)
            {
                var entry = plan.Entries[(int)(index - 1)];
                if (!EntryStartSector(entry, out var start)) continue;
                if (!ProtectedConflict(entry, start, out var protectedName)) continue;
                LogMessage?.Invoke($"保护分区：XML 条目 {entry.Label} 与 {protectedName} 的物理范围重叠，已跳过");
                plan.Entries.RemoveAt((int)(index - 1));
            }
        }

        ulong totalBytes = 0;
        foreach (var entry in plan.Entries)
        {
            if (!EntryStartSector(entry, out var resolvedStart))
            {
                Fail($"LUN {entry.Lun} 容量未知，未能解析动态扇区：{entry.StartSectorExpression}。请重新读取分区表", false);
                return;
            }
            if (ProtectedConflict(entry, resolvedStart, out var protectedName))
            {
                Fail($"XML 条目 {entry.Label} 与受保护分区 {protectedName} 的物理范围重叠，任务已终止", false);
                return;
            }
            if (entry.SectorSize != SectorSize())
            {
                Fail($"{entry.Filename} 的扇区大小为 {entry.SectorSize}，当前存储配置为 {SectorSize()}", false);
                return;
            }
            if (entry.SectorCount > ulong.MaxValue / entry.SectorSize)
            {
                Fail($"XML 写入范围溢出：{entry.Filename}", false);
                return;
            }

            if (entry.DynamicSuper)
            {
                var dynamicPlan = new DynamicSuperPlan();
                if (!DynamicSuperPlanner.Create(entry.XmlPath, entry.SourcePath,
                                                dynamicPlan, out error))
                {
                    Fail($"super META 计划错误：{error}", false);
                    return;
                }
                if (dynamicPlan.BlockDeviceSize > entry.SectorCount * entry.SectorSize)
                {
                    Fail("super_def 容量大于 XML 中的 super 分区", false);
                    return;
                }
                totalBytes = SaturatingAdd(totalBytes, dynamicPlan.TotalWriteBytes);
                continue;
            }

            FileStream file;
            try
            {
                file = new FileStream(entry.SourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            }
            catch (Exception ex)
            {
                Fail($"打开镜像失败：{ex.Message}", false);
                return;
            }
            using (file)
            {
                var sparse = entry.Sparse || SparseImage.HasSparseMagic(file);
                if (sparse)
                {
                    if (entry.FileSectorOffset != 0)
                    {
                        Fail($"sparse 镜像要求 file_sector_offset 为 0：{entry.Filename}", false);
                        return;
                    }
                    if (!SparseImage.Inspect(file, out var sparseInfo, out var sparseError))
                    {
                        Fail($"{entry.Filename}：{sparseError}", false);
                        return;
                    }
                    if (sparseInfo.BlockSize % entry.SectorSize != 0 ||
                        sparseInfo.ExpandedSize > entry.SectorCount * entry.SectorSize)
                    {
                        Fail($"sparse 展开范围超出 XML 目标：{entry.Filename}", false);
                        return;
                    }
                    ulong sparseTransferBytes = 0;
                    foreach (var extent in sparseInfo.Extents)
                    {
                        if (extent.Type == SparseExtentType.Skip) continue;
                        sparseTransferBytes = SaturatingAdd(
                            sparseTransferBytes, extent.BlockCount * sparseInfo.BlockSize);
                    }
                    totalBytes = SaturatingAdd(totalBytes, sparseTransferBytes);
                }
                else
                {
                    var offset = entry.FileSectorOffset * entry.SectorSize;
                    var available = (ulong)file.Length - offset;
                    totalBytes = SaturatingAdd(totalBytes,
                                               Math.Min(available, entry.SectorCount * entry.SectorSize));
                }
            }
        }
        if (totalBytes == 0)
        {
            Fail("XML 中没有可传输的数据", false);
            return;
        }

        SetState(SessionState.Writing, "按 rawprogram XML 刷写");
        LogMessage?.Invoke($"XML 计划已载入：{plan.XmlPaths.Count} 个 XML，{plan.Entries.Count} 个 program 条目");
        foreach (var warning in plan.Warnings) LogMessage?.Invoke(warning);
        if (plan.HasSplitSuper())
        {
            LogMessage?.Invoke($"检测到 {plan.SuperEntryCount} 个 super 条目：启用免合并分段刷写");
        }

        ProgressChanged?.Invoke(0, 0);
        OverallProgressChanged?.Invoke(0, totalBytes);
        ulong completed = 0;
        var activeIndex = 0;
        var activeCount = plan.Entries.Count;

        void RestoreProgress()
        {
            if (_firehose == null) return;
            _firehose.SetProgressCallback((current, total) => ProgressChanged?.Invoke(current, total));
        }

        foreach (var entry in plan.Entries)
        {
            if (!EntryStartSector(entry, out var targetStartSector))
            {
                RestoreProgress();
                Fail($"解析动态扇区失败：{entry.StartSectorExpression}", false);
                return;
            }
            ++activeIndex;
            var sourceName = entry.Filename;
            var resolvedFileName = Path.GetFileName(entry.SourcePath);
            if (!entry.DynamicSuper && resolvedFileName.Length != 0 &&
                !string.Equals(resolvedFileName, entry.Filename, StringComparison.OrdinalIgnoreCase))
            {
                sourceName += $" [{resolvedFileName}]";
            }
            LogMessage?.Invoke($"XML {activeIndex}/{activeCount}：{sourceName} → {(entry.Label.Length == 0 ? "(无标签)" : entry.Label)}，LUN {entry.Lun}，起始扇区 {targetStartSector}");

            if (entry.DynamicSuper)
            {
                var dynamicPlan = new DynamicSuperPlan();
                if (!DynamicSuperPlanner.Create(entry.XmlPath, entry.SourcePath,
                                                dynamicPlan, out error))
                {
                    RestoreProgress();
                    Fail($"super META 计划错误：{error}", false);
                    return;
                }
                LogMessage?.Invoke(
                    $"当前 super.img 为空：开启免合并写入（META），LP {dynamicPlan.MetadataMajorVersion}.{dynamicPlan.MetadataMinorVersion}，{dynamicPlan.MetadataSlotCount} 个槽位，{dynamicPlan.Images.Count} 个镜像");
                LogMessage?.Invoke($"写入 super metadata：{dynamicPlan.MetadataImage.Length} 字节");
                using (var metadataBuffer = new MemoryStream(dynamicPlan.MetadataImage, false))
                {
                    if (!ProgramDeviceRange(metadataBuffer,
                                            (ulong)dynamicPlan.MetadataImage.Length,
                                            targetStartSector, entry.Lun, entry.SectorSize,
                                            "super", dynamicPlan.MetadataFilename,
                                            ref completed, totalBytes, out error))
                    {
                        RestoreProgress();
                        Fail($"super metadata 写入失败：{error}");
                        return;
                    }
                }

                for (long imageIndex = 0; imageIndex < dynamicPlan.Images.Count; ++imageIndex)
                {
                    var image = dynamicPlan.Images[(int)imageIndex];
                    LogMessage?.Invoke($"super {imageIndex + 1}/{dynamicPlan.Images.Count}：{image.PartitionName} ← {Path.GetFileName(image.SourcePath)}，分配 {image.AllocatedSize} 字节");
                    var source = new DynamicImageDevice(image);
                    if (!source.Initialize(out error))
                    {
                        RestoreProgress();
                        Fail($"动态分区 {image.PartitionName}：{error}", false);
                        return;
                    }
                    using (source)
                    {
                        foreach (var extent in image.Extents)
                        {
                            if (extent.PhysicalOffset % entry.SectorSize != 0 ||
                                extent.ByteCount % entry.SectorSize != 0)
                            {
                                RestoreProgress();
                                Fail($"动态分区 extent 未按 Firehose 扇区对齐：{image.PartitionName}", false);
                                return;
                            }
                            var targetSector = targetStartSector +
                                extent.PhysicalOffset / entry.SectorSize;
                            if (!ProgramDeviceRange(source, extent.ByteCount, targetSector,
                                                    entry.Lun, entry.SectorSize,
                                                    "super", dynamicPlan.MetadataFilename,
                                                    ref completed, totalBytes, out error))
                            {
                                RestoreProgress();
                                Fail($"动态分区 {image.PartitionName} 写入失败：{error}");
                                return;
                            }
                        }
                    }
                }
                continue;
            }

            FileStream file;
            try
            {
                file = new FileStream(entry.SourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            }
            catch (Exception ex)
            {
                RestoreProgress();
                Fail($"打开镜像失败：{ex.Message}", false);
                return;
            }
            using (file)
            {
                var resolvedPayload = Path.GetFileName(entry.SourcePath);
                var payloadFilename = resolvedPayload.Length == 0 ? entry.Filename : resolvedPayload;
                var sparse = entry.Sparse || SparseImage.HasSparseMagic(file);
                if (sparse)
                {
                    if (!SparseImage.Inspect(file, out var sparseInfo, out error))
                    {
                        RestoreProgress();
                        Fail($"{entry.Filename}：{error}", false);
                        return;
                    }
                    LogMessage?.Invoke($"Android sparse：{sparseInfo.TotalBlocks} 个块，展开 {sparseInfo.ExpandedSize} 字节，不生成临时 raw 文件");
                    foreach (var extent in sparseInfo.Extents)
                    {
                        var extentBytes = extent.BlockCount * sparseInfo.BlockSize;
                        var targetSector = targetStartSector +
                            extent.OutputBlock * sparseInfo.BlockSize / entry.SectorSize;
                        var ok = true;
                        if (extent.Type == SparseExtentType.Skip)
                        {
                            continue;
                        }
                        else if (extent.Type == SparseExtentType.Raw)
                        {
                            ok = ProgramFileRange(file, extent.FileOffset, extentBytes, targetSector,
                                                  entry.Lun, entry.SectorSize, entry.Label,
                                                  payloadFilename, ref completed, totalBytes, out error);
                        }
                        else
                        {
                            ok = ProgramFillRange(extent.FillValue, extentBytes, targetSector,
                                                  entry.Lun, entry.SectorSize, entry.Label,
                                                  payloadFilename, ref completed, totalBytes, out error);
                        }
                        if (!ok)
                        {
                            RestoreProgress();
                            Fail($"XML 条目 {entry.Filename} 写入失败：{error}");
                            return;
                        }
                    }
                }
                else
                {
                    var offset = entry.FileSectorOffset * entry.SectorSize;
                    var bytes = Math.Min((ulong)file.Length - offset,
                                         entry.SectorCount * entry.SectorSize);
                    if (!ProgramFileRange(file, offset, bytes, targetStartSector, entry.Lun,
                                          entry.SectorSize, entry.Label, payloadFilename,
                                          ref completed, totalBytes, out error))
                    {
                        RestoreProgress();
                        Fail($"XML 条目 {entry.Filename} 写入失败：{error}");
                        return;
                    }
                }
            }
        }

        var patchPaths = new List<string>();
        foreach (var xmlPath in plan.XmlPaths)
        {
            var xmlInfo = new FileInfo(xmlPath);
            var match = RawProgramName.Match(xmlInfo.Name);
            if (!match.Success) continue;
            var patchPath = Path.Combine(xmlInfo.Directory?.FullName ?? string.Empty,
                                         $"patch{match.Groups[1].Value}.xml");
            if (File.Exists(patchPath) &&
                !patchPaths.Contains(patchPath, StringComparer.OrdinalIgnoreCase))
            {
                patchPaths.Add(patchPath);
            }
        }

        var appliedPatches = 0;
        foreach (var patchPath in patchPaths)
        {
            LogMessage?.Invoke($"Patch：{Path.GetFileName(patchPath)}");
            FileStream patchFile;
            try
            {
                patchFile = new FileStream(patchPath, FileMode.Open, FileAccess.Read, FileShare.Read);
            }
            catch (Exception ex)
            {
                RestoreProgress();
                Fail($"打开 Patch XML 失败：{ex.Message}", false);
                return;
            }
            using (patchFile)
            {
                var settings = new XmlReaderSettings
                {
                    ConformanceLevel = ConformanceLevel.Document,
                    DtdProcessing = DtdProcessing.Ignore,
                    IgnoreComments = true,
                    IgnoreWhitespace = true,
                };
                XmlReader reader;
                try
                {
                    reader = XmlReader.Create(patchFile, settings);
                }
                catch (Exception ex)
                {
                    RestoreProgress();
                    Fail($"打开 Patch XML 失败：{ex.Message}", false);
                    return;
                }
                using (reader)
                {
                    while (true)
                    {
                        bool moved;
                        try
                        {
                            moved = reader.Read();
                        }
                        catch (XmlException ex)
                        {
                            RestoreProgress();
                            Fail($"Patch XML 解析失败：{ex.Message}", false);
                            return;
                        }
                        if (!moved) break;
                        if (reader.NodeType != XmlNodeType.Element ||
                            !string.Equals(reader.Name, "patch", StringComparison.OrdinalIgnoreCase))
                        {
                            continue;
                        }
                        if (!string.Equals(reader.GetAttribute("filename"), "DISK",
                                           StringComparison.OrdinalIgnoreCase))
                        {
                            continue;
                        }
                        ulong patchSectorSize = 0;
                        ulong byteOffset = 0;
                        ulong lun = 0;
                        ulong sizeInBytes = 0;
                        if (!ParseUnsignedText(reader.GetAttribute("SECTOR_SIZE_IN_BYTES"), out patchSectorSize) ||
                            !ParseUnsignedText(reader.GetAttribute("byte_offset"), out byteOffset) ||
                            !ParseUnsignedText(reader.GetAttribute("physical_partition_number"), out lun) ||
                            !ParseUnsignedText(reader.GetAttribute("size_in_bytes"), out sizeInBytes) ||
                            lun > uint.MaxValue)
                        {
                            RestoreProgress();
                            Fail($"Patch XML 参数错误：{Path.GetFileName(patchPath)}", false);
                            return;
                        }
                        var diskSectors = _lunSectorCounts.TryGetValue((uint)lun, out var count) ? count : 0;
                        if (!ResolvePatchExpression(reader.GetAttribute("start_sector") ?? string.Empty,
                                                    diskSectors, out var startSector) ||
                            !ResolvePatchExpression(reader.GetAttribute("value") ?? string.Empty,
                                                    diskSectors, out var value))
                        {
                            RestoreProgress();
                            Fail($"Patch 表达式解析失败：LUN {lun}", false);
                            return;
                        }
                        if (!_firehose!.Patch((uint)patchSectorSize, byteOffset, (uint)lun,
                                              sizeInBytes, startSector, value, out error))
                        {
                            RestoreProgress();
                            Fail($"Patch {Path.GetFileName(patchPath)} 应用失败：{error}");
                            return;
                        }
                        ++appliedPatches;
                    }
                }
            }
        }
        if (appliedPatches > 0)
        {
            LogMessage?.Invoke($"GPT Patch 应用完成：{appliedPatches} 条 DISK patch");
        }

        LogMessage?.Invoke("设置启动分区…");
        var bootableValue = _storage == StorageType.Ufs ? 1U : 0U;
        if (_firehose!.SetBootableStorageDrive(bootableValue, out error))
        {
            LogMessage?.Invoke($"设置启动分区完成：value={bootableValue}");
        }
        else
        {
            LogMessage?.Invoke($"设置启动分区未确认，保留已完成刷写：{error}");
        }

        RestoreProgress();
        Finish($"XML 刷机完成：{activeCount} 个 program 条目，super 全程免合并");
    }

    // ------------------------------------------------------------------ 擦除

    public void ErasePartition(PartitionInfo partition)
    {
        if (!EnsureReady("擦除分区")) return;
        SetState(SessionState.Erasing, $"擦除 {partition.Name}");
        ProgressChanged?.Invoke(0, 1);
        OverallProgressChanged?.Invoke(0, 1);

        if (!_firehose!.Erase(partition.FirstLba, partition.SectorCount, partition.Lun,
                              SectorSize(), out var error, partition.Name,
                              partition.Name + ".bin"))
        {
            Fail(error);
            return;
        }
        ProgressChanged?.Invoke(1, 1);
        OverallProgressChanged?.Invoke(1, 1);
        Finish($"分区 {partition.Name} 擦除完成");
    }

    public void ErasePartitions(IReadOnlyList<PartitionInfo> partitions,
                                bool allowProtected = false)
    {
        if (!EnsureReady("批量擦除分区")) return;
        var filtered = new List<PartitionInfo>();
        foreach (var partition in partitions)
        {
            if (!allowProtected && PartitionProtection.IsProtectedPartitionName(partition.Name))
            {
                LogMessage?.Invoke($"保护分区：跳过擦除 {partition.Name}");
            }
            else
            {
                filtered.Add(partition);
            }
        }
        if (filtered.Count == 0)
        {
            Fail("批量擦除列表为空", false);
            return;
        }
        SetState(SessionState.Erasing, $"批量擦除 {filtered.Count} 个分区");
        // C++: setProgressCallback({}) —— 置空回调（擦除阶段不报进度）。
        _firehose!.SetProgressCallback(null!);
        ProgressChanged?.Invoke(0, 1);
        OverallProgressChanged?.Invoke(0, (ulong)filtered.Count);
        for (long index = 0; index < filtered.Count; ++index)
        {
            if (_cancelled.IsCancelled)
            {
                Fail("操作已停止", false);
                return;
            }
            var partition = filtered[(int)index];
            ProgressChanged?.Invoke(0, 1);
            LogMessage?.Invoke($"擦除 {index + 1}/{filtered.Count}：{partition.Name}（LUN {partition.Lun}，起始扇区 {partition.FirstLba}）");
            if (!_firehose.Erase(partition.FirstLba, partition.SectorCount, partition.Lun,
                                 SectorSize(), out var error, partition.Name,
                                 partition.Name + ".bin"))
            {
                Fail($"擦除分区 {partition.Name} 失败：{error}");
                return;
            }
            ProgressChanged?.Invoke(1, 1);
            OverallProgressChanged?.Invoke((ulong)(index + 1), (ulong)filtered.Count);
        }
        _firehose.SetProgressCallback((current, total) => ProgressChanged?.Invoke(current, total));
        Finish($"批量擦除完成：{filtered.Count} 个分区");
    }

    // ------------------------------------------------------------------ 重启 / 停止

    public void RebootDevice(bool toEdl = false)
    {
        var destination = toEdl ? "9008 模式" : "系统";
        if (!EnsureReady($"重启到{destination}")) return;
        SetState(SessionState.Rebooting, $"重启到{destination}");
        ProgressChanged?.Invoke(0, 1);
        OverallProgressChanged?.Invoke(0, 1);
        if (!_firehose!.Reboot(toEdl, out var error))
        {
            Fail(error);
            return;
        }
        ProgressChanged?.Invoke(1, 1);
        OverallProgressChanged?.Invoke(1, 1);
        _firehose = null;
        _transport!.Close();
        _deviceInfo = new DeviceInfo();
        _lunSectorCounts.Clear();
        _currentPartitions.Clear();
        DeviceInfoChanged?.Invoke(_deviceInfo);
        PartitionsReady?.Invoke(new List<PartitionInfo>());
        _busy = false;
        SetState(SessionState.Disconnected, $"设备正在重启到{destination}");
        OperationFinished?.Invoke(true, $"重启到{destination}命令已发送");
    }

    public void RequestStop()
    {
        _cancelled.Cancel();
    }
}
