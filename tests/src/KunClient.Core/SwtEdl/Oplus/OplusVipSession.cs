using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace KunFlashTool.Core.SwtEdl.Oplus;

// Package discovery for the native OPPO VIP protocol implemented by FirehoseClient.
public static class OplusVipSession
{
    private static bool ContainsAny(string value, params string[] needles)
    {
        foreach (var needle in needles)
        {
            if (value.Contains(needle, StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    private sealed class ProgrammerCandidate
    {
        public string Path = "";
        public string Directory = "";
        public long Size;
        public int Score;
    }

    private static readonly Regex NumberedProgrammerSuffix =
        new(@"(?:devprg)?programmer(\d+)$", RegexOptions.IgnoreCase);

    private static int NumberedProgrammerPriority(string stem)
    {
        var match = NumberedProgrammerSuffix.Match(stem);
        if (!match.Success) return 0;
        var ok = int.TryParse(match.Groups[1].Value, out var number);
        // Programmer1 is the canonical package entry.  Later files usually carry
        // alternate certificate chains for another hardware/root-key revision.
        return ok ? Math.Max(1, 100 - number) : 0;
    }

    private static bool BetterPath(int score, string path, int currentScore, string currentPath)
    {
        return score > currentScore ||
               (score == currentScore &&
                (currentPath.Length == 0 ||
                 string.Compare(path, currentPath, StringComparison.OrdinalIgnoreCase) < 0));
    }

    private static bool IsStandaloneSignatureArtifact(string stem)
    {
        return stem == "sig" || stem.StartsWith("sig_", StringComparison.Ordinal) ||
               stem.StartsWith("sign", StringComparison.Ordinal) ||
               stem.StartsWith("signature", StringComparison.Ordinal);
    }

    public static bool DiscoverPackage(string directory, StorageType storage,
                                       out OplusVipPackage package, out string error,
                                       bool recursive = true, string preferredProgrammerPath = "")
    {
        package = new OplusVipPackage();
        error = string.Empty;
        if (!Directory.Exists(directory))
        {
            error = "VIP 引导包目录不存在";
            return false;
        }

        var rootPath = Path.GetFullPath(directory);

        // Common packages keep all three components together. Resolve that layout
        // before walking a potentially huge firmware tree on the GUI thread.
        if (recursive)
        {
            if (DiscoverPackage(rootPath, storage, out var directPackage, out _, false, preferredProgrammerPath))
            {
                package = directPackage;
                return true;
            }
        }

        var programmer = string.Empty;
        var digest = string.Empty;
        var signature = string.Empty;
        var discoveredProgrammers = new List<ProgrammerCandidate>();
        var programmerScore = -1;
        var digestScore = -1;
        var signatureScore = -1;

        var preferredPath = preferredProgrammerPath.Length != 0 ? Path.GetFullPath(preferredProgrammerPath) : string.Empty;
        var searchOption = recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly;
        var files = Directory.GetFiles(rootPath, "*", searchOption)
                             .OrderBy(p => Path.GetFileName(p), StringComparer.Ordinal);
        foreach (var filePath in files)
        {
            var stem = Path.GetFileNameWithoutExtension(filePath).ToLowerInvariant();
            var suffix = Path.GetExtension(filePath).TrimStart('.').ToLowerInvariant();
            var fileDirectory = Path.GetDirectoryName(filePath) ?? "";
            var fileInfo = new FileInfo(filePath);

            if (suffix == "elf" || suffix == "melf" || suffix == "mbn" || suffix == "bin")
            {
                var score = 0;
                if (stem.Contains("devprg")) score += 180;
                if (stem.Contains("programmer")) score += 160;
                if (stem.Contains("prog")) score += 120;
                if (stem.Contains("firehose")) score += 100;
                if (stem.Contains("ddr")) score += 30;
                if (storage == StorageType.Ufs && stem.Contains("ufs")) score += 50;
                if (storage == StorageType.Emmc && stem.Contains("emmc")) score += 50;
                if (suffix == "elf") score += 20;
                if (suffix == "melf") score += 30;
                score += NumberedProgrammerPriority(stem);
                if (preferredProgrammerPath.Length != 0 &&
                    string.Equals(filePath, preferredPath, StringComparison.OrdinalIgnoreCase))
                {
                    score += 1000;
                }
                if (!ContainsAny(stem, "digest", "design", "signature") &&
                    !IsStandaloneSignatureArtifact(stem) && score > 0)
                {
                    discoveredProgrammers.Add(new ProgrammerCandidate
                    {
                        Path = filePath,
                        Directory = fileDirectory,
                        Size = fileInfo.Length,
                        Score = score,
                    });
                    if (BetterPath(score, filePath, programmerScore, programmer))
                    {
                        programmer = filePath;
                        programmerScore = score;
                    }
                }
            }

            var currentDigestScore = -1;
            if (stem.Contains("digest")) currentDigestScore = 220;
            else if (stem.Contains("design")) currentDigestScore = 150;
            if (currentDigestScore >= 0)
            {
                if (stem == "digest") currentDigestScore += 100;
                else if (stem.StartsWith("digest_", StringComparison.Ordinal)) currentDigestScore += 70;
                if (stem.Contains("ufs") && storage == StorageType.Ufs) currentDigestScore += 30;
                if (stem.Contains("emmc") && storage == StorageType.Emmc) currentDigestScore += 30;
                if (BetterPath(currentDigestScore, filePath, digestScore, digest))
                {
                    digest = filePath;
                    digestScore = currentDigestScore;
                }
            }

            var currentSignatureScore = -1;
            if (stem.Contains("signature")) currentSignatureScore = 240;
            else if (stem.Contains("sign") &&
                     !ContainsAny(stem, "digest", "design")) currentSignatureScore = 180;
            else if (stem == "sig" || stem.StartsWith("sig_", StringComparison.Ordinal) ||
                     stem.EndsWith("_sig", StringComparison.Ordinal)) currentSignatureScore = 120;
            if (stem == "sign") currentSignatureScore += 100;
            else if (stem.StartsWith("sign_", StringComparison.Ordinal)) currentSignatureScore += 70;
            if (BetterPath(currentSignatureScore, filePath, signatureScore, signature))
            {
                signature = filePath;
                signatureScore = currentSignatureScore;
            }
        }

        if (programmerScore <= 0 || digest.Length == 0 || signature.Length == 0)
        {
            error = $"目录中需要 Programmer、Digest 和 Sign 三类文件；当前识别：{(programmer.Length == 0 ? "—" : Path.GetFileName(programmer))} / {(digest.Length == 0 ? "—" : Path.GetFileName(digest))} / {(signature.Length == 0 ? "—" : Path.GetFileName(signature))}";
            return false;
        }

        package.Enabled = true;
        package.PackageDirectory = rootPath;
        package.ProgrammerPath = programmer;
        package.ProgrammerCandidates.Clear();
        var preferredDir = Path.GetDirectoryName(programmer) ?? "";
        var preferredSize = new FileInfo(programmer).Length;
        discoveredProgrammers.Sort((left, right) =>
        {
            if (left.Score != right.Score) return right.Score - left.Score;
            return string.Compare(left.Path, right.Path, StringComparison.OrdinalIgnoreCase);
        });
        // Only equal-size files beside the preferred image are considered automatic
        // certificate alternatives. Different-size Programmer3/4 builds may require
        // a different Digest/Sign pair and remain available for explicit selection.
        foreach (var candidate in discoveredProgrammers)
        {
            if (string.Equals(candidate.Directory, preferredDir, StringComparison.OrdinalIgnoreCase) &&
                candidate.Size == preferredSize)
            {
                package.ProgrammerCandidates.Add(candidate.Path);
            }
        }
        if (!package.ProgrammerCandidates.Any(x => string.Equals(x, programmer, StringComparison.OrdinalIgnoreCase)))
        {
            package.ProgrammerCandidates.Insert(0, programmer);
        }
        package.DigestPath = digest;
        package.SignaturePath = signature;
        return true;
    }
}
