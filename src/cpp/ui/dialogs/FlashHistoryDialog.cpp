#include "ui/dialogs/FlashHistoryDialog.h"

#include "kc/diag/FlashHistory.h"
#include "kc/tools/TextTable.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

constexpr int kDialogWidth = 940;
constexpr int kDialogHeight = 560;

void centerOn(QWidget* w, QWidget* parent)
{
    if (!w) return;
    QScreen* screen = nullptr;
    if (parent) {
        screen = QApplication::screenAt(parent->mapToGlobal(parent->rect().center()));
    }
    if (!screen) screen = QApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const QSize sz = w->size();
    w->move(avail.center().x() - sz.width() / 2, avail.center().y() - sz.height() / 2);
}

/// 表格列（界面显示用）。TXT 导出复用同一份定义；CSV 走 kc::diag::recordsToCsv()，
/// 那边多一个「消息」列 —— 界面不显示它（一行放不下），但导出时必须有，排查全靠它。
QStringList historyHeader()
{
    return {QStringLiteral("时间"), QStringLiteral("操作"), QStringLiteral("设备"),
            QStringLiteral("来源包"), QStringLiteral("分区"), QStringLiteral("结果"),
            QStringLiteral("耗时")};
}

QStringList recordRow(const kc::diag::FlashRecord& record)
{
    const QString seconds = QStringLiteral("%1 秒")
        .arg(static_cast<double>(record.durationMs) / 1000.0, 0, 'f', 2);
    return {record.time.isValid()
                ? record.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : QStringLiteral("—"),
            record.operation.isEmpty() ? QStringLiteral("—") : record.operation,
            record.device.isEmpty() ? QStringLiteral("—") : record.device,
            record.package.isEmpty() ? QStringLiteral("—") : record.package,
            record.partitions.isEmpty() ? QStringLiteral("—") : record.partitions,
            record.success ? QStringLiteral("成功") : QStringLiteral("失败"),
            seconds};
}

} // namespace

FlashHistoryDialog::FlashHistoryDialog(QWidget* parent)
    : GlassDialog(parent)
{
    setMinimumSize(kDialogWidth, kDialogHeight);
    setTitle(QStringLiteral("刷机记录"));

    auto* hint = new QLabel(
        QStringLiteral("每次写入 / 擦除 / 回读 / XML 刷机完成后自动记一条，"
                       "只留最近 %1 条。导出给售后或群友时两种格式都行。")
            .arg(kc::diag::FlashHistory::MaxRecords));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 12px;")
                            .arg(ThemeManager::instance().isDark()
                                     ? QStringLiteral("rgba(255,255,255,0.66)")
                                     : QStringLiteral("rgba(18,22,28,0.66)")));
    bodyLayout()->addWidget(hint);

    m_table = new QTableWidget(0, historyHeader().size());
    m_table->setObjectName(QStringLiteral("flashHistoryTable"));
    m_table->setHorizontalHeaderLabels(historyHeader());
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    bodyLayout()->addWidget(m_table, 1);

    m_empty = new QLabel(QStringLiteral("还没有刷机记录 —— 完成一次刷写 / 擦除 / 回读后，这里会出现第一条。"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 12px;")
                               .arg(ThemeManager::instance().isDark()
                                        ? QStringLiteral("rgba(255,255,255,0.52)")
                                        : QStringLiteral("rgba(18,22,28,0.52)")));
    bodyLayout()->addWidget(m_empty, 1);

    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_summary->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 11.5px;")
                                 .arg(ThemeManager::instance().isDark()
                                          ? QStringLiteral("rgba(255,255,255,0.58)")
                                          : QStringLiteral("rgba(18,22,28,0.58)")));
    bodyLayout()->addWidget(m_summary);

    addButton(QStringLiteral("清空"), ButtonKind::Ghost, [this] {
        kc::diag::FlashHistory::instance().clear();
        reload();
    });
    addButton(QStringLiteral("导出 TXT"), ButtonKind::Ghost, [this] {
        const QVector<kc::diag::FlashRecord> records =
            kc::diag::FlashHistory::instance().records();
        if (records.isEmpty()) {
            m_summary->setText(QStringLiteral("没有记录可导出。"));
            return;
        }
        QVector<QStringList> rows;
        rows.reserve(records.size());
        for (const kc::diag::FlashRecord& record : records) rows.push_back(recordRow(record));
        const QString suggested = QStandardPaths::writableLocation(
                                      QStandardPaths::DocumentsLocation)
            + QStringLiteral("/KunClient-刷机记录-%1.txt")
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出刷机记录（TXT）"), suggested,
            QStringLiteral("文本文件 (*.txt);;所有文件 (*.*)"));
        if (path.isEmpty()) return;
        QString error;
        if (!kc::tools::writeTextFile(path,
                                      kc::tools::buildPlainTable(historyHeader(), rows),
                                      &error)) {
            m_summary->setText(QStringLiteral("导出失败：%1").arg(error));
            return;
        }
        m_summary->setText(QStringLiteral("已导出 %1 条到 %2")
                               .arg(records.size()).arg(QFileInfo(path).fileName()));
    });
    addButton(QStringLiteral("导出 CSV"), ButtonKind::Primary, [this] {
        const QVector<kc::diag::FlashRecord> records =
            kc::diag::FlashHistory::instance().records();
        if (records.isEmpty()) {
            m_summary->setText(QStringLiteral("没有记录可导出。"));
            return;
        }
        const QString suggested = QStandardPaths::writableLocation(
                                      QStandardPaths::DocumentsLocation)
            + QStringLiteral("/KunClient-刷机记录-%1.csv")
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出刷机记录（CSV）"), suggested,
            QStringLiteral("CSV 表格 (*.csv);;所有文件 (*.*)"));
        if (path.isEmpty()) return;
        // 🔴 直接复用 kc::diag::recordsToCsv()：CSV 的列口径只有那一份定义
        //    （含「消息」列，排查时最有用），单测也钉在那里。UI 自己再拼一份
        //    就会出现「界面导出 7 列、代码里测的 8 列」这种双口径。
        //    写盘走 writeTextFile（UTF-8 + BOM），Excel 双击不乱码。
        QString error;
        if (!kc::tools::writeTextFile(path, kc::diag::recordsToCsv(records), &error)) {
            m_summary->setText(QStringLiteral("导出失败：%1").arg(error));
            return;
        }
        m_summary->setText(QStringLiteral("已导出 %1 条到 %2（Excel 可直接打开）")
                               .arg(records.size()).arg(QFileInfo(path).fileName()));
    });
    addButton(QStringLiteral("关闭"), ButtonKind::Ghost, [this] { accept(); });

    reload();
    resize(kDialogWidth, kDialogHeight);
    centerOn(this, parentWidget());
}

