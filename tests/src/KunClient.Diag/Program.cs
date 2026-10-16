using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Management;
using System.Text;
using System.Threading;

Console.OutputEncoding = Encoding.UTF8;

Console.WriteLine("╔══════════════════════════════════════════════════════════╗");
Console.WriteLine("║   KunClient Device Diagnostic                         ║");
Console.WriteLine("╚══════════════════════════════════════════════════════════╝");
Console.WriteLine();

// Step 1: Check WMI for Qualcomm devices
Console.WriteLine("[1] Checking WMI for Qualcomm 9008 devices...");
try
{
    using var searcher = new ManagementObjectSearcher(
        "SELECT Name, PNPDeviceID, Status, Service, DeviceID FROM Win32_PnPEntity " +
        "WHERE PNPDeviceID LIKE \"%VID_05C6%\"");

    var devices = new List<ManagementObject>();
    foreach (ManagementObject obj in searcher.Get())
        devices.Add(obj);

    Console.WriteLine($"    Found {devices.Count} Qualcomm device(s):");
    foreach (var dev in devices)
    {
        var name = dev["Name"]?.ToString() ?? "Unknown";
        var pnpId = dev["PNPDeviceID"]?.ToString() ?? "";
        var status = dev["Status"]?.ToString() ?? "Unknown";
        var service = dev["Service"]?.ToString() ?? "No service";
        var deviceId = dev["DeviceID"]?.ToString() ?? "";
        
        Console.WriteLine($"    - {name}");
        Console.WriteLine($"      PNPDeviceID: {pnpId}");
        Console.WriteLine($"      Status: {status}");
        Console.WriteLine($"      Service: {service}");
        Console.WriteLine($"      DeviceID: {deviceId}");
        Console.WriteLine();
    }
}
catch (Exception ex)
{
    Console.WriteLine($"    Error: {ex.Message}");
}

// Step 2: Check serial ports
Console.WriteLine("[2] Checking serial ports...");
foreach (var port in SerialPort.GetPortNames().OrderBy(p => p))
{
    try
    {
        using var p = new SerialPort(port, 115200, Parity.None, 8, StopBits.One)
        {
            ReadTimeout = 500,
            WriteTimeout = 500,
            DtrEnable = false,
            RtsEnable = false,
            Handshake = Handshake.None
        };
        p.Open();
        Console.WriteLine($"    {port}: Opened successfully");
        p.Close();
        Console.WriteLine($"    {port}: Closed");
    }
    catch (Exception ex)
    {
        Console.WriteLine($"    {port}: Error - {ex.Message}");
    }
}
Console.WriteLine();

// Step 3: Comprehensive protocol detection
string targetPort = "COM5";
Console.WriteLine($"[3] Comprehensive protocol detection on {targetPort}...");
Console.WriteLine();

// This test tries ALL possible protocols: Sahara, Firehose, NAND PRG
// It follows the exact flow from bkerler/edl:
// 1. Open port with DTR=HIGH
// 2. Try to read (get HELLO_REQ or other response)
// 3. If no data, send Firehose probe (XML nop)
// 4. If no data, send NAND PRG magic packet
// 5. If Sahara END_TRANSFER, try RESET_REQ
// 6. If RESET succeeds, toggle DTR and try again

