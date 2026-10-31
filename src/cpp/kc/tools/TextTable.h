#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// ─────────────────────────────────────────────────────────────────────────────
//  分区表导出：CSV / 等宽纯文本
//
//  EDL 页读出 GPT 之后要能把分区表导出成文件。两种格式各有一个「不处理就一定
//  出错」的坑，都在这一层一次性解决；UI 只负责拿字符串或者调用写盘函数。
//
//  1) CSV —— 目标是用 Excel 双击直接打开
//     · 编码必须写成 **UTF-8 带 BOM**。Excel 见到没有 BOM 的 UTF-8 会按系统
//       ANSI 代码页（简体中文机器上是 GBK）解码，中文分区名整片乱码；BOM 是
//       让 Excel 认 UTF-8 的唯一可靠信号。
//     · 行分隔符用 `\r\n`：Excel 与 Windows 记事本都认。只写 `\n` 时部分老
//       工具会把整个文件当成一行。
//     · 字段按 RFC4180 转义：含 `,` `"` `\r` `\n` 的字段整体加双引号、内部
//       的 `"` 翻倍。不含特殊字符的字段**不加引号** —— 导出的表在编辑器里
//       更好看，也更好 diff。
//
//  2) TXT —— 目标是等宽字体下看得齐
//     · 中文在等宽字体里占两个字符格。按 1 算的话带中文的行整体左移，后面
//       所有列永远对不上；所以按「显示宽度」补空格：CJK / 全角算 2，其余算 1。
//     · 组合字符（é 拆成 e + 声调符那种）与 emoji 一律按 1 算 —— 分区名里
//       基本不会出现，为它们做完整的 UAX #11 分段不值得。
//
//  这一层是纯逻辑 + 一个写盘函数，只依赖 QtCore；字符串拼装部分可以脱离磁盘
//  直接单测（见 tests/TextTableTest.cpp）。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {

/// 单个字段的 CSV 转义：含 `,` `"` `\r` `\n` 时整体加双引号，内部 `"` 翻倍。
/// 不含特殊字符时原样返回（不加引号 —— 这样导出的表在编辑器里更好看）。
QString csvEscape(const QString& field);

/// 拼 CSV 文本（含表头行）。行分隔符用 `\r\n`（Excel 与记事本都认）。
/// 每行字段数少于表头时补空列，多于时多出来的列照样输出（不丢数据）。
/// 行与行之间只放分隔符，**末尾不加行尾符**：调用方 split 之后正好是 N 行。
QString buildCsv(const QStringList& header, const QVector<QStringList>& rows);

/// 写 CSV 文件：UTF-8 **带 BOM** + buildCsv 的内容。
/// 为什么带 BOM：Excel 打开无 BOM 的 UTF-8 CSV 会把中文显示成乱码。
/// 落盘走 QSaveFile（先写临时文件再原子替换），失败时返回 false 并填中文错误。
bool writeCsvFile(const QString& path, const QStringList& header,
                  const QVector<QStringList>& rows, QString* error = nullptr);

/// 写纯文本文件：UTF-8 带 BOM（与 CSV 同口径，记事本/Excel 都不会乱码）+ 内容原样。
/// 目录不存在时自动创建；用 QSaveFile 原子写，失败填中文 error。
/// 典型用法：writeTextFile(path, buildPlainTable(header, rows), &error)。
bool writeTextFile(const QString& path, const QString& text, QString* error = nullptr);

/// 等宽对齐的纯文本表格：列之间两个空格，每列按「显示宽度」右补空格对齐
/// （CJK / 全角字符算 2，其余算 1；组合字符与 emoji 按 1 算即可，不追求完美）。
/// 第二行是分隔线（用 `-` 填满每列宽度）。rows 为空时只有表头 + 分隔线。
/// 最后一列不补空格（行尾留白没有意义）；行分隔符同样用 `\r\n`。
QString buildPlainTable(const QStringList& header, const QVector<QStringList>& rows);

/// 单个字符的显示宽度（单测直接调）：ASCII / 半角 = 1，CJK 与全角 = 2。
/// 注意单个 QChar 只能看到基本平面（U+FFFF 以内），增补平面的字由整串重载处理。
int displayWidth(QChar ch);

/// 整串的显示宽度。
int displayWidth(const QString& text);

} // namespace kc::tools
