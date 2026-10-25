#include "ui/pages/MtkFlashPage.h"

#include "kc/mtk/MtkDevice.h"
#include "kc/mtk/MtkWorker.h"
#include "kc/mtk/ScatterParser.h"
#include "ui/ShellStatus.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>   // 分区表：QTableWidget / QTableWidgetItem 都在这个头里
#include <QThread>
#include <QThreadPool>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

constexpr int kColCheck = 0;
constexpr int kColName = 1;
constexpr int kColStart = 2;
constexpr int kColSize = 3;
constexpr int kColFile = 4;
constexpr int kColStatus = 5;

/// 把一段函数体丢进全局线程池（等价 C# 的 Task.Run）。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

void runPooled(std::function<void()> fn)
{
    QThreadPool::globalInstance()->start(new LambdaTask(std::move(fn)));
}

/// 保护分区判定：scatter 名单命中，或 region/operation_type 里带 PROTECTED / BINREGION。
bool isProtectedPartition(const kc::mtk::ScatterInfo& info, const kc::mtk::ScatterPartition& part)
{
    if (info.protectedParts.contains(part.name, Qt::CaseInsensitive)) return true;
    const QString upperRegion = part.region.toUpper();
    const QString upperOp = part.operationType.toUpper();
    return upperRegion.contains(QStringLiteral("PROTECTED"))
        || upperOp.contains(QStringLiteral("PROTECTED"))
        || upperOp.contains(QStringLiteral("BINREGION"));
}

} // namespace

MtkFlashPage::MtkFlashPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("MTK"));
    buildUi();
}

MtkFlashPage::~MtkFlashPage()
{
    stopWorker();
}