try
{
    using var port = new SerialPort(targetPort, 115200, Parity.None, 8, StopBits.One)
    {
        ReadTimeout = 300,
        WriteTimeout = 300,
        DtrEnable = true,  // DTR HIGH from start
        RtsEnable = true,
        Handshake = Handshake.None
    };
    port.Open();
    Console.WriteLine("    Port opened (DTR=HIGH, RTS=HIGH)");
    
    // Phase 1: Read first response (Sahara HELLO_REQ, Firehose XML, or NAND PRG)
    Console.WriteLine("    Phase 1: Waiting for initial packet...");
    byte[]? initialData = ReadWithTimeout(port, 3000);
    
    if (initialData == null || initialData.Length == 0)
    {
        Console.WriteLine("    Phase 1: No initial data");
    }
    else
    {
        Console.WriteLine($"    Phase 1: Received {initialData.Length} bytes");
        Console.WriteLine("    Hex dump:");
        PrintHex(initialData);
        
        // Check for Firehose XML
        string str = System.Text.Encoding.UTF8.GetString(initialData);
        if (str.Contains("<?xml"))
        {
            Console.WriteLine("    ✅ Firehose mode detected!");
            port.Close();
            return;
        }
        
        // Check for NAND PRG
        if (initialData.Length > 1 && initialData[0] == 0x7E)
        {
            Console.WriteLine("    ✅ NAND PRG mode detected!");
            port.Close();
            return;
        }
        
        // Check for Sahara commands
        if (initialData.Length >= 4)
        {
            uint cmd = BitConverter.ToUInt32(initialData, 0);
            Console.WriteLine($"    Sahara command: 0x{cmd:X}");
            
            if (cmd == 0x01)  // HELLO_REQ
            {
                Console.WriteLine("    ✅ HELLO_REQ received!");
                ParseAndHandleHelloRequest(port, initialData);
                port.Close();
                return;
            }
            else if (cmd == 0x04)  // END_TRANSFER
            {
                Console.WriteLine("    ⚠️ END_TRANSFER - device in error state");
                // Continue to recovery
            }
        }
    }
    
    // Phase 2: No initial data or END_TRANSFER - try Firehose probe
    Console.WriteLine();
    Console.WriteLine("    Phase 2: Firehose probe (XML nop)...");
    port.Write(System.Text.Encoding.UTF8.GetBytes("<?xml version=\"1.0\" ?><data><nop /></data>"), 0, 38);
    
    byte[]? firehoseResp = ReadWithTimeout(port, 2000);
    if (firehoseResp != null && firehoseResp.Length > 0)
    {
        Console.WriteLine($"    Firehose response: {firehoseResp.Length} bytes");
        string str2 = System.Text.Encoding.UTF8.GetString(firehoseResp);
        if (str2.Contains("<?xml"))
        {
            Console.WriteLine("    ✅ Firehose mode confirmed!");
            port.Close();
            return;
        }
        
        if (firehoseResp.Length >= 4)
        {
            uint cmd2 = BitConverter.ToUInt32(firehoseResp, 0);
            Console.WriteLine($"    Response cmd: 0x{cmd2:X}");
            
            if (cmd2 == 0x04)  // END_TRANSFER
            {
                Console.WriteLine("    ⚠️ Still END_TRANSFER after Firehose probe");
                // Try NAND PRG probe
            }
            else if (cmd2 == 0x01)
            {
                Console.WriteLine("    ✅ HELLO_REQ after probe!");
                ParseAndHandleHelloRequest(port, firehoseResp);
                port.Close();
                return;
            }
        }
    }
    else
    {
        Console.WriteLine("    No response to Firehose probe");
    }
    
    // Phase 3: Try NAND PRG magic packet
    Console.WriteLine();
    Console.WriteLine("    Phase 3: NAND PRG magic packet...");
    var nandPrgMagic = new byte[] { 0x7E, 0x11, 0x00, 0x12, 0x00, 0xA0, 0xE3, 0x00, 0x00, 0xC1, 0xE5, 0x01, 0x40, 0xA0, 0xE3, 0x1E, 0xFF, 0x2F, 0xE1, 0x4B, 0xD9, 0x7E };
    port.Write(nandPrgMagic, 0, nandPrgMagic.Length);
    
    byte[]? nandResp = ReadWithTimeout(port, 2000);
    if (nandResp != null && nandResp.Length > 1)
    {
        if (nandResp[1] == 0x12)
        {
            Console.WriteLine("    ✅ NAND PRG mode detected!");
            port.Close();
            return;
        }
        
        if (nandResp.Length >= 4)
        {
            uint cmd3 = BitConverter.ToUInt32(nandResp, 0);
            Console.WriteLine($"    NAND PRG response: cmd=0x{cmd3:X}");
            
            if (cmd3 == 0x04)
            {
                Console.WriteLine("    ⚠️ Still END_TRANSFER after NAND PRG probe");
            }
        }
    }
    else
    {
        Console.WriteLine("    No response to NAND PRG probe");
    }
    
    // Phase 4: Try Sahara RESET_REQ
    Console.WriteLine();
    Console.WriteLine("    Phase 4: Sahara RESET_REQ...");
    byte[]? resetResp = SendAndReceive(port, 0x07, 0, 2000);
    
    if (resetResp != null && resetResp.Length >= 4)
    {
        uint resetCmd = BitConverter.ToUInt32(resetResp, 0);
        Console.WriteLine($"    RESET response: cmd=0x{resetCmd:X}");
        
        if (resetCmd == 0x08)
        {
            Console.WriteLine("    ✅ RESET_RSP! Device reset!");
            
            // Toggle DTR to trigger HELLO_REQ after reset
            port.DtrEnable = false;
            Thread.Sleep(200);
            port.DtrEnable = true;
            Thread.Sleep(300);
            
            Console.WriteLine("    Waiting for HELLO_REQ after reset...");
            byte[]? postResetData = ReadWithTimeout(port, 3000);
            if (postResetData != null && postResetData.Length >= 4)
            {
                uint postResetCmd = BitConverter.ToUInt32(postResetData, 0);
                Console.WriteLine($"    Post-reset response: cmd=0x{postResetCmd:X}");
                
                if (postResetCmd == 0x01)
                {
                    Console.WriteLine("    ✅✅✅ HELLO_REQ after RESET!");
                    ParseAndHandleHelloRequest(port, postResetData);
                }
            }
        }
        else if (resetCmd == 0x04)
        {
            Console.WriteLine("    ⚠️ END_TRANSFER in response to RESET_REQ");
        }
    }
    else
    {
        Console.WriteLine("    No response to RESET_REQ");
    }
    
    port.Close();
    Console.WriteLine("    Port closed.");
}
catch (Exception ex)
{
    Console.WriteLine($"    Error: {ex.Message}");
}

