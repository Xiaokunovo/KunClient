using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO.Ports;
using System.Threading;
using KunFlashTool.Core.SwtEdl;

namespace KunFlashTool.Core.SwtEdl.Transport;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/transport/SerialTransport.{h,cpp}`。
///
/// <para>为什么自己维护一个读缓冲：QSerialPort 有一个应用可见的内部缓冲，可做「只看不消费」的 peek；
/// C# 的 <see cref="SerialPort"/> 没有等价语义，所以这里用 <c>_buffer</c> 手工实现
/// <see cref="PeekExactly"/> / <see cref="ReadExactly"/> / <see cref="ReadUntil"/>。</para>
/// </summary>
public sealed class SerialTransport : IDisposable
{
    private readonly SerialPort _port = new();
    private readonly List<byte> _buffer = new();
    private readonly EdlCancel? _cancelled;
    private string _openDiagnostics = string.Empty;

    public SerialTransport(EdlCancel? cancelled = null)
    {
        _cancelled = cancelled;
    }

    public void Dispose()
    {
        Close();
    }

    public bool Open(string portName, out string error)
    {
        error = string.Empty;
        Close();
        _port.PortName = portName;
        // Match the Qualcomm/.NET host configuration. qcusbser normally treats the baud
        // rate as virtual, but some Windows driver revisions do gate their USB receive
        // path on the complete 115200 8N1 + DTR configuration.
        _port.BaudRate = 115200; // QSerialPort::Baud115200
        _port.DataBits = 8;      // QSerialPort::Data8
        _port.Parity = Parity.None;
        _port.StopBits = StopBits.One;
        _port.Handshake = Handshake.None;
        // C++：m_port.setReadBufferSize(16 MiB)，用于限制 Qt 用户态缓冲。
        _port.ReadBufferSize = 16 * 1024 * 1024;

        try
        {
            _port.Open();
        }
        catch (Exception ex)
        {
            error = $"打开 {portName} 失败：{ex.Message}";
            return false;
        }

        // C++ 侧用 m_port.handle() 拿到 HANDLE 调 SetupComm / ClearCommError。
        // C# 的 SerialPort.BaseStream 拿不到 HANDLE：拿不到就跳过，
        // driverQueueConfigured 保持 false（对应文案「系统默认」）。
        // 拿不到不是错误，不能因此让 open 失败。
        var driverQueueConfigured = false;
        if (OperatingSystem.IsWindows())
        {
            // 无法取得 HANDLE，跳过内核队列设置。
        }

        // QSerialPort initializes NoFlowControl with both control lines deasserted.
        // Several qcusbser builds do not arm the bulk-IN endpoint until DTR is asserted.
        bool dtrConfigured;
        try
        {
            _port.DtrEnable = true;
            dtrConfigured = true;
        }
        catch
        {
            // 等价于 m_port.clearError()：控制线失败不影响打开。
            dtrConfigured = false;
        }
        try
        {
            _port.RtsEnable = false;
        }
        catch
        {
            // 等价于 m_port.clearError()。（C++ 侧 rtsConfigured 未用于文案。）
        }

        // Qualcomm targets may emit Sahara HELLO immediately when the handle opens.
        // Clearing the input queue here drops that one-shot 48-byte packet on some devices.
        _buffer.Clear();
        _openDiagnostics = $"115200 8N1，DTR={(dtrConfigured ? "1" : "驱动未确认")}，RTS=0，驱动接收队列={(driverQueueConfigured ? "4 MiB" : "系统默认")}";
        return true;
    }

    public void Close()
    {
        if (_port.IsOpen)
        {
            _port.Close();
        }
        _buffer.Clear();
        _openDiagnostics = string.Empty;
    }

    public bool IsOpen => _port.IsOpen;

    public string PortName => _port.PortName;

    public string OpenDiagnostics => _openDiagnostics;

    public void Clear()
    {
        _buffer.Clear();
        if (_port.IsOpen)
        {
            try
            {
                _port.DiscardInBuffer();
                _port.DiscardOutBuffer();
            }
            catch
            {
                // 端口刚好被拔掉时 Discard 可能抛异常，忽略。
            }
        }
    }

    private bool WasCancelled(out string error)
    {
        if (_cancelled is { IsCancelled: true })
        {
            error = "操作已停止";
            return true;
        }
        error = string.Empty;
        return false;
    }

    public bool WriteAll(byte[] data, int timeoutMs, out string error)
    {
        error = string.Empty;
        if (!_port.IsOpen)
        {
            error = "串口尚未打开";
            return false;
        }

        var timer = Stopwatch.StartNew();
        long written = 0;
        while (written < data.Length)
        {
            if (WasCancelled(out error)) return false;
            try
            {
                // SerialPort.Write 是「整块接受或抛异常」，没有 QIODevice 那种部分写入的返回值；
                // 这里把请求量当作实际写入量，保持与 C++ 相同的循环结构。
                var chunk = data.Length - (int)written;
                _port.Write(data, (int)written, chunk);
                written += chunk;
            }
            catch (Exception ex)
            {
                error = $"串口写入失败：{ex.Message}";
                return false;
            }

            var remaining = timeoutMs - (int)timer.ElapsedMilliseconds;
            if (remaining <= 0 || !WaitForBytesWritten(Math.Min(remaining, 250)))
            {
                if (_port.BytesToWrite > 0 && timer.ElapsedMilliseconds >= timeoutMs)
                {
                    error = $"串口写入超时（{written}/{data.Length} 字节）";
                    return false;
                }
            }
        }
        while (_port.BytesToWrite > 0)
        {
            if (WasCancelled(out error)) return false;
            var remaining = timeoutMs - (int)timer.ElapsedMilliseconds;
            if (remaining <= 0 || !WaitForBytesWritten(Math.Min(remaining, 250)))
            {
                if (timer.ElapsedMilliseconds >= timeoutMs)
                {
                    error = "等待串口发送完成超时";
                    return false;
                }
            }
        }
        return true;
    }

    private bool FillBuffer(int timeoutMs, out string error)
    {
        error = string.Empty;
        if (WasCancelled(out error)) return false;
        try
        {
            if (_port.BytesToRead == 0 && !WaitForReadyRead(Math.Min(timeoutMs, 250)))
            {
                // 读取超时在 C++ 侧不算错误（error() == TimeoutError 时同样返回 true）。
                return true;
            }
            DrainAvailable();
            while (WaitForReadyRead(2))
            {
                DrainAvailable();
            }
            return true;
        }
        catch (Exception ex)
        {
            error = $"串口读取失败：{ex.Message}";
            return false;
        }
    }

    /// <summary>
    /// 模拟 <c>QSerialPort::waitForReadyRead</c>：在 timeoutMs 内轮询等待接收缓冲出现字节。
    /// <para>返回 true 表示已有数据可读；返回 false 表示等待超时（C++ 侧同样不是错误）。</para>
    /// </summary>
    private bool WaitForReadyRead(int timeoutMs)
    {
        if (timeoutMs <= 0) timeoutMs = 1;
        var deadline = Environment.TickCount64 + timeoutMs;
        while (_port.BytesToRead == 0)
        {
            if (Environment.TickCount64 >= deadline) return false;
            Thread.Sleep(1);
        }
        return true;
    }

    private bool WaitForBytesWritten(int timeoutMs)
    {
        if (timeoutMs <= 0) timeoutMs = 1;
        var deadline = Environment.TickCount64 + timeoutMs;
        while (_port.BytesToWrite > 0)
        {
            if (Environment.TickCount64 >= deadline) return false;
            Thread.Sleep(1);
        }
        return true;
    }

    private void DrainAvailable()
    {
        while (_port.BytesToRead > 0)
        {
            var chunk = new byte[_port.BytesToRead];
            var read = _port.Read(chunk, 0, chunk.Length);
            if (read <= 0) break;
            if (read != chunk.Length) Array.Resize(ref chunk, read);
            _buffer.AddRange(chunk);
        }
    }

    public bool ReadExactly(long size, out byte[] output, int timeoutMs, out string error)
    {
        output = Array.Empty<byte>();
        error = string.Empty;
        if (size < 0)
        {
            error = "读取参数错误";
            return false;
        }

        var timer = Stopwatch.StartNew();
        while (_buffer.Count < size)
        {
            if (timer.ElapsedMilliseconds >= timeoutMs)
            {
                error = $"串口读取超时（{_buffer.Count}/{size} 字节）";
                return false;
            }
            if (!FillBuffer(timeoutMs - (int)timer.ElapsedMilliseconds, out error)) return false;
        }

        output = TakeFirst(size);
        return true;
    }

    public bool PeekExactly(long size, out byte[] output, int timeoutMs, out string error)
    {
        output = Array.Empty<byte>();
        error = string.Empty;
        if (size < 0)
        {
            error = "预读参数错误";
            return false;
        }

        var timer = Stopwatch.StartNew();
        while (_buffer.Count < size)
        {
            if (timer.ElapsedMilliseconds >= timeoutMs)
            {
                error = $"串口预读超时（{_buffer.Count}/{size} 字节）";
                return false;
            }
            if (!FillBuffer(timeoutMs - (int)timer.ElapsedMilliseconds, out error)) return false;
        }

        // 只看不消费：复制前 size 字节，但不动 _buffer。
        output = CopyFirst(size);
        return true;
    }

    public bool ReadUntil(byte[] marker, out byte[] output, long maximumSize, int timeoutMs, out string error)
    {
        output = Array.Empty<byte>();
        error = string.Empty;
        if (marker == null || marker.Length == 0)
        {
            error = "帧读取参数错误";
            return false;
        }

        var timer = Stopwatch.StartNew();
        for (;;)
        {
            var position = IndexOf(_buffer, marker);
            if (position >= 0)
            {
                var frameSize = position + marker.Length;
                output = TakeFirst(frameSize);
                // Keep every byte after the XML terminator. Firehose may coalesce the
                // ACK and the beginning of a RAW read into one USB transfer. GPT data
                // commonly starts with hundreds of zero bytes; treating those bytes as
                // XML padding truncates the RAW payload and makes the host wait forever
                // for a tail that the target already sent.
                return true;
            }
            if (_buffer.Count > maximumSize)
            {
                error = $"接收帧超过限制：{maximumSize} 字节";
                return false;
            }
            if (timer.ElapsedMilliseconds >= timeoutMs)
            {
                error = "等待协议帧超时";
                return false;
            }
            if (!FillBuffer(timeoutMs - (int)timer.ElapsedMilliseconds, out error)) return false;
        }
    }

    private byte[] CopyFirst(long size)
    {
        var count = (int)size;
        var result = new byte[count];
        _buffer.CopyTo(0, result, 0, count);
        return result;
    }

    private byte[] TakeFirst(long size)
    {
        var result = CopyFirst(size);
        _buffer.RemoveRange(0, (int)size);
        return result;
    }

    private static long IndexOf(List<byte> haystack, byte[] needle)
    {
        var limit = haystack.Count - needle.Length;
        for (var i = 0; i <= limit; i++)
        {
            var found = true;
            for (var j = 0; j < needle.Length; j++)
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
}
