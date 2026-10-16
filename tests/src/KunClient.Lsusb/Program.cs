using System;
using System.Collections.Generic;
using System.Management;

namespace KunFlashTool.Tools;

/// <summary>
/// A minimal lsusb.exe substitute for Windows.
/// Queries WMI for Qualcomm USB devices and outputs them in a format
/// compatible with the KunFlashTool device scanner.
///
/// Output format (one line per device):
///   Bus 001 Device 003: ID 05c6:9008 Qualcomm HS-USB QDLoader 9008 (COM5)
/// </summary>
internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT Name, PNPDeviceID FROM Win32_PnPEntity");

            int busNum = 1;
            int deviceNum = 1;

            foreach (ManagementObject obj in searcher.Get())
            {
                var name = obj["Name"]?.ToString() ?? "";
                var pnpId = obj["PNPDeviceID"]?.ToString() ?? "";

                // Filter: must be a USB device AND Qualcomm VID
                if (!pnpId.StartsWith("USB\\", StringComparison.OrdinalIgnoreCase))
                    continue;
                if (!pnpId.Contains("VID_05C6", StringComparison.OrdinalIgnoreCase))
                    continue;

                // Extract VID:PID from PNPDeviceID like "USB\VID_05C6&PID_9008\6&E7B2..."
                var vidPid = ExtractVidPid(pnpId);
                if (vidPid == null)
                    continue;

                var pid = vidPid.Value.pid;
                if (!IsQualcommEdlPid(pid))
                    continue;

                // Extract COM port from name if present
                var comPort = ExtractComPort(name);
                var displayName = CleanDisplayName(name);

                var line = $"Bus {busNum:D3} Device {deviceNum:D3}: ID {vidPid.Value.vid:X4}:{pid:X4} {displayName}";
                if (!string.IsNullOrEmpty(comPort))
                    line += $" ({comPort})";

                Console.WriteLine(line);
                deviceNum++;
            }

            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"lsusb error: {ex.Message}");
            return 1;
        }
    }

    private static (int vid, int pid)? ExtractVidPid(string pnpId)
    {
        var vidMatch = System.Text.RegularExpressions.Regex.Match(
            pnpId, @"VID_([0-9A-Fa-f]{4})");
        var pidMatch = System.Text.RegularExpressions.Regex.Match(
            pnpId, @"PID_([0-9A-Fa-f]{4})");

        if (!vidMatch.Success || !pidMatch.Success)
            return null;

        return (
            Convert.ToInt32(vidMatch.Groups[1].Value, 16),
            Convert.ToInt32(pidMatch.Groups[1].Value, 16)
        );
    }

    private static bool IsQualcommEdlPid(int pid)
    {
        int[] known = {
            0x9008, 0x900E, 0x9011, 0x902B, 0x902D,
            0x9075, 0x9076, 0x908B, 0x9091, 0x909E,
            0x90A9, 0x90B5, 0x90BF, 0x90C0, 0x90C3,
            0x90D8, 0x90DC, 0x90DD, 0x90DF, 0x91D1,
            0x91DB, 0x91DC, 0xA008, 0xA009
        };
        if (Array.IndexOf(known, pid) >= 0)
            return true;
        return pid >= 0x9000 && pid <= 0x9FFF;
    }

    private static string ExtractComPort(string name)
    {
        var match = System.Text.RegularExpressions.Regex.Match(
            name, @"\(COM\d+\)");
        return match.Success ? match.Value.Trim('(', ')') : "";
    }

    private static string CleanDisplayName(string name)
    {
        var idx = name.IndexOf(" (COM", StringComparison.OrdinalIgnoreCase);
        if (idx > 0)
            return name.Substring(0, idx);
        return name;
    }
}
