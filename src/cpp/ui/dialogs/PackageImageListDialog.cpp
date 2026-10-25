#include "ui/dialogs/PackageImageListDialog.h"

#include "storage/RawProgramParser.h"
#include "ui/dialogs/LoaderPickerDialog.h"
#include "ui/theme/ThemeManager.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QVBoxLayout>

#include <functional>
#include <map>

namespace kc::ui {
namespace {

class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

} // namespace

PackageImageListDialog::PackageImageListDialog(QWidget* parent)
    : AnimatedDialog(parent)
{
    setWindowTitle(QStringLiteral("包内镜像清单"));
    setObjectName(QStringLiteral("packageImageListDialog"));
    setModal(true);
    resize(860, 600);
    buildUi();
}

void PackageImageListDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(8);
    m_onlyFlash = new QCheckBox(QStringLiteral("只看会刷写的条目"), this);
    toolRow->addWidget(m_onlyFlash);
    toolRow->addStretch();
    layout->addLayout(toolRow);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("分区名"), QStringLiteral("第几块"), QStringLiteral("LUN"),
        QStringLiteral("起始扇区"), QStringLiteral("扇区数"), QStringLiteral("镜像"),
        QStringLiteral("状态") });
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    layout->addWidget(m_table, 1);

    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("dialogHintLabel"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    layout->addWidget(m_empty, 1);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("dialogHintLabel"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    buttonRow->addStretch();
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    buttonRow->addWidget(close);
    layout->addLayout(buttonRow);

    connect(m_onlyFlash, &QCheckBox::toggled, this, &PackageImageListDialog::applyFilter);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

void PackageImageListDialog::setEmpty(const QString& text)
{
    m_all.clear();
    m_extraSummary.clear();
    m_table->clearContents();
    m_table->setRowCount(0);
    m_empty->setText(text);
    m_empty->setVisible(true);
    m_table->setVisible(false);
    m_summary->setText(text);
}

void PackageImageListDialog::showList(QWidget* parent, const QString& imageDir,
                                      const QStringList& excludedLabels)
{
    PackageImageListDialog dialog(parent);
    dialog.loadPackage(imageDir, excludedLabels);
    dialog.exec();
}

void PackageImageListDialog::loadPackage(const QString& imageDir,
                                         const QStringList& excludedLabels)
{
    // C# LoadPackage 主体。excluded 为「保数据刷写」排除的分区（大小写不敏感）。
    QSet<QString> excluded;
    for (const QString& label : excludedLabels)
        if (!label.isEmpty()) excluded.insert(label.toLower());

    if (imageDir.trimmed().isEmpty() || !QDir(imageDir).exists()) {
        setEmpty(QStringLiteral("资料目录不存在，请先在主界面选择正确的固件 / 镜像目录。"));
        return;
    }

    const QDir dir(imageDir);
    QStringList xmlFiles;
    for (const QFileInfo& fi :
         dir.entryInfoList({ QStringLiteral("rawprogram*.xml") }, QDir::Files))
        xmlFiles.append(fi.absoluteFilePath());
    std::sort(xmlFiles.begin(), xmlFiles.end());
    if (xmlFiles.isEmpty()) {
        setEmpty(QStringLiteral("资料目录内没有 rawprogram XML。"
                                "这种包只能按镜像文件名匹配分区刷写，没有条目清单可列。"));
        return;
    }

    // 解析在 worker 线程跑（XML 可能很大），完成后回 UI 填表。
    const QDir imageDirAbs = QDir(dir.absolutePath());
    QPointer<PackageImageListDialog> guard(this);
    const QStringList xmls = xmlFiles;
    const QSet<QString> excludedCopy = excluded;
    m_summary->setText(QStringLiteral("正在解析 rawprogram XML…"));

    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, xmls,
                                                         imageDirAbs, excludedCopy] {
        swt::edl::RawProgramPlan plan;
        QString parseError;
        QVector<swt::edl::RawProgramEntry> entries;
        QStringList badXml;

        for (const QString& xml : xmls) {
            swt::edl::RawProgramPlan one;
            QString err;
            if (swt::edl::RawProgramParser::parse(QStringList{ xml }, &one, &err)) {
                entries += one.entries;
            } else {
                badXml.append(QStringLiteral("%1（%2）")
                                  .arg(QFileInfo(xml).fileName(),
                                       err.isEmpty() ? QStringLiteral("解析失败") : err));
            }
        }
        Q_UNUSED(plan);
        Q_UNUSED(parseError);

        // ── 多块镜像块号：同一 (label, LUN) 出现多次即为一组，按包内顺序编号 #i/n。
        //    键必须落到每个条目自己的下标上（C# 注释：用「分组」当键会把同一组的
        //    六行反复覆盖成最后一个块号）。
        std::map<std::pair<QString, quint32>, QVector<int>> groups;
        for (int i = 0; i < entries.size(); ++i)
            groups[{ entries[i].label.toLower(), entries[i].lun }].append(i);

        std::map<int, std::pair<int, int>> chunkByIndex;   // 条目下标 → (第几块, 共几块)
        std::map<QString, int> chunkTotals;                // label → 最大块数
        for (const auto& g : groups) {
            if (g.second.size() <= 1) continue;
            for (int i = 0; i < g.second.size(); ++i)
                chunkByIndex[g.second[i]] = { i + 1, static_cast<int>(g.second.size()) };
            chunkTotals[g.first.first] =
                qMax(chunkTotals[g.first.first], static_cast<int>(g.second.size()));
        }

        QVector<ImageRow> rows;
        int willFlash = 0, placeholder = 0, missing = 0, keptData = 0;

        for (int i = 0; i < entries.size(); ++i) {
            const swt::edl::RawProgramEntry& e = entries.at(i);
            QString label = e.label.trimmed();
            if (label.isEmpty())
                label = QFileInfo(e.filename).completeBaseName();
            const QString fileName = e.filename;
            const bool isEmptyFile = fileName.trimmed().isEmpty();

            QString status;
            bool will = false;
            if (isEmptyFile) {
                status = QStringLiteral("占位（不刷）");
                ++placeholder;
            } else if (!QFile::exists(imageDirAbs.filePath(fileName))) {
                status = QStringLiteral("镜像缺失");
                ++missing;
            } else if (excludedCopy.contains(label.toLower())) {
                status = QStringLiteral("保数据跳过");
                ++keptData;
            } else {
                status = QStringLiteral("会刷写");
                will = true;
                ++willFlash;
            }

            ImageRow row;
            row.label = label;
            const auto chunkIt = chunkByIndex.find(i);
            if (chunkIt != chunkByIndex.end())
                row.chunkText = QStringLiteral("#%1/%2")
                                    .arg(chunkIt->second.first)
                                    .arg(chunkIt->second.second);
            row.lun = e.lun;
            // start_sector 原样显示：表达式（NUM_DISK_SECTORS-5.）能让人看出这条
            // 写的是分区表结构位置，而不是普通数据。无表达式时退回解析出的数值。
            row.startSectorText = e.startSectorExpression.trimmed().isEmpty()
                                      ? (e.startSectorResolved
                                             ? QString::number(e.startSector)
                                             : QStringLiteral("?"))
                                      : e.startSectorExpression;
            row.numSectorsText =
                e.sectorCount > 0 ? QString::number(e.sectorCount) : QString();

            const QString fileFullPath = imageDirAbs.filePath(fileName);
            if (fileName.trimmed().isEmpty())
                row.fileText = QStringLiteral("（无）");
            else if (!QFile::exists(fileFullPath))
                row.fileText = fileName;
            else
                row.fileText =
                    QStringLiteral("%1  (%2)")
                        .arg(fileName,
                             LoaderPickerDialog::formatSize(QFileInfo(fileFullPath).size()));
            row.statusText = status;
            row.willFlash = will;
            rows.append(row);
        }

        // 按 LUN 归拢（LUN0 在前的读法与 QFIL 一致）；稳定排序，同 LUN 内保持
        // 包内块序（super_1..6 / userdata_1..10 不能被打乱）。
        std::stable_sort(rows.begin(), rows.end(),
                         [](const ImageRow& a, const ImageRow& b) { return a.lun < b.lun; });

        // 汇总行。
        QList<quint32> luns;
        for (const ImageRow& r : rows)
            if (!luns.contains(r.lun)) luns.append(r.lun);
        std::sort(luns.begin(), luns.end());
        QStringList lunText;
        for (quint32 l : luns) lunText.append(QStringLiteral("LUN%1").arg(l));

        QStringList sectorSizes;
        for (const swt::edl::RawProgramEntry& e : entries) {
            const QString s = e.sectorSize > 0 ? QString::number(e.sectorSize) : QString();
            if (!s.isEmpty() && !sectorSizes.contains(s)) sectorSizes.append(s);
        }

        QStringList multi;
        for (const auto& kv : chunkTotals)
            multi.append(QStringLiteral("%1 ×%2").arg(kv.first).arg(kv.second));

        QString summary =
            QStringLiteral("共 %1 条 <program>（%2 个 rawprogram，%3）· "
                           "会刷写 %4 · 占位 %5 · 保数据跳过 %6 · 镜像缺失 %7")
                .arg(entries.size())
                .arg(xmls.size())
                .arg(lunText.join(QStringLiteral("、LUN")))
                .arg(willFlash)
                .arg(placeholder)
                .arg(keptData)
                .arg(missing);
        if (!sectorSizes.isEmpty())
            summary += QStringLiteral(" · 包内扇区 %1 B").arg(sectorSizes.join('/'));
        if (!multi.isEmpty())
            summary += QStringLiteral(" · 多块镜像分组: ") + multi.join(QStringLiteral("、"));
        if (!badXml.isEmpty())
            summary += QStringLiteral("\n解析失败: ") + badXml.join(QStringLiteral("；"));
        if (missing > 0)
            summary += QStringLiteral("\n有镜像文件缺失的条目不会被刷写，"
                                      "请检查资料目录是否解压完整。");

        QMetaObject::invokeMethod(this, [this, guard, rows, summary] {
            if (!guard) return;
            m_all = rows;
            m_extraSummary = summary;
            m_empty->setVisible(false);
            m_table->setVisible(true);
            m_summary->setText(summary);
            applyFilter();
        }, Qt::QueuedConnection);
    }));
}