void MtkFlashPage::onActivated()
{
    // 轻量扫描：只刷新「设备」读数，不握手（握手要点按钮，避免切页就占口）。
    scanDevicesOnly();
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI 构建
// ─────────────────────────────────────────────────────────────────────────────
void MtkFlashPage::buildUi()
{
    // 2026-10-17：本页是「裸行布局」（没有 #card），控件直接浮在壁纸上 ——
    // 铺一张与卡片同材质的整页玻璃板，用户口径「这页没实现液态玻璃」即指此处。
    setGlassSheet(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // ── scatter 行 ──────────────────────────────────────────────────────
    auto* scatterRow = new QHBoxLayout;
    scatterRow->setSpacing(8);
    auto* scatterLabel = new QLabel(QStringLiteral("Scatter 文件:"), this);
    scatterLabel->setObjectName(QStringLiteral("statusLabel"));
    scatterRow->addWidget(scatterLabel);

    m_scatterEdit = new QLineEdit(this);
    m_scatterEdit->setPlaceholderText(QStringLiteral("选择线刷包内的 scatter.txt（分区表 + 镜像清单）"));
    m_scatterEdit->setReadOnly(true);
    scatterRow->addWidget(m_scatterEdit, 1);

    m_browseButton = new QPushButton(QStringLiteral("浏览…"), this);
    m_browseButton->setObjectName(QStringLiteral("primaryButton"));
    m_browseButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_browseButton);
    connect(m_browseButton, &QPushButton::clicked, this, [this] { pickScatterFile(); });
    scatterRow->addWidget(m_browseButton);

    m_reparseButton = new QPushButton(QStringLiteral("重新解析"), this);
    m_reparseButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_reparseButton);
    connect(m_reparseButton, &QPushButton::clicked, this, [this] {
        if (!m_scatterPath.isEmpty()) loadScatterFile(m_scatterPath);
    });
    scatterRow->addWidget(m_reparseButton);
    layout->addLayout(scatterRow);

    // ── DA 行 ───────────────────────────────────────────────────────────
    //  DA 文件（MTK_AllInOne_DA.bin）是「刷写」的必需品：BROM 只能识别芯片，
    //  一切分区读写都由 DA 完成。preloader 不单独选 —— 直接从 scatter 的
    //  PRELOADER 分区取（部分平台需要它提供 DRAM 参数）。
    auto* daRow = new QHBoxLayout;
    daRow->setSpacing(8);
    auto* daLabel = new QLabel(QStringLiteral("DA 文件:"), this);
    daLabel->setObjectName(QStringLiteral("statusLabel"));
    daRow->addWidget(daLabel);

    m_daEdit = new QLineEdit(this);
    m_daEdit->setPlaceholderText(
        QStringLiteral("选择 MTK_AllInOne_DA.bin（刷写必需；BROM 自身只能识别芯片）"));
    m_daEdit->setReadOnly(true);
    daRow->addWidget(m_daEdit, 1);

    m_daBrowseButton = new QPushButton(QStringLiteral("浏览…"), this);
    m_daBrowseButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_daBrowseButton);
    connect(m_daBrowseButton, &QPushButton::clicked, this, [this] { pickDaFile(); });
    daRow->addWidget(m_daBrowseButton);
    layout->addLayout(daRow);

    // ── 读数行：芯片 / 存储 / 设备 / HW code ────────────────────────────
    auto* readingRow = new QHBoxLayout;
    readingRow->setSpacing(10);

    auto makeReading = [](const QString& label, QLabel** value, QWidget* parent) {
        auto* box = new QWidget(parent);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto* key = new QLabel(label, box);
        key->setObjectName(QStringLiteral("statusLabel"));
        *value = new QLabel(QStringLiteral("-"), box);
        (*value)->setObjectName(QStringLiteral("statusValue"));
        row->addWidget(key);
        row->addWidget(*value);
        return box;
    };
    readingRow->addWidget(makeReading(QStringLiteral("芯片:"), &m_chipValue, this));
    readingRow->addWidget(makeReading(QStringLiteral("存储:"), &m_storageValue, this));
    readingRow->addWidget(makeReading(QStringLiteral("设备:"), &m_deviceValue, this));
    readingRow->addWidget(makeReading(QStringLiteral("HW code:"), &m_hwCodeValue, this));
    readingRow->addStretch();

    m_disconnectButton = new QPushButton(QStringLiteral("断开连接"), this);
    m_disconnectButton->setCursor(Qt::PointingHandCursor);
    m_disconnectButton->setEnabled(false);
    m_busyButtons.append(m_disconnectButton);
    connect(m_disconnectButton, &QPushButton::clicked, this, [this] { disconnectDevice(); });
    readingRow->addWidget(m_disconnectButton);

    m_connectButton = new QPushButton(QStringLiteral("连接设备"), this);
    m_connectButton->setObjectName(QStringLiteral("primaryButton"));
    m_connectButton->setCursor(Qt::PointingHandCursor);
    m_connectButton->setToolTip(QStringLiteral("扫描 BROM 口 → 握手 → 读芯片 ID → 引导 DA（需要上面的 DA 文件）"));
    m_busyButtons.append(m_connectButton);
    connect(m_connectButton, &QPushButton::clicked, this, [this] { connectDevice(); });
    readingRow->addWidget(m_connectButton);
    layout->addLayout(readingRow);

    // ── 动作行 ──────────────────────────────────────────────────────────
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);

    m_downloadButton = new QPushButton(QStringLiteral("下载选中"), this);
    m_downloadButton->setObjectName(QStringLiteral("primaryButton"));
    m_downloadButton->setToolTip(QStringLiteral("经 DA 的 WRITE_DATA 写入所选分区（全速）"));
    m_readbackButton = new QPushButton(QStringLiteral("读回选中"), this);
    m_readbackButton->setToolTip(QStringLiteral("经 DA 的 READ_DATA 读回所选分区，落盘到 scatter 同目录的 readback/"));
    m_eraseButton = new QPushButton(QStringLiteral("擦除选中"), this);
    m_eraseButton->setToolTip(QStringLiteral("经 DA 的 FORMAT 擦除所选分区（整分区清零，不可回滚）"));
    m_cancelButton = new QPushButton(QStringLiteral("取消"), this);
    m_cancelButton->setToolTip(QStringLiteral("中止当前写入/读回/擦除（设备侧不会回滚已写入的数据）"));
    m_cancelButton->setEnabled(false);
    m_selectAllButton = new QPushButton(QStringLiteral("全选"), this);
    m_selectNoneButton = new QPushButton(QStringLiteral("全不选"), this);
    for (QPushButton* b : { m_downloadButton, m_readbackButton, m_eraseButton, m_selectAllButton, m_selectNoneButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        actionRow->addWidget(b);
    }
    actionRow->addWidget(m_cancelButton);
    connect(m_downloadButton, &QPushButton::clicked, this, [this] { downloadSelected(); });
    connect(m_readbackButton, &QPushButton::clicked, this, [this] { readbackSelected(); });
    connect(m_eraseButton, &QPushButton::clicked, this, [this] { eraseSelected(); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_worker) m_worker->requestCancel();
        logWarning(QStringLiteral("已请求取消，等待当前数据块收尾…"));
    });
    connect(m_selectAllButton, &QPushButton::clicked, this, [this] { setAllChecks(true); });
    connect(m_selectNoneButton, &QPushButton::clicked, this, [this] { setAllChecks(false); });

    actionRow->addStretch();

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("搜索分区"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setFixedWidth(180);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        applyFilter(text.trimmed());
    });
    actionRow->addWidget(m_filterEdit);
    layout->addLayout(actionRow);

    // ── 分区表 ──────────────────────────────────────────────────────────
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    // 表头一次性铺好（未加载 scatter 时也必须是列名，不能露出 Qt 默认的 1/2/3…）。
    applyHeaderLayout();
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
        if (column == kColFile) pickImageFor(row);
    });
    layout->addWidget(m_table, 1);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    log->setMinimumHeight(140);
    layout->addWidget(log);

    updateActionButtons();
}

