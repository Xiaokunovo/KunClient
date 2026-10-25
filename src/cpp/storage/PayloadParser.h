#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PayloadParser — Android OTA `payload.bin`（CrAU）解析与分区提取
//
//  🔴 与 ui/pages/PayloadPage 里那套「PAD 私有容器」解析是**两回事**：
//     真 OTA 的 payload.bin 是 `CrAU` + protobuf `DeltaArchiveManifest`，
//     旧实现只在前 1MB 里扫分区名、拿不到 offset/length，提取出来是空文件。
//
//  文件布局：
//      [24 字节头][manifest(protobuf)][metadata 签名][操作数据区]
//    头：magic "CrAU"(4) / version u64 **大端** = 2 / manifest_size u64 BE /
//        metadata_signature_size u32 BE；数据区起点 = 24 + manifest_size + sig_size。
//
//  manifest（只解我们需要的字段，最小 protobuf 解析，不引入 protobuf 库）：
//    DeltaArchiveManifest.partitions        = 13
//    PartitionUpdate.partition_name         = 1
//    PartitionUpdate.new_partition_info     = 7  (PartitionInfo.size = 1)
//    PartitionUpdate.operations             = 8  (InstallOperation)
//    InstallOperation.type                  = 1  (varint)
//    InstallOperation.data_offset           = 2  (varint)
//    InstallOperation.data_length           = 3  (varint)
//    InstallOperation.dst_extents           = 6  (Extent)
//    Extent.start_block                     = 1 / Extent.num_blocks = 2
//
//  支持的 InstallOperation 类型：
//    REPLACE(0) / REPLACE_BZ(1) / REPLACE_XZ(8) / REPLACE_ZSTD(14) /
//    ZERO(6) / DISCARD(7)
//  增量类（SOURCE_COPY/SOURCE_BSDIFF/BSDIFF/BROTLI_BSDIFF/PUFFDIFF/ZUCCHINI/
//  LZ4DIFF_*）需要旧镜像，直接报「不支持增量包」，不做半吊子处理。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace swt::edl {

struct PayloadPartitionInfo {
    QString name;
    quint64 size = 0;         ///< new_partition_info.size；缺失时按 extent 推算
    quint64 dataBytes = 0;    ///< 该分区所有 op 的 data_length 之和
    int operationCount = 0;
};

struct PayloadInfo {
    quint64 fileSize = 0;
    quint64 manifestSize = 0;
    quint64 dataOffset = 0;   ///< 操作数据区起点
    QVector<PayloadPartitionInfo> partitions;
};

class PayloadParser final {
public:
    /// 操作块大小（update_engine 固定 4096）。
    static constexpr quint64 BlockSize = 4096;

    /// 只看文件头 4 字节是否 "CrAU"（不读整包）。
    static bool isCrAU(const QString& path);

    /// 解析 manifest，列出分区（不提取）。
    static bool inspect(const QString& path, PayloadInfo* info, QString* error);

    /// 提取分区到 outputDirectory/<分区名>.img。
    /// partitions 为空 = 全部；extracted 返回实际写出的文件名（相对路径）。
    static bool extract(const QString& path, const QString& outputDirectory,
                        const QStringList& partitions, QStringList* extracted, QString* error,
                        const std::function<void(int percent, const QString& message)>& progress = {},
                        const std::function<bool()>& cancel = {});

    /// 类型名（日志用）。
    static QString operationTypeName(int type);
};

} // namespace swt::edl
