using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace KunFlashTool.Core.Services;

/// <summary>
/// Super 分区合并服务
/// 支持 super_def.*.json 配置解析，simg2img 转换 + lpmake 合并
/// 参考: OplusEdlTool Services/SuperMergeService.cs
/// </summary>
public sealed class SuperMergeService
{
    private readonly Action<string>? _log;
    private readonly Action<int>? _progress;
    private bool _isCancelled;

    public SuperMergeService(Action<string>? log = null, Action<int>? progress = null)
    {
        _log = log;
        _progress = progress;
    }

    public class SuperConfig
    {
        public SuperMeta? SuperMeta { get; set; }
        public List<BlockDevice> BlockDevices { get; set; } = [];
        public List<Group> Groups { get; set; } = [];
        public List<Partition> Partitions { get; set; } = [];
    }

    public class SuperMeta
    {
        public string Path { get; set; } = "";
        public string Size { get; set; } = "65536";
    }

    public class BlockDevice
    {
        public string Name { get; set; } = "super";
        public string Size { get; set; } = "";
        public string BlockSize { get; set; } = "4096";
        public string Alignment { get; set; } = "1048576";
    }

    public class Group
    {
        public string Name { get; set; } = "";
        public string? MaximumSize { get; set; }
    }

    public class Partition
    {
        public string Name { get; set; } = "";
        public string? Path { get; set; }
        public string? Size { get; set; }
        public string GroupName { get; set; } = "";
        public bool IsDynamic { get; set; } = true;
    }

    public void Cancel() => _isCancelled = true;

    public SuperConfig? ParseSuperDefJson(string jsonPath)
    {
        try
        {
            if (!File.Exists(jsonPath))
            {
                _log?.Invoke($"Error: JSON file not found - {jsonPath}");
                return null;
            }

            var jsonText = File.ReadAllText(jsonPath);
            using var doc = JsonDocument.Parse(jsonText);
            var root = doc.RootElement;
            var config = new SuperConfig();

            if (root.TryGetProperty("super_meta", out var superMeta))
            {
                config.SuperMeta = new SuperMeta
                {
                    Path = superMeta.TryGetProperty("path", out var p) ? p.GetString() ?? "" : "",
                    Size = superMeta.TryGetProperty("size", out var s) ? s.GetString() ?? "65536" : "65536"
                };
            }

            if (root.TryGetProperty("block_devices", out var blockDevices))
            {
                foreach (var dev in blockDevices.EnumerateArray())
                {
                    config.BlockDevices.Add(new BlockDevice
                    {
                        Name = dev.TryGetProperty("name", out var n) ? n.GetString() ?? "super" : "super",
                        Size = dev.TryGetProperty("size", out var sz) ? sz.GetString() ?? "" : "",
                        BlockSize = dev.TryGetProperty("block_size", out var bs) ? bs.GetString() ?? "4096" : "4096",
                        Alignment = dev.TryGetProperty("alignment", out var al) ? al.GetString() ?? "1048576" : "1048576"
                    });
                }
            }

            if (root.TryGetProperty("groups", out var groups))
            {
                foreach (var grp in groups.EnumerateArray())
                {
                    config.Groups.Add(new Group
                    {
                        Name = grp.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "",
                        MaximumSize = grp.TryGetProperty("maximum_size", out var ms) ? ms.GetString() : null
                    });
                }
            }

            if (root.TryGetProperty("partitions", out var partitions))
            {
                foreach (var part in partitions.EnumerateArray())
                {
                    config.Partitions.Add(new Partition
                    {
                        Name = part.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "",
                        Path = part.TryGetProperty("path", out var p) ? p.GetString() : null,
                        Size = part.TryGetProperty("size", out var s) ? s.GetString() : null,
                        GroupName = part.TryGetProperty("group_name", out var g) ? g.GetString() ?? "" : "",
                        IsDynamic = part.TryGetProperty("is_dynamic", out var d) && d.GetBoolean()
                    });
                }
            }

            _log?.Invoke($"Parsed config: {config.Partitions.Count} partitions");
            return config;
        }
        catch (Exception ex)
        {
            _log?.Invoke($"Error parsing JSON: {ex.Message}");
            return null;
        }
    }

