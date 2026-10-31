#include "kc/tools/TextTable.h"

#include <QByteArray>
#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

// ─────────────────────────────────────────────────────────────────────────────
//  CSV 与等宽文本表格的实现
//
//  这一层只做字符串拼装 + 一次原子写盘，没有任何 UI / 设备依赖，
//  单测可以整段喂进去逐字符比对。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {
namespace {

/// UTF-8 BOM 的三个字节。写成显式长度，避免 `\xEF\xBB\xBF` 后面万一跟了
/// 十六进制字符被当成同一个转义序列续读（经典 C/C++ 坑）。
const QByteArray& utf8Bom()
{
    static const QByteArray bom("\xEF\xBB\xBF", 3);
    return bom;
}

/// CSV 与 TXT 共用的落盘逻辑：UTF-8 带 BOM + 原子替换。
/// kind 只用于拼中文错误信息（「无法创建 CSV 文件」/「无法创建文本文件」）。
/// createDir 为真时先把父目录建出来（TXT 导出会，CSV 导出不会 —— 见两个
/// 公开函数的注释）。
bool writeUtf8WithBom(const QString& path, const QString& text, const QString& kind,
                      bool createDir, QString* error)
{
    if (error) error->clear();
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("导出路径为空");
        return false;
    }

    if (createDir) {
        const QString parent = QFileInfo(path).absolutePath();
        if (!parent.isEmpty() && !QDir().mkpath(parent)) {
            if (error) {
                *error = QStringLiteral("无法创建导出目录：%1").arg(QDir::toNativeSeparators(parent));
            }
            return false;
        }
    }

    // QSaveFile：先写同目录下的临时文件，commit() 时才原子替换目标。
    // 写到一半磁盘满 / 进程被杀，用户手里的旧文件不会被截成半个。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("无法创建%1文件：%2").arg(kind, file.errorString());
        }
        return false;
    }

    // BOM 必须在最前面，且与正文一起写成 UTF-8：Excel / 记事本靠它判断这是
    // UTF-8，否则在简体中文系统上会按 GBK 解码，中文整片乱码。
    const QByteArray payload = utf8Bom() + text.toUtf8();
    if (file.write(payload) != static_cast<qint64>(payload.size())) {
        file.cancelWriting();
        if (error) {
            *error = QStringLiteral("写入%1数据失败：%2").arg(kind, file.errorString());
        }
        return false;
    }
    if (!file.commit()) {
        if (error) *error = QStringLiteral("保存%1文件失败：%2").arg(kind, file.errorString());
        return false;
    }
    return true;
}

/// 宽字符判定（按 Unicode 区间，够分区表用）。
/// 这些区间的字在等宽字体里占两格；其余（含 ASCII、拉丁、组合字符、emoji）
/// 一律按一格算 —— 分区名里不会出现需要 UAX #11 完整分段才能排齐的东西。
bool isWide(char32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F)    // 谚文字母（Hangul Jamo）
        || (cp >= 0x2E80 && cp <= 0x303E)    // CJK 部首补充 ~ CJK 符号和标点（含 U+3000 全角空格）
        || (cp >= 0x3041 && cp <= 0x33FF)    // 平假名 / 片假名 / 注音 / CJK 兼容
        || (cp >= 0x3400 && cp <= 0x4DBF)    // CJK 统一表意文字扩展 A
        || (cp >= 0x4E00 && cp <= 0x9FFF)    // CJK 统一表意文字（常用汉字）
        || (cp >= 0xA000 && cp <= 0xA4CF)    // 彝文
        || (cp >= 0xAC00 && cp <= 0xD7A3)    // 谚文音节
        || (cp >= 0xF900 && cp <= 0xFAFF)    // CJK 兼容表意文字
        || (cp >= 0xFE30 && cp <= 0xFE6F)    // CJK 兼容形式
        || (cp >= 0xFF00 && cp <= 0xFF60)    // 全角 ASCII（全角字母/数字/标点）
        || (cp >= 0xFFE0 && cp <= 0xFFE6)    // 全角符号（￥ 等）
        || (cp >= 0x20000 && cp <= 0x2FFFD)  // CJK 扩展 B~F（增补平面）
        || (cp >= 0x30000 && cp <= 0x3FFFD); // CJK 扩展 G（增补平面）
}

