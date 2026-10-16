using System;
using System.Collections.Generic;

namespace KunFlashTool.Core.SwtEdl;

/// <summary>
/// SWT-EDL 的公共类型层。**逐文件对应 `third_party/swt-edl/src/core/Types.h`**，
/// 字段名/枚举序/文案一字不改，方便两边对照排查。
///
/// <para>Qt 类型到 C# 的固定映射（全工程一致）：<br/>
/// <c>QString→string</c>、<c>QStringList→List&lt;string&gt;</c>、<c>QByteArray→byte[]</c>、
/// <c>QVector&lt;T&gt;→List&lt;T&gt;</c>、<c>QHash&lt;K,V&gt;→Dictionary&lt;K,V&gt;</c>、
/// <c>quint64→ulong</c>、<c>quint32→uint</c>、<c>quint16→ushort</c>、<c>qsizetype→long</c>、
/// <c>std::function</c>→委托。</para>
/// </summary>
public enum StorageType
{
    Emmc,
    Ufs,
}

public enum SessionState
{
    Disconnected,
    OpeningPort,
    SaharaHandshake,
    UploadingProgrammer,
    OplusVipVerification,
    WaitingForFirehose,
    Configuring,
    Ready,
    Reading,
    Writing,
    Erasing,
    Rebooting,
    Failed,
}

/// <summary>状态文案与稳定机器标识（对应 Types.h 的 storageName/stateText/stateKey）。</summary>
public static class EdlTypes
{
    public static string StorageName(StorageType type) => type == StorageType.Ufs ? "ufs" : "emmc";

    public static string StateText(SessionState state) => state switch
    {
        SessionState.Disconnected => "未连接",
        SessionState.OpeningPort => "正在打开端口",
        SessionState.SaharaHandshake => "Sahara 握手",
        SessionState.UploadingProgrammer => "传输 Programmer",
        SessionState.OplusVipVerification => "OPPO VIP 验证",
        SessionState.WaitingForFirehose => "等待 Firehose",
        SessionState.Configuring => "配置存储",
        SessionState.Ready => "EDL 已就绪",
        SessionState.Reading => "正在读取",
        SessionState.Writing => "正在写入",
        SessionState.Erasing => "正在擦除",
        SessionState.Rebooting => "正在重启",
        SessionState.Failed => "操作失败",
        _ => "未知状态",
    };

    /// <summary>
    /// 状态的**稳定机器标识**（小驼峰 ASCII），供外部程序（脚本 / headless 驱动）判断用。
    /// <para>⚠️ 与 <see cref="StateText"/> 分开是有意的：文案是给人看的中文、随时可能改措辞；
    /// 而这套 id 会被外部程序当协议值比对，**改动等于破坏兼容**。加状态可以，改已有 id 不行。</para>
    /// </summary>
    public static string StateKey(SessionState state) => state switch
    {
        SessionState.Disconnected => "disconnected",
        SessionState.OpeningPort => "openingPort",
        SessionState.SaharaHandshake => "saharaHandshake",
        SessionState.UploadingProgrammer => "uploadingProgrammer",
        SessionState.OplusVipVerification => "oplusVipVerification",
        SessionState.WaitingForFirehose => "waitingForFirehose",
        SessionState.Configuring => "configuring",
        SessionState.Ready => "ready",
        SessionState.Reading => "reading",
        SessionState.Writing => "writing",
        SessionState.Erasing => "erasing",
        SessionState.Rebooting => "rebooting",
        SessionState.Failed => "failed",
        _ => "unknown",
    };
}

public sealed class PartitionInfo
{
    public string Name { get; set; } = "";
    public string TypeGuid { get; set; } = "";
    public string UniqueGuid { get; set; } = "";
    public uint Lun { get; set; }
    public ulong FirstLba { get; set; }
    public ulong LastLba { get; set; }
    public ulong Attributes { get; set; }

    public ulong SectorCount => LastLba >= FirstLba ? LastLba - FirstLba + 1 : 0;

    public PartitionInfo Clone() => (PartitionInfo)MemberwiseClone();
}

public sealed class DeviceInfo
{
    public string PortName { get; set; } = "";
    public string Storage { get; set; } = "";
    public string Programmer { get; set; } = "";
    public string Serial { get; set; } = "";
    public string Hwid { get; set; } = "";
    public string PkHash { get; set; } = "";
    public string VipMode { get; set; } = "";
    public string Brand { get; set; } = "";
    public string Model { get; set; } = "";
    public string MsmId { get; set; } = "";
    public string SaharaVersion { get; set; } = "";
    public string SblVersion { get; set; } = "";
    public string StorageProduct { get; set; } = "";
    public string ProductName { get; set; } = "";
    public string AndroidVersion { get; set; } = "";
    public string SystemVersion { get; set; } = "";
    public string SecurityPatch { get; set; } = "";
    public string BuildFingerprint { get; set; } = "";
    public ulong StorageBytes { get; set; }

    public DeviceInfo Clone() => (DeviceInfo)MemberwiseClone();
}

public sealed class OplusVipPackage
{
    public bool Enabled { get; set; }
    public string PackageDirectory { get; set; } = "";
    public string ProgrammerPath { get; set; } = "";

    /// <summary>
    /// 有些 OPPO 包里同一个 Firehose 载荷被多把 OEM 根签名过（例如 DevprgProgrammer1/2.melf）。
    /// 发现时把确定性优选的那个排在最前，同时把兼容的兄弟文件一起带出来，
    /// 这样 Sahara 在遇到认证 NAK 之后还能依次回落。
    /// </summary>
    public List<string> ProgrammerCandidates { get; set; } = new();

    public string DigestPath { get; set; } = "";
    public string SignaturePath { get; set; } = "";
}

public delegate void LogCallback(string message);

public delegate void ProgressCallback(ulong completed, ulong total);

/// <summary>
/// 对应 C++ 侧的 <c>std::atomic_bool</c> 取消标志。
/// <para>为什么不用 <see cref="System.Threading.CancellationToken"/>：那个**不可重置**，
/// 而 EdlWorker 每开始一个新操作都要把上一轮的取消状态清掉（C++ 侧也是直接 <c>store(false)</c>）。</para>
/// </summary>
public sealed class EdlCancel
{
    private volatile bool _cancelled;

    public bool IsCancelled => _cancelled;

    public void Cancel() => _cancelled = true;

    public void Reset() => _cancelled = false;
}