    public string? FindSuperDefJson(string romBaseDir)
    {
        var metaDir = Path.Combine(romBaseDir, "META");
        if (!Directory.Exists(metaDir))
        {
            _log?.Invoke("META folder not found");
            return null;
        }

        var jsonFiles = Directory.GetFiles(metaDir, "super_def.*.json");
        if (jsonFiles.Length == 0)
        {
            _log?.Invoke("super_def.*.json not found in META folder");
            return null;
        }

        _log?.Invoke($"Found config: {Path.GetFileName(jsonFiles[0])}");
        return jsonFiles[0];
    }

    public async Task<bool> MergeSuperAsync(string imagesDir, SuperConfig config, string? romBaseDir = null, CancellationToken cancellationToken = default)
    {
        _isCancelled = false;
        romBaseDir ??= Directory.GetParent(imagesDir)?.FullName ?? imagesDir;

        var simg2imgPath = FindTool("simg2img.exe");
        var lpmakePath = FindTool("lpmake.exe");

        if (simg2imgPath == null) { _log?.Invoke("simg2img.exe not found"); return false; }
        if (lpmakePath == null) { _log?.Invoke("lpmake.exe not found"); return false; }

        var localSimg2img = Path.Combine(imagesDir, "simg2img.exe");
        var localLpmake = Path.Combine(imagesDir, "lpmake.exe");

        try
        {
            File.Copy(simg2imgPath, localSimg2img, true);
            File.Copy(lpmakePath, localLpmake, true);
            _log?.Invoke("Tools copied to IMAGES folder");
        }
        catch (Exception ex)
        {
            _log?.Invoke($"Failed to copy tools: {ex.Message}");
            return false;
        }

        // 2026-10-02 修复：原先用两个平行列表（partitionsWithPath × rawFiles）按下标配对，
        // 一旦某个镜像缺失或 simg2img 转换失败，后面的分区会拿到**前一个分区**的 raw 镜像
        // （--image B="A.raw"），等尺寸分区会静默刷入错误内容。改成「分区+产物」成对携带，
        // 转换失败的分区只跳过自己，绝不影响后续配对。
        var rawParts = new List<(Partition Part, string RawPath)>();
        var partitionsWithPath = config.Partitions.Where(p => !string.IsNullOrEmpty(p.Path)).ToList();
        int totalPartitions = partitionsWithPath.Count;
        int currentIndex = 0;

        try
        {
            _log?.Invoke("Stage 1: Converting img to raw...");

            foreach (var partition in partitionsWithPath)
            {
                if (_isCancelled || cancellationToken.IsCancellationRequested)
                {
                    _log?.Invoke("Operation cancelled");
                    return false;
                }

                var imgPath = Path.Combine(romBaseDir, partition.Path!.Replace("/", "\\"));
                if (!File.Exists(imgPath))
                {
                    _log?.Invoke($"Warning: {partition.Path} not found, skipping");
                    continue;
                }

                var imgFileName = Path.GetFileName(imgPath);
                var rawFileName = Path.GetFileNameWithoutExtension(imgFileName) + ".raw";
                var rawPath = Path.Combine(imagesDir, rawFileName);

                _log?.Invoke($"Converting {imgFileName}...");
                _progress?.Invoke((currentIndex * 50) / totalPartitions);

                var success = await RunProcessAsync(localSimg2img, $"\"{imgPath}\" \"{rawPath}\"", imagesDir);

                if (success && File.Exists(rawPath))
                {
                    rawParts.Add((partition, rawPath));
                    _log?.Invoke($"Converted: {imgFileName} -> {rawFileName}");
                }
                else
                {
                    _log?.Invoke($"Failed to convert: {imgFileName}");
                }

                currentIndex++;
            }

            if (rawParts.Count == 0)
            {
                _log?.Invoke("No raw files generated, aborting merge");
                return false;
            }

            _log?.Invoke("Stage 2: Merging raw files to super.img...");
            _progress?.Invoke(50);

            var outputPath = Path.Combine(imagesDir, "super.img");
            var lpmakeArgs = BuildLpmakeArgs(config, rawParts, imagesDir, outputPath);

            _log?.Invoke($"Running lpmake with {rawParts.Count} partitions...");
            var mergeSuccess = await RunProcessAsync(localLpmake, lpmakeArgs, imagesDir);

            if (mergeSuccess && File.Exists(outputPath))
            {
                var fileInfo = new FileInfo(outputPath);
                var sizeGB = fileInfo.Length / (1024.0 * 1024.0 * 1024.0);
                _log?.Invoke($"Super merge completed! Size: {sizeGB:F2} GB");
                _log?.Invoke($"Output: {outputPath}");
                _progress?.Invoke(100);

                _log?.Invoke("Cleaning up temporary raw files...");
                foreach (var (_, rawFile) in rawParts)
                {
                    try { if (File.Exists(rawFile)) File.Delete(rawFile); } catch { }
                }

                return true;
            }
            else
            {
                _log?.Invoke("lpmake failed to create super.img");
                return false;
            }
        }
        finally
        {
            try
            {
                if (File.Exists(localSimg2img)) File.Delete(localSimg2img);
                if (File.Exists(localLpmake)) File.Delete(localLpmake);
                _log?.Invoke("Tools cleaned up");
            }
            catch { }
        }
    }