void MtkFlashPage::refreshTable()
{
    m_updatingTable = true;
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(m_scatter.partitions.size()));

    for (int i = 0; i < m_scatter.partitions.size(); ++i) {
        syncRowToTable(i);
    }

    applyHeaderLayout();

    m_updatingTable = false;
}

void MtkFlashPage::applyHeaderLayout()
{
    const QStringList headers = { QStringLiteral(""), QStringLiteral("分区"), QStringLiteral("起始地址"),
                                  QStringLiteral("大小"), QStringLiteral("镜像文件"), QStringLiteral("状态") };
    m_table->setHorizontalHeaderLabels(headers);
    auto* header = m_table->horizontalHeader();
    header->setSectionResizeMode(kColCheck, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kColName, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kColStart, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kColSize, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kColFile, QHeaderView::Stretch);
    header->setSectionResizeMode(kColStatus, QHeaderView::ResizeToContents);
}

void MtkFlashPage::syncRowToTable(int index)
{
    if (index < 0 || index >= m_scatter.partitions.size()) return;
    const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(index);

    const QString fileText = m_fileOverride.contains(part.name)
                                 ? m_fileOverride.value(part.name)
                                 : (part.absoluteFilePath.isEmpty() ? QStringLiteral("-")
                                                                    : part.absoluteFilePath);

    auto* check = new QTableWidgetItem();
    check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    check->setCheckState(part.enabled ? Qt::Checked : Qt::Unchecked);
    check->setData(Qt::UserRole, part.name);
    m_table->setItem(index, kColCheck, check);

    m_table->setItem(index, kColName, new QTableWidgetItem(part.name));
    m_table->setItem(index, kColStart, new QTableWidgetItem(formatAddr(part.startAddress)));
    m_table->setItem(index, kColSize, new QTableWidgetItem(formatSize(static_cast<qint64>(part.size))));
    m_table->setItem(index, kColFile, new QTableWidgetItem(fileText));

    QString statusText;
    if (!part.isDownload) {
        statusText = QStringLiteral("不下载（scatter 标记）");
    } else if (!part.fileExists && !m_fileOverride.contains(part.name)) {
        statusText = QStringLiteral("镜像缺失");
    } else {
        statusText = QStringLiteral("就绪");
    }
    auto* status = new QTableWidgetItem(statusText);
    m_table->setItem(index, kColStatus, status);
}

void MtkFlashPage::applyFilter(const QString& text)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* nameItem = m_table->item(row, kColName);
        if (!nameItem) continue;
        const bool match = text.isEmpty() || nameItem->text().contains(text, Qt::CaseInsensitive);
        m_table->setRowHidden(row, !match);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  scatter / DA 流程
// ─────────────────────────────────────────────────────────────────────────────
void MtkFlashPage::pickScatterFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 scatter 文件"), QString(),
        QStringLiteral("MTK scatter (*.txt *.xml);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    loadScatterFile(path);
}

