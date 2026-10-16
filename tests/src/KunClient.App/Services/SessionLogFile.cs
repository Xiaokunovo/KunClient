using System;
using System.IO;
using System.Text;

namespace KunFlashTool.App.Services;

/// <summary>
/// Streams every log line to <c>logs/KunFlashTool_yyyyMMdd_HHmmss.log</c> next to
/// the executable so a session's full log survives crashes and can be shared
/// as-is. One file per application run; writing is fail-safe (never throws to
/// callers — logging must never take the app down).
/// </summary>
public static class SessionLogFile
{
    private static readonly object Gate = new();
    private static StreamWriter? _writer;

    public static void Append(DateTime timestamp, string level, string message)
    {
        try
        {
            lock (Gate)
            {
                if (_writer == null)
                {
                    var dir = Path.Combine(AppContext.BaseDirectory, "logs");
                    Directory.CreateDirectory(dir);
                    var path = Path.Combine(dir, $"KunClient_{timestamp:yyyyMMdd_HHmmss}.log");
                    _writer = new StreamWriter(path, append: false, Encoding.UTF8) { AutoFlush = true };
                }
                _writer.WriteLine($"[{timestamp:HH:mm:ss}] [{level}] {message}");
            }
        }
        catch
        {
            // Disk full / anti-virus lock / shutdown race — drop the line silently.
        }
    }
}