void PackageImageListDialog::applyFilter()
{
    const bool onlyFlash = m_onlyFlash->isChecked();
    QVector<ImageRow> view;
    for (const ImageRow& row : m_all)
        if (!onlyFlash || row.willFlash) view.append(row);

    m_table->clearContents();
    m_table->setRowCount(view.size());
    for (int i = 0; i < view.size(); ++i) {
        const ImageRow& row = view.at(i);
        m_table->setItem(i, 0, new QTableWidgetItem(row.label));
        m_table->setItem(i, 1, new QTableWidgetItem(row.chunkText));
        m_table->setItem(i, 2, new QTableWidgetItem(row.lunText()));
        m_table->setItem(i, 3, new QTableWidgetItem(row.startSectorText));
        m_table->setItem(i, 4, new QTableWidgetItem(row.numSectorsText));
        m_table->setItem(i, 5, new QTableWidgetItem(row.fileText));
        m_table->setItem(i, 6, new QTableWidgetItem(row.statusText));
        setStatusColor(i, row.statusText);
    }

    m_empty->setText(QStringLiteral("没有符合条件的条目。"));
    m_empty->setVisible(view.isEmpty());
}

void PackageImageListDialog::setStatusColor(int row, const QString& status)
{
    auto* item = m_table->item(row, 6);
    if (!item) return;

    const ThemeManager& theme = ThemeManager::instance();
    QColor color;
    if (status == QStringLiteral("会刷写"))
        color = theme.color(ColorToken::Success);
    else if (status == QStringLiteral("镜像缺失"))
        color = theme.color(ColorToken::Danger);
    else if (status == QStringLiteral("保数据跳过"))
        color = theme.color(ColorToken::Warning);
    else
        color = theme.color(ColorToken::TextFaint);
    item->setForeground(color);
}

} // namespace kc::ui