void FlashHistoryDialog::showEvent(QShowEvent* event)
{
    GlassDialog::showEvent(event);
    // 每次打开都重读：用户可能在外面又刷了一次，或者另开了一个窗口。
    reload();
}

void FlashHistoryDialog::reload()
{
    const QVector<kc::diag::FlashRecord> records = kc::diag::FlashHistory::instance().records();

    m_table->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const kc::diag::FlashRecord& record = records.at(row);
        const QStringList cells = recordRow(record);
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (column == 5) {
                // 结果列着色：与 EDL 分区表状态列同一套色（深色用亮青绿 / 亮黄，
                // 浅色用深一档，保证在浅色玻璃上也有对比度）。
                const bool dark = ThemeManager::instance().isDark();
                item->setForeground(QColor(record.success
                    ? (dark ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))
                    : (dark ? QStringLiteral("#ff7b72") : QStringLiteral("#c0392b"))));
            }
            // 完整原始消息不占列宽：悬停能看到 worker 的原话，排查时有用。
            item->setToolTip(record.message.isEmpty()
                                 ? cells.at(column)
                                 : QStringLiteral("%1\n\n原始消息：%2")
                                       .arg(cells.at(column), record.message));
            m_table->setItem(row, column, item);
        }
    }

    const bool empty = records.isEmpty();
    m_table->setVisible(!empty);
    m_empty->setVisible(empty);

    if (empty) {
        m_summary->setText(QStringLiteral("记录文件：%1")
                               .arg(kc::diag::FlashHistory::instance().filePath()));
    } else {
        int failed = 0;
        for (const kc::diag::FlashRecord& record : records) {
            if (!record.success) ++failed;
        }
        m_summary->setText(QStringLiteral("共 %1 条（成功 %2 · 失败 %3）　记录文件：%4")
                               .arg(records.size())
                               .arg(records.size() - failed)
                               .arg(failed)
                               .arg(kc::diag::FlashHistory::instance().filePath()));
    }
}

} // namespace kc::ui
