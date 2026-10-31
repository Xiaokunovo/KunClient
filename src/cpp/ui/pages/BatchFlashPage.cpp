#include "ui/pages/BatchFlashPage.h"

#include "ui/widgets/LogView.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace kc::ui {

namespace {

/// 列定义（界面显示用）。
const char* const kHeaders[] = {"序列号", "机型", "固件包", "状态", "说明"};

QString stateToText(kc::tools::QueueState s)
{
    return kc::tools::queueStateName(s);
}

} // namespace

BatchFlashPage::BatchFlashPage(QWidget* parent) : BasePage(parent)
{
    setTitle(QStringLiteral("批量刷机"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    // ── 顶部说明 ──────────────────────────────────────────────────────────
    auto* hint = new QLabel(this);
    hint->setObjectName(QStringLiteral("settingsHint"));
    hint->setWordWrap(true);
    hint->setText(QStringLiteral(
        "把要刷的设备排进队列，程序会逐个提示你换设备。"
        "本项目的刷机链路是单设备架构，所以这里是「排队 + 逐个提示」而不是同时刷多台 —— "
        "它的价值是不会漏、不用记。"));
    root->addWidget(hint);

    // ── 当前设备 + 操作行 ─────────────────────────────────────────────────
    auto* opRow = new QHBoxLayout;
    opRow->setSpacing(8);

    m_deviceLabel = new QLabel(this);
    m_deviceLabel->setObjectName(QStringLiteral("statusValue"));
    m_deviceLabel->setText(QStringLiteral("当前设备：未连接"));
    opRow->addWidget(m_deviceLabel);
    opRow->addStretch();

    m_addButton = new QPushButton(QStringLiteral("加入当前设备"), this);
    m_addButton->setCursor(Qt::PointingHandCursor);
    m_addButton->setToolTip(QStringLiteral("把当前连着的设备加进队列"));
    opRow->addWidget(m_addButton);

    m_packageButton = new QPushButton(QStringLiteral("设置固件包…"), this);
    m_packageButton->setCursor(Qt::PointingHandCursor);
    m_packageButton->setToolTip(QStringLiteral("给所有待处理的项设同一个包"));
    opRow->addWidget(m_packageButton);

    root->addLayout(opRow);

    // ── 队列表格 ──────────────────────────────────────────────────────────
    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("batchFlashTable"));
    m_table->setColumnCount(5);
    QStringList headers;
    for (const char* h : kHeaders) headers << QString::fromUtf8(h);
    m_table->setHorizontalHeaderLabels(headers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0, 150);
    m_table->setColumnWidth(1, 160);
    m_table->setColumnWidth(2, 240);
    m_table->setColumnWidth(3, 90);
    root->addWidget(m_table, 1);

    // ── 底部：汇总 + 按钮 ─────────────────────────────────────────────────
    auto* bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(8);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("statusLabel"));
    bottomRow->addWidget(m_summary);
    bottomRow->addStretch();

    m_resetButton = new QPushButton(QStringLiteral("重置全部"), this);
    m_resetButton->setCursor(Qt::PointingHandCursor);
    m_resetButton->setToolTip(QStringLiteral("把所有项拉回待处理（整批重刷）"));
    bottomRow->addWidget(m_resetButton);

    m_clearButton = new QPushButton(QStringLiteral("清空队列"), this);
    m_clearButton->setCursor(Qt::PointingHandCursor);
    bottomRow->addWidget(m_clearButton);

    m_startButton = new QPushButton(QStringLiteral("开始 / 继续"), this);
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    m_startButton->setCursor(Qt::PointingHandCursor);
    bottomRow->addWidget(m_startButton);

    root->addLayout(bottomRow);

    // ── 日志 ──────────────────────────────────────────────────────────────
    auto* logCard = new QFrame(this);
    logCard->setObjectName(QStringLiteral("card"));
    auto* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(12, 10, 12, 10);
    logLayout->setSpacing(6);
    auto* logTitle = new QLabel(QStringLiteral("执行日志"), logCard);
    logTitle->setObjectName(QStringLiteral("groupHeader"));
    logLayout->addWidget(logTitle);
    m_log = new LogView(logCard);
    m_log->setMinimumHeight(120);
    logLayout->addWidget(m_log, 1);
    registerLogView(m_log);
    root->addWidget(logCard, 1);

    // ── 接线 ──────────────────────────────────────────────────────────────
    connect(m_addButton, &QPushButton::clicked, this, &BatchFlashPage::addCurrentDevice);
    connect(m_packageButton, &QPushButton::clicked, this, &BatchFlashPage::setPackageForPending);
    connect(m_startButton, &QPushButton::clicked, this, &BatchFlashPage::startOrContinue);

    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        m_queue.clear();
        m_currentSerial.clear();
        rebuildTable();
        refreshSummary();
        m_log->appendInfo(QStringLiteral("队列已清空。"));
    });

    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        m_queue.reset();
        m_currentSerial.clear();
        rebuildTable();
        refreshSummary();
        m_log->appendInfo(QStringLiteral("全部重置为待处理。"));
    });

    rebuildTable();
    refreshSummary();
}

void BatchFlashPage::setCurrentDevice(const QString& summary)
{
    m_currentDevice = summary;
    m_deviceLabel->setText(summary.trimmed().isEmpty()
                               ? QStringLiteral("当前设备：未连接")
                               : QStringLiteral("当前设备：%1").arg(summary));
}

