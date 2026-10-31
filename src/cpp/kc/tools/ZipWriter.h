#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// ─────────────────────────────────────────────────────────────────────────────
//  极简 ZIP 打包器（method 0 = stored，不压缩）
//
//  为什么自己写：诊断包只有几 MB 文本（最近几天的会话日志 + system-info.txt），
//  压缩收益极小，而引入 zlib / QuaZip 会拖累单文件产物的体积 —— 那是本项目的
//  硬指标。所以这里只实现「存储」这一种最简 ZIP：结构严格照 PKWARE APPNOTE，
//  不写 ZIP64（条目数 > 65535 或总量 > 4 GiB 直接报错，绝不产出半截坏包）。
//
//  几个刻意的取舍：
//  - **固定 DOS 时间戳**（1980-01-01 00:00，time=0、date=0x0021）：zip 里不带
//    「打包时刻」，同样输入必然产出同样字节，单测才能做确定性断言，也方便复现
//    用户发来的包到底是怎么打出来的。
//  - **文件名一律 UTF-8**：本地头与中央目录的通用标志位都置 bit 11（0x0800），
//    中文名在 Windows 资源管理器、7-Zip、macOS 归档工具里都不会变乱码。
//  - **QSaveFile 原子落盘**：写一半断电/磁盘满时不会留下半成品 zip 让用户误发。
//
//  只依赖 QtCore（QString / QByteArray / QSaveFile），纯逻辑、可单测；
//  不做目录条目（诊断包里都是文件，解压时目录会自动建出来）。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {

/// 极简 ZIP 打包器（method 0 = stored，不压缩）。
/// 为什么自己写：诊断包只有几 MB 文本，压缩收益极小，而引入 zlib/QuaZip 会拖累单文件产物。
class ZipWriter
{
public:
    ZipWriter() = default;

    /// 追加一个文件条目。name 用 `/` 分隔的相对路径（如 "logs/session-20261029.log"）；
    /// 反斜杠会被统一换成 `/`；重名后加的覆盖前面的。data 为空也允许（0 字节文件合法）。
    void addFile(const QString& name, const QByteArray& data);

    /// 写出 zip。失败返回 false 并填 *error。用 QSaveFile 保证原子（写一半断电不留半成品）。
    bool writeTo(const QString& path, QString* error = nullptr) const;

    int fileCount() const;
    /// 打包后的总字节数（含本地头 + 中央目录 + EOCD），单测用来断言结构。
    qint64 totalBytes() const;

    /// CRC-32（IEEE 802.3 多项式 0xEDB88320，初值/异或 0xFFFFFFFF）——ZIP 规范要求的那个。
    static quint32 crc32(const QByteArray& data);

private:
    struct Entry
    {
        QByteArray name;  ///< 已规范化的 UTF-8 文件名（反斜杠已换成 `/`）
        QByteArray data;  ///< 原始字节（method 0 原样存储）
        quint32 crc = 0;  ///< 加入时就算好，写盘时直接填进本地头与中央目录
    };

    QVector<Entry> m_entries;
};

} // namespace kc::tools
