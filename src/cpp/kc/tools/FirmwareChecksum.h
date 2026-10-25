#pragma once

#include "kc/security/Digest.h"

#include <QByteArray>
#include <QString>
#include <QVector>

// ─────────────────────────────────────────────────────────────────────────────
//  固件校验文件（.md5 / .sha256）的读写口径
//
//  格式对齐 coreutils 的 `md5sum` / `sha256sum`：
//      <32/64 位小写十六进制><两个空格><文件名>
//  文本模式下第二个字符是空格，二进制模式（Windows 上 `sha256sum -b` 或
//  某些打包工具）是 `*`。两种都要认 —— 用户手里的校验文件不一定是谁生成的。
//
//  这一层只做**纯文本解析/格式化**（不碰磁盘），文件 IO 与进度留给 UI，
//  这样单测可以直接喂字符串。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {

struct ChecksumRecord
{
    QString hash;   ///< 小写十六进制（解析时统一转小写）
    QString name;   ///< 文件名（相对校验文件所在目录；可能含子路径）
};

/// 生成一行校验记录：`<hash>  <name>`（两个空格，与 coreutils 一致）。
/// hash 会被转成小写；name 里的换行会被剔除（否则一行变两行、文件就废了）。
QString formatChecksumLine(const QString& hash, const QString& name);

/// 解析校验文件内容。跳过空行、`#` 注释行与格式不合法的行（不报错，
/// 由调用方比对「解析出几条 / 文件里几条」来决定要不要提示）。
QVector<ChecksumRecord> parseChecksumText(const QByteArray& text);

/// 校验文件默认命名：单个文件 → `<文件名>.sha256`；目录批量 → `SHA256SUMS`。
QString checksumFileName(const QString& targetName, kc::crypto::HashAlgo algo);

/// 把用户输入的十六进制串规范化（去空白、转小写）；长度与算法不符时返回空串。
QString normalizeHash(const QString& raw, kc::crypto::HashAlgo algo);

} // namespace kc::tools