void MtkFlashPage::pickDaFile()
{
    const QString startDir = m_scatterPath.isEmpty() ? QString() : QFileInfo(m_scatterPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 DA 文件"), startDir,
        QStringLiteral("MTK DA (*.bin);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    setDaFile(path);
}

void MtkFlashPage::setDaFile(const QString& path)
{
    m_daPath = path;
    m_daEdit->setText(path);
    m_daReady = false;
    if (!path.isEmpty()) logInfo(QStringLiteral("已选择 DA 文件：%1").arg(path));
    updateActionButtons();
}

QString MtkFlashPage::preloaderPath() const
{
    if (!m_scatter.valid) return QString();
    for (const kc::mtk::ScatterPartition& part : m_scatter.partitions) {
        if (part.name.compare(QStringLiteral("preloader"), Qt::CaseInsensitive) != 0) continue;
        if (m_fileOverride.contains(part.name)) return m_fileOverride.value(part.name);
        if (part.fileExists) return part.absoluteFilePath;
    }
    return QString();
}

void MtkFlashPage::loadScatterFile(const QString& path)
{
    const kc::mtk::ScatterInfo info = kc::mtk::ScatterParser::parseFile(path);
    if (!info.valid) {
        error(info.errorMessage.isEmpty() ? QStringLiteral("scatter 解析失败") : info.errorMessage);
        logError(QStringLiteral("scatter 解析失败：%1").arg(path));
        return;
    }

    m_scatter = info;
    m_scatterPath = path;
    m_fileOverride.clear();
    m_scatterEdit->setText(path);
    m_chipValue->setText(info.chipName.isEmpty() ? QStringLiteral("-") : info.chipName);
    m_storageValue->setText(kc::mtk::storageTypeName(info.storage));

    int downloadCount = 0;
    int missingCount = 0;
    for (const kc::mtk::ScatterPartition& part : info.partitions) {
        if (part.isDownload) ++downloadCount;
        if (part.isDownload && !part.fileExists) ++missingCount;
    }

    // 兜底：scatter 里 file_name 写法与磁盘不一致时，按文件在 scatter 同目录再找一遍。
    if (missingCount > 0) {
        autoMatchImages();
        missingCount = 0; // 重算（autoMatchImages 已就地改 m_scatter，不重算日志会少报）
        for (const kc::mtk::ScatterPartition& part : m_scatter.partitions) {
            if (part.isDownload && !part.fileExists) ++missingCount;
        }
    }

    refreshTable();
    logSuccess(QStringLiteral("scatter 解析成功：%1（%2 个分区，%3 个标记下载，%4 个镜像缺失）")
                   .arg(info.chipName.isEmpty() ? QStringLiteral("未知芯片") : info.chipName)
                   .arg(info.partitions.size())
                   .arg(downloadCount)
                   .arg(missingCount));
    for (const QString& warning : info.warnings) {
        logWarning(warning);
    }
    if (!info.protectedParts.isEmpty()) {
        logWarning(QStringLiteral("保护分区（写入前会二次确认）：%1")
                       .arg(info.protectedParts.join(QStringLiteral(", "))));
    }
    success(QStringLiteral("已加载 scatter：%1 个分区").arg(info.partitions.size()));
    updateActionButtons();
}

void MtkFlashPage::autoMatchImages()
{
    if (m_scatterPath.isEmpty() || !m_scatter.valid) return;
    const QDir baseDir(QFileInfo(m_scatterPath).absolutePath());
    int matched = 0;
    for (kc::mtk::ScatterPartition& part : m_scatter.partitions) {
        if (part.fileName.isEmpty()) continue;
        const QString abs = baseDir.absoluteFilePath(part.fileName);
        if (QFileInfo::exists(abs)) {
            part.absoluteFilePath = abs;
            part.fileExists = true;
            ++matched;
        }
    }
    if (matched > 0) {
        refreshTable();
        logInfo(QStringLiteral("镜像目录匹配：%1 个文件就位").arg(matched));
    }
}

void MtkFlashPage::pickImageFor(int tableRow)
{
    if (tableRow < 0 || tableRow >= m_scatter.partitions.size()) return;
    const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(tableRow);

    const QString startDir = m_scatterPath.isEmpty()
                                 ? QString()
                                 : QFileInfo(m_scatterPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("为 %1 选择镜像").arg(part.name), startDir,
        QStringLiteral("镜像文件 (*.img *.bin *.mbn *.dtb);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    m_fileOverride.insert(part.name, path);
    refreshTable();
    logInfo(QStringLiteral("%1 镜像手动指定：%2").arg(part.name, path));
}

// ─────────────────────────────────────────────────────────────────────────────
//  设备流程（worker 线程）
// ─────────────────────────────────────────────────────────────────────────────
void MtkFlashPage::ensureWorker()
{
    if (m_worker) return;

    qRegisterMetaType<QVector<kc::mtk::MtkJob>>("QVector<kc::mtk::MtkJob>");

    m_workerThread = new QThread(this);
    m_workerThread->setObjectName(QStringLiteral("mtk-worker"));
    m_worker = new kc::mtk::MtkWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &kc::mtk::MtkWorker::logLine, this,
            [this](const QString& level, const QString& message) {
                if (level == QStringLiteral("error")) logError(message);
                else if (level == QStringLiteral("warning")) logWarning(message);
                else if (level == QStringLiteral("success")) logSuccess(message);
                else logInfo(message);
            });

    connect(m_worker, &kc::mtk::MtkWorker::rowStatus, this, [this](int row, const QString& text) {
        if (QTableWidgetItem* item = m_table->item(row, kColStatus)) item->setText(text);
    });

    connect(m_worker, &kc::mtk::MtkWorker::progressChanged, this, [this](qint64 done, qint64 total) {
        if (total <= 0) return;
        const double percent = double(done) * 100.0 / double(total);
        ShellStatus::report(std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, percent);
    });

    connect(m_worker, &kc::mtk::MtkWorker::connectionResult, this,
            [this](bool ok, uint16_t hwCode, const QString& comPort, const QString& message) {
                if (ok) {
                    m_hwCode = hwCode;
                    m_connectedComPort = comPort;
                    m_hwCodeValue->setText(QStringLiteral("0x%1").arg(hwCode, 4, 16, QLatin1Char('0')));
                    m_deviceValue->setText(QStringLiteral("%1（%2）")
                                               .arg(kc::mtk::deviceStageName(m_deviceStage), comPort));
                    setShellStatus(QStringLiteral("MTK %1").arg(comPort),
                                   kc::mtk::deviceStageName(m_deviceStage), QStringLiteral("已连接"));
                    success(QStringLiteral("MTK 设备已连接（%1）").arg(comPort));
                } else {
                    m_daReady = false;
                    error(message.isEmpty() ? QStringLiteral("连接失败") : message);
                }
                updateActionButtons();
            });

    connect(m_worker, &kc::mtk::MtkWorker::daResult, this, [this](bool ok, const QString& message) {
        m_daReady = ok;
        if (ok) {
            success(QStringLiteral("DA 已就绪，可以开始刷写"));
        } else {
            error(message.isEmpty() ? QStringLiteral("DA 引导失败") : message);
        }
        updateActionButtons();
    });

    connect(m_worker, &kc::mtk::MtkWorker::jobsFinished, this, [this](bool ok, const QString& message) {
        ShellStatus::endTask();
        m_busy.fetchAndSubRelease(1);
        setBusy(false);
        if (ok) success(message);
        else error(message.isEmpty() ? QStringLiteral("操作中止") : message);
        updateActionButtons();
    });

    m_workerThread->start();
}

void MtkFlashPage::stopWorker()
{
    if (!m_workerThread) {
        m_worker = nullptr;
        return;
    }
    if (m_worker) m_worker->requestCancel(); // 先请求取消，长循环每块都会看这个标志
    if (m_workerThread->isRunning() && m_worker) {
        // closeSession 必须在 worker 线程执行（那里创建了 QSerialPort）
        QMetaObject::invokeMethod(m_worker, "closeSession", Qt::BlockingQueuedConnection);
        m_workerThread->quit();
        m_workerThread->wait(5000);
    }
    delete m_worker;
    m_worker = nullptr;
    delete m_workerThread;
    m_workerThread = nullptr;
}

void MtkFlashPage::scanDevicesOnly()
{
    runPooled([this] {
        const QVector<kc::mtk::MtkUsbDevice> devices = kc::mtk::scanMtkDevices();
        QString text = QStringLiteral("未连接");
        for (const kc::mtk::MtkUsbDevice& device : devices) {
            if (device.stage == kc::mtk::DeviceStage::Disconnected) continue;
            text = QStringLiteral("%1（%2）")
                       .arg(kc::mtk::deviceStageName(device.stage),
                            device.comPort.isEmpty() ? QStringLiteral("无端口") : device.comPort);
            break;
        }
        runOnUi([this, text] { m_deviceValue->setText(text); });
    });
}

void MtkFlashPage::connectDevice()
{
    if (m_busy.fetchAndAddAcquire(1) != 0) {
        m_busy.fetchAndSubRelease(1);
        return;
    }
    setBusy(true);
    logInfo(QStringLiteral("正在扫描 MTK 设备（VID 0x0E8D：BROM 0x0003 / Preloader 0x2000 / DA 0x2001）…"));

    // 扫描在线程池里做（纯查询），拿到端口后再投给长驻 worker。
    QPointer<MtkFlashPage> guard(this);
    runPooled([this, guard] {
        kc::mtk::MtkUsbDevice target;
        QString failure;
        if (!kc::mtk::findMtkDevice(kc::mtk::DeviceStage::Disconnected, &target)) {
            failure = QStringLiteral(
                "未找到 MTK 设备。请将设备关机后按住音量键（或双键）插入 USB 进入 BROM/9008 模式。");
        } else if (target.comPort.isEmpty()) {
            failure = QStringLiteral(
                "已看到 MTK 设备（%1）但取不到 COM 口：请先安装 MTK USB 驱动（设备管理器无感叹号）。")
                          .arg(kc::mtk::deviceStageName(target.stage));
        }

        runOnUi([this, guard, target, failure] {
            if (!guard) return;
            if (!failure.isEmpty()) {
                m_busy.fetchAndSubRelease(1);
                setBusy(false);
                error(failure);
                logError(failure);
                updateActionButtons();
                return;
            }

            m_deviceStage = target.stage;
            m_deviceValue->setText(QStringLiteral("%1（%2）")
                                       .arg(kc::mtk::deviceStageName(target.stage), target.comPort));
            logInfo(QStringLiteral("发现设备：%1（%2，PID 0x%3），开始握手…")
                        .arg(kc::mtk::deviceStageName(target.stage), target.comPort)
                        .arg(target.pid, 4, 16, QLatin1Char('0')));

            ensureWorker();
            const QString da = m_daPath;
            const QString pre = preloaderPath();
            QMetaObject::invokeMethod(m_worker, "openAndBoot", Qt::QueuedConnection,
                                      Q_ARG(QString, target.comPort),
                                      Q_ARG(QString, da),
                                      Q_ARG(QString, pre));
        });
    });
}

void MtkFlashPage::disconnectDevice()
{
    if (m_worker) m_worker->requestCancel();
    if (m_workerThread && m_workerThread->isRunning() && m_worker) {
        QMetaObject::invokeMethod(m_worker, "closeSession", Qt::BlockingQueuedConnection);
    }
    m_connectedComPort.clear();
    m_deviceStage = kc::mtk::DeviceStage::Disconnected;
    m_hwCode = 0;
    m_daReady = false;
    m_deviceValue->setText(QStringLiteral("未连接"));
    m_hwCodeValue->setText(QStringLiteral("-"));
    logInfo(QStringLiteral("已断开 MTK 设备连接（DA 会话已释放）"));
    updateActionButtons();
}

void MtkFlashPage::applyDeviceReading(const kc::mtk::MtkUsbDevice& device, const QString& hwCodeText)
{
    m_deviceValue->setText(QStringLiteral("%1（%2）")
                               .arg(kc::mtk::deviceStageName(device.stage), device.comPort));
    m_hwCodeValue->setText(hwCodeText);
    setShellStatus(QStringLiteral("MTK %1").arg(device.comPort),
                   kc::mtk::deviceStageName(device.stage), QStringLiteral("已连接"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  传输流程
// ─────────────────────────────────────────────────────────────────────────────
bool MtkFlashPage::confirmProtected(const QVector<int>& rows, const QString& verb)
{
    QStringList protectedNames;
    for (int row : rows) {
        if (row < 0 || row >= m_scatter.partitions.size()) continue;
        const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(row);
        if (isProtectedPartition(m_scatter, part)) protectedNames << part.name;
    }
    if (protectedNames.isEmpty()) return true;

    const auto answer = QMessageBox::warning(
        this, QStringLiteral("保护分区确认"),
        QStringLiteral("以下分区被标记为保护/校准分区：\n\n%1\n\n"
                       "%2错误内容（或直接擦掉）可能导致设备无法开机、"
                       "传感器/射频校准数据丢失，且通常不可恢复。\n\n"
                       "确定要%2这些分区吗？")
            .arg(protectedNames.join(QStringLiteral(", ")), verb),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        logWarning(QStringLiteral("用户确认%1保护分区：%2")
                       .arg(verb, protectedNames.join(QStringLiteral(", "))));
        return true;
    }
    logInfo(QStringLiteral("已取消：保护分区%1未确认").arg(verb));
    return false;
}

void MtkFlashPage::downloadSelected()
{
    if (!m_scatter.valid) {
        error(QStringLiteral("请先加载 scatter 文件"));
        return;
    }
    if (m_connectedComPort.isEmpty()) {
        error(QStringLiteral("请先连接设备"));
        return;
    }
    if (!m_daReady) {
        error(QStringLiteral("DA 尚未就绪：请在「连接设备」前选择 DA 文件（MTK_AllInOne_DA.bin）。"));
        return;
    }
    if (m_busy.loadRelaxed() != 0) {
        error(QStringLiteral("有操作正在进行中"));
        return;
    }

    QVector<int> rows;
    QStringList missing;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* checkItem = m_table->item(row, kColCheck);
        if (!checkItem || checkItem->checkState() != Qt::Checked) continue;
        if (row >= m_scatter.partitions.size()) continue;

        const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(row);
        const QString filePath = m_fileOverride.contains(part.name)
                                     ? m_fileOverride.value(part.name)
                                     : part.absoluteFilePath;
        if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
            missing << part.name;
            continue;
        }
        rows.append(row);
    }

    if (!missing.isEmpty()) {
        error(QStringLiteral("以下分区镜像缺失，请先补齐：%1").arg(missing.join(QStringLiteral(", "))));
        return;
    }
    if (rows.isEmpty()) {
        error(QStringLiteral("没有勾选任何可下载分区"));
        return;
    }
    if (!confirmProtected(rows)) return;

    // 组作业（镜像路径在本线程算好，worker 只做 IO）
    QVector<kc::mtk::MtkJob> jobs;
    jobs.reserve(rows.size());
    for (int row : rows) {
        const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(row);
        kc::mtk::MtkJob job;
        job.row = row;
        job.name = part.name;
        job.filePath = m_fileOverride.contains(part.name) ? m_fileOverride.value(part.name)
                                                          : part.absoluteFilePath;
        job.addr = part.startAddress;
        job.size = part.size;
        job.region = part.region;
        job.storage = part.storage;
        jobs.append(job);
    }

    m_busy.storeRelease(1);
    setBusy(true);
    ShellStatus::beginTask(QStringLiteral("下载选中"));
    ensureWorker();
    QMetaObject::invokeMethod(m_worker, "writeJobs", Qt::QueuedConnection,
                              Q_ARG(QVector<kc::mtk::MtkJob>, jobs));
}

void MtkFlashPage::readbackSelected()
{
    if (!m_scatter.valid) {
        error(QStringLiteral("请先加载 scatter 文件"));
        return;
    }
    if (m_connectedComPort.isEmpty()) {
        error(QStringLiteral("请先连接设备"));
        return;
    }
    if (!m_daReady) {
        error(QStringLiteral("DA 尚未就绪：读回分区同样需要 DA 高速通道。"));
        return;
    }
    if (m_busy.loadRelaxed() != 0) {
        error(QStringLiteral("有操作正在进行中"));
        return;
    }

    QVector<int> rows;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* checkItem = m_table->item(row, kColCheck);
        if (!checkItem || checkItem->checkState() != Qt::Checked) continue;
        if (row < m_scatter.partitions.size()) rows.append(row);
    }
    if (rows.isEmpty()) {
        error(QStringLiteral("没有勾选任何分区"));
        return;
    }

    const QString outDir = QDir(QFileInfo(m_scatterPath).absolutePath())
                               .absoluteFilePath(QStringLiteral("readback"));
    QDir().mkpath(outDir);

    QVector<kc::mtk::MtkJob> jobs;
    jobs.reserve(rows.size());
    for (int row : rows) {
        const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(row);
        kc::mtk::MtkJob job;
        job.row = row;
        job.name = part.name;
        job.filePath = QDir(outDir).absoluteFilePath(part.name + QStringLiteral(".img"));
        job.addr = part.startAddress;
        job.size = part.size;
        job.region = part.region;
        job.storage = part.storage;
        jobs.append(job);
    }
    logInfo(QStringLiteral("读回文件将保存到：%1").arg(outDir));

    m_busy.storeRelease(1);
    setBusy(true);
    ShellStatus::beginTask(QStringLiteral("读回选中"));
    ensureWorker();
    QMetaObject::invokeMethod(m_worker, "readJobs", Qt::QueuedConnection,
                              Q_ARG(QVector<kc::mtk::MtkJob>, jobs));
}

// 擦除 = DA 的 FORMAT。与写入最大的区别：**没有镜像可回滚**，
// 所以这里挡两道 —— ① 总量/清单确认（默认 No）；② 保护分区二次确认。
void MtkFlashPage::eraseSelected()
{
    if (!m_scatter.valid) {
        error(QStringLiteral("请先加载 scatter 文件"));
        return;
    }
    if (m_connectedComPort.isEmpty()) {
        error(QStringLiteral("请先连接设备"));
        return;
    }
    if (!m_daReady) {
        error(QStringLiteral("DA 尚未就绪：擦除同样需要 DA 高速通道。"));
        return;
    }
    if (m_busy.loadRelaxed() != 0) {
        error(QStringLiteral("有操作正在进行中"));
        return;
    }

    QVector<int> rows;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* checkItem = m_table->item(row, kColCheck);
        if (!checkItem || checkItem->checkState() != Qt::Checked) continue;
        if (row < m_scatter.partitions.size()) rows.append(row);
    }
    if (rows.isEmpty()) {
        error(QStringLiteral("没有勾选任何分区"));
        return;
    }

    QStringList names;
    quint64 total = 0;
    for (int row : rows) {
        const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(row);
        names << part.name;
        total += part.size;
    }

    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认擦除"),
        QStringLiteral("即将擦除 %1 个分区（共 %2）：\n\n%3\n\n"
                       "擦除会把分区内容清零/置为未定义，**没有镜像可以回滚**。\n"
                       "擦掉 preloader / boot / vbmeta 等关键分区可能导致设备无法开机。\n\n"
                       "确定继续吗？")
            .arg(rows.size())
            .arg(formatSize(static_cast<qint64>(total)))
            .arg(names.join(QStringLiteral(", "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        logInfo(QStringLiteral("已取消：擦除未确认"));
        return;
    }
    if (!confirmProtected(rows, QStringLiteral("擦除"))) return;

    QVector<kc::mtk::MtkJob> jobs;
    jobs.reserve(rows.size());
    for (int row : rows) {
        const kc::mtk::ScatterPartition& part = m_scatter.partitions.at(row);
        kc::mtk::MtkJob job;
        job.row = row;
        job.name = part.name;
        job.addr = part.startAddress;   // 擦除不需要镜像路径
        job.size = part.size;
        job.region = part.region;
        job.storage = part.storage;
        jobs.append(job);
    }

    m_busy.storeRelease(1);
    setBusy(true);
    ShellStatus::beginTask(QStringLiteral("擦除选中"));
    ensureWorker();
    QMetaObject::invokeMethod(m_worker, "formatJobs", Qt::QueuedConnection,
                              Q_ARG(QVector<kc::mtk::MtkJob>, jobs));
}

// ─────────────────────────────────────────────────────────────────────────────
//  杂项
// ─────────────────────────────────────────────────────────────────────────────
void MtkFlashPage::setAllChecks(bool checked)
{
    m_updatingTable = true;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* checkItem = m_table->item(row, kColCheck);
        if (!checkItem) continue;
        checkItem->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        if (row < m_scatter.partitions.size()) {
            m_scatter.partitions[row].enabled = checked;
        }
    }
    m_updatingTable = false;
}

void MtkFlashPage::setBusy(bool busy)
{
    for (QPushButton* button : m_busyButtons) {
        button->setEnabled(!busy);
    }
    m_disconnectButton->setEnabled(!busy && !m_connectedComPort.isEmpty());
    m_cancelButton->setEnabled(busy);
    if (!busy) updateActionButtons();
}

void MtkFlashPage::updateActionButtons()
{
    const bool hasScatter = m_scatter.valid;
    const bool connected = !m_connectedComPort.isEmpty();
    m_downloadButton->setEnabled(hasScatter && connected && m_daReady);
    m_readbackButton->setEnabled(hasScatter && connected && m_daReady);
    m_eraseButton->setEnabled(hasScatter && connected && m_daReady);
    m_reparseButton->setEnabled(!m_scatterPath.isEmpty());
    m_disconnectButton->setEnabled(connected);
}

QString MtkFlashPage::formatSize(qint64 bytes) const
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
}

QString MtkFlashPage::formatAddr(uint64_t addr) const
{
    return QStringLiteral("0x%1").arg(addr, 16, 16, QLatin1Char('0'));
}

} // namespace kc::ui