Console.WriteLine();

// ============================================================
// Helper functions
// ============================================================

byte[]? ReadWithTimeout(SerialPort port, int timeoutMs) => TryReadWithTimeout(port, timeoutMs);

byte[]? TryReadWithTimeout(SerialPort port, int timeoutMs)
{
    try
    {
        port.ReadTimeout = timeoutMs;
        byte[] buffer = new byte[1024];
        int read = port.Read(buffer, 0, buffer.Length);
        if (read > 0)
        {
            byte[] result = new byte[read];
            Array.Copy(buffer, result, read);
            return result;
        }
        return null;
    }
    catch (TimeoutException)
    {
        return null;
    }
}

byte[]? SendAndReceive(SerialPort port, uint cmd, uint payloadSize, int responseTimeoutMs)
{
    // Send command header (8 bytes)
    byte[] request = new byte[8];
    BitConverter.GetBytes(cmd).CopyTo(request, 0);
    BitConverter.GetBytes((uint)8 + payloadSize).CopyTo(request, 4);
    port.Write(request, 0, 8);
    
    // Read response
    port.ReadTimeout = responseTimeoutMs;
    byte[]? response = TryReadWithTimeout(port, responseTimeoutMs);
    
    if (response != null && response.Length >= 8)
    {
        uint respCmd = BitConverter.ToUInt32(response, 0);
        uint respLen = BitConverter.ToUInt32(response, 4);
        Console.WriteLine($"    -> Sent cmd=0x{cmd:X}, got response cmd=0x{respCmd:X}, len={respLen}");
        return response;
    }
    
    return response;
}

Console.WriteLine();

// ============================================================
// Helper functions
// ============================================================

byte[]? TryReadFullPacket(SerialPort port, int timeoutMs)
{
    try
    {
        port.ReadTimeout = timeoutMs;
        
        // Read header (8 bytes)
        byte[] header = new byte[8];
        int headerPos = 0;
        long start = DateTime.UtcNow.Ticks;
        
        while (headerPos < 8)
        {
            int toRead = 8 - headerPos;
            int read = port.Read(header, headerPos, toRead);
            headerPos += read;
            
            if (headerPos >= 8) break;
            
            long elapsedMs = (DateTime.UtcNow.Ticks - start) / TimeSpan.TicksPerMillisecond;
            if (elapsedMs > timeoutMs)
            {
                if (headerPos > 0)
                {
                    byte[] partial = new byte[headerPos];
                    Array.Copy(header, partial, headerPos);
                    return partial;
                }
                return null;
            }
            Thread.Sleep(5);
        }
        
        uint cmd = BitConverter.ToUInt32(header, 0);
        uint length = BitConverter.ToUInt32(header, 4);
        
        // Validate length
        if (length == 0 || length > 10000)
        {
            // Invalid length, return header only
            return header;
        }
        
        // Read remaining data
        byte[] fullPacket = new byte[length];
        Array.Copy(header, fullPacket, 8);
        int remaining = (int)length - 8;
        int readPos = 0;
        
        while (readPos < remaining)
        {
            int toRead = remaining - readPos;
            int read = port.Read(fullPacket, 8 + readPos, toRead);
            readPos += read;
            
            if (readPos >= remaining) break;
            
            long elapsedMs = (DateTime.UtcNow.Ticks - start) / TimeSpan.TicksPerMillisecond;
            if (elapsedMs > timeoutMs) break;
            Thread.Sleep(5);
        }
        
        // Trim to actual size
        if (readPos < remaining)
        {
            byte[] result = new byte[8 + readPos];
            Array.Copy(fullPacket, result, result.Length);
            return result;
        }
        
        return fullPacket;
    }
    catch (TimeoutException)
    {
        return null;
    }
}