void BatchFlashPage::rebuildTable()
{
    const auto& items = m_queue.items();
    m_table->setRowCount(items.size());
    for (int i = 0; i < items.size(); ++i) {
        const kc::tools::QueueItem& it = items.at(i);
        m_table->setItem(i, 0, new QTableWidgetItem(it.serial));
        m_table->setItem(i, 1, new QTableWidgetItem(it.model));
        m_table->setItem(i, 2, new QTableWidgetItem(it.packagePath));
        m_table->setItem(i, 3, new QTableWidgetItem(stateToText(it.state)));
        m_table->setItem(i, 4, new QTableWidgetItem(it.message));
    }
}

void BatchFlashPage::refreshSummary()
{
    const kc::tools::QueueSummary s = m_queue.summary();
    m_summary->setText(QStringLiteral("共 %1 项 · 成功 %2 · 失败 %3 · 跳过 %4")
                           .arg(s.total).arg(s.done).arg(s.failed).arg(s.skipped));
    // 「开始」按钮只在还有待处理项时可用 —— 否则点了什么都不会发生，像坏了。
    m_startButton->setEnabled(m_queue.hasPending());
}

void BatchFlashPage::addCurrentDevice()
{
    const QString summary = m_currentDevice.trimmed();
    if (summary.isEmpty()) {
        notify(QStringLiteral("还没有连上设备。先到「Qualcomm」页连接设备再回来。"),
               /*level=*/2);
        return;
    }

    // 序列号从设备摘要里取（格式 "COM7 · 机型 · 序列号"，最后一段）。
    QString serial = summary;
    const QStringList parts = summary.split(QStringLiteral(" · "));
    if (parts.size() >= 2) serial = parts.last().trimmed();

    kc::tools::QueueItem item;
    item.serial = serial;
    item.model = parts.size() >= 2 ? parts.at(parts.size() - 2).trimmed() : QString();
    item.packagePath = QString();

    if (!m_queue.add(item)) {
        notify(QStringLiteral("加不进去：序列号为空，或者这台设备已经在队列里了。"),
               /*level=*/2);
        return;
    }
    m_log->appendInfo(QStringLiteral("已加入队列：%1").arg(summary));
    rebuildTable();
    refreshSummary();
}

void BatchFlashPage::setPackageForPending()
{
    if (!m_queue.hasPending()) {
        notify(QStringLiteral("队列里没有待处理的项。"), /*level=*/1);
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要刷的固件包"), QString(),
        QStringLiteral("刷机 XML (*.xml);;镜像文件 (*.img);;所有文件 (*)"));
    if (path.isEmpty()) return;

    // 逐个设包（FlashQueue 没有批量设包的接口 —— 保持模型层最小）。
    for (const kc::tools::QueueItem& it : m_queue.items()) {
        if (it.state != kc::tools::QueueState::Pending) continue;
        kc::tools::QueueItem copy = it;
        copy.packagePath = path;
        m_queue.remove(it.serial);
        m_queue.add(copy);
    }
    m_log->appendInfo(QStringLiteral("已给待处理项设置固件包：%1")
                          .arg(QDir::toNativeSeparators(path)));
    rebuildTable();
    refreshSummary();
}

void BatchFlashPage::startOrContinue()
{
    kc::tools::QueueItem next;
    if (!m_queue.next(&next)) {
        notify(QStringLiteral("没有待处理的项了。"), /*level=*/1);
        return;
    }
    if (next.packagePath.trimmed().isEmpty()) {
        // 没设包就开刷是没意义的，把它退回 Pending 并提示。
        m_queue.resetOne(next.serial);
        rebuildTable();
        refreshSummary();
        notify(QStringLiteral("这一项还没有固件包。先点「设置固件包…」。"), /*level=*/2);
        return;
    }

    m_currentSerial = next.serial;
    m_log->appendInfo(QStringLiteral("开始处理：%1（%2）")
                          .arg(next.serial, QDir::toNativeSeparators(next.packagePath)));
    rebuildTable();
    refreshSummary();

    // 转交外壳。**不在这里假装成功** —— 真正的刷机结果由 onFlashFinished 写回。
    emit requestDeviceAction(QStringLiteral("flash"),
                             QStringLiteral("%1|%2").arg(next.serial, next.packagePath));
}

void BatchFlashPage::markItem(bool success, const QString& message)
{
    if (m_currentSerial.isEmpty()) return;
    if (success) {
        m_queue.markDone(m_currentSerial, message);
    } else {
        m_queue.markFailed(m_currentSerial, message);
    }
    m_currentSerial.clear();
    rebuildTable();
    refreshSummary();
}

void BatchFlashPage::onFlashFinished(bool success, const QString& message)
{
    markItem(success, message);
    if (!m_currentSerial.isEmpty()) return;

    if (m_queue.hasPending()) {
        m_log->appendInfo(QStringLiteral("这一项结束（%1）。还有待处理项，"
                                         "换好设备后点「开始 / 继续」。")
                              .arg(success ? QStringLiteral("成功") : QStringLiteral("失败")));
    } else {
        const kc::tools::QueueSummary s = m_queue.summary();
        m_log->appendSuccess(QStringLiteral("整批结束：成功 %1 / 失败 %2 / 跳过 %3")
                                 .arg(s.done).arg(s.failed).arg(s.skipped));
    }
}

} // namespace kc::ui
