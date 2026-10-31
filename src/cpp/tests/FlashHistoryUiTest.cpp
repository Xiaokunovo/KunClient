// 2026-10-29 新增功能的结构回归：
//   ① 刷机记录：完成一次操作后 FlashHistory 里真的多一条，且「连接 / 读 GPT」这类
//      过程动作**不**进历史；
//   ② 分区表右键菜单 / 「高级功能 ▾」里的三个新条目（刷机记录 / 导出 CSV / 导出 TXT）
//      存在且可用性口径正确（不需要连接也能看历史，但没分区表时导出置灰）；
//   ③ EDL 日志头多了「导出」按钮。
//
// 不依赖 show()、不点会弹 QFileDialog 的按钮 —— 沿用 MainWindowLogTest.cpp 的
// 「objectName + property + flags + 反射调槽」范式。

#include "kc/diag/FlashHistory.h"
#include "ui/MainWindow.h"
#include "ui/widgets/LogView.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL " << what << '\n';
    }
}

QPushButton* findButtonByText(const QWidget& root, const QString& text)
{
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

QComboBox* findCombo(const QWidget& root, const QString& objectName)
{
    for (QComboBox* combo : root.findChildren<QComboBox*>()) {
        if (combo->objectName() == objectName) return combo;
    }
    return nullptr;
}

/// 下拉里是否有这条动作（data 非空 = 动作条目，分组标题的 data 是空的）。
bool hasAction(const QComboBox& combo, const QString& text)
{
    for (int i = 0; i < combo.count(); ++i) {
        if (combo.itemText(i) == text && !combo.itemData(i).toString().isEmpty()) return true;
    }
    return false;
}

bool actionEnabled(const QComboBox& combo, const QString& text)
{
    auto* model = qobject_cast<QStandardItemModel*>(combo.model());
    if (!model) return false;
    for (int i = 0; i < combo.count(); ++i) {
        if (combo.itemText(i) != text) continue;
        QStandardItem* item = model->item(i);
        return item && (item->flags() & Qt::ItemIsEnabled);
    }
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    // 记录目录隔离：绝不能让测试往真实用户目录写历史。
    QTemporaryDir historyDir;
    check(historyDir.isValid(), "temp dir for flash history");
    if (!historyDir.isValid()) return 1;
    kc::diag::FlashHistory::setDirForTest(historyDir.path());

    swt::edl::MainWindow window;

    // ── ① 高级功能下拉里的三个新条目 ────────────────────────────────────────
    QComboBox* advanced = findCombo(window, QStringLiteral("advancedCombo"));
    if (!advanced) {
        std::cerr << "advanced combo was not created\n";
        return 2;
    }
    check(hasAction(*advanced, QStringLiteral("刷机记录")),
          "advanced combo has 刷机记录");
    check(hasAction(*advanced, QStringLiteral("导出分区表（CSV）")),
          "advanced combo has 导出分区表（CSV）");
    check(hasAction(*advanced, QStringLiteral("导出分区表（TXT）")),
          "advanced combo has 导出分区表（TXT）");

    // 未连接 + 分区表为空时：刷机记录**仍然可用**（这正是它的价值），
    // 两个导出因为没数据而置灰。这条口径写反了就等于把「设备刷坏了想回看」
    // 这个最需要它的场景堵死了。
    check(actionEnabled(*advanced, QStringLiteral("刷机记录")),
          "刷机记录 stays enabled while disconnected (local-only history)");
    check(!actionEnabled(*advanced, QStringLiteral("导出分区表（CSV）")),
          "CSV export is disabled while the partition table is empty");
    check(!actionEnabled(*advanced, QStringLiteral("导出分区表（TXT）")),
          "TXT export is disabled while the partition table is empty");

    // ── ② 分区表右键菜单：策略必须是 CustomContextMenu ──────────────────────
    auto* table = window.findChild<QTableWidget*>(QStringLiteral("partitionTable"));
    if (!table) {
        std::cerr << "partition table was not created\n";
        return 3;
    }
    check(table->contextMenuPolicy() == Qt::CustomContextMenu,
          "partition table opts into the custom context menu");

    // ── ③ 日志头「导出」按钮 ────────────────────────────────────────────────
    check(findButtonByText(window, QStringLiteral("导出")) != nullptr,
          "EDL log header exposes an export button");
    // 原有按钮不能因为加了新按钮而丢：Debug / 清空 / 导出 三个并排。
    check(findButtonByText(window, QStringLiteral("清空")) != nullptr,
          "log header still has 清空");
    check(window.findChild<QPushButton*>(QStringLiteral("debugButton")) != nullptr,
          "log header still has the Debug toggle");

    // ── ④ 记录链路：连接成功 / 读 GPT 不落历史 ─────────────────────────────
    // 这两类是「过程」，不是用户要回看的「刷机结果」—— 全记下来会把真正重要的
    // 那几条刷写记录淹掉。用反射调 operationFinished，模拟 worker 的完成信号。
    check(QMetaObject::invokeMethod(
              &window, "operationFinished", Qt::DirectConnection, Q_ARG(bool, true),
              Q_ARG(QString, QStringLiteral("设备连接成功"))),
          "operationFinished(设备连接成功) is invokable");
    check(QMetaObject::invokeMethod(
              &window, "operationFinished", Qt::DirectConnection, Q_ARG(bool, true),
              Q_ARG(QString, QStringLiteral("分区表读取完成，共 69 个分区"))),
          "operationFinished(分区表读取完成) is invokable");
    check(kc::diag::FlashHistory::instance().records().isEmpty(),
          "connect / read-GPT completions do not enter the flash history");

    // ── ⑤ 直接验 FlashHistory 的落盘口径（记录窗的数据源）────────────────────
    // UI 侧的 beginFlashOperation 是私有方法，测不到；这里验证「写入一条 → 读回来」
    // 这条数据通路本身，以及中文 / 逗号在 CSV 导出里的转义。
    {
        kc::diag::FlashRecord record;
        record.time = QDateTime::currentDateTime();
        record.operation = QStringLiteral("写入");
        record.device = QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd");
        record.package = QStringLiteral("rawprogram0.xml, rawprogram1.xml");
        record.partitions = QStringLiteral("boot, system, vendor 等 3 个");
        record.success = true;
        record.message = QStringLiteral("批量写入完成：3 个分区");
        record.durationMs = 12345;
        check(kc::diag::FlashHistory::instance().append(record),
              "FlashHistory::append writes a record");

        const QVector<kc::diag::FlashRecord> records =
            kc::diag::FlashHistory::instance().records();
        check(records.size() == 1, "history holds exactly one record");
        if (records.size() == 1) {
            const kc::diag::FlashRecord& back = records.first();
            check(back.operation == record.operation, "operation round-trips");
            check(back.device == record.device, "device round-trips (中文 + 分隔符)");
            check(back.package == record.package, "package round-trips (含逗号)");
            check(back.success && back.durationMs == 12345,
                  "success flag and duration round-trip");
        }

        const QString csv = kc::diag::recordsToCsv(records);
        check(csv.contains(QStringLiteral("\"rawprogram0.xml, rawprogram1.xml\"")),
              "CSV quotes a field that contains a comma");
        check(csv.startsWith(QStringLiteral("时间,操作,设备")),
              "CSV starts with the documented header");

        kc::diag::FlashHistory::instance().clear();
        check(kc::diag::FlashHistory::instance().records().isEmpty(),
              "clear() empties the history");
        check(!QFile::exists(kc::diag::FlashHistory::instance().filePath()),
              "clear() removes the history file");
    }

    if (failures == 0) {
        std::cout << "flash-history-ui-tests: all checks passed\n";
        return 0;
    }
    std::cerr << "flash-history-ui-tests: " << failures << " check(s) failed\n";
    return 1;
}
