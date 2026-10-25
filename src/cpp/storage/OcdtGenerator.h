#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  OcdtGenerator — OCDT 分区镜像生成 / 分析（欧加 TDCO 容器）
//
//  移植自 YukiClient 的 `Core/Violet/OcdtGenerator.cs`（757 行，纯逻辑、零 UI），
//  字节布局与加解密逐字节对齐，并补齐了原文件只有注释没实现的平台差异：
//
//   0x0000  4 字节  "TDCO"          魔数
//   0x0004  2 字节  0x0001          版本
//   0x0006  2 字节  0x0000          保留
//   0x0008  4 字节  0x10            配置偏移
//   0x000C  4 字节  0x34            配置长度（52 字节）
//   0x0010  52 字节 加密配置块
//   0x1000  512 字节 OSIG 签名块（两块平台都有）
//   0x2000  512 字节 OSIG 副本（**只有 128KB 平台写**）
//
//  两个平台的镜像长度不同：联发科 8MB（8*1024*1024）、高通 128KB（128*1024）。
//
//  配置块加密（key = ASCII "geyixue"，7 字节）：
//    加密 c[i] = ROL4(p[i] ^ key[i % 7])
//    解密 p[i] = ROR4(c[i]) ^ key[i % 7]
//  明文里 [0..1] / [4..5] / [8..9] 各放一遍 projId（小端 16 位）。
//
//  🔴 8MB 平台的 OSIG 只有「旧款联发科」（MtkWithOsig 表）才写：表外机型整块留零。
//     传了备份镜像但长度 < 0x1200 时**同样不写**（与参考实现一致，不是笔误）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>
#include <QVector>

namespace swt::edl {

enum class OcdtPlatform {
    Mtk8Mb,        ///< 联发科，8MB 镜像
    Qualcomm128Kb  ///< 高通，128KB 镜像
};

enum class OcdtVariant {
    Standard,       ///< 标准：只有三处 projId
    Fill02,         ///< 0x02 / 0x06 / 0x0A 三个标记字节写 0x02
    GeyixueFill,    ///< 配置块 [0x16..0x33] 用 geyixue 密钥填充
    MixedProjectId  ///< 三处 projId 不一致，配置块取自固定配置库（PGZ110 / RMX3461）
};

struct OcdtModelEntry {
    const char* model;
    int projectId;
};

class OcdtGenerator final {
public:
    static constexpr int ConfigOffset = 0x10;
    static constexpr int ConfigSize = 0x34;
    static constexpr int OsigOffset = 0x1000;
    static constexpr int OsigBackupOffset = 0x2000;
    static constexpr int OsigBlockSize = 0x200;
    static constexpr int OsigBackupMinLength = 0x1200;
    static constexpr int ConfigTailOffset = 22;
    static constexpr qint64 FileSize8Mb = 8LL * 1024 * 1024;
    static constexpr qint64 FileSize128Kb = 128LL * 1024;

    /// 机型代号 → 项目号（大小写敏感，与参考实现一致；命中返回 true）。
    static bool tryGetProjectId(const QString& modelCode, int* projectId);

    /// 机型代号 → 推荐变体（PGCM10→Fill02，PJU110/PJV110→GeyixueFill，
    /// PGZ110/RMX3461→MixedProjectId，其余 Standard）。
    static OcdtVariant recommendedVariant(const QString& modelCode);

    /// 该项目号是否属于「旧款联发科、需要有效 OSIG」。
    static bool needsOsigBackup(int projectId);

    /// 平台 / 变体的字符串口径（与 UI 下拉、文件名共用）。
    static QString platformKey(OcdtPlatform platform);          ///< "8mb" / "128kb"
    static QString variantKey(OcdtVariant variant);             ///< "standard" / ...
    static bool parsePlatform(const QString& text, OcdtPlatform* platform);
    static bool parseVariant(const QString& text, OcdtVariant* variant);

    /// 输出默认名：ocdt_{projId}_{8mb|128kb}.img
    static QString defaultFileName(int projectId, OcdtPlatform platform);

    /// 纯函数：按参数构造整幅镜像（不碰磁盘，便于单测）。
    /// osigBackup 语义：只取 [0x1000, 0x1200) 这 512 字节；长度不足视为没有备份。
    static QByteArray buildImage(OcdtPlatform platform, int projectId, OcdtVariant variant,
                                 const QString& modelCode = {}, const QByteArray& osigBackup = {});

    /// 写文件：父目录不存在时自动创建。失败时 error 给可读原因。
    static bool generate(OcdtPlatform platform, int projectId, OcdtVariant variant,
                         const QString& outputPath, const QString& modelCode = {},
                         const QByteArray& osigBackup = {}, QString* error = nullptr);

    /// 分析已有 OCDT 镜像，返回人话报告（认不出时整份以 "[错误]" 开头）。
    static QString analyze(const QString& imagePath);

    /// 内置机型表（UI 自动补全用）。
    static const QVector<OcdtModelEntry>& modelTable();

    /// geyixue 加/解密（纯函数，单测直接比对）。
    static QByteArray geyixueEncrypt(const QByteArray& plain);
    static QByteArray geyixueDecrypt(const QByteArray& encrypted);
};

} // namespace swt::edl