/// 单行 CSV：先把字段数补齐到 columnCount（不足补空串），再逐字段转义后
/// 用逗号拼起来。超出 columnCount 的字段**不截断** —— 宁可多一列也不能丢数据。
QString csvLine(const QStringList& fields, qsizetype columnCount)
{
    const qsizetype count = fields.size() > columnCount ? fields.size() : columnCount;
    QStringList cells;
    cells.reserve(count);
    for (qsizetype i = 0; i < count; ++i)
        cells.push_back(csvEscape(i < fields.size() ? fields.at(i) : QString()));
    return cells.join(QLatin1Char(','));
}

/// 一行表格的列数上限（表头与所有行的最大列数）。
/// 表头比数据短时也照数据的最宽行来排，不然多出来的列会被挤成一行。
qsizetype widestColumnCount(const QStringList& header, const QVector<QStringList>& rows)
{
    qsizetype count = header.size();
    for (const QStringList& row : rows) {
        if (row.size() > count) count = row.size();
    }
    return count;
}

/// 取某行第 column 列；缺列当空串（表格右侧允许不齐）。
QString cellAt(const QStringList& fields, qsizetype column)
{
    return column < fields.size() ? fields.at(column) : QString();
}

} // namespace

QString csvEscape(const QString& field)
{
    // 只有真会影响解析的字符才触发引号：逗号（分隔符）、双引号（转义符本身）、
    // CR/LF（换行）。其它字符（含中文、制表符）原样输出，导出文件更干净。
    bool needsQuoting = false;
    for (const QChar ch : field) {
        if (ch == QLatin1Char(',') || ch == QLatin1Char('"') || ch == QLatin1Char('\r') ||
            ch == QLatin1Char('\n')) {
            needsQuoting = true;
            break;
        }
    }
    if (!needsQuoting) return field;

    // RFC4180：整体包一层双引号，内部的每个 `"` 再写一遍（`""`）。
    QString quoted;
    quoted.reserve(field.size() + 2);
    quoted += QLatin1Char('"');
    for (const QChar ch : field) {
        if (ch == QLatin1Char('"')) quoted += QLatin1Char('"');
        quoted += ch;
    }
    quoted += QLatin1Char('"');
    return quoted;
}

QString buildCsv(const QStringList& header, const QVector<QStringList>& rows)
{
    // 行分隔符固定 `\r\n`：Excel 与 Windows 记事本都认；只写 `\n` 时老工具会
    // 把整份文件读成一行。末尾不加行尾符，调用方按 `\r\n` 切出来正好是 N 行。
    QStringList lines;
    lines.reserve(rows.size() + 1);
    lines.push_back(csvLine(header, header.size()));
    for (const QStringList& row : rows)
        lines.push_back(csvLine(row, header.size()));
    return lines.join(QStringLiteral("\r\n"));
}

bool writeCsvFile(const QString& path, const QStringList& header,
                  const QVector<QStringList>& rows, QString* error)
{
    // CSV 不替调用方建目录：用户手输路径时，拼错一级目录应当立刻报错，
    // 而不是悄悄在磁盘上造出一串空目录（TXT 导出另有一条会建目录的路径）。
    return writeUtf8WithBom(path, buildCsv(header, rows), QStringLiteral("CSV"), false, error);
}

