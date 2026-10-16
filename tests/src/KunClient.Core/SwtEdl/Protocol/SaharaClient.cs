using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;
using KunFlashTool.Core.SwtEdl;
using KunFlashTool.Core.SwtEdl.Transport;

namespace KunFlashTool.Core.SwtEdl.Protocol;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/protocol/Sahara.{h,cpp}`。
/// </summary>
public sealed class SaharaClient
{
    private const uint SaharaHello = 0x01;
    private const uint SaharaHelloResponse = 0x02;
    private const uint SaharaReadData = 0x03;
    private const uint SaharaEndTransfer = 0x04;
    private const uint SaharaDone = 0x05;
    private const uint SaharaDoneResponse = 0x06;
    private const uint SaharaCommandReady = 0x0b;
    private const uint SaharaSwitchMode = 0x0c;
    private const uint SaharaExecuteRequest = 0x0d;
    private const uint SaharaExecuteResponse = 0x0e;
    private const uint SaharaExecuteData = 0x0f;
    private const uint SaharaReadData64 = 0x12;
    private const uint SaharaResetStateMachine = 0x13;

    private const uint SerialNumberRead = 0x01;
    private const uint MsmHardwareIdRead = 0x02;
    private const uint OemPkHashRead = 0x03;
    private const uint SblSoftwareVersionRead = 0x07;
    private const uint ChipIdV3Read = 0x0a;

    private readonly SerialTransport _transport;
    private readonly EdlCancel _cancelled;
    private readonly LogCallback? _log;
    private readonly ProgressCallback? _progress;

    public SaharaClient(SerialTransport transport, EdlCancel cancelled, LogCallback log, ProgressCallback progress)
    {
        _transport = transport;
        _cancelled = cancelled;
        _log = log;
        _progress = progress;
    }

    private static uint ReadU32(byte[] data, long offset)
        => BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan((int)offset, 4));

    private static ulong ReadU64(byte[] data, long offset)
        => BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan((int)offset, 8));

    private static ushort ReadU16(byte[] data, long offset)
        => BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan((int)offset, 2));

    private static void AppendU32(List<byte> data, uint value)
    {
        Span<byte> le = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(le, value);
        data.AddRange(le.ToArray());
    }

    private static string HexBytes(byte[] data, int count)
        => Convert.ToHexString(data, 0, count).ToLowerInvariant();

    private static string CpuName(string msmId)
    {
        var chip = DeviceIdentity.QualcommChipFromMsmId(msmId);
        if (chip.Length != 0) return DeviceIdentity.QualcommChipDisplayName(chip);
        return msmId.Length == 0 ? "Qualcomm" : msmId.ToUpperInvariant();
    }

    private static string VendorName(uint oemId) => oemId switch
    {
        0x0072 => "Xiaomi",
        // EDL exposes the signing/OEM family rather than the consumer sub-brand.
        // OPPO, OnePlus and realme programmers therefore use the OPLUS family name.
        0x0050 or 0x0051 or 0x0052 => "OPLUS",
        0x0001 => "Qualcomm",
        0x0017 => "Samsung",
        0x0019 => "LG",
        0x001a => "Motorola",
        0x001b => "Lenovo",
        0x001e => "Huawei",
        0x0021 => "ZTE",
        0x0022 => "Sony",
        0x0023 => "ASUS",
        0x0025 => "vivo",
        0x0030 => "Nokia",
        0x0031 => "Google",
        _ => $"OEM 0x{oemId.ToString("x4")}".ToUpperInvariant(),
    };

    private static string SaharaStatusText(uint status) => status switch
    {
        0x01 => "INVALID_CMD",
        0x02 => "PROTOCOL_MISMATCH",
        0x05 => "INVALID_PACKET_SIZE",
        0x06 => "UNEXPECTED_IMAGE_ID",
        0x07 => "INVALID_HEADER_SIZE",
        0x08 => "INVALID_DATA_SIZE",
        0x09 => "INVALID_IMAGE_TYPE",
        0x0c => "GENERAL_TX_RX_ERROR",
        0x14 => "INVALID_ELF_HEADER",
        0x16 => "TIMEOUT_RX",
        0x17 => "TIMEOUT_TX",
        0x21 => "HASH_TABLE_AUTH_FAILURE（Programmer 签名链与设备不匹配）",
        0x22 => "HASH_VERIFICATION_FAILURE（Programmer 数据校验失败）",
        0x23 => "HASH_TABLE_NOT_FOUND（Programmer 缺少哈希表）",
        _ => "UNKNOWN_STATUS",
    };

    private bool ReadPacket(out byte[] packet, out uint command, int timeoutMs, out string error)
    {
        packet = Array.Empty<byte>();
        command = 0;
        error = string.Empty;

        if (!_transport.ReadExactly(8, out var header, timeoutMs, out error)) return false;

        var packetCommand = ReadU32(header, 0);
        var packetLength = ReadU32(header, 4);
        if (packetLength < 8 || packetLength > 1024 * 1024)
        {
            error = $"Sahara 包长度异常：{packetLength}";
            return false;
        }

        var body = Array.Empty<byte>();
        if (packetLength > 8 &&
            !_transport.ReadExactly(packetLength - 8, out body, timeoutMs, out error))
        {
            return false;
        }

        var combined = new byte[header.Length + body.Length];
        Buffer.BlockCopy(header, 0, combined, 0, header.Length);
        Buffer.BlockCopy(body, 0, combined, header.Length, body.Length);
        packet = combined;
        command = packetCommand;
        return true;
    }

    private bool ReadHelloPacket(out byte[] packet, int timeoutMs, out string error)
    {
        packet = Array.Empty<byte>();
        error = string.Empty;
        if (timeoutMs <= 0)
        {
            error = "Sahara Hello 读取参数错误";
            return false;
        }

        var timer = Stopwatch.StartNew();
        long skippedBytes = 0;
        var receiveError = string.Empty;

        // A reopened USB serial handle can expose a few stale bytes or split the
        // Sahara header across completions. Scan for a valid HELLO header so one
        // stray byte does not leave every later packet permanently misaligned.
        while (timer.ElapsedMilliseconds < timeoutMs)
        {
            var remaining = timeoutMs - (int)timer.ElapsedMilliseconds;
            if (!_transport.PeekExactly(8, out var header, Math.Max(1, remaining), out receiveError))
            {
                error = receiveError;
                return false;
            }

            var command = ReadU32(header, 0);
            var length = ReadU32(header, 4);
            if (command == SaharaHello && length >= 48 && length <= 4096)
            {
                var packetRemaining = timeoutMs - (int)timer.ElapsedMilliseconds;
                if (packetRemaining <= 0 ||
                    !_transport.ReadExactly(length, out packet, packetRemaining, out receiveError))
                {
                    error = receiveError;
                    return false;
                }
                if (skippedBytes > 0 && _log != null)
                {
                    _log($"[Sahara][DEBUG] Hello 重同步：跳过 {skippedBytes} 个前导字节");
                }
                return true;
            }

            if (!_transport.ReadExactly(1, out _, Math.Max(1, remaining), out receiveError))
            {
                error = receiveError;
                return false;
            }
            skippedBytes++;
            if (skippedBytes >= 64 * 1024)
            {
                error = "Sahara Hello 重同步超过 64 KiB";
                return false;
            }
        }

        error = $"等待 Sahara Hello 超时（已跳过 {skippedBytes} 个前导字节）";
        return false;
    }

    private bool SendHelloResponse(uint mode, out string error, uint version = 2, uint minimumVersion = 1)
    {
        error = string.Empty;
        var response = new List<byte>(48);
        AppendU32(response, SaharaHelloResponse);
        AppendU32(response, 48);
        AppendU32(response, version);
        AppendU32(response, minimumVersion);
        AppendU32(response, 0);
        AppendU32(response, mode);
        // Qualcomm's command-mode handshake uses the six reserved dwords as the
        // canonical 1..6 sequence. Several Sahara v3 boot ROMs accept CMD_READY
        // with zeroes but then return empty EXECUTE responses.
        for (uint i = 0; i < 6; i++)
        {
            AppendU32(response, mode == 3 ? i + 1 : 0);
        }
        return _transport.WriteAll(response.ToArray(), 3000, out error);
    }

    private bool ExecuteCommand(uint commandId, out byte[] data, out string error)
    {
        data = Array.Empty<byte>();
        error = string.Empty;

        var request = new List<byte>(12);
        AppendU32(request, SaharaExecuteRequest);
        AppendU32(request, 12);
        AppendU32(request, commandId);
        if (!_transport.WriteAll(request.ToArray(), 1500, out error)) return false;

        if (!ReadPacket(out var response, out var responseCommand, 1200, out error)) return false;
        if (responseCommand != SaharaExecuteResponse || response.Length < 16)
        {
            error = $"Sahara EXECUTE 0x{commandId:x} 收到异常响应 0x{responseCommand:x}";
            return false;
        }
        var responseId = ReadU32(response, 8);
        var dataLength = ReadU32(response, 12);
        if (responseId != commandId || dataLength == 0 || dataLength > 1024 * 1024)
        {
            error = $"Sahara EXECUTE 0x{commandId:x} 返回长度异常：{dataLength}";
            return false;
        }

        var dataRequest = new List<byte>(12);
        AppendU32(dataRequest, SaharaExecuteData);
        AppendU32(dataRequest, 12);
        AppendU32(dataRequest, commandId);
        if (!_transport.WriteAll(dataRequest.ToArray(), 1500, out error)) return false;
        return _transport.ReadExactly(dataLength, out data, 1800, out error);
    }

    private bool ReadDeviceInfo(byte[] hello, DeviceInfo? deviceInfo, out byte[] nextHello, out string error)
    {
        nextHello = Array.Empty<byte>();
        error = string.Empty;
        if (deviceInfo == null || hello.Length < 48)
        {
            error = "Sahara Hello 或设备信息参数异常";
            return false;
        }

        var deviceVersion = ReadU32(hello, 8);
        var negotiatedVersion = Math.Clamp(deviceVersion, 1u, 3u);
        deviceInfo.SaharaVersion = deviceVersion.ToString(CultureInfo.InvariantCulture);
        if (_log != null)
        {
            _log($"Sahara 版本 {deviceVersion}，进入命令模式读取设备信息");
        }
        if (!SendHelloResponse(3, out error, negotiatedVersion, negotiatedVersion)) return false;

        if (!ReadPacket(out var packet, out var command, 1800, out error) || command != SaharaCommandReady)
        {
            if (command != SaharaCommandReady && packet.Length != 0)
            {
                error = $"Sahara 命令模式期待 CMD_READY，收到 0x{command:x}";
            }
            return false;
        }

        var version3 = deviceVersion >= 3;
        byte[] Query(uint commandId)
        {
            if (!ExecuteCommand(commandId, out var result, out var commandError))
            {
                if (_log != null)
                {
                    _log($"[Sahara][DEBUG] 命令 0x{commandId:x} 跳过：{commandError}");
                }
                return Array.Empty<byte>();
            }
            return result;
        }

        // Execute command ids are independent of the 64-bit Sahara packet ids.
        // Serial remains 0x01 on V3; 0x14 is not a serial command.
        var serial = Query(SerialNumberRead);
        if (serial.Length >= 4)
        {
            deviceInfo.Serial = ReadU32(serial, 0).ToString("x8");
        }

        uint oemId = 0;
        if (version3)
        {
            var chip = Query(ChipIdV3Read);
            if (chip.Length >= 46)
            {
                var msmId = ReadU32(chip, 36);
                var modelId = ReadU16(chip, 42);
                deviceInfo.MsmId = msmId.ToString("x8");
                oemId = ReadU16(chip, 40);
                if (oemId == 0)
                {
                    oemId = ReadU16(chip, 44);
                }
                deviceInfo.Hwid = deviceInfo.MsmId +
                    oemId.ToString("x4") +
                    modelId.ToString("x4");
            }
            else
            {
                var hardware = Query(MsmHardwareIdRead);
                if (hardware.Length >= 8)
                {
                    var hardwareId = ReadU64(hardware, 0).ToString("x16");
                    deviceInfo.Hwid = hardwareId;
                    deviceInfo.MsmId = hardwareId.Substring(0, 8);
                    var ok = uint.TryParse(hardwareId.Substring(8, 4), NumberStyles.HexNumber,
                                           CultureInfo.InvariantCulture, out var parsed);
                    oemId = ok ? parsed : 0;
                }
            }
        }
        else
        {
            var hardware = Query(MsmHardwareIdRead);
            if (hardware.Length >= 8)
            {
                var hardwareId = ReadU64(hardware, 0).ToString("x16");
                deviceInfo.Hwid = hardwareId;
                deviceInfo.MsmId = hardwareId.Substring(0, 8);
                var ok = uint.TryParse(hardwareId.Substring(8, 4), NumberStyles.HexNumber,
                                       CultureInfo.InvariantCulture, out var parsed);
                oemId = ok ? parsed : 0;
            }
        }

        if (deviceInfo.MsmId.Length != 0) deviceInfo.Model = CpuName(deviceInfo.MsmId);
        if (oemId != 0) deviceInfo.Brand = VendorName(oemId);

        var pkHash = Query(OemPkHashRead);
        if (pkHash.Length >= 32) deviceInfo.PkHash = HexBytes(pkHash, 32);
        var sblVersion = Query(SblSoftwareVersionRead);
        if (sblVersion.Length >= 4)
        {
            deviceInfo.SblVersion = ReadU32(sblVersion, 0).ToString("x8");
        }

        // Sahara v3 returns to the image-transfer handshake when SWITCH_MODE is
        // sent with Command (3). Keep ImageTxPending (0) as a compatibility path
        // for older boot ROM revisions.
        if (!SendSwitchMode(version3 ? 3u : 0u, out var switchError))
        {
            error = switchError;
            return false;
        }
        if (ReadHelloPacket(out nextHello, 2500, out error))
        {
            return true;
        }

        // A few boot ROM revisions use the literal ImageTxPending value instead.
        nextHello = Array.Empty<byte>();
        if (!SendSwitchMode(0, out switchError))
        {
            error = switchError;
            return false;
        }
        if (ReadHelloPacket(out nextHello, 2500, out error))
        {
            return true;
        }
        if (error.Length == 0)
        {
            error = "Sahara 命令模式退出后未收到新的 Hello";
        }
        return false;
    }

    private bool SendSwitchMode(uint mode, out string error)
    {
        error = string.Empty;
        var packet = new List<byte>(12);
        AppendU32(packet, SaharaSwitchMode);
        AppendU32(packet, 12);
        AppendU32(packet, mode);
        return _transport.WriteAll(packet.ToArray(), 3000, out error);
    }

    private bool SendResetStateMachine(out string error)
    {
        error = string.Empty;
        var packet = new List<byte>(8);
        AppendU32(packet, SaharaResetStateMachine);
        AppendU32(packet, 8);
        return _transport.WriteAll(packet.ToArray(), 3000, out error);
    }

    private bool WaitForHello(out byte[] packet, out string error)
    {
        packet = Array.Empty<byte>();
        error = string.Empty;
        var receiveError = string.Empty;
        // connectDevice() already performed a passive state probe. A second long
        // wait here only delays BootROMs which emit HELLO after SWITCH_MODE.
        if (ReadHelloPacket(out packet, 600, out receiveError))
        {
            return true;
        }

        for (var attempt = 0; attempt < 3; attempt++)
        {
            if (_cancelled.IsCancelled)
            {
                error = "操作已停止";
                return false;
            }
            if (_log != null)
            {
                _log($"Sahara 唤醒 {attempt + 1}/3：切换命令模式并重置状态机");
            }

            // Some qcusbser sessions retain the target in command mode. Send both
            // parts of the kick back-to-back; waiting between them costs a full
            // serial timeout on BootROMs which react only to SWITCH_MODE.
            if (!SendHelloResponse(3, out var writeError))
            {
                receiveError = writeError;
                continue;
            }
            Thread.Sleep(40);
            if (!SendSwitchMode(0, out writeError))
            {
                receiveError = writeError;
                continue;
            }
            Thread.Sleep(40);
            var switchTimeout = attempt == 0 ? 1200 : 2200;
            if (ReadHelloPacket(out packet, switchTimeout, out receiveError))
            {
                return true;
            }

            if (!SendResetStateMachine(out writeError))
            {
                receiveError = writeError;
                continue;
            }
            Thread.Sleep(60);
            var resetTimeout = attempt == 0 ? 1200 : 2500;
            if (ReadHelloPacket(out packet, resetTimeout, out receiveError))
            {
                return true;
            }
            Thread.Sleep(attempt == 0 ? 80 : 180);
        }

        error = $"Sahara 状态机唤醒后仍未收到 Hello：{receiveError}";
        return false;
    }

    private bool SendProgrammerRange(FileStream file, ulong offset, ulong length, ref ulong transferred, out string error)
    {
        error = string.Empty;
        if (offset > (ulong)file.Length ||
            length > (ulong)file.Length - offset)
        {
            error = $"设备请求的 Programmer 范围越界：offset={offset}, length={length}";
            return false;
        }
        try
        {
            file.Seek((long)offset, SeekOrigin.Begin);
        }
        catch (Exception ex)
        {
            error = $"定位 Programmer 失败：{ex.Message}";
            return false;
        }

        var remaining = length;
        while (remaining > 0)
        {
            if (_cancelled.IsCancelled)
            {
                error = "操作已停止";
                return false;
            }
            var request = (int)Math.Min(remaining, 1024UL * 1024);
            var block = new byte[request];
            var read = file.Read(block, 0, request);
            if (read != request)
            {
                error = $"读取 Programmer 失败：读取 {read}/{request} 字节";
                return false;
            }
            if (!_transport.WriteAll(block, 10000, out error)) return false;
            remaining -= (ulong)block.Length;
            transferred += (ulong)block.Length;
            if (_progress != null) _progress(Math.Min(transferred, (ulong)file.Length), (ulong)file.Length);
        }
        return true;
    }

    public bool UploadProgrammer(string filePath, DeviceInfo? deviceInfo, bool querySaharaIdentity,
                                 out string error, out uint endTransferStatus)
    {
        error = string.Empty;
        endTransferStatus = 0;

        FileStream programmer;
        try
        {
            programmer = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        }
        catch (Exception ex)
        {
            error = $"打开 Programmer 失败：{ex.Message}";
            return false;
        }

        using (programmer)
        {
            if (programmer.Length <= 0)
            {
                error = "Programmer 文件为空";
                return false;
            }

            if (_log != null) _log("等待 Sahara Hello 数据包…");
            if (!WaitForHello(out var packet, out error)) return false;

            var imageHello = packet;
            if (querySaharaIdentity)
            {
                if (!ReadDeviceInfo(packet, deviceInfo, out imageHello, out var infoError))
                {
                    if (_log != null)
                    {
                        _log($"Sahara 设备信息读取未完整完成，恢复映像传输模式：{infoError}");
                    }
                    if (!WaitForHello(out imageHello, out error)) return false;
                }
            }
            else
            {
                var version = ReadU32(packet, 8);
                if (deviceInfo != null) deviceInfo.SaharaVersion = version.ToString(CultureInfo.InvariantCulture);
                if (_log != null)
                {
                    _log($"Sahara 版本 {version}，VIP 模式跳过命令态查询，直接传输 Programmer");
                }
            }
            packet = imageHello;

            if (_log != null && deviceInfo != null &&
                (deviceInfo.Serial.Length != 0 || deviceInfo.Hwid.Length != 0 ||
                 deviceInfo.MsmId.Length != 0))
            {
                var version = deviceInfo.SaharaVersion.Length == 0
                    ? "—" : deviceInfo.SaharaVersion;
                var model = deviceInfo.Model.Length == 0
                    ? "Qualcomm" : deviceInfo.Model;
                var brand = deviceInfo.Brand.Length == 0
                    ? "Qualcomm" : deviceInfo.Brand;
                var identity = deviceInfo.Hwid.Length == 0
                    ? deviceInfo.Serial : deviceInfo.Hwid;
                _log($"Sahara 设备信息：版本 {version}，{brand}，{model}，ID {(identity.Length == 0 ? "—" : identity)}");
            }
            else if (_log != null && deviceInfo != null && querySaharaIdentity)
            {
                _log($"Sahara 版本 {(deviceInfo.SaharaVersion.Length == 0 ? "—" : deviceInfo.SaharaVersion)}，命令查询未返回身份字段，将从 Firehose 日志补全");
            }

            if (_log != null)
            {
                _log($"Sahara 版本 {ReadU32(packet, 8)}，发送 Hello Response");
            }
            if (!SendHelloResponse(0, out error)) return false;

            ulong transferred = 0;
            for (;;)
            {
                if (!ReadPacket(out packet, out var command, 10000, out error)) return false;
                if (command == SaharaReadData)
                {
                    if (packet.Length < 20)
                    {
                        error = "Sahara READ_DATA 包过短";
                        return false;
                    }
                    var offset = (ulong)ReadU32(packet, 12);
                    var length = (ulong)ReadU32(packet, 16);
                    if (!SendProgrammerRange(programmer, offset, length, ref transferred, out error)) return false;
                    continue;
                }
                if (command == SaharaReadData64)
                {
                    if (packet.Length < 32)
                    {
                        error = "Sahara READ_DATA_64 包过短";
                        return false;
                    }
                    var offset = ReadU64(packet, 16);
                    var length = ReadU64(packet, 24);
                    if (!SendProgrammerRange(programmer, offset, length, ref transferred, out error)) return false;
                    continue;
                }
                if (command == SaharaEndTransfer)
                {
                    if (packet.Length < 16)
                    {
                        error = "Sahara END_TRANSFER 包过短";
                        return false;
                    }
                    var status = ReadU32(packet, 12);
                    if (status != 0)
                    {
                        endTransferStatus = status;
                        error = $"Programmer 传输失败，Sahara 状态：{status} / 0x{status:x2} {SaharaStatusText(status)}";
                        return false;
                    }

                    var done = new List<byte>(8);
                    AppendU32(done, SaharaDone);
                    AppendU32(done, 8);
                    if (!_transport.WriteAll(done.ToArray(), 3000, out error)) return false;

                    if (!ReadPacket(out packet, out command, 5000, out error)) return false;
                    if (command != SaharaDoneResponse)
                    {
                        error = $"预期 Sahara DONE_RESPONSE，收到 0x{command:x}";
                        return false;
                    }
                    if (_progress != null) _progress((ulong)programmer.Length, (ulong)programmer.Length);
                    if (_log != null) _log("Programmer 传输完成");
                    return true;
                }

                error = $"未处理的 Sahara 命令：0x{command:x}";
                return false;
            }
        }
    }

    public bool FinishPendingTransfer(out string error)
    {
        error = string.Empty;
        if (!ReadPacket(out var packet, out var command, 2500, out error)) return false;
        if (command != SaharaEndTransfer || packet.Length < 16)
        {
            error = $"期待 Sahara END_TRANSFER，收到 0x{command:x}";
            return false;
        }
        var status = ReadU32(packet, 12);
        if (status != 0)
        {
            error = $"Sahara END_TRANSFER 状态异常：{status}";
            return false;
        }

        var done = new List<byte>(8);
        AppendU32(done, SaharaDone);
        AppendU32(done, 8);
        if (!_transport.WriteAll(done.ToArray(), 3000, out error)) return false;
        if (!ReadPacket(out packet, out command, 5000, out error)) return false;
        if (command != SaharaDoneResponse)
        {
            error = $"期待 Sahara DONE_RESPONSE，收到 0x{command:x}";
            return false;
        }
        if (_log != null) _log("检测到已完成的 Programmer 传输，Sahara DONE 已确认");
        return true;
    }
}