void ParseAndHandleHelloRequest(SerialPort port, byte[] helloReq)
{
    if (helloReq.Length < 48)
    {
        Console.WriteLine($"    ⚠️ HELLO_REQ too short: {helloReq.Length} bytes (expected 48)");
        return;
    }
    
    uint version = BitConverter.ToUInt32(helloReq, 8);
    uint versionMin = BitConverter.ToUInt32(helloReq, 12);
    uint cmdPacketLen = BitConverter.ToUInt32(helloReq, 16);
    uint mode = BitConverter.ToUInt32(helloReq, 20);
    Console.WriteLine($"    Device info: Version={version}, VersionMin={versionMin}, CmdPacketLen={cmdPacketLen}, Mode=0x{mode:X}");
    
    // Send HELLO_RSP back
    var helloRsp = new byte[48];
    BitConverter.GetBytes((uint)0x02).CopyTo(helloRsp, 0);    // HELLO_RSP
    BitConverter.GetBytes((uint)48).CopyTo(helloRsp, 4);       // length
    BitConverter.GetBytes(version).CopyTo(helloRsp, 8);         // version
    BitConverter.GetBytes((uint)1).CopyTo(helloRsp, 12);       // version_min
    BitConverter.GetBytes(cmdPacketLen).CopyTo(helloRsp, 16);   // max_cmd_len
    BitConverter.GetBytes((uint)3).CopyTo(helloRsp, 20);       // mode = COMMAND
    BitConverter.GetBytes((uint)1).CopyTo(helloRsp, 24);       // reserved1
    BitConverter.GetBytes((uint)2).CopyTo(helloRsp, 28);       // reserved2
    BitConverter.GetBytes((uint)3).CopyTo(helloRsp, 32);       // reserved3
    BitConverter.GetBytes((uint)4).CopyTo(helloRsp, 36);       // reserved4
    BitConverter.GetBytes((uint)5).CopyTo(helloRsp, 40);       // reserved5
    BitConverter.GetBytes((uint)6).CopyTo(helloRsp, 44);       // reserved6
    port.Write(helloRsp, 0, 48);
    Console.WriteLine("    HELLO_RSP sent!");
    
    // Read CMD_READY
    byte[]? readyPacket = TryReadFullPacket(port, 2000);
    if (readyPacket != null && readyPacket.Length >= 4)
    {
        uint readyCmd = BitConverter.ToUInt32(readyPacket, 0);
        Console.WriteLine($"    CMD_READY response: cmd=0x{readyCmd:X}");
        if (readyCmd == 0x0B)
        {
            Console.WriteLine("    ✅✅✅ SAHARA HANDSHAKE COMPLETE! Device in COMMAND mode!");
        }
        else if (readyCmd == 0x04)
        {
            uint status = readyPacket.Length >= 16 ? BitConverter.ToUInt32(readyPacket, 12) : 0;
            Console.WriteLine($"    ⚠️ END_TRANSFER after HELLO_RSP: status=0x{status:X} ({GetErrorDesc(status)})");
        }
        else
        {
            Console.WriteLine("    Unexpected response after HELLO_RSP:");
            PrintHex(readyPacket);
        }
    }
}

void PrintHex(byte[] data)
{
    for (int i = 0; i < data.Length; i += 16)
    {
        string hex = string.Join(" ", data.Skip(i).Take(Math.Min(16, data.Length - i)).Select(b => b.ToString("X2")));
        Console.WriteLine($"      {i:X4}: {hex}");
    }
}

// Helper function for error description
string GetErrorDesc(uint status)
{
    var errors = new Dictionary<uint, string>
    {
        { 0x00, "Invalid command received in current state" },
        { 0x01, "Protocol mismatch between host and target" },
        { 0x02, "Invalid target protocol version" },
        { 0x03, "Invalid host protocol version" },
        { 0x04, "Invalid packet size received" },
        { 0x05, "Unexpected image ID received" },
        { 0x06, "Invalid image header size received" },
        { 0x07, "Invalid image data size received" },
        { 0x08, "Invalid image type received" },
        { 0x0D, "Cannot receive specified number of program headers" },
        { 0x14, "Unknown host error received in HELLO_RESP" },
        { 0x15, "Timeout while receiving data" },
        { 0x16, "Timeout while transmitting data" },
        { 0x17, "Invalid mode received from host" },
    };
    return errors.ContainsKey(status) ? errors[status] : $"Unknown error (0x{status:X})";
}

// Step 4: Summary
Console.WriteLine();
Console.WriteLine("╔══════════════════════════════════════════════════════════╗");
Console.WriteLine("║   Summary                                              ║");
Console.WriteLine("╚══════════════════════════════════════════════════════════╝");
Console.WriteLine();
Console.WriteLine("If the device is not responding to serial commands:");
Console.WriteLine("  1. The device may be in Firehose mode (not Sahara)");
Console.WriteLine("  2. The qcusbser driver may need reinstallation");
Console.WriteLine("  3. The device may need a full power cycle");
Console.WriteLine();
Console.WriteLine("Recommended actions:");
Console.WriteLine("  - Try using QPST or QFIL to verify the device works");
Console.WriteLine("  - Uninstall and reinstall the qcusbser driver");
Console.WriteLine("  - Try a different USB port");
Console.WriteLine("  - If possible, install WinUSB driver and use that instead");