bool writeTextFile(const QString& path, const QString& text, QString* error)
{
    // 与 CSV 唯一的差别：目录不存在时先建出来。刷机记录导出的路径通常由
    // 「文件名 + 时间戳」拼成，调用方没义务先 QDir::mkpath。
    return writeUtf8WithBom(path, text, QStringLiteral("文本"), true, error);
}

QString buildPlainTable(const QStringList& header, const QVector<QStringList>& rows)
{
    const qsizetype columnCount = widestColumnCount(header, rows);
    // 连表头都没有：没有任何列可排，直接给空串（否则会返回一个只有 CRLF 的怪文件）。
    if (columnCount == 0) return QString();

    // ① 每列宽度 = 表头与所有行里最宽的那个（按显示宽度，中文算 2）。
    QVector<int> widths(columnCount, 0);
    const auto widenWith = [&widths](const QStringList& fields) {
        for (qsizetype i = 0; i < fields.size(); ++i) {
            const int width = displayWidth(fields.at(i));
            if (width > widths.at(i)) widths[i] = width;
        }
    };
    widenWith(header);
    for (const QStringList& row : rows) widenWith(row);

    // ② 逐行补空格。
    //   最后一列不补：一是不留行尾空白，二是**不能碰数据本身** —— 如果最后一
    //   列的文本自己就以空格结尾，那是有意义的字符，不能当填充削掉。所以这里
    //   先算出这一行真正写到第几列（最右的非空列），只补它左边的那些列。
    const auto lastContentColumn = [](const QStringList& fields) {
        for (qsizetype i = fields.size(); i > 0; --i) {
            if (!fields.at(i - 1).isEmpty()) return i - 1;
        }
        return qsizetype(-1);
    };
    const auto formatLine = [&widths, &lastContentColumn](const QStringList& fields) {
        const qsizetype last = lastContentColumn(fields);
        QString line;
        for (qsizetype i = 0; i <= last; ++i) {
            const QString cell = cellAt(fields, i);
            if (i > 0) line += QStringLiteral("  ");   // 列之间固定两个空格
            line += cell;
            if (i < last) {
                // 按显示宽度补空格：QString::size() 是 UTF-16 码元数，中文会被
                // 少算一半，直接用它对不齐。
                const int pad = widths.at(i) - displayWidth(cell);
                if (pad > 0) line += QString(pad, QLatin1Char(' '));
            }
        }
        return line;
    };

    QStringList lines;
    lines.reserve(rows.size() + 2);
    lines.push_back(formatLine(header));

    // ③ 分隔线：每列用 `-` 填满该列宽度，列间同样两个空格。
    //   宽度为 0 的列（表头与所有行都是空串）没有内容可对齐，不算进去。
    qsizetype lastWidthColumn = columnCount - 1;
    while (lastWidthColumn > 0 && widths.at(lastWidthColumn) == 0) --lastWidthColumn;
    QString separator;
    for (qsizetype i = 0; i <= lastWidthColumn; ++i) {
        if (i > 0) separator += QStringLiteral("  ");
        separator += QString(widths.at(i), QLatin1Char('-'));
    }
    lines.push_back(separator);

    for (const QStringList& row : rows) lines.push_back(formatLine(row));
    return lines.join(QStringLiteral("\r\n"));
}

int displayWidth(QChar ch)
{
    return isWide(static_cast<char32_t>(ch.unicode())) ? 2 : 1;
}

int displayWidth(const QString& text)
{
    int width = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        // 增补平面（CJK 扩展 B 及以后）在 UTF-16 里是代理对，占两个 QChar；
        // 必须合成码点再判宽，否则会按两个半角算成 2 —— 巧了结果一样，
        // 但 U+20000 这类字在等宽字体里确实占两格，这里按码点算才说得通。
        if (ch.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            const char32_t cp = QChar::surrogateToUcs4(ch, text.at(i + 1));
            width += isWide(cp) ? 2 : 1;
            ++i;
            continue;
        }
        width += displayWidth(ch);
    }
    return width;
}

} // namespace kc::tools
