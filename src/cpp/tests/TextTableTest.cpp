#include "kc/tools/TextTable.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

/// 逐行比较：把表格按 `\r\n` 切开（末尾没有行尾符，切出来就是 N 行）。
QStringList tableLines(const QString& table)
{
    return table.split(QStringLiteral("\r\n"));
}

} // namespace

// 分区表导出（CSV / 等宽 TXT）的纯逻辑回归。
// 这里钉死的都是「错了用户一眼能看出来但代码很难自证」的点：
// Excel 中文乱码（BOM）、RFC4180 转义、以及中文列对齐（显示宽度）。
int main()
{
    using kc::tools::buildCsv;
    using kc::tools::buildPlainTable;
    using kc::tools::csvEscape;
    using kc::tools::displayWidth;
    using kc::tools::writeCsvFile;
    using kc::tools::writeTextFile;

    // ── ① csvEscape：只对真会影响解析的字符加引号 ─────────────────────────
    check(csvEscape(QStringLiteral("boot")) == QStringLiteral("boot"),
          "plain field is returned as-is without quotes");
    check(csvEscape(QStringLiteral("a,b")) == QStringLiteral("\"a,b\""),
          "field with a comma is quoted");
    check(csvEscape(QStringLiteral("a\"b")) == QStringLiteral("\"a\"\"b\""),
          "inner quotes are doubled and the field is quoted");
    check(csvEscape(QStringLiteral("a\nb")) == QStringLiteral("\"a\nb\""),
          "field with a newline is quoted and the newline preserved");
    check(csvEscape(QStringLiteral("a\r\nb")) == QStringLiteral("\"a\r\nb\""),
          "CRLF inside a field is quoted and preserved");
    check(csvEscape(QString()).isEmpty(), "empty field stays empty");
    // 中文本身不是特殊字符，不该被加引号（导出的表在编辑器里更好看）。
    check(csvEscape(QStringLiteral("超级分区")) == QStringLiteral("超级分区"),
          "Chinese text needs no quoting");
    // 只含分号/制表符的字段不是 CSV 分隔符，同样不加引号。
    check(csvEscape(QStringLiteral("a;b\tc")) == QStringLiteral("a;b\tc"),
          "semicolon and tab are not CSV specials");

    // ── ② buildCsv：表头 + 两行，CRLF 分隔，补列 / 不截断 ─────────────────
    {
        const QStringList header{ QStringLiteral("序号"), QStringLiteral("分区名"),
                                  QStringLiteral("大小") };
        const QVector<QStringList> rows{
            { QStringLiteral("1"), QStringLiteral("boot"), QStringLiteral("64 MB") },
            { QStringLiteral("2"), QStringLiteral("超级分区,主"), QStringLiteral("8 GB") },
        };
        const QString csv = buildCsv(header, rows);
        const QStringList lines = tableLines(csv);
        check(lines.size() == 3, "buildCsv writes header plus two rows");
        check(lines.value(0) == QStringLiteral("序号,分区名,大小"),
              "header line is comma-joined without quotes");
        check(lines.value(1) == QStringLiteral("1,boot,64 MB"), "first data row");
        check(lines.value(2) == QStringLiteral("2,\"超级分区,主\",8 GB"),
              "comma inside a Chinese name is escaped");
        check(!csv.endsWith(QStringLiteral("\r\n")), "no trailing CRLF after the last row");

        // 字段数不足：补空列（否则 Excel 会把后面的列整体左移）
        const QString padded =
            buildCsv(QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") },
                     QVector<QStringList>{ QStringList{ QStringLiteral("1") } });
        check(tableLines(padded).value(1) == QStringLiteral("1,,"),
              "short row is padded with empty columns");

        // 字段数超出表头：多出来的列照样输出，绝不丢数据
        const QString extra = buildCsv(
            QStringList{ QStringLiteral("a"), QStringLiteral("b") },
            QVector<QStringList>{ QStringList{ QStringLiteral("1"), QStringLiteral("2"),
                                               QStringLiteral("3") } });
        check(tableLines(extra).value(1) == QStringLiteral("1,2,3"),
              "row wider than the header keeps its extra columns");
    }

    // ── ③ writeCsvFile：UTF-8 BOM + 正文与 buildCsv 完全一致 ───────────────
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temporary directory created");
        const QString path = QDir(dir.path()).filePath(QStringLiteral("partition.csv"));

        const QStringList header{ QStringLiteral("分区名"), QStringLiteral("大小") };
        const QVector<QStringList> rows{
            { QStringLiteral("boot"), QStringLiteral("64 MB") },
            { QStringLiteral("超级分区"), QStringLiteral("8 GB") },
        };

        QString error = QStringLiteral("stale");
        const bool ok = writeCsvFile(path, header, rows, &error);
        check(ok, "writeCsvFile reports success");
        check(error.isEmpty(), "writeCsvFile clears the error on success");

        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "written CSV can be reopened");
        const QByteArray bytes = file.readAll();
        file.close();

        // 前三个字节必须是 EF BB BF —— Excel 靠它认 UTF-8，缺了中文必乱码。
        check(bytes.size() > 3, "written CSV is longer than just a BOM");
        check(bytes.startsWith(QByteArray("\xEF\xBB\xBF", 3)), "file starts with the UTF-8 BOM");
        check(bytes.mid(3) == buildCsv(header, rows).toUtf8(),
              "bytes after the BOM equal buildCsv() encoded as UTF-8");

        // 写失败路径：父目录不存在 → 返回 false，且 error 非空（不能静默成功）
        QString failError;
        const bool failed = writeCsvFile(
            QDir(dir.path()).filePath(QStringLiteral("no-such-dir/partition.csv")),
            header, rows, &failError);
        check(!failed, "writeCsvFile fails when the directory does not exist");
        check(!failError.isEmpty(), "failure path fills a non-empty error message");
    }

    // ── ③b writeTextFile：同样带 BOM，且父目录不存在时自动创建 ──────────────
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temporary directory created (text export)");

        const QStringList header{ QStringLiteral("分区名"), QStringLiteral("大小") };
        const QVector<QStringList> rows{
            { QStringLiteral("boot"), QStringLiteral("64 MB") },
            { QStringLiteral("超级分区"), QStringLiteral("8 GB") },
        };
        const QString table = buildPlainTable(header, rows);

        // 刻意指向一个还不存在的子目录：writeTextFile 负责 mkpath
        const QString path = QDir(dir.path()).filePath(
            QStringLiteral("records/2026/partition.txt"));
        QString error = QStringLiteral("stale");
        const bool ok = writeTextFile(path, table, &error);
        check(ok, "writeTextFile creates missing directories and succeeds");
        check(error.isEmpty(), "writeTextFile clears the error on success");

        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "written TXT can be reopened");
        const QByteArray bytes = file.readAll();
        file.close();
        check(bytes.startsWith(QByteArray("\xEF\xBB\xBF", 3)), "TXT starts with the UTF-8 BOM");
        check(bytes.mid(3) == table.toUtf8(), "TXT body equals the passed text as UTF-8");

        // 空文本也是合法输入：只有 BOM，不能当失败
        const QString emptyPath = QDir(dir.path()).filePath(QStringLiteral("empty.txt"));
        QString emptyError;
        const bool emptyOk = writeTextFile(emptyPath, QString(), &emptyError);
        check(emptyOk && emptyError.isEmpty(), "empty text is written without error");
        QFile emptyFile(emptyPath);
        if (emptyFile.open(QIODevice::ReadOnly)) {
            check(emptyFile.readAll() == QByteArray("\xEF\xBB\xBF", 3),
                  "empty text still produces a BOM-only file");
            emptyFile.close();
        }
    }

    // ── ④ displayWidth：中文按 2 格 ───────────────────────────────────────
    check(displayWidth(QStringLiteral("abc")) == 3, "ASCII string width is its length");
    check(displayWidth(QStringLiteral("分区名")) == 6, "three CJK characters are six cells wide");
    check(displayWidth(QStringLiteral("a分b")) == 4, "mixed ASCII and CJK adds up");
    check(displayWidth(QString()) == 0, "empty string has zero width");
    check(displayWidth(QChar(u'a')) == 1, "QChar overload: ASCII is 1");
    check(displayWidth(QChar(u'分')) == 2, "QChar overload: CJK is 2");
    check(displayWidth(QStringLiteral("，")) == 2, "full-width comma is 2");
    check(displayWidth(QStringLiteral("ABC")) == 3, "uppercase ASCII stays 1");

    // ── ⑤ buildPlainTable：等宽对齐 / 分隔线 / 无尾随空格 / 空表 ───────────
    {
        // 最后一列的值显示宽度一致（都 4 格），各行才会真正等宽 —— 最后一列
        // 按规范不补尾随空格，所以它本身不齐时整行宽度必然不同，这是设计使然。
        const QStringList header{ QStringLiteral("分区名"), QStringLiteral("大小") };
        const QVector<QStringList> rows{
            { QStringLiteral("boot"), QStringLiteral("64MB") },
            { QStringLiteral("超级分区"), QStringLiteral("12MB") },
        };
        const QString table = buildPlainTable(header, rows);
        const QStringList lines = tableLines(table);
        check(lines.size() == 4, "plain table is header + separator + two rows");
        if (lines.size() == 4) {
            // 每行的显示宽度必须一致，否则等宽字体下后面的列就是歪的。
            const int expected = displayWidth(lines.at(0));
            for (int i = 1; i < lines.size(); ++i) {
                if (displayWidth(lines.at(i)) != expected) {
                    std::cerr << "  line " << i << " width " << displayWidth(lines.at(i))
                              << " != header width " << expected << '\n';
                }
            }
            check(displayWidth(lines.at(1)) == expected, "separator width matches the header");
            check(displayWidth(lines.at(2)) == expected, "first data row is padded to the header width");
            check(displayWidth(lines.at(3)) == expected, "second data row is padded to the header width");

            // 分隔线只由 `-` 和列间空格组成
            bool separatorOk = !lines.at(1).isEmpty();
            for (const QChar ch : lines.at(1)) {
                if (ch != QLatin1Char('-') && ch != QLatin1Char(' ')) separatorOk = false;
            }
            check(separatorOk, "separator line consists of '-' and column gaps");
            check(lines.at(1).contains(QStringLiteral("--")), "separator has a run of dashes");

            // 最后一列不补空格：每行都不能以空格结尾
            bool noTrailing = true;
            for (const QString& line : lines) {
                if (line.endsWith(QLatin1Char(' '))) noTrailing = false;
            }
            check(noTrailing, "no line has trailing spaces");

            // 最后一列的内容原样落在行尾（说明确实没被补空格）
            check(lines.at(2).endsWith(QStringLiteral("64MB")), "last column is not padded");
            // 第一列按「显示宽度」补齐：超级分区(8) 比 boot(4) 多 4 格空格
            check(lines.at(2).contains(QStringLiteral("boot    ")), "CJK column pads by display width");
        }

        // 数据比表头宽时，列宽取两者的较大值（表头本身在最后一列，不补空格）
        const QString wide = buildPlainTable(
            QStringList{ QStringLiteral("名") },
            QVector<QStringList>{ QStringList{ QStringLiteral("超级分区") } });
        const QStringList wideLines = tableLines(wide);
        check(wideLines.size() == 3, "single-column table has three lines");
        check(wideLines.value(0) == QStringLiteral("名"), "single-column header has no trailing padding");
        check(wideLines.value(1) == QStringLiteral("--------"), "separator fills the widest cell");
        check(wideLines.value(2) == QStringLiteral("超级分区"), "data cell keeps its text");

        // rows 为空：只有表头 + 分隔线
        const QString emptyRows = buildPlainTable(header, {});
        const QStringList emptyLines = tableLines(emptyRows);
        check(emptyLines.size() == 2, "empty rows still write header plus separator");
        check(displayWidth(emptyLines.value(0)) == displayWidth(emptyLines.value(1)),
              "empty-table separator matches the header width");
        check(!emptyRows.contains(QStringLiteral("boot")), "empty table has no data lines");

        // 数据行比表头短：缺的列当空串。因为「最后一列不补空格」，这种行本来
        // 就比表头短（补出来的空格会被削掉），关键是它不能带尾随空白。
        const QString shortRow = buildPlainTable(
            header, QVector<QStringList>{ QStringList{ QStringLiteral("boot") } });
        const QStringList shortLines = tableLines(shortRow);
        check(shortLines.size() == 3, "short row table has three lines");
        check(shortLines.value(2) == QStringLiteral("boot"),
              "missing trailing columns collapse to no trailing spaces");
        check(!shortLines.value(2).endsWith(QLatin1Char(' ')),
              "short row has no trailing spaces");
        check(displayWidth(shortLines.value(0)) == displayWidth(shortLines.value(1)),
              "header and separator still agree on width when rows are short");

        // 连表头都没有：不该产出只有换行符的怪文件
        check(buildPlainTable({}, {}).isEmpty(), "empty header and rows produce an empty string");
        check(buildCsv({}, {}).isEmpty(), "empty header and rows produce an empty CSV");
    }

    if (failures == 0) {
        std::cout << "texttable-tests: all checks passed\n";
        return 0;
    }
    std::cerr << "texttable-tests: " << failures << " check(s) failed\n";
    return 1;
}
