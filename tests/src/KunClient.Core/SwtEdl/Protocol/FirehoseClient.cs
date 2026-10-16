using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Xml;
using KunFlashTool.Core.SwtEdl;
using KunFlashTool.Core.SwtEdl.Transport;

namespace KunFlashTool.Core.SwtEdl.Protocol;

public sealed class FirehoseResponse
{
    public bool Ack { get; set; }
    public bool Nak { get; set; }
    public bool RawMode { get; set; }
    public ulong NegotiatedPayload { get; set; }
    public List<string> Messages { get; } = new();   // 对应 QStringList messages
    public string Xml { get; set; } = "";
}

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/protocol/Firehose.{h,cpp}`。
/// </summary>
public sealed class FirehoseClient
{
    private static readonly Regex ChipPattern = new(
        @"Chip\s+serial\s+num\s*\(0x([0-9a-f]+)\).*Chip\s+ID\s*\(0x([0-9a-f]+)\)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex MsmPattern = new(
        @"MSM[_ ]ID\s*[:=]\s*([A-Za-z0-9_-]+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex VipRequestPattern = new(
        @"receiving\s+the\s+partition\s+info\s+of\s+size\s+(\d+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex VipAcceptedPattern = new(
        @"partition_num\s*=\s*(\d+).*partition_info_size\s*=\s*(\d+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex InvalidTokenPattern = new(
        "[^A-Za-z0-9_.-]", RegexOptions.Compiled);

    private static readonly byte[] XmlDeclarationMarker = Encoding.ASCII.GetBytes("<?xml");
    private static readonly byte[] DataStartMarker = Encoding.ASCII.GetBytes("<data");
    private static readonly byte[] DataEndMarker = Encoding.ASCII.GetBytes("</data>");

    private readonly SerialTransport _transport;
    private readonly EdlCancel _cancelled;
    private readonly LogCallback? _log;
    private ProgressCallback? _progress;
    private ulong _maximumPayload = 1024 * 1024;
    private bool _oplusVip = false;
    private Dictionary<string, string> _vipSignedFilenames = new();
    private string _detectedSerial = "";
    private string _detectedChipId = "";
    private string _detectedModel = "";
    private string _detectedStorageType = "";
    private string _detectedStorageProduct = "";
    private ulong _detectedStorageBytes = 0;

    public FirehoseClient(SerialTransport transport, EdlCancel cancelled, LogCallback log, ProgressCallback progress)
    {
        _transport = transport;
        _cancelled = cancelled;
        _log = log;
        _progress = progress;
    }

    private sealed class VipIdentity
    {
        public string Label { get; set; } = "";
        public string Filename { get; set; } = "";
    }

    private static string XmlEnvelope(string element)
        => $"<?xml version=\"1.0\" ?><data>{element}</data>";

    private static string JoinedMessages(FirehoseResponse response)
        => response.Messages.Count == 0 ? "目标返回 NAK" : string.Join(" | ", response.Messages);

    /// <summary>
    /// 设备在认证前只接受 nop / sig 两类标签 —— 这是**安全启动（Secure Boot）机型**的标志：
    /// BootROM 已放行 Programmer（Sahara 校验通过），但 Firehose 侧要求先做签名认证
    /// （<c>sig</c> / VIP Digest→Verify→Sign→SHA-256→Configure），
    /// 否则除了 nop / sig 以外的任何命令（包括 Configure）都会被回 NAK。
    /// </summary>
    private const string AuthRequiredMarker =
        "Only nop and sig tag can be recevied before authentication";

    internal static bool IsAuthRequired(FirehoseResponse response)
    {
        foreach (var message in response.Messages)
        {
            if (message.Contains(AuthRequiredMarker, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    /// <summary>安全启动机型在认证前拒命令时的**可执行**错误文案（不是笼统的「返回 NAK」）。</summary>
    internal static string AuthRequiredMessage(string operation)
        => $"{operation}失败：设备要求在写入前完成签名认证。"
           + "这是安全启动机型的正常限制：BootROM 已放行 Programmer，但 Firehose 只接受 nop / sig，"
           + "必须先走授权通道（勾选「OPPO VIP」并选择带 Digest + Sign 的引导包）才能执行读取 / 写入。";

    /// <summary>
    /// 对应 C++ 的 <c>fixedAsciiField(QByteArrayView, QString*)</c>：这里传入数据 + 起始 + 长度以代替切片。
    /// </summary>
    private static bool FixedAsciiField(byte[] data, int start, int length, out string value)
    {
        value = string.Empty;
        var end = length;
        for (var i = 0; i < length; ++i)
        {
            if (data[start + i] == 0)
            {
                end = i;
                break;
            }
        }
        for (var i = 0; i < end; ++i)
        {
            var ch = data[start + i];
            if (ch < 0x20 || ch > 0x7e) return false;
        }
        for (var i = end; i < length; ++i)
        {
            if (data[start + i] != 0) return false;
        }
        value = Encoding.Latin1.GetString(data, start, end).Trim();
        return true;
    }

    /// <summary>
    /// 对应 C++ 的 <c>QByteArray::indexOf(needle, from)</c>：返回字节序列首次出现的位置，找不到返回 -1。
    /// </summary>
    private static long IndexOf(byte[] haystack, byte[] needle, long from)
    {
        var limit = (long)haystack.Length - needle.Length;
        for (var i = from; i <= limit; ++i)
        {
            var found = true;
            for (var j = 0; j < needle.Length; ++j)
            {
                if (haystack[i + j] != needle[j])
                {
                    found = false;
                    break;
                }
            }
            if (found) return i;
        }
        return -1;
    }

    /// <summary>对应 C++ 的 <c>QIODevice::readAll()</c>（读取当前位置到末尾）。</summary>
    private static byte[] ReadAll(Stream file)
    {
        using var memory = new MemoryStream();
        file.CopyTo(memory);
        return memory.ToArray();
    }

    private static bool TryReadAll(Stream file, out byte[] data, out string error)
    {
        error = string.Empty;
        data = Array.Empty<byte>();
        try
        {
            data = ReadAll(file);
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    private static Dictionary<string, string> ParseVipSignedFilenames(string digestPath)
    {
        const long recordSize = 132;
        const long labelOffset = 32;
        const long filenameOffset = 64;
        const long fieldSize = 32;

        FileStream file;
        try
        {
            file = new FileStream(digestPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        }
        catch
        {
            return new Dictionary<string, string>();
        }

        byte[] data;
        using (file)
        {
            if (!TryReadAll(file, out data, out _)) return new Dictionary<string, string>();
        }
        if (data.Length < recordSize) return new Dictionary<string, string>();

        // OPPO Digest variants wrap the same 132-byte partition records in several
        // different containers. Determine the record alignment from stable labels
        // instead of assuming one ELF/program-header layout.
        var alignmentVotes = new Dictionary<long, int>();
        var anchors = new[]
        {
            Encoding.ASCII.GetBytes("ssd\0"), Encoding.ASCII.GetBytes("persist\0"),
            Encoding.ASCII.GetBytes("misc\0"), Encoding.ASCII.GetBytes("PrimaryGPT\0"),
            Encoding.ASCII.GetBytes("BackupGPT\0"),
        };
        foreach (var anchor in anchors)
        {
            long position = 0;
            while ((position = IndexOf(data, anchor, position)) >= 0)
            {
                if (position >= labelOffset)
                {
                    var key = (position - labelOffset) % recordSize;
                    alignmentVotes[key] = alignmentVotes.TryGetValue(key, out var votes) ? votes + 1 : 1;
                }
                position += anchor.Length;
            }
        }

        // ⚠️ C# 的 Dictionary 枚举顺序与 Qt 的 QHash 不同，但这里和 C++ 一样只在「严格更大」时替换，
        //    所以并列票数的取舍结果与 C++ 结构一致。
        long alignment = -1;
        var bestVotes = 0;
        foreach (var vote in alignmentVotes)
        {
            if (vote.Value > bestVotes)
            {
                alignment = vote.Key;
                bestVotes = vote.Value;
            }
        }
        if (alignment < 0 || bestVotes < 2) return new Dictionary<string, string>();

        var result = new Dictionary<string, string>();
        for (long record = alignment; record + recordSize <= data.Length; record += recordSize)
        {
            if (!FixedAsciiField(data, (int)(record + labelOffset), (int)fieldSize, out var label) ||
                !FixedAsciiField(data, (int)(record + filenameOffset), (int)fieldSize, out var filename) ||
                label.Length == 0)
            {
                continue;
            }

            var flagsValid = true;
            for (long offset = 96; offset < 116; offset += 4)
            {
                var flag = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan((int)(record + offset), 4));
                if (flag > 1)
                {
                    flagsValid = false;
                    break;
                }
            }
            if (!flagsValid) continue;
            result[label.ToLowerInvariant()] = filename;
        }
        return result;
    }

    private static string FirehoseToken(string value, string fallback)
    {
        value = Path.GetFileName(value).Trim();
        value = InvalidTokenPattern.Replace(value, "_");
        if (value.Length > 96) value = value.Substring(0, 96);
        return value.Length == 0 ? fallback : value;
    }

    private static void AppendVipIdentity(List<VipIdentity> identities, string label, string filename)
    {
        foreach (var current in identities)
        {
            if (string.Equals(current.Label, label, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(current.Filename, filename, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }
        }
        identities.Add(new VipIdentity { Label = label, Filename = filename });
    }

    private static bool IsBenignFirehoseNoise(string message)
    {
        // A USB zero-length packet may be reported as command id 0 by OPPO's
        // programmer. It is transport framing rather than an operation failure;
        // the following XML command is still parsed and acknowledged normally.
        return string.Equals(message.Trim(),
                             "ERROR: Failed to read XML command 0",
                             StringComparison.OrdinalIgnoreCase);
    }

    private static List<VipIdentity> VipIdentityStrategies(Dictionary<string, string> signedFilenames,
                                                           string label, string filename,
                                                           uint lun, bool directGptRead = false)
    {
        var result = new List<VipIdentity>();
        if (directGptRead)
        {
            result.Add(new VipIdentity { Label = "PrimaryGPT", Filename = $"gpt_main{lun}.bin" });
            return result;
        }

        var safeLabel = FirehoseToken(label, "buffer");

        // PrimaryGPT/BackupGPT occur once per UFS LUN. A label-only hash keeps the
        // last record, so always derive these filenames from the requested LUN.
        if (string.Equals(safeLabel, "PrimaryGPT", StringComparison.OrdinalIgnoreCase))
        {
            AppendVipIdentity(result, "PrimaryGPT", $"gpt_main{lun}.bin");
        }
        else if (string.Equals(safeLabel, "BackupGPT", StringComparison.OrdinalIgnoreCase))
        {
            AppendVipIdentity(result, "BackupGPT", $"gpt_backup{lun}.bin");
        }

        if (signedFilenames.TryGetValue(label.Trim().ToLowerInvariant(), out var signedFile) &&
            signedFile.Length != 0)
        {
            AppendVipIdentity(result, safeLabel, FirehoseToken(signedFile, "buffer.bin"));
        }

        AppendVipIdentity(result, "BackupGPT", $"gpt_backup{lun}.bin");
        AppendVipIdentity(result, safeLabel, "gpt_backup0.bin");
        AppendVipIdentity(result, safeLabel, FirehoseToken(filename, safeLabel + ".bin"));
        AppendVipIdentity(result, safeLabel, safeLabel + ".img");
        AppendVipIdentity(result, safeLabel, safeLabel + ".bin");
        AppendVipIdentity(result, "ssd", "ssd");
        AppendVipIdentity(result, "buffer", "buffer.bin");
        AppendVipIdentity(result, "", "");
        return result;
    }

    private static string VipAttributes(VipIdentity identity, string suffix = "")
    {
        if (identity.Label.Length == 0) return suffix;
        if (identity.Filename.Length == 0)
        {
            return $" label=\"{identity.Label}\"{suffix}";
        }
        return $" filename=\"{identity.Filename}\" label=\"{identity.Label}\"{suffix}";
    }

    private static string JsonValueToString(JsonElement obj, string name)
    {
        return obj.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";
    }

    private static ulong JsonValueToULongLong(JsonElement obj, string name)
    {
        if (!obj.TryGetProperty(name, out var value)) return 0;
        switch (value.ValueKind)
        {
            case JsonValueKind.Number:
                if (value.TryGetUInt64(out var unsigned)) return unsigned;
                return value.TryGetDouble(out var number) ? (ulong)number : 0;
            case JsonValueKind.String:
                return ulong.TryParse(value.GetString(), NumberStyles.Integer,
                                      CultureInfo.InvariantCulture, out var parsed)
                    ? parsed : 0;
            default:
                return 0;
        }
    }

    private void ObserveMessages(List<string> messages)
    {
        foreach (var message in messages)
        {
            var chipMatch = ChipPattern.Match(message);
            if (chipMatch.Success)
            {
                _detectedSerial = chipMatch.Groups[1].Value.ToLowerInvariant();
                var chip = chipMatch.Groups[2].Value.ToLowerInvariant();
                if (ulong.TryParse(chip, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var chipValue) &&
                    chipValue != 0)
                {
                    _detectedChipId = chip;
                }
            }
            var chipName = DeviceIdentity.NormalizeQualcommChipName(message);
            if (chipName.Length != 0) _detectedModel = chipName;
            var msmMatch = MsmPattern.Match(message);
            if (msmMatch.Success && _detectedModel.Length == 0)
            {
                _detectedModel = msmMatch.Groups[1].Value.ToUpperInvariant();
            }

            var jsonStart = message.IndexOf('{');
            if (jsonStart < 0) continue;
            JsonDocument document;
            try
            {
                document = JsonDocument.Parse(message.Substring(jsonStart));
            }
            catch (JsonException)
            {
                continue;
            }
            using (document)
            {
                if (document.RootElement.ValueKind != JsonValueKind.Object) continue;
                if (!document.RootElement.TryGetProperty("storage_info", out var storage) ||
                    storage.ValueKind != JsonValueKind.Object)
                {
                    continue;
                }
                if (!storage.EnumerateObject().Any()) continue;
                var memory = JsonValueToString(storage, "mem_type").Trim();
                var product = JsonValueToString(storage, "prod_name").Trim();
                if (memory.Length != 0) _detectedStorageType = memory.ToUpperInvariant();
                if (product.Length != 0) _detectedStorageProduct = product;
                var totalBlocks = JsonValueToULongLong(storage, "total_blocks");
                var blockSize = JsonValueToULongLong(storage, "block_size");
                if (totalBlocks > 0 && blockSize > 0 &&
                    totalBlocks <= ulong.MaxValue / blockSize)
                {
                    _detectedStorageBytes = totalBlocks * blockSize;
                }
            }
        }
    }

    public void EnrichDeviceInfo(DeviceInfo info)
    {
        if (info == null) return;
        if (info.Serial.Length == 0) info.Serial = _detectedSerial;
        // Firehose runtime output describes the programmer actually accepted by
        // this device, so it takes precedence over the package-file hint.
        if (_detectedModel.Length != 0) info.Model = DeviceIdentity.QualcommChipDisplayName(_detectedModel);
        if (info.Hwid.Length == 0)
        {
            info.Hwid = _detectedChipId.Length != 0
                ? _detectedChipId + _detectedSerial : _detectedSerial;
        }
        if (_detectedStorageType.Length != 0) info.Storage = _detectedStorageType;
        info.StorageProduct = _detectedStorageProduct;
        info.StorageBytes = _detectedStorageBytes;
    }

    private static FirehoseResponse ParseResponse(byte[] frame)
    {
        var result = new FirehoseResponse();
        var xmlStart = IndexOf(frame, XmlDeclarationMarker, 0);
        if (xmlStart < 0) xmlStart = IndexOf(frame, DataStartMarker, 0);
        var xmlFrame = xmlStart > 0 ? frame[(int)xmlStart..] : frame;
        result.Xml = Encoding.UTF8.GetString(xmlFrame);
        // C++ 的 QXmlStreamReader 把开头的 XML 声明当作 StartDocument（不是 start 元素）。
        // XmlReader 的 Fragment 模式对声明更挑，这里先剥掉声明再喂给它，result.Xml 保持原文。
        var parseText = result.Xml;
        if (parseText.StartsWith("<?xml", StringComparison.Ordinal))
        {
            var declarationEnd = parseText.IndexOf("?>", StringComparison.Ordinal);
            if (declarationEnd >= 0) parseText = parseText.Substring(declarationEnd + 2);
        }
        try
        {
            var settings = new XmlReaderSettings
            {
                ConformanceLevel = ConformanceLevel.Fragment,
                DtdProcessing = DtdProcessing.Ignore,
            };
            // C++ 用 QXmlStreamReader；这里用 XmlReader 逐节点走，只取 start 元素的属性。
            using var stringReader = new StringReader(parseText);
            using var reader = XmlReader.Create(stringReader, settings);
            while (reader.Read())
            {
                if (reader.NodeType != XmlNodeType.Element) continue;
                for (var i = 0; i < reader.AttributeCount; ++i)
                {
                    reader.MoveToAttribute(i);
                    var name = reader.Name;
                    var value = reader.Value;
                    if (string.Equals(name, "value", StringComparison.OrdinalIgnoreCase))
                    {
                        if (string.Equals(value, "ACK", StringComparison.OrdinalIgnoreCase)) result.Ack = true;
                        if (string.Equals(value, "NAK", StringComparison.OrdinalIgnoreCase)) result.Nak = true;
                    }
                    if (string.Equals(name, "rawmode", StringComparison.OrdinalIgnoreCase))
                    {
                        result.RawMode = string.Equals(value, "true", StringComparison.OrdinalIgnoreCase);
                    }
                    if (string.Equals(name, "MaxPayloadSizeToTargetInBytes", StringComparison.OrdinalIgnoreCase))
                    {
                        if (ulong.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var payload))
                        {
                            result.NegotiatedPayload = payload;
                        }
                    }
                    if (string.Equals(name, "value", StringComparison.OrdinalIgnoreCase) &&
                        !string.Equals(value, "ACK", StringComparison.OrdinalIgnoreCase) &&
                        !string.Equals(value, "NAK", StringComparison.OrdinalIgnoreCase))
                    {
                        result.Messages.Add(value);
                    }
                }
                reader.MoveToElement();
            }
        }
        catch (XmlException ex)
        {
            result.Messages.Add(ex.Message);
        }
        return result;
    }

    private bool ReceiveResponse(out FirehoseResponse response, int timeoutMs, out string error)
    {
        response = new FirehoseResponse();
        error = string.Empty;
        var timer = Stopwatch.StartNew();
        // Modern OPPO programmers emit one <log> frame per supported function before
        // acknowledging the first VIP blob. SM8550 commonly exceeds 32 XML frames.
        for (var frameIndex = 0; frameIndex < 1024; ++frameIndex)
        {
            var remaining = timeoutMs - (int)timer.ElapsedMilliseconds;
            if (remaining <= 0)
            {
                error = "等待 Firehose ACK 超时";
                return false;
            }
            if (!_transport.ReadUntil(DataEndMarker, out var frame, 2 * 1024 * 1024,
                                      remaining, out error))
            {
                return false;
            }
            var current = ParseResponse(frame);
            ObserveMessages(current.Messages);
            response.Xml += current.Xml;
            response.Messages.AddRange(current.Messages);
            response.Ack = response.Ack || current.Ack;
            response.Nak = response.Nak || current.Nak;
            response.RawMode = current.RawMode;
            if (current.NegotiatedPayload > 0)
            {
                response.NegotiatedPayload = current.NegotiatedPayload;
            }
            foreach (var message in current.Messages)
            {
                if (IsBenignFirehoseNoise(message)) continue;
                _log?.Invoke($"Firehose: {message}");
            }
            if (current.Ack || current.Nak) return true;
        }
        error = "Firehose 日志帧超过 1024，未发现 ACK/NAK";
        return false;
    }

    private bool WaitForVipPartitionRequest(out ulong requestedCapacity, int timeoutMs, out string error)
    {
        requestedCapacity = 0;
        error = string.Empty;
        var timer = Stopwatch.StartNew();

        for (var frameIndex = 0; frameIndex < 1024; ++frameIndex)
        {
            var remaining = timeoutMs - (int)timer.ElapsedMilliseconds;
            if (remaining <= 0)
            {
                error = "等待 VIP Partition Info 请求超时";
                return false;
            }
            if (!_transport.ReadUntil(DataEndMarker, out var frame, 2 * 1024 * 1024,
                                      remaining, out error))
            {
                return false;
            }
            var current = ParseResponse(frame);
            ObserveMessages(current.Messages);
            foreach (var message in current.Messages)
            {
                if (IsBenignFirehoseNoise(message)) continue;
                _log?.Invoke($"Firehose: {message}");
                var match = VipRequestPattern.Match(message);
                if (!match.Success) continue;
                if (!ulong.TryParse(match.Groups[1].Value, NumberStyles.Integer,
                                    CultureInfo.InvariantCulture, out var requested) ||
                    requested == 0 || requested > 16 * 1024 * 1024)
                {
                    error = $"VIP Partition Info 容量异常：{match.Groups[1].Value}";
                    return false;
                }
                requestedCapacity = requested;
                return true;
            }
            if (current.Nak)
            {
                error = $"Programmer 在 VIP 请求前返回 NAK：{JoinedMessages(current)}";
                return false;
            }
        }
        error = "VIP 启动日志帧超过 1024，未发现 Partition Info 请求";
        return false;
    }

    private bool ReceiveVipPartitionResult(out FirehoseResponse response, int timeoutMs, out string error)
    {
        response = new FirehoseResponse();
        error = string.Empty;
        var timer = Stopwatch.StartNew();

        for (var frameIndex = 0; frameIndex < 256; ++frameIndex)
        {
            var remaining = timeoutMs - (int)timer.ElapsedMilliseconds;
            if (remaining <= 0)
            {
                error = "等待 VIP Partition Info 解析结果超时";
                return false;
            }
            if (!_transport.ReadUntil(DataEndMarker, out var frame, 2 * 1024 * 1024,
                                      remaining, out error))
            {
                return false;
            }
            var current = ParseResponse(frame);
            ObserveMessages(current.Messages);
            response.Xml += current.Xml;
            response.Messages.AddRange(current.Messages);
            response.Ack = response.Ack || current.Ack;
            response.Nak = response.Nak || current.Nak;
            foreach (var message in current.Messages)
            {
                if (IsBenignFirehoseNoise(message)) continue;
                _log?.Invoke($"Firehose: {message}");
                var match = VipAcceptedPattern.Match(message);
                if (!match.Success) continue;
                response.Ack = true;
                _log?.Invoke($"VIP Partition Info 已接收：{match.Groups[1].Value} 个分区，记录 {match.Groups[2].Value} 字节");
                return true;
            }
            if (current.Ack || current.Nak) return true;
        }
        error = "VIP Partition Info 响应帧超过 256";
        return false;
    }

    private static string FirehoseCommandTag(string xml)
    {
        // 对应 C++：xml.section('<', 3, 3).section(' ', 0, 0)（取出 <data><xxx ... 的标签名）。
        var fields = xml.Split('<');
        if (fields.Length <= 3) return string.Empty;
        var inner = fields[3].Split(' ');
        return inner.Length > 0 ? inner[0] : string.Empty;
    }

    private bool Transact(string xml, out FirehoseResponse response, int timeoutMs, out string error)
    {
        // C++ 侧 receiveResponse 会先 *response = {}；writeAll 失败时不经过它，这里同样先重置。
        response = new FirehoseResponse();
        error = string.Empty;
        var request = Encoding.UTF8.GetBytes(xml);
        _log?.Invoke($"发送 Firehose 命令：{FirehoseCommandTag(xml)}");
        if (!_transport.WriteAll(request, timeoutMs, out error)) return false;
        return ReceiveResponse(out response, timeoutMs, out error);
    }

    private static bool CheckAck(FirehoseResponse response, string operation, out string error)
    {
        error = string.Empty;
        if (response.Ack && !response.Nak) return true;
        error = IsAuthRequired(response)
            ? AuthRequiredMessage(operation)
            : $"{operation}失败：{JoinedMessages(response)}";
        return false;
    }

    public bool Configure(StorageType storage, out string error)
    {
        error = string.Empty;
        var memory = EdlTypes.StorageName(storage);
        for (var attempt = 0; attempt < 2; ++attempt)
        {
            var command = XmlEnvelope(
                $"<configure MemoryName=\"{memory}\" Verbose=\"0\" AlwaysValidate=\"0\" " +
                $"MaxPayloadSizeToTargetInBytes=\"{_maximumPayload}\" ZlpAwareHost=\"1\" SkipStorageInit=\"0\" " +
                "CheckDevinfo=\"0\" EnableFlash=\"1\" />");

            if (!Transact(command, out var response, 8000, out error)) return false;
            if (response.NegotiatedPayload > 0)
            {
                _maximumPayload = Math.Clamp(response.NegotiatedPayload, 4096UL, 16UL * 1024 * 1024);
            }
            if (response.Ack && !response.Nak)
            {
                _log?.Invoke($"Firehose 已配置，Payload {_maximumPayload / 1024} KiB");
                return true;
            }
            // 设备在认证前拒绝非 nop/sig 命令 → 这是安全启动机型，Configure 必然 NAK，
            // 立刻把原因（而不是笼统的「返回 NAK」）交给上层，避免无意义的二次重试。
            if (IsAuthRequired(response))
            {
                error = AuthRequiredMessage("Firehose Configure");
                return false;
            }
        }
        error = "Firehose Configure 返回 NAK";
        return false;
    }

    private bool SendVipFile(string path, string label, out string error)
    {
        error = string.Empty;
        FileStream file;
        try
        {
            file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        }
        catch (Exception ex)
        {
            error = $"打开 {label} 失败：{ex.Message}";
            return false;
        }
        using (file)
        {
            var size = file.Length;
            if (size <= 0 || size > 16 * 1024 * 1024)
            {
                error = $"{label} 文件大小异常：{size}";
                return false;
            }
            if (!TryReadAll(file, out var payload, out var readError) || payload.LongLength != size)
            {
                error = $"读取 {label} 失败：{readError}";
                return false;
            }

            _log?.Invoke($"原生 VIP RAW：发送 {Path.GetFileName(path)}（{payload.Length} 字节）");
            if (!_transport.WriteAll(payload, 30000, out error)) return false;

            if (!ReceiveResponse(out var response, 20000, out error)) return false;
            return CheckAck(response, label, out error);
        }
    }

    private bool SendInitialVipPartition(string path, out string error)
    {
        error = string.Empty;
        FileStream file;
        try
        {
            file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        }
        catch (Exception ex)
        {
            error = $"打开 Digest 失败：{ex.Message}";
            return false;
        }
        byte[] payload;
        using (file)
        {
            // QFile::readAll() 失败时为空数组；读失败这里同样落到下面的「文件大小异常」分支。
            TryReadAll(file, out payload, out _);
        }
        if (payload.Length == 0 || payload.LongLength > 16 * 1024 * 1024)
        {
            error = $"Digest 文件大小异常：{payload.Length}";
            return false;
        }

        ulong requestedCapacity = 0;
        _log?.Invoke("等待 Programmer 请求 VIP Partition Info…");
        var explicitRequest = WaitForVipPartitionRequest(out requestedCapacity, 800, out var requestError);
        if (!explicitRequest)
        {
            if (_cancelled.IsCancelled)
            {
                error = requestError;
                return false;
            }
            // Several OPPO programmers enable a short boot watchdog before their
            // verbose Firehose banner has fully drained. Qualcomm host tools are
            // tolerant of the host queuing Digest early, so race the watchdog when
            // an explicit size request has not arrived in the first window.
            _log?.Invoke(
                $"VIP 请求窗口内未见 Partition Info，立即预发送 Digest，避免 Programmer 看门狗复位（{requestError}）");
            requestedCapacity = Math.Max(36864UL, (ulong)payload.Length);
        }
        if ((ulong)payload.Length > requestedCapacity)
        {
            error = $"Digest 大于 Programmer 缓冲区：{payload.Length}/{requestedCapacity} 字节";
            return false;
        }

        _log?.Invoke($"原生 VIP RAW：按设备请求发送 {Path.GetFileName(path)}（{payload.Length}/{requestedCapacity} 字节）");
        // The size in the request is target buffer capacity, not the transfer size.
        // Qualcomm's host sends this 28,472-byte ELF as-is, without 36,864-byte padding.
        if (!_transport.WriteAll(payload, 30000, out error)) return false;

        if (!ReceiveVipPartitionResult(out var response, 20000, out error)) return false;
        return CheckAck(response, "Digest", out error);
    }

    public bool InitializeOplusVip(string digestPath, string signaturePath, StorageType storage, out string error)
    {
        error = string.Empty;
        _vipSignedFilenames = ParseVipSignedFilenames(digestPath);
        if (_log != null && _vipSignedFilenames.Count != 0)
        {
            _log.Invoke($"VIP Digest 元数据已解析：{_vipSignedFilenames.Count} 个标签");
        }
        _progress?.Invoke(0, 5);
        _log?.Invoke("OPPO VIP 1/5：等待请求并发送 Digest");
        if (!SendInitialVipPartition(digestPath, out error)) return false;
        _progress?.Invoke(1, 5);

        _log?.Invoke("OPPO VIP 2/5：原生发送 Verify");
        if (!Transact(XmlEnvelope("<verify value=\"ping\" EnableVip=\"1\"/>"),
                      out var response, 15000, out error) ||
            !CheckAck(response, "VIP Verify", out error))
        {
            return false;
        }
        _progress?.Invoke(2, 5);

        _log?.Invoke("OPPO VIP 3/5：原生发送 Sign");
        if (!SendVipFile(signaturePath, "Sign", out error)) return false;
        _progress?.Invoke(3, 5);

        _log?.Invoke("OPPO VIP 4/5：原生发送 SHA-256 Init");
        if (!Transact(XmlEnvelope("<sha256init Verbose=\"1\"/>"),
                      out response, 15000, out error) ||
            !CheckAck(response, "SHA-256 Init", out error))
        {
            return false;
        }
        _progress?.Invoke(4, 5);

        _log?.Invoke("OPPO VIP 5/5：原生发送 Configure");
        var command = XmlEnvelope(
            $"<configure MemoryName=\"{EdlTypes.StorageName(storage)}\" Verbose=\"0\" AlwaysValidate=\"0\" " +
            "MaxDigestTableSizeInBytes=\"8192\" " +
            "MaxPayloadSizeToTargetInBytes=\"1048576\" " +
            "ZlpAwareHost=\"1\" SkipStorageInit=\"0\" " +
            "CheckDevinfo=\"0\" EnableFlash=\"1\" />");
        if (!Transact(command, out response, 20000, out error) ||
            !CheckAck(response, "VIP Configure", out error))
        {
            return false;
        }
        if (response.NegotiatedPayload > 0)
        {
            _maximumPayload = Math.Clamp(response.NegotiatedPayload, 4096UL, 16UL * 1024 * 1024);
        }
        _progress?.Invoke(5, 5);
        _oplusVip = true;
        _log?.Invoke("OPPO VIP 原生协议初始化完成");
        return true;
    }

    public void ResumeOplusVip(string digestPath)
    {
        _vipSignedFilenames = ParseVipSignedFilenames(digestPath);
        _oplusVip = true;
        _log?.Invoke($"已接管现有 OPPO VIP Firehose 会话，载入 {_vipSignedFilenames.Count} 个签名标签");
    }

    public bool Ping(out string error, int timeoutMs = 5000)
    {
        error = string.Empty;
        if (!Transact(XmlEnvelope("<nop value=\"ping\" />"), out var response,
                      Math.Max(250, timeoutMs), out error)) return false;
        return CheckAck(response, "NOP", out error);
    }

    public bool GetStorageInfo(uint lun, out string error)
    {
        error = string.Empty;
        var command = XmlEnvelope($"<getstorageinfo physical_partition_number=\"{lun}\" />");
        if (!Transact(command, out var response, 8000, out error)) return false;
        return CheckAck(response, "读取存储信息", out error);
    }

    public bool ReadToDevice(Stream output, ulong startSector, ulong sectorCount, uint lun,
                             uint sectorSize, out string error, string label = "", string filename = "")
    {
        error = string.Empty;
        if (!output.CanWrite || sectorCount == 0 || sectorSize == 0)
        {
            error = "读取目标参数错误";
            return false;
        }
        if (sectorCount > ulong.MaxValue / sectorSize)
        {
            error = "读取范围溢出";
            return false;
        }

        var totalBytes = sectorCount * sectorSize;
        List<VipIdentity> identities;
        if (_oplusVip)
        {
            identities = VipIdentityStrategies(_vipSignedFilenames, label, filename, lun,
                                               label.Trim().Length == 0 && startSector <= 33);
        }
        else
        {
            identities = new List<VipIdentity> { new() };
        }

        var sizeKb = (totalBytes / 1024.0).ToString("f1", CultureInfo.InvariantCulture);
        var enteredRawMode = false;
        var lastReject = string.Empty;
        var response = new FirehoseResponse();
        for (var index = 0; index < identities.Count; ++index)
        {
            var identity = identities[index];
            var attributes = string.Empty;
            if (_oplusVip)
            {
                attributes = VipAttributes(identity, $" size_in_KB=\"{sizeKb}\" sparse=\"false\"");
                _log?.Invoke(identity.Label.Length == 0
                    ? "VIP READ：尝试无标签身份"
                    : $"VIP READ：label={identity.Label}，filename={identity.Filename}");
            }

            var command = XmlEnvelope(
                $"<read SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" num_partition_sectors=\"{sectorCount}\" " +
                $"start_sector=\"{startSector}\" physical_partition_number=\"{lun}\"{attributes} />");

            if (!Transact(command, out response, 8000, out var attemptError))
            {
                error = attemptError;
                return false;
            }
            if (response.Ack && !response.Nak && response.RawMode)
            {
                enteredRawMode = true;
                break;
            }
            lastReject = JoinedMessages(response);
            if (_oplusVip && index + 1 < identities.Count)
            {
                _log?.Invoke("VIP READ：当前身份未通过，继续尝试");
            }
        }
        if (!enteredRawMode)
        {
            error = lastReject.Length == 0
                ? "设备未进入 RAW 读取模式"
                : $"进入读取模式失败：{lastReject}";
            return false;
        }

        ulong completed = 0;
        while (completed < totalBytes)
        {
            if (_cancelled.IsCancelled)
            {
                error = "操作已停止";
                return false;
            }
            var request = (long)Math.Min(_maximumPayload, totalBytes - completed);
            // Windows qcusbser may defer the final short transfer for several seconds
            // when a read ends on a USB packet boundary. Keep the RAW phase alive long
            // enough for the driver to deliver the tail instead of desynchronizing the
            // following XML command.
            if (!_transport.ReadExactly(request, out var block, 30000, out error)) return false;
            try
            {
                // .NET 的 Stream.Write 要么整块写完要么抛异常，没有 QIODevice::write 的部分写入计数。
                output.Write(block, 0, block.Length);
            }
            catch (Exception ex)
            {
                error = $"写入输出文件失败：{ex.Message}";
                return false;
            }
            completed += (ulong)block.Length;
            _progress?.Invoke(completed, totalBytes);
        }

        if (!ReceiveResponse(out response, 15000, out error)) return false;
        return CheckAck(response, "完成读取", out error);
    }

    public bool ReadToByteArray(out byte[] output, ulong startSector, ulong sectorCount, uint lun,
                                uint sectorSize, out string error, string label = "", string filename = "")
    {
        output = Array.Empty<byte>();
        error = string.Empty;
        if (sectorCount > 1024 * 1024)
        {
            error = "内存读取范围异常";
            return false;
        }
        using var buffer = new MemoryStream();
        var ok = ReadToDevice(buffer, startSector, sectorCount, lun, sectorSize, out error,
                              label, filename);
        output = buffer.ToArray();
        return ok;
    }

    public bool Program(Stream input, ulong inputSize, ulong startSector, ulong maximumSectors,
                        uint lun, uint sectorSize, out string error, string label = "", string filename = "")
    {
        error = string.Empty;
        if (!input.CanRead || inputSize == 0 || sectorSize == 0)
        {
            error = "镜像参数错误";
            return false;
        }
        var sectors = (inputSize + sectorSize - 1) / sectorSize;
        if (maximumSectors > 0 && sectors > maximumSectors)
        {
            error = $"镜像需要 {sectors} 个扇区，目标分区仅有 {maximumSectors} 个扇区";
            return false;
        }

        List<VipIdentity> identities;
        if (_oplusVip)
        {
            // OPPO's native gpt-backup RW mode uses a LUN-local BackupGPT identity
            // for arbitrary program ranges. Prefer it immediately; signed metadata
            // identities remain as compatibility fallbacks for other programmers.
            identities = new List<VipIdentity>();
            AppendVipIdentity(identities, "BackupGPT", $"gpt_backup{lun}.bin");
            var fallbacks = VipIdentityStrategies(_vipSignedFilenames, label, filename, lun);
            foreach (var identity in fallbacks)
            {
                AppendVipIdentity(identities, identity.Label, identity.Filename);
            }
        }
        else
        {
            identities = new List<VipIdentity> { new() };
        }

        var enteredRawMode = false;
        var lastReject = string.Empty;
        var response = new FirehoseResponse();
        for (var index = 0; index < identities.Count; ++index)
        {
            var identity = identities[index];
            var attributes = _oplusVip
                ? VipAttributes(identity, " sparse=\"false\"") : string.Empty;
            if (_oplusVip)
            {
                _log?.Invoke(identity.Label.Length == 0
                    ? "VIP PROGRAM：尝试无标签身份"
                    : $"VIP PROGRAM：label={identity.Label}，filename={identity.Filename}");
            }

            var command = XmlEnvelope(
                $"<program SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" num_partition_sectors=\"{sectors}\" " +
                $"start_sector=\"{startSector}\" physical_partition_number=\"{lun}\"{attributes} />");

            if (!Transact(command, out response, 8000, out var attemptError))
            {
                error = attemptError;
                return false;
            }
            if (response.Ack && !response.Nak && response.RawMode)
            {
                enteredRawMode = true;
                break;
            }
            lastReject = JoinedMessages(response);
            if (_oplusVip && index + 1 < identities.Count)
            {
                _log?.Invoke("VIP PROGRAM：当前身份未通过，继续尝试");
            }
        }
        if (!enteredRawMode)
        {
            error = lastReject.Length == 0
                ? "设备未进入 RAW 写入模式"
                : $"进入写入模式失败：{lastReject}";
            return false;
        }

        var transferBytes = sectors * sectorSize;
        ulong completed = 0;
        while (completed < transferBytes)
        {
            if (_cancelled.IsCancelled)
            {
                error = "操作已停止";
                return false;
            }
            var request = (long)Math.Min(_maximumPayload, transferBytes - completed);
            var sourceRemaining = completed < inputSize ? inputSize - completed : 0;
            var sourceRequest = (long)Math.Min((ulong)request, sourceRemaining);
            var readError = string.Empty;
            var block = sourceRequest > 0
                ? ReadUpTo(input, sourceRequest, out readError) : Array.Empty<byte>();
            if (block.Length != sourceRequest && completed < inputSize)
            {
                error = $"读取镜像失败：{readError}";
                return false;
            }
            if (block.Length < request)
            {
                var padded = new byte[(int)request];
                Array.Copy(block, padded, block.Length);
                block = padded;
            }
            if (!_transport.WriteAll(block, 15000, out error)) return false;
            completed += (ulong)block.Length;
            _progress?.Invoke(Math.Min(completed, inputSize), inputSize);
        }

        if (!ReceiveResponse(out response, 15000, out error)) return false;
        return CheckAck(response, "完成写入", out error);
    }

    public bool Erase(ulong startSector, ulong sectorCount, uint lun, uint sectorSize,
                      out string error, string label = "", string filename = "")
    {
        error = string.Empty;
        if (sectorCount == 0 || sectorSize == 0)
        {
            error = "擦除范围参数错误";
            return false;
        }

        List<VipIdentity> identities;
        if (_oplusVip)
        {
            // fh_loader's OPPO special RW mode forwards erase with the real label and
            // deliberately omits filename. This differs from read/program matching.
            identities = new List<VipIdentity>();
            AppendVipIdentity(identities, FirehoseToken(label, "partition"), "");
            var fallbacks = VipIdentityStrategies(_vipSignedFilenames, label, filename, lun);
            foreach (var identity in fallbacks)
            {
                AppendVipIdentity(identities, identity.Label, identity.Filename);
            }
        }
        else
        {
            identities = new List<VipIdentity> { new() };
        }

        var lastReject = string.Empty;
        for (var index = 0; index < identities.Count; ++index)
        {
            var identity = identities[index];
            var attributes = _oplusVip ? VipAttributes(identity) : string.Empty;
            if (_oplusVip)
            {
                _log?.Invoke(identity.Label.Length == 0
                    ? "VIP ERASE：尝试无标签身份"
                    : identity.Filename.Length == 0
                        ? $"VIP ERASE：label={identity.Label}，无 filename"
                        : $"VIP ERASE：label={identity.Label}，filename={identity.Filename}");
            }
            var command = XmlEnvelope(
                $"<erase SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" num_partition_sectors=\"{sectorCount}\" " +
                $"start_sector=\"{startSector}\" physical_partition_number=\"{lun}\"{attributes} />");

            if (!Transact(command, out var response, 30000, out var attemptError))
            {
                error = attemptError;
                return false;
            }
            if (response.Ack && !response.Nak) return true;
            lastReject = JoinedMessages(response);
            if (_oplusVip && index + 1 < identities.Count)
            {
                _log?.Invoke("VIP ERASE：当前身份未通过，继续尝试");
            }
        }
        error = $"擦除分区失败：{lastReject}";
        return false;
    }

    public bool Patch(uint sectorSize, ulong byteOffset, uint lun, ulong sizeInBytes,
                      string startSector, string value, out string error)
    {
        error = string.Empty;
        if (sectorSize == 0 || sizeInBytes == 0 || startSector.Length == 0 || value.Length == 0)
        {
            error = "Patch 参数错误";
            return false;
        }
        var command = XmlEnvelope(
            $"<patch SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" byte_offset=\"{byteOffset}\" filename=\"DISK\" " +
            $"physical_partition_number=\"{lun}\" size_in_bytes=\"{sizeInBytes}\" " +
            $"start_sector=\"{startSector}\" value=\"{value}\" />");
        if (!Transact(command, out var response, 30000, out error)) return false;
        return CheckAck(response, "应用 GPT Patch", out error);
    }

    public bool SetBootableStorageDrive(uint value, out string error)
    {
        error = string.Empty;
        var command = XmlEnvelope($"<setbootablestoragedrive value=\"{value}\" />");
        if (!Transact(command, out var response, 30000, out error)) return false;
        return CheckAck(response, "设置启动存储器", out error);
    }

    public bool Reboot(bool toEdl, out string error)
    {
        error = string.Empty;
        var value = toEdl ? "reset_to_edl" : "reset";
        var attributes = toEdl
            ? $"value=\"{value}\" verbose=\"0\" DelayInSeconds=\"1\""
            : $"value=\"{value}\" verbose=\"0\"";
        var command = Encoding.UTF8.GetBytes(XmlEnvelope($"<power {attributes} />"));
        _log?.Invoke($"发送 Firehose 命令：power {value}");
        if (!_transport.WriteAll(command, 3000, out error)) return false;

        // Power commands are different from data commands: several programmers
        // drop USB as soon as the command is accepted, before an XML ACK reaches
        // the host. OneToolBox uses the same fire-and-close behavior. Consume an
        // ACK when present, reject an explicit NAK, and otherwise treat a completed
        // write followed by port transition as a successfully submitted command.
        if (!ReceiveResponse(out var response, toEdl ? 2500 : 1500, out _))
        {
            _log?.Invoke("Firehose Power 已提交，设备切换端口时未返回最终 ACK");
            return true;
        }
        if (response.Nak)
        {
            error = "Firehose Power 返回 NAK";
            return false;
        }
        return response.Ack;
    }

    public void SetProgressCallback(ProgressCallback progress) => _progress = progress;

    public ulong MaximumPayload => _maximumPayload;

    /// <summary>对应 C++ 的 <c>QIODevice::read(n)</c>：最多读 n 字节，返回实际读到的数据。</summary>
    private static byte[] ReadUpTo(Stream input, long count, out string error)
    {
        error = string.Empty;
        var buffer = new byte[(int)count];
        var offset = 0;
        try
        {
            while (offset < count)
            {
                var read = input.Read(buffer, offset, (int)(count - offset));
                if (read <= 0) break;
                offset += read;
            }
        }
        catch (Exception ex)
        {
            error = ex.Message;
        }
        if (offset != count) Array.Resize(ref buffer, offset);
        return buffer;
    }
}