    private string? FindTool(string toolName)
    {
        // Check Tools directory first
        var toolsDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Tools");
        var toolPath = Path.Combine(toolsDir, toolName);
        if (File.Exists(toolPath)) return toolPath;

        // Check PATH
        var pathVar = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (var dir in pathVar.Split(Path.PathSeparator))
        {
            var candidate = Path.Combine(dir.Trim(), toolName);
            if (File.Exists(candidate)) return candidate;
        }

        return null;
    }

    private string BuildLpmakeArgs(SuperConfig config, List<(Partition Part, string RawPath)> rawParts, string imagesDir, string outputPath)
    {
        var args = new List<string>();
        if (config.SuperMeta != null)
            args.Add($"--metadata-size {config.SuperMeta.Size}");
        args.Add("--metadata-slots 2");
        foreach (var dev in config.BlockDevices)
            args.Add($"--device {dev.Name}:{dev.Size}");
        foreach (var grp in config.Groups)
        {
            if (!string.IsNullOrEmpty(grp.MaximumSize))
                args.Add($"--group {grp.Name}:{grp.MaximumSize}");
        }

        // 成对携带（见 MergeSuperAsync 里的修复说明）：只合并「转换成功」的分区，绝不按位置错配。
        foreach (var (part, rawFile) in rawParts)
        {
            args.Add($"--partition {part.Name}:readonly:{part.Size}:{part.GroupName}");
            args.Add($"--image {part.Name}=\"{rawFile}\"");
        }

        args.Add($"--output \"{outputPath}\"");
        return string.Join(" ", args);
    }

    private async Task<bool> RunProcessAsync(string fileName, string arguments, string workingDir)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = fileName,
                Arguments = arguments,
                WorkingDirectory = workingDir,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            };

            using var process = new Process { StartInfo = psi };

            process.OutputDataReceived += (s, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                    _log?.Invoke($"[tool] {e.Data}");
            };

            process.ErrorDataReceived += (s, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                    _log?.Invoke($"[tool ERR] {e.Data}");
            };

            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            await Task.Run(() => process.WaitForExit());
            return process.ExitCode == 0;
        }
        catch (Exception ex)
        {
            _log?.Invoke($"Process error: {ex.Message}");
            return false;
        }
    }
}
