using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace KunFlashTool.Core.SwtEdl;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/storage/RawProgramWriter.cpp`：
/// 备份完成后按 LUN 分组生成 <c>rawprogram&lt;lun&gt;.xml</c>，方便直接拿去做 QFIL 整包刷写。
///
/// <para>⚠️ 这里**手写 XML** 而不是用 <c>XmlWriter</c>，是为了跟 Qt 的
/// <c>QXmlStreamWriter(autoFormatting)</c> 输出逐字节对齐：属性顺序、4 空格缩进、
/// 自闭合标签、以及 <c>QIODevice::Text</c> 在 Windows 上把 \n 翻成 \r\n。
/// XmlWriter 会强制带 <c>encoding=</c> 声明、且一旦 <c>WriteRaw</c> 就关掉缩进 —— 两头都不对。</para>
/// </summary>
public static class RawProgramWriter
{
    private const string NewLine = "\r\n";

    public static bool WriteBackup(string outputDirectory,
                                  IReadOnlyList<PartitionInfo> partitions,
                                  IReadOnlyList<string> imageNames,
                                  uint sectorSize,
                                  List<string> generatedPaths,
                                  out string error)
    {
        error = string.Empty;
        generatedPaths.Clear();
        if (partitions.Count == 0 || partitions.Count != imageNames.Count || sectorSize == 0)
        {
            error = "生成 rawprogram XML 的参数异常";
            return false;
        }

        if (!Directory.Exists(outputDirectory))
        {
            error = $"XML 输出目录不存在：{outputDirectory}";
            return false;
        }

        // QMap 是按 key 有序的 → 生成顺序按 LUN 升序（不是 xmlPaths 的输入顺序）。
        var entriesByLun = new SortedDictionary<uint, List<int>>();
        for (var index = 0; index < partitions.Count; ++index)
        {
            var lun = partitions[index].Lun;
            if (!entriesByLun.TryGetValue(lun, out var list))
            {
                list = new List<int>();
                entriesByLun[lun] = list;
            }
            list.Add(index);
        }

        foreach (var kv in entriesByLun)
        {
            var path = Path.Combine(outputDirectory, $"rawprogram{kv.Key}.xml");
            var text = new StringBuilder();
            text.Append("<?xml version=\"1.0\"?>").Append(NewLine);
            text.Append("<data>").Append(NewLine);

            foreach (var index in kv.Value)
            {
                var partition = partitions[index];
                var byteCount = partition.SectorCount * sectorSize;
                text.Append("    <program");
                AppendAttribute(text, "SECTOR_SIZE_IN_BYTES", sectorSize.ToString(CultureInfo.InvariantCulture));
                AppendAttribute(text, "file_sector_offset", "0");
                AppendAttribute(text, "filename", imageNames[index]);
                AppendAttribute(text, "label", partition.Name);
                AppendAttribute(text, "num_partition_sectors", partition.SectorCount.ToString(CultureInfo.InvariantCulture));
                AppendAttribute(text, "physical_partition_number", partition.Lun.ToString(CultureInfo.InvariantCulture));
                AppendAttribute(text, "size_in_KB", (byteCount / 1024UL).ToString(CultureInfo.InvariantCulture));
                AppendAttribute(text, "sparse", "false");
                AppendAttribute(text, "start_byte_hex", "0x" + (partition.FirstLba * sectorSize).ToString("x", CultureInfo.InvariantCulture));
                AppendAttribute(text, "start_sector", partition.FirstLba.ToString(CultureInfo.InvariantCulture));
                text.Append("/>").Append(NewLine);
            }

            text.Append("</data>").Append(NewLine);

            try
            {
                // QSaveFile = 先写临时文件再原子替换；这里用同目录临时文件 + File.Replace 复刻，
                // 避免中途失败留下半截 XML（刷机包被截断比整个没生成更危险）。
                var temp = path + ".new";
                File.WriteAllText(temp, text.ToString(), new UTF8Encoding(false));
                if (File.Exists(path)) File.Delete(path);
                File.Move(temp, path);
            }
            catch (Exception ex)
            {
                error = $"保存 XML 失败：{ex.Message}";
                return false;
            }

            generatedPaths.Add(path);
        }
        return true;
    }

    private static void AppendAttribute(StringBuilder sb, string name, string value) =>
        sb.Append(' ').Append(name).Append("=\"").Append(EscapeXml(value)).Append('"');

    private static string EscapeXml(string value)
    {
        if (value.IndexOfAny(new[] { '&', '<', '>', '"', '\'' }) < 0) return value;
        var sb = new StringBuilder(value.Length + 8);
        foreach (var ch in value)
        {
            sb.Append(ch switch
            {
                '&' => "&amp;",
                '<' => "&lt;",
                '>' => "&gt;",
                '"' => "&quot;",
                '\'' => "&apos;",
                _ => ch.ToString(),
            });
        }
        return sb.ToString();
    }
}
