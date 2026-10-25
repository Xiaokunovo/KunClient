#include "ui/MainWindow.h"

#include "app/EdlWorker.h"
#include "core/PartitionProtection.h"
#include "oplus/OplusVipSession.h"
#include "storage/RawProgramParser.h"
#include "storage/SparseImage.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScreen>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizeGrip>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QStyle>
#include <QWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <limits>
#include <utility>

namespace swt::edl {
namespace {

class WindowTitleBar final : public QFrame {
public:
    explicit WindowTitleBar(QMainWindow* window)
        : QFrame(window), m_window(window)
    {
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_window && m_window->windowHandle()) {
            m_window->windowHandle()->startSystemMove();
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_window) {
            m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
            event->accept();
            return;
        }
        QFrame::mouseDoubleClickEvent(event);
    }

private:
    QMainWindow* m_window = nullptr;
};

QWidget* makeProgressCell(QWidget* parent, QProgressBar** bar, QLabel** label,
                          const QString& initialText)
{
    auto* cell = new QWidget(parent);
    cell->setObjectName(QStringLiteral("progressCell"));
    auto* stack = new QGridLayout(cell);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);
    *bar = new QProgressBar(cell);
    (*bar)->setRange(0, 1000);
    (*bar)->setValue(0);
    (*bar)->setTextVisible(false);
    *label = new QLabel(initialText, cell);
    (*label)->setObjectName(QStringLiteral("progressOverlay"));
    (*label)->setAlignment(Qt::AlignCenter);
    (*label)->setAttribute(Qt::WA_TransparentForMouseEvents);
    stack->addWidget(*bar, 0, 0);
    stack->addWidget(*label, 0, 0);
    return cell;
}

enum class RawprogramXmlKind {
    Other,
    Standard,
    Nonstandard
};

struct RawprogramXmlName {
    RawprogramXmlKind kind = RawprogramXmlKind::Other;
    QString family;
    int index = -1;
};

RawprogramXmlName parseRawprogramXmlName(const QString& fileName)
{
    static const QRegularExpression standard(
        QStringLiteral("^(rawprogram)([0-9]+)\\.xml$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nonstandard(
        QStringLiteral("^(rawprogram_.+?)([0-9]+)\\.xml$"),
        QRegularExpression::CaseInsensitiveOption);
    auto match = standard.match(fileName);
    if (match.hasMatch()) {
        return {RawprogramXmlKind::Standard, match.captured(1).toLower(),
                match.captured(2).toInt()};
    }
    match = nonstandard.match(fileName);
    if (match.hasMatch()) {
        return {RawprogramXmlKind::Nonstandard, match.captured(1).toLower(),
                match.captured(2).toInt()};
    }
    return {};
}

QString rawprogramXmlFilters()
{
    return QStringLiteral(
        "标准 XML (rawprogram?.xml rawprogram??.xml rawprogram???.xml);;"
        "非标准 XML (rawprogram_*.xml)");
}

bool validateXmlCategory(const QStringList& paths, QString* error)
{
    bool standard = false;
    bool nonstandard = false;
    for (const QString& path : paths) {
        const RawprogramXmlKind kind = parseRawprogramXmlName(QFileInfo(path).fileName()).kind;
        standard = standard || kind == RawprogramXmlKind::Standard;
        nonstandard = nonstandard || kind == RawprogramXmlKind::Nonstandard;
    }
    if (standard && nonstandard) {
        if (error) {
            *error = QStringLiteral("标准 XML 与非标准 XML 属于两套刷写方案，请分开选择。");
        }
        return false;
    }
    return true;
}

QString xmlCategoryName(const QStringList& paths)
{
    for (const QString& path : paths) {
        const RawprogramXmlKind kind = parseRawprogramXmlName(QFileInfo(path).fileName()).kind;
        if (kind == RawprogramXmlKind::Standard) return QStringLiteral("标准 XML");
        if (kind == RawprogramXmlKind::Nonstandard) return QStringLiteral("非标准 XML");
    }
    return QStringLiteral("XML");
}

QStringList expandRawprogramXmlSelection(const QStringList& selectedPaths)
{
    QStringList paths;
    for (const QString& selected : selectedPaths) {
        const QFileInfo selectedInfo(selected);
        const QString absolutePath = selectedInfo.absoluteFilePath();
        if (!paths.contains(absolutePath, Qt::CaseInsensitive)) paths.push_back(absolutePath);
        const RawprogramXmlName selectedName = parseRawprogramXmlName(selectedInfo.fileName());
        if (selectedName.kind == RawprogramXmlKind::Other) continue;
        const QFileInfoList siblings = selectedInfo.dir().entryInfoList(
            {QStringLiteral("rawprogram*.xml")}, QDir::Files, QDir::Name);
        for (const QFileInfo& siblingInfo : siblings) {
            const RawprogramXmlName siblingName = parseRawprogramXmlName(siblingInfo.fileName());
            if (siblingName.kind != selectedName.kind ||
                siblingName.family.compare(selectedName.family, Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString sibling = siblingInfo.absoluteFilePath();
            if (!paths.contains(sibling, Qt::CaseInsensitive)) paths.push_back(sibling);
        }
    }
    std::sort(paths.begin(), paths.end(), [](const QString& left, const QString& right) {
        const RawprogramXmlName leftName = parseRawprogramXmlName(QFileInfo(left).fileName());
        const RawprogramXmlName rightName = parseRawprogramXmlName(QFileInfo(right).fileName());
        const int familyOrder = leftName.family.compare(rightName.family, Qt::CaseInsensitive);
        if (familyOrder != 0) return familyOrder < 0;
        if (leftName.index >= 0 && rightName.index >= 0 &&
            leftName.index != rightName.index) {
            return leftName.index < rightName.index;
        }
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return paths;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("QST · Qualcomm 9008 Tool"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1152, 748);
    setMinimumSize(1024, 700);

    auto* central = new QFrame(this);
    central->setObjectName(QStringLiteral("root"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    root->addWidget(buildHeader());

    auto* content = new QWidget(central);
    content->setObjectName(QStringLiteral("contentArea"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(10, 8, 10, 0);
    contentLayout->setSpacing(0);
    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildLeftCard());
    splitter->addWidget(buildRightCard());
    splitter->setHandleWidth(10);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({554, 565});
    contentLayout->addWidget(splitter);
    root->addWidget(content, 1);

    auto* footerFrame = new QFrame(central);
    footerFrame->setObjectName(QStringLiteral("footer"));
    footerFrame->setFixedHeight(33);
    auto* footer = new QHBoxLayout(footerFrame);
    footer->setContentsMargins(14, 0, 13, 0);
    auto* hint = new QLabel(QStringLiteral("遇到长时间无响应时，请耐心等待，避免重复操作。"), footerFrame);
    hint->setObjectName(QStringLiteral("footerWarning"));
    m_protocolStatus = new QLabel(QStringLiteral("协议通信状态：正常"), footerFrame);
    m_protocolStatus->setObjectName(QStringLiteral("protocolStatus"));
    m_bottomStatus = new QLabel(QStringLiteral("●  等待连接"), footerFrame);
    m_bottomStatus->setObjectName(QStringLiteral("footerStatus"));
    footer->addWidget(hint);
    footer->addStretch();
    footer->addWidget(m_protocolStatus);
    footer->addSpacing(40);
    footer->addWidget(m_bottomStatus);
    root->addWidget(footerFrame);

    setCentralWidget(central);

    m_autoScanTimer = new QTimer(this);
    m_autoScanTimer->setInterval(1500);
    connect(m_autoScanTimer, &QTimer::timeout, this, &MainWindow::refreshPorts);
    connect(m_autoScanCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("connection/autoScan"), checked);
        if (checked) {
            refreshPorts();
            m_autoScanTimer->start();
        } else {
            m_autoScanTimer->stop();
        }
    });

    m_progressAnimationTimer = new QTimer(this);
    m_progressAnimationTimer->setInterval(16);
    m_progressAnimationTimer->setTimerType(Qt::PreciseTimer);
    connect(m_progressAnimationTimer, &QTimer::timeout, this, [this] {
        auto advance = [](QProgressBar* bar, int target) {
            const int current = bar->value();
            const int delta = target - current;
            if (delta == 0) return true;
            if (delta < 0) {
                bar->setValue(target);
                return true;
            }
            const int step = qMax(1, (delta + 5) / 6);
            bar->setValue(qMin(target, current + step));
            return bar->value() == target;
        };
        const bool taskDone = advance(m_progressBar, m_taskProgressTarget);
        const bool overallDone = advance(m_totalProgressBar, m_overallProgressTarget);
        m_progressText->setText(QStringLiteral("当前任务  %1%")
                                    .arg(m_progressBar->value() / 10.0, 0, 'f', 1));
        m_totalProgressText->setText(QStringLiteral("总体进度  %1%")
                                         .arg(m_totalProgressBar->value() / 10.0,
                                              0, 'f', 1));
        if (taskDone && overallDone) m_progressAnimationTimer->stop();
    });

    QSettings settings;
    m_autoScanCheck->setChecked(
        settings.value(QStringLiteral("connection/autoScan"), true).toBool());
    m_autoRebootCheck->setChecked(
        settings.value(QStringLiteral("operation/autoReboot"), false).toBool());
    m_protectPartitionsCheck->setChecked(
        settings.value(QStringLiteral("operation/protectPartitions"), true).toBool());
    m_readInfoCheck->setChecked(
        settings.value(QStringLiteral("operation/readVersionInfo"), true).toBool());
    applyTheme(settings.value(QStringLiteral("appearance/darkTheme"), true).toBool());

    m_worker = new EdlWorker;
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &MainWindow::connectRequested, m_worker, &EdlWorker::connectDevice);
    connect(this, &MainWindow::disconnectRequested, m_worker, &EdlWorker::disconnectDevice);
    connect(this, &MainWindow::readGptRequested, m_worker, &EdlWorker::readPartitionTable);
    connect(this, &MainWindow::readVersionInfoRequested,
            m_worker, &EdlWorker::readSystemVersionInfo);
    connect(this, &MainWindow::backupPartitionsRequested, m_worker, &EdlWorker::backupPartitions);
    connect(this, &MainWindow::flashPartitionsRequested, m_worker, &EdlWorker::flashPartitions);
    connect(this, &MainWindow::flashXmlRequested, m_worker, &EdlWorker::flashXml);
    connect(this, &MainWindow::erasePartitionsRequested, m_worker, &EdlWorker::erasePartitions);
    connect(this, &MainWindow::rebootRequested, m_worker, &EdlWorker::rebootDevice);
    connect(this, &MainWindow::stopRequested, m_worker, &EdlWorker::requestStop,
            Qt::DirectConnection);

    connect(m_worker, &EdlWorker::logMessage, this, &MainWindow::appendLog,
            Qt::QueuedConnection);
    connect(m_worker, &EdlWorker::stateChanged, this, &MainWindow::updateState);
    connect(m_worker, &EdlWorker::progressChanged, this, &MainWindow::updateProgress);
    connect(m_worker, &EdlWorker::overallProgressChanged,
            this, &MainWindow::updateOverallProgress);
    connect(m_worker, &EdlWorker::deviceInfoChanged, this, &MainWindow::updateDeviceInfo);
    connect(m_worker, &EdlWorker::partitionsReady, this, &MainWindow::showPartitions);
    connect(m_worker, &EdlWorker::operationFinished, this, &MainWindow::operationFinished);
    m_workerThread.start();

    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        appendLog(QStringLiteral("正在停止当前任务…"));
        emit stopRequested();
    });

    refreshPorts();
    appendLog(QStringLiteral("QST EDL 初始化完成"));
    appendLog(QStringLiteral("请选择 Qualcomm 9008 串口和 Firehose Programmer"));
    setBusy(false);
}

MainWindow::~MainWindow()
{
    emit stopRequested();
    if (m_workerThread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "disconnectDevice", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait(5000);
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_programmerEdit && event->type() == QEvent::MouseButtonDblClick) {
        chooseProgrammer();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    const auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_DEVICECHANGE &&
        !m_connected && m_autoScanCheck && m_autoScanCheck->isChecked()) {
        QTimer::singleShot(120, this, &MainWindow::refreshPorts);
    }
    if (nativeMessage && nativeMessage->message == WM_NCHITTEST && !isMaximized()) {
        constexpr int border = 6;
        const QPoint cursor = mapFromGlobal(QCursor::pos());
        const bool left = cursor.x() >= 0 && cursor.x() < border;
        const bool right = cursor.x() <= width() && cursor.x() > width() - border;
        const bool top = cursor.y() >= 0 && cursor.y() < border;
        const bool bottom = cursor.y() <= height() && cursor.y() > height() - border;
        if (top && left) *result = HTTOPLEFT;
        else if (top && right) *result = HTTOPRIGHT;
        else if (bottom && left) *result = HTBOTTOMLEFT;
        else if (bottom && right) *result = HTBOTTOMRIGHT;
        else if (left) *result = HTLEFT;
        else if (right) *result = HTRIGHT;
        else if (top) *result = HTTOP;
        else if (bottom) *result = HTBOTTOM;
        else return QMainWindow::nativeEvent(eventType, message, result);
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

QWidget* MainWindow::buildHeader()
{
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("topArea"));
    auto* column = new QVBoxLayout(header);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto* titleBar = new WindowTitleBar(this);
    titleBar->setObjectName(QStringLiteral("titleBar"));
    titleBar->setFixedHeight(40);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(15, 0, 6, 0);
    titleLayout->setSpacing(8);

    auto* badge = new QLabel(QStringLiteral("Q"), titleBar);
    badge->setObjectName(QStringLiteral("appBadge"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(18, 18);
    auto* title = new QLabel(QStringLiteral("QST"), titleBar);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* minimize = new QPushButton(QStringLiteral("—"), titleBar);
    minimize->setObjectName(QStringLiteral("windowButton"));
    minimize->setFixedSize(42, 32);
    auto* close = new QPushButton(QStringLiteral("×"), titleBar);
    close->setObjectName(QStringLiteral("closeButton"));
    close->setFixedSize(42, 32);
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(close, &QPushButton::clicked, this, &QWidget::close);

    titleLayout->addWidget(badge);
    titleLayout->addWidget(title);
    titleLayout->addStretch();
    titleLayout->addWidget(minimize);
    titleLayout->addWidget(close);
    column->addWidget(titleBar);

    auto* navigation = new QFrame(header);
    navigation->setObjectName(QStringLiteral("navigation"));
    navigation->setFixedHeight(43);
    auto* navigationLayout = new QHBoxLayout(navigation);
    navigationLayout->setContentsMargins(25, 0, 15, 0);
    navigationLayout->setSpacing(26);
    auto* quick = new QPushButton(QStringLiteral("快速操作"), navigation);
    quick->setObjectName(QStringLiteral("navButton"));
    quick->setProperty("active", true);
    quick->setFixedHeight(42);
    auto* advanced = new QPushButton(QStringLiteral("高级功能"), navigation);
    advanced->setObjectName(QStringLiteral("navButton"));
    advanced->setFixedHeight(42);
    advanced->setEnabled(false);
    advanced->setToolTip(QStringLiteral("当前版本聚焦 EDL 功能"));
    auto* auxiliary = new QPushButton(QStringLiteral("辅助功能"), navigation);
    auxiliary->setObjectName(QStringLiteral("navButton"));
    auxiliary->setFixedHeight(42);
    auxiliary->setEnabled(false);
    auxiliary->setToolTip(QStringLiteral("当前版本聚焦 EDL 功能"));
    m_themeButton = new QPushButton(navigation);
    m_themeButton->setObjectName(QStringLiteral("themeButton"));
    m_themeButton->setCursor(Qt::PointingHandCursor);
    connect(m_themeButton, &QPushButton::clicked, this, [this] {
        applyTheme(!m_darkTheme);
    });
    navigationLayout->addWidget(quick);
    navigationLayout->addWidget(advanced);
    navigationLayout->addWidget(auxiliary);
    navigationLayout->addStretch();
    navigationLayout->addWidget(m_themeButton);
    column->addWidget(navigation);
    return header;
}

QWidget* MainWindow::buildLeftCard()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(8);

    auto* tabs = new QFrame(card);
    tabs->setObjectName(QStringLiteral("cardTabs"));
    tabs->setFixedHeight(40);
    auto* tabLayout = new QHBoxLayout(tabs);
    tabLayout->setContentsMargins(17, 0, 0, 0);
    tabLayout->setSpacing(20);
    auto* flashTab = new QPushButton(QStringLiteral("刷机界面"), tabs);
    flashTab->setObjectName(QStringLiteral("cardTab"));
    flashTab->setProperty("active", true);
    flashTab->setFixedHeight(40);
    auto* otherTab = new QPushButton(QStringLiteral("其他界面"), tabs);
    otherTab->setObjectName(QStringLiteral("cardTab"));
    otherTab->setFixedHeight(40);
    otherTab->setEnabled(false);
    tabLayout->addWidget(flashTab);
    tabLayout->addWidget(otherTab);
    tabLayout->addStretch();
    layout->addWidget(tabs);

    auto* body = new QWidget(card);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(10, 3, 10, 0);
    bodyLayout->setSpacing(8);

    auto* connectionRow = new QHBoxLayout;
    connectionRow->addWidget(new QLabel(QStringLiteral("串口"), card));
    m_portCombo = new QComboBox(card);
    m_portCombo->setMinimumWidth(120);
    connectionRow->addWidget(m_portCombo, 1);
    m_autoScanCheck = new QCheckBox(QStringLiteral("自动扫描"), card);
    connectionRow->addWidget(m_autoScanCheck);
    connectionRow->addSpacing(12);
    connectionRow->addWidget(new QLabel(QStringLiteral("存储"), card));
    m_storageCombo = new QComboBox(card);
    m_storageCombo->addItem(QStringLiteral("UFS"), static_cast<int>(StorageType::Ufs));
    m_storageCombo->addItem(QStringLiteral("eMMC"), static_cast<int>(StorageType::Emmc));
    m_storageCombo->setFixedWidth(82);
    connectionRow->addWidget(m_storageCombo);
    bodyLayout->addLayout(connectionRow);

    auto* tableHost = new QWidget(card);
    auto* tableStack = new QGridLayout(tableHost);
    tableStack->setContentsMargins(0, 0, 0, 0);
    m_partitionTable = new QTableWidget(0, 5, tableHost);
    m_partitionTable->setObjectName(QStringLiteral("partitionTable"));
    m_partitionTable->setHorizontalHeaderLabels({QString(), QStringLiteral("分区名"),
                                                   QStringLiteral("文件名"), QStringLiteral("大小"),
                                                   QStringLiteral("状态")});
    m_partitionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_partitionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_partitionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_partitionTable->setAlternatingRowColors(true);
    m_partitionTable->verticalHeader()->setVisible(false);
    m_partitionTable->horizontalHeader()->setStretchLastSection(true);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_partitionTable->setColumnWidth(0, 34);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_partitionTable->setMinimumHeight(260);
    tableStack->addWidget(m_partitionTable, 0, 0);
    m_emptyState = new QLabel(QStringLiteral("选择 rawprogram XML 后，这里会显示分区镜像\n\n连接设备后可双击分区选择独立镜像"), tableHost);
    m_emptyState->setObjectName(QStringLiteral("emptyState"));
    m_emptyState->setAlignment(Qt::AlignCenter);
    m_emptyState->setAttribute(Qt::WA_TransparentForMouseEvents);
    tableStack->addWidget(m_emptyState, 0, 0, Qt::AlignCenter);
    bodyLayout->addWidget(tableHost, 1);

    m_selectAllCheck = new QCheckBox(m_partitionTable->horizontalHeader());
    m_selectAllCheck->setObjectName(QStringLiteral("headerCheck"));
    m_selectAllCheck->setFixedSize(18, 18);
    m_selectAllCheck->move(9, 9);
    m_selectAllCheck->raise();
    m_checkedCountLabel = new QLabel(card);
    m_checkedCountLabel->hide();
    connect(m_selectAllCheck, &QCheckBox::toggled, this, [this](bool checked) {
        const QSignalBlocker blocker(m_partitionTable);
        for (int row = 0; row < m_partitionTable->rowCount(); ++row) {
            if (auto* item = m_partitionTable->item(row, 0)) {
                item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
            refreshPartitionRowStatus(row);
        }
        updateCheckedCount();
    });
    connect(m_partitionTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem* item) {
                if (item && item->column() == 0) {
                    refreshPartitionRowStatus(item->row());
                    updateCheckedCount();
                }
            });
    connect(m_partitionTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { choosePartitionImage(row); });

    auto* directoryRow = new QHBoxLayout;
    m_firmwareDirectoryEdit = new QLineEdit(card);
    m_firmwareDirectoryEdit->setReadOnly(true);
    m_firmwareDirectoryEdit->setPlaceholderText(QStringLiteral("选择 rawprogram XML..."));
    m_firmwareBrowseButton = new QPushButton(QStringLiteral("选择资料"), card);
    m_firmwareBrowseButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_firmwareBrowseButton, &QPushButton::clicked, this,
            &MainWindow::chooseFirmwareDirectory);
    directoryRow->addWidget(m_firmwareDirectoryEdit, 1);
    directoryRow->addWidget(m_firmwareBrowseButton);
    bodyLayout->addLayout(directoryRow);

    auto* programmerRow = new QHBoxLayout;
    m_programmerEdit = new QLineEdit(card);
    m_programmerEdit->setPlaceholderText(QStringLiteral("双击选择 firehose 文件..."));
    m_programmerEdit->installEventFilter(this);
    auto* modeLabel = new QLabel(QStringLiteral("模式"), card);
    modeLabel->setObjectName(QStringLiteral("strongLabel"));
    m_modeCombo = new QComboBox(card);
    m_modeCombo->addItem(QStringLiteral("EDL"));
    m_modeCombo->setFixedWidth(82);
    programmerRow->addWidget(m_programmerEdit, 1);
    programmerRow->addWidget(modeLabel);
    programmerRow->addWidget(m_modeCombo);
    bodyLayout->addLayout(programmerRow);

    auto* chipsetRow = new QHBoxLayout;
    m_chipsetCombo = new QComboBox(card);
    m_chipsetCombo->addItem(QStringLiteral("官方高通骁龙芯片（Qualcomm EDL）"));
    m_connectButton = new QPushButton(QStringLiteral("查找分区"), card);
    m_connectButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    chipsetRow->addWidget(m_chipsetCombo, 1);
    chipsetRow->addWidget(m_connectButton);
    bodyLayout->addLayout(chipsetRow);

    m_vipDirectoryEdit = new QLineEdit(card);
    m_vipDirectoryEdit->hide();
    m_vipBrowseButton = new QPushButton(card);
    m_vipBrowseButton->hide();
    connect(m_vipBrowseButton, &QPushButton::clicked, this,
            &MainWindow::chooseVipPackage);

    auto* optionsRow = new QHBoxLayout;
    optionsRow->setSpacing(10);
    m_autoRebootCheck = new QCheckBox(QStringLiteral("自动重启"), card);
    m_autoRebootCheck->setToolTip(QStringLiteral("读取或写入任务完成后自动重启到系统"));
    connect(m_autoRebootCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("operation/autoReboot"), checked);
    });
    m_vipCheck = new QCheckBox(QStringLiteral("OPPO VIP"), card);
    m_vipCheck->setToolTip(QStringLiteral("执行 Digest → Verify → Sign → SHA-256 → Configure 序列"));
    m_generateXmlCheck = new QCheckBox(QStringLiteral("生成 XML"), card);
    m_generateXmlCheck->setChecked(
        QSettings().value(QStringLiteral("backup/generateXml"), true).toBool());
    m_generateXmlCheck->setToolTip(
        QStringLiteral("回读所选分区后，按 LUN 生成可再次刷写的 rawprogram XML"));
    connect(m_generateXmlCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("backup/generateXml"), checked);
    });
    m_protectPartitionsCheck = new QCheckBox(QStringLiteral("保护分区"), card);
    m_protectPartitionsCheck->setToolTip(
        QStringLiteral("保护校准、基带指纹及 OPLUS NV 分区；写入和擦除时默认跳过"));
    connect(m_protectPartitionsCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("operation/protectPartitions"), checked);
        for (int row = 0; row < m_partitions.size(); ++row) refreshPartitionRowStatus(row);
    });
    m_readInfoCheck = new QCheckBox(QStringLiteral("读取信息"), card);
    m_readInfoCheck->setToolTip(QStringLiteral("读取 GPT 后自动解析系统版本、Android 版本与安全补丁"));
    connect(m_readInfoCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("operation/readVersionInfo"), checked);
    });
    optionsRow->addWidget(m_autoRebootCheck);
    optionsRow->addWidget(m_vipCheck);
    optionsRow->addWidget(m_generateXmlCheck);
    optionsRow->addWidget(m_protectPartitionsCheck);
    optionsRow->addWidget(m_readInfoCheck);
    optionsRow->addStretch();
    bodyLayout->addLayout(optionsRow);

    connect(m_vipCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked || m_busy || m_connected) return;
        if (m_vipDirectoryEdit->text().isEmpty()) {
            const QFileInfo programmer(m_programmerEdit->text());
            const QString candidate = programmer.isFile() ? programmer.absolutePath()
                : (!m_selectedXmlPaths.isEmpty()
                       ? QFileInfo(m_selectedXmlPaths.front()).absolutePath()
                       : QFileInfo(m_firmwareDirectoryEdit->text()).absolutePath());
            if (!candidate.isEmpty()) m_vipDirectoryEdit->setText(candidate);
        }
        if (!m_vipDirectoryEdit->text().isEmpty() && loadVipPackage(false)) return;
        QTimer::singleShot(0, this, &MainWindow::chooseVipPackage);
    });
    connect(m_storageCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_vipCheck->isChecked() && !m_vipDirectoryEdit->text().isEmpty()) {
            loadVipPackage(false);
        }
    });

    auto* actionRow = new QHBoxLayout;
    m_readGptButton = new QPushButton(QStringLiteral("读取分区表"), card);
    m_readVersionButton = new QPushButton(QStringLiteral("读取版本"), card);
    m_backupButton = new QPushButton(QStringLiteral("读取分区"), card);
    m_eraseButton = new QPushButton(QStringLiteral("擦除分区"), card);
    m_flashButton = new QPushButton(QStringLiteral("写入分区"), card);
    m_xmlFlashButton = m_flashButton;
    m_flashButton->setObjectName(QStringLiteral("primaryButton"));
    connect(m_readGptButton, &QPushButton::clicked, this, [this] {
        if (m_connected) {
            setBusy(true);
            emit readGptRequested();
        } else {
            toggleConnection();
        }
    });
    connect(m_readVersionButton, &QPushButton::clicked, this, [this] {
        if (!m_connected) {
            m_readVersionAfterGpt = true;
            toggleConnection();
        } else if (m_partitions.isEmpty()) {
            m_readVersionAfterGpt = true;
            setBusy(true);
            emit readGptRequested();
        } else {
            setBusy(true);
            emit readVersionInfoRequested();
        }
    });
    connect(m_backupButton, &QPushButton::clicked, this, &MainWindow::chooseBackup);
    connect(m_flashButton, &QPushButton::clicked, this, [this] {
        if (checkedPartitions().isEmpty()) chooseXmlAndFlash();
        else chooseImageAndFlash();
    });
    connect(m_eraseButton, &QPushButton::clicked, this, &MainWindow::confirmErase);
    actionRow->addWidget(m_readGptButton);
    actionRow->addWidget(m_readVersionButton);
    actionRow->addWidget(m_backupButton);
    actionRow->addWidget(m_eraseButton);
    actionRow->addWidget(m_flashButton);
    bodyLayout->addLayout(actionRow);
    bodyLayout->addSpacing(28);
    layout->addWidget(body, 1);
    return card;
}

QWidget* MainWindow::makeInfoCell(const QString& title, QLabel** valueLabel)
{
    auto* cell = new QWidget;
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* key = new QLabel(title, cell);
    key->setObjectName(QStringLiteral("infoKey"));
    auto* value = new QLabel(QStringLiteral("—"), cell);
    value->setObjectName(QStringLiteral("infoValue"));
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(key);
    layout->addWidget(value);
    *valueLabel = value;
    return cell;
}

QWidget* MainWindow::buildRightCard()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(0);

    auto* logHeaderWidget = new QFrame(card);
    logHeaderWidget->setObjectName(QStringLiteral("sectionHeader"));
    logHeaderWidget->setFixedHeight(40);
    auto* logHeader = new QHBoxLayout(logHeaderWidget);
    logHeader->setContentsMargins(14, 0, 11, 0);
    auto* logTitle = new QLabel(QStringLiteral("操作日志"), logHeaderWidget);
    logTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_debugLogButton = new QPushButton(QStringLiteral("Debug"), logHeaderWidget);
    m_debugLogButton->setObjectName(QStringLiteral("debugButton"));
    m_debugLogButton->setCheckable(true);
    m_debugLogButton->setToolTip(QStringLiteral("开启后显示 Firehose 原始帧、命令及身份重试等完整日志"));
    connect(m_debugLogButton, &QPushButton::toggled, this, [this](bool checked) {
        m_debugLogEnabled = checked;
        m_debugLogButton->setText(checked ? QStringLiteral("Debug：完整") : QStringLiteral("Debug"));
        rebuildLogView();
    });
    auto* clear = new QPushButton(QStringLiteral("清空"), logHeaderWidget);
    clear->setObjectName(QStringLiteral("secondaryButton"));
    connect(clear, &QPushButton::clicked, this, [this] {
        m_logHistory.clear();
        m_logView->clear();
    });
    logHeader->addWidget(logTitle);
    logHeader->addStretch();
    logHeader->addWidget(m_debugLogButton);
    logHeader->addWidget(clear);
    layout->addWidget(logHeaderWidget);

    m_logView = new QTextEdit(card);
    m_logView->setReadOnly(true);
    m_logView->setUndoRedoEnabled(false);
    m_logView->document()->setMaximumBlockCount(50000);
    m_logView->setObjectName(QStringLiteral("logView"));
    m_logView->setMinimumHeight(255);
    m_logView->setMaximumHeight(280);
    layout->addWidget(m_logView, 1);

    auto* infoHeader = new QFrame(card);
    infoHeader->setObjectName(QStringLiteral("sectionHeader"));
    infoHeader->setFixedHeight(43);
    auto* infoHeaderLayout = new QHBoxLayout(infoHeader);
    infoHeaderLayout->setContentsMargins(14, 0, 0, 0);
    auto* infoTitle = new QLabel(QStringLiteral("设备信息"), infoHeader);
    infoTitle->setObjectName(QStringLiteral("sectionTitle"));
    infoHeaderLayout->addWidget(infoTitle);
    infoHeaderLayout->addStretch();
    layout->addWidget(infoHeader);

    auto* infoBody = new QWidget(card);
    infoBody->setMinimumHeight(168);
    auto* infoGrid = new QGridLayout(infoBody);
    infoGrid->setContentsMargins(14, 9, 14, 9);
    infoGrid->setHorizontalSpacing(38);
    infoGrid->setVerticalSpacing(8);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("品牌"), &m_brandValue), 0, 0);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("芯片"), &m_modelValue), 0, 1);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("CPUID"), &m_cpuidValue), 0, 2);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("用户区大小"), &m_storageValue), 1, 0);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("当前模式"), &m_modeValue), 1, 1);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("存储型号"), &m_systemValue), 1, 2);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("设备名称"), &m_productNameValue), 2, 0);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("Android"), &m_androidValue), 2, 1);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("系统版本"), &m_versionValue), 2, 2);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("安全补丁"), &m_securityPatchValue), 3, 0);
    infoGrid->setColumnStretch(0, 1);
    infoGrid->setColumnStretch(1, 1);
    infoGrid->setColumnStretch(2, 1);
    layout->addWidget(infoBody);

    m_portValue = new QLabel(card);
    m_portValue->hide();
    m_programmerValue = new QLabel(card);
    m_programmerValue->hide();
    m_systemValue->setText(QStringLiteral("—"));

    m_stateBanner = new QLabel(QStringLiteral("连接设备后即可开始操作"), card);
    m_stateBanner->setObjectName(QStringLiteral("stateBanner"));
    m_stateBanner->setFixedHeight(36);
    m_stateBanner->setContentsMargins(12, 0, 12, 0);
    layout->addWidget(m_stateBanner);

    auto* progressArea = new QWidget(card);
    auto* progressLayout = new QGridLayout(progressArea);
    progressLayout->setContentsMargins(0, 9, 12, 0);
    progressLayout->setHorizontalSpacing(9);
    progressLayout->setVerticalSpacing(8);
    auto* taskProgress = makeProgressCell(progressArea, &m_progressBar, &m_progressText,
                                           QStringLiteral("当前任务  0.0%"));
    auto* totalProgress = makeProgressCell(progressArea, &m_totalProgressBar,
                                            &m_totalProgressText,
                                            QStringLiteral("总体进度  0.0%"));
    m_progressBar->setObjectName(QStringLiteral("taskProgressBar"));
    m_totalProgressBar->setObjectName(QStringLiteral("overallProgressBar"));
    m_speedText = new QLabel(QStringLiteral("速率  0.00 MB/s"), card);
    m_speedText->setObjectName(QStringLiteral("speedText"));
    m_speedText->setMinimumWidth(116);
    m_speedText->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_stopButton = new QPushButton(QStringLiteral("■  停止"), card);
    m_stopButton->setObjectName(QStringLiteral("dangerButton"));

    m_rebootButton = new QPushButton(QStringLiteral("重启设备  ▾"), card);
    m_rebootButton->setObjectName(QStringLiteral("secondaryButton"));
    m_rebootMenu = new QMenu(m_rebootButton);
    m_rebootMenu->setObjectName(QStringLiteral("rebootMenu"));
    QAction* rebootSystem = m_rebootMenu->addAction(QStringLiteral("重启到系统"));
    QAction* rebootEdl = m_rebootMenu->addAction(QStringLiteral("重启到 9008"));
    connect(rebootSystem, &QAction::triggered, this, [this] { confirmReboot(false); });
    connect(rebootEdl, &QAction::triggered, this, [this] { confirmReboot(true); });
    connect(m_rebootButton, &QPushButton::clicked, this, [this] {
        if (!m_rebootMenu) return;
        const QSize menuSize = m_rebootMenu->sizeHint();
        const QPoint below = m_rebootButton->mapToGlobal(
            QPoint(0, m_rebootButton->height() + 4));
        const QPoint above = m_rebootButton->mapToGlobal(
            QPoint(0, -menuSize.height() - 4));
        const QRect available = m_rebootButton->screen()->availableGeometry();
        m_rebootMenu->popup(available.contains(
                                QRect(below, menuSize)) ? below : above);
    });
    progressLayout->addWidget(taskProgress, 0, 0, 1, 2);
    progressLayout->addWidget(m_speedText, 0, 2, 1, 2);
    progressLayout->addWidget(totalProgress, 1, 0, 1, 2);
    progressLayout->addWidget(m_rebootButton, 1, 2);
    progressLayout->addWidget(m_stopButton, 1, 3);
    progressLayout->setColumnStretch(0, 1);
    progressLayout->setColumnStretch(1, 1);
    layout->addWidget(progressArea);
    return card;
}

void MainWindow::refreshPorts()
{
    if (m_connected) return;
    const auto ports = QSerialPortInfo::availablePorts();
    QStringList snapshotParts;
    for (const QSerialPortInfo& port : ports) {
        snapshotParts.push_back(QStringLiteral("%1|%2|%3|%4")
            .arg(port.portName(), port.description())
            .arg(port.hasVendorIdentifier() ? port.vendorIdentifier() : 0, 4, 16, QLatin1Char('0'))
            .arg(port.hasProductIdentifier() ? port.productIdentifier() : 0, 4, 16, QLatin1Char('0')));
    }
    const QString snapshot = snapshotParts.join(QLatin1Char(';'));
    if (snapshot == m_lastPortSnapshot && m_portCombo->count() > 0) return;
    m_lastPortSnapshot = snapshot;

    const QString previous = m_portCombo->currentData().toString();
    m_portCombo->clear();
    int preferred = -1;
    for (const QSerialPortInfo& port : ports) {
        const QString details = QStringLiteral("%1  ·  %2")
                                    .arg(port.portName(), port.description().isEmpty()
                                                             ? QStringLiteral("串口设备")
                                                             : port.description());
        m_portCombo->addItem(details, port.portName());
        const QString fingerprint = port.description() + QLatin1Char(' ') + port.manufacturer();
        const bool qualcommUsb = port.hasVendorIdentifier() &&
                                 port.vendorIdentifier() == 0x05c6;
        if (preferred < 0 && (qualcommUsb ||
                              fingerprint.contains(QStringLiteral("9008"), Qt::CaseInsensitive) ||
                              fingerprint.contains(QStringLiteral("Qualcomm"), Qt::CaseInsensitive) ||
                              fingerprint.contains(QStringLiteral("QDLoader"), Qt::CaseInsensitive))) {
            preferred = m_portCombo->count() - 1;
        }
        if (port.portName() == previous) preferred = m_portCombo->count() - 1;
    }
    if (preferred >= 0) m_portCombo->setCurrentIndex(preferred);
    if (ports.isEmpty()) m_portCombo->addItem(QStringLiteral("未发现串口"), QString());
    if (!m_busy && !m_connected && m_readGptButton) {
        m_readGptButton->setEnabled(!m_portCombo->currentData().toString().isEmpty());
    }
    appendLog(QStringLiteral("扫描完成，发现 %1 个串口").arg(ports.size()));
}

void MainWindow::chooseProgrammer()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 Firehose Programmer"), {},
        QStringLiteral("Firehose Programmer (*.elf *.melf *.mbn *.bin);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    m_programmerEdit->setText(path);

    OplusVipPackage package;
    QString error;
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    const QString directory = QFileInfo(path).absolutePath();
    // A file chosen from Desktop or a firmware root may have hundreds of
    // thousands of descendants. Only inspect its sibling files here; explicit
    // package-directory selection remains the recursive discovery entry point.
    if (OplusVipSession::discoverPackage(directory, storage, &package, &error,
                                         false, path)) {
        m_vipDirectoryEdit->setText(directory);
        m_vipPackage = package;
        m_programmerEdit->setText(package.programmerPath);
        m_vipDirectoryEdit->setToolTip(
            QStringLiteral("Programmer: %1\nDigest: %2\nSign: %3\n候选签名数: %4")
                .arg(QFileInfo(package.programmerPath).fileName(),
                     QFileInfo(package.digestPath).fileName(),
                     QFileInfo(package.signaturePath).fileName())
                .arg(package.programmerCandidates.size()));
        {
            // Avoid the toggled handler recursively scanning the same directory
            // while chooseProgrammer() is still on the GUI thread.
            const QSignalBlocker blocker(m_vipCheck);
            m_vipCheck->setChecked(true);
        }
        appendLog(QStringLiteral("同目录已识别为 OPPO VIP 引导包：%1 / %2 / %3（%4 个签名候选）")
                      .arg(QFileInfo(package.programmerPath).fileName(),
                           QFileInfo(package.digestPath).fileName(),
                           QFileInfo(package.signaturePath).fileName())
                      .arg(package.programmerCandidates.size()));
    } else {
        // The newly selected file is a standard standalone programmer. Drop a
        // previously cached VIP package so Connect uses exactly this file.
        m_vipPackage = {};
        m_vipDirectoryEdit->clear();
        m_vipDirectoryEdit->setToolTip({});
        const QSignalBlocker blocker(m_vipCheck);
        m_vipCheck->setChecked(false);
    }
}

void MainWindow::chooseFirmwareDirectory()
{
    QString initialPath;
    if (!m_selectedXmlPaths.isEmpty()) {
        initialPath = QFileInfo(m_selectedXmlPaths.front()).absolutePath();
    } else if (!m_firmwareDirectoryEdit->text().isEmpty()) {
        const QFileInfo current(m_firmwareDirectoryEdit->text());
        initialPath = current.isDir() ? current.absoluteFilePath() : current.absolutePath();
    }
    const QStringList selectedPaths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择刷机 XML"), initialPath, rawprogramXmlFilters());
    if (selectedPaths.isEmpty()) return;

    const QStringList xmlPaths = expandRawprogramXmlSelection(selectedPaths);
    QString categoryError;
    if (!validateXmlCategory(xmlPaths, &categoryError)) {
        QMessageBox::warning(this, QStringLiteral("XML 类型"), categoryError);
        return;
    }
    RawProgramPlan plan;
    QString error;
    if (!RawProgramParser::parse(xmlPaths, &plan, &error)) {
        QMessageBox::warning(this, QStringLiteral("XML 资料"), error);
        appendLog(QStringLiteral("XML 资料解析失败：%1").arg(error));
        return;
    }

    m_selectedXmlPaths = plan.xmlPaths;
    const QFileInfo firstXml(m_selectedXmlPaths.front());
    const QString category = xmlCategoryName(m_selectedXmlPaths);
    const QString display = m_selectedXmlPaths.size() == 1
        ? QDir::toNativeSeparators(firstXml.absoluteFilePath())
        : QStringLiteral("[%1] %2  +%3").arg(category, firstXml.fileName())
              .arg(m_selectedXmlPaths.size() - 1);
    m_firmwareDirectoryEdit->setText(display);
    m_firmwareDirectoryEdit->setToolTip(
        m_selectedXmlPaths.join(QLatin1Char('\n')));

    if (!m_connected) {
        const QSignalBlocker blocker(m_partitionTable);
        m_partitionTable->setRowCount(plan.entries.size());
        for (int row = 0; row < plan.entries.size(); ++row) {
            const RawProgramEntry& entry = plan.entries.at(row);
            const QFileInfo image(entry.sourcePath);
            const QStringList values{
                QString(),
                entry.label.isEmpty() ? QStringLiteral("—") : entry.label,
                image.fileName().isEmpty() ? entry.filename : image.fileName(),
                image.isFile() ? formatBytes(static_cast<quint64>(image.size()))
                               : QStringLiteral("—"),
                entry.dynamicSuper ? QStringLiteral("META 免合并")
                                   : (image.isFile() ? QStringLiteral("就绪")
                                                     : QStringLiteral("镜像缺失，跳过"))
            };
            for (int column = 0; column < values.size(); ++column) {
                auto* item = new QTableWidgetItem(values.at(column));
                item->setFlags(item->flags() & ~Qt::ItemIsEditable &
                               ~Qt::ItemIsUserCheckable);
                item->setToolTip(entry.sourcePath);
                m_partitionTable->setItem(row, column, item);
            }
        }
        m_emptyState->hide();
    }
    appendLog(QStringLiteral("%1 资料已载入：%2 个 XML，%3 个可刷写条目")
                  .arg(category).arg(plan.xmlPaths.size()).arg(plan.entries.size()));
    for (const QString& warning : plan.warnings) appendLog(warning);

    OplusVipPackage package;
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    QString packageDirectory = firstXml.absolutePath();
    if (firstXml.dir().dirName().compare(QStringLiteral("IMAGES"), Qt::CaseInsensitive) == 0) {
        packageDirectory = QDir(packageDirectory).absoluteFilePath(QStringLiteral(".."));
    }
    if (!OplusVipSession::discoverPackage(packageDirectory, storage, &package, &error) &&
        packageDirectory.compare(firstXml.absolutePath(), Qt::CaseInsensitive) != 0) {
        OplusVipSession::discoverPackage(firstXml.absolutePath(), storage, &package, &error);
    }
    if (package.enabled) {
        m_vipDirectoryEdit->setText(package.packageDirectory);
        m_vipPackage = package;
        m_programmerEdit->setText(package.programmerPath);
        m_vipCheck->setChecked(true);
    }
}

void MainWindow::chooseVipPackage()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 OPPO VIP 引导包目录"), m_vipDirectoryEdit->text());
    if (directory.isEmpty()) return;
    m_vipDirectoryEdit->setText(directory);
    loadVipPackage(true);
}

bool MainWindow::loadVipPackage(bool showDialog)
{
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    QString error;
    OplusVipPackage package;
    if (!OplusVipSession::discoverPackage(m_vipDirectoryEdit->text(), storage, &package, &error)) {
        m_vipPackage = {};
        if (showDialog) QMessageBox::warning(this, QStringLiteral("VIP 引导包"), error);
        else appendLog(QStringLiteral("VIP 引导包识别：%1").arg(error));
        return false;
    }
    m_vipPackage = package;
    m_programmerEdit->setText(package.programmerPath);
    m_vipDirectoryEdit->setToolTip(QStringLiteral("Programmer: %1\nDigest: %2\nSign: %3\n候选签名数: %4")
        .arg(QFileInfo(package.programmerPath).fileName(), QFileInfo(package.digestPath).fileName(),
             QFileInfo(package.signaturePath).fileName())
        .arg(package.programmerCandidates.size()));
    appendLog(QStringLiteral("VIP 引导包已识别：%1 / %2 / %3（%4 个同载荷签名候选）")
        .arg(QFileInfo(package.programmerPath).fileName(), QFileInfo(package.digestPath).fileName(),
             QFileInfo(package.signaturePath).fileName())
        .arg(package.programmerCandidates.size()));
    return true;
}

void MainWindow::toggleConnection()
{
    if (m_connected) {
        m_readGptAfterConnect = false;
        setBusy(true);
        emit disconnectRequested();
        return;
    }
    const QString port = m_portCombo->currentData().toString();
    QString programmer = m_programmerEdit->text().trimmed();
    if (port.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("连接参数"),
                                 QStringLiteral("请选择有效的 Qualcomm 9008 串口。"));
        return;
    }
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    OplusVipPackage vipPackage;
    if (m_vipCheck->isChecked()) {
        if (!loadVipPackage(true)) return;
        vipPackage = m_vipPackage;
        programmer = vipPackage.programmerPath;
    }
    m_readGptAfterConnect = true;
    setBusy(true);
    emit connectRequested(port, programmer, storage, vipPackage);
}

PartitionInfo MainWindow::selectedPartition(bool* ok) const
{
    const int row = m_partitionTable->currentRow();
    const bool valid = row >= 0 && row < m_partitions.size();
    if (ok) *ok = valid;
    return valid ? m_partitions.at(row) : PartitionInfo{};
}

QVector<PartitionInfo> MainWindow::checkedPartitions() const
{
    QVector<PartitionInfo> selected;
    for (int row = 0; row < m_partitionTable->rowCount() && row < m_partitions.size(); ++row) {
        const QTableWidgetItem* item = m_partitionTable->item(row, 0);
        if (item && item->checkState() == Qt::Checked) selected.push_back(m_partitions.at(row));
    }
    return selected;
}

void MainWindow::updateCheckedCount()
{
    const int count = checkedPartitions().size();
    if (m_checkedCountLabel) {
        m_checkedCountLabel->setText(QStringLiteral("已勾选 %1 个分区").arg(count));
    }
    if (m_selectAllCheck) {
        const QSignalBlocker blocker(m_selectAllCheck);
        m_selectAllCheck->setChecked(count > 0 && count == m_partitionTable->rowCount());
    }
}

void MainWindow::refreshPartitionRowStatus(int row)
{
    if (row < 0 || row >= m_partitions.size()) return;
    QTableWidgetItem* statusItem = m_partitionTable->item(row, 4);
    const QTableWidgetItem* checkItem = m_partitionTable->item(row, 0);
    if (!statusItem || !checkItem) return;
    const bool checked = checkItem->checkState() == Qt::Checked;
    const bool imageSelected = row < m_partitionImagePaths.size() &&
        QFileInfo(m_partitionImagePaths.at(row)).isFile();
    const QString& name = m_partitions.at(row).name;
    const bool guarded = isProtectedPartitionName(name) &&
        m_protectPartitionsCheck && m_protectPartitionsCheck->isChecked();

    if (imageSelected) {
        statusItem->setText(QStringLiteral("镜像已选择"));
        statusItem->setForeground(QColor(
            m_darkTheme ? QStringLiteral("#3ddc97") : QStringLiteral("#138451")));
    } else if (checked) {
        statusItem->setText(QStringLiteral("双击选择镜像"));
        statusItem->setForeground(QColor(
            m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09")));
    } else {
        statusItem->setText(guarded ? QStringLiteral("保护已开启") : QStringLiteral("就绪"));
        statusItem->setForeground(QColor(guarded
            ? (m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09"))
            : (m_darkTheme ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))));
    }
}

bool MainWindow::applyProtectionPolicy(QVector<PartitionInfo>* partitions,
                                       QStringList* imagePaths,
                                       const QString& operation,
                                       bool* allowProtected)
{
    if (!partitions || !allowProtected) return false;
    const bool enabled = m_protectPartitionsCheck &&
                         m_protectPartitionsCheck->isChecked();
    *allowProtected = !enabled;
    if (!enabled) return !partitions->isEmpty();

    QStringList protectedNames;
    for (const PartitionInfo& partition : std::as_const(*partitions)) {
        if (isProtectedPartitionName(partition.name) &&
            !protectedNames.contains(partition.name, Qt::CaseInsensitive)) {
            protectedNames.push_back(partition.name);
        }
    }
    if (protectedNames.isEmpty()) return !partitions->isEmpty();

    QMessageBox confirmation(
        QMessageBox::Warning, QStringLiteral("保护分区确认"),
        QStringLiteral("%1任务包含以下校准、基带指纹或设备配置分区：\n\n%2\n\n"
                       "默认动作会跳过这些分区，其余分区继续执行。")
            .arg(operation, protectedNames.join(QStringLiteral("、"))),
        QMessageBox::NoButton, this);
    QPushButton* continueButton = confirmation.addButton(
        QStringLiteral("继续操作保护分区"), QMessageBox::AcceptRole);
    QPushButton* skipButton = confirmation.addButton(
        QStringLiteral("跳过保护分区"), QMessageBox::RejectRole);
    confirmation.setDefaultButton(skipButton);
    confirmation.exec();
    if (confirmation.clickedButton() == continueButton) {
        *allowProtected = true;
        appendLog(QStringLiteral("保护分区二次确认已通过：%1").arg(
            protectedNames.join(QStringLiteral("、"))));
        return true;
    }

    for (qsizetype index = partitions->size(); index > 0; --index) {
        if (!isProtectedPartitionName(partitions->at(index - 1).name)) continue;
        partitions->removeAt(index - 1);
        if (imagePaths && index - 1 < imagePaths->size()) imagePaths->removeAt(index - 1);
    }
    appendLog(QStringLiteral("保护分区已跳过：%1").arg(
        protectedNames.join(QStringLiteral("、"))));
    return !partitions->isEmpty();
}

void MainWindow::chooseBackup()
{
    const QVector<PartitionInfo> partitions = checkedPartitions();
    if (partitions.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("读取分区"),
                                 QStringLiteral("请先勾选需要读取的分区。"));
        return;
    }
    QSettings settings;
    const QString initialParent = settings.value(
        QStringLiteral("backup/parentDirectory"), QDir::homePath()).toString();
    const QString parentDirectory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 QST 回读文件夹的保存位置"), initialParent,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (parentDirectory.isEmpty()) return;
    settings.setValue(QStringLiteral("backup/parentDirectory"), parentDirectory);

    QDir parent(parentDirectory);
    const QString baseName = QStringLiteral("QST-回读-%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")));
    QString folderName = baseName;
    for (int suffix = 1; parent.exists(folderName); ++suffix) {
        folderName = QStringLiteral("%1-%2").arg(baseName).arg(suffix, 2, 10, QLatin1Char('0'));
    }
    if (!parent.mkpath(folderName)) {
        QMessageBox::warning(this, QStringLiteral("回读保存目录"),
                             QStringLiteral("创建回读文件夹失败：%1")
                                 .arg(parent.filePath(folderName)));
        return;
    }
    const QString directory = QDir::toNativeSeparators(
        QFileInfo(parent.filePath(folderName)).absoluteFilePath());
    appendLog(QStringLiteral("回读保存目录：%1").arg(directory));
    setBusy(true);
    emit backupPartitionsRequested(partitions, directory,
                                   m_generateXmlCheck && m_generateXmlCheck->isChecked());
}

void MainWindow::choosePartitionImage(int row)
{
    if (m_busy || !m_connected || row < 0 || row >= m_partitions.size()) return;
    const QTableWidgetItem* checkItem = m_partitionTable->item(row, 0);
    if (!checkItem || checkItem->checkState() != Qt::Checked) {
        QMessageBox::information(this, QStringLiteral("选择镜像"),
                                 QStringLiteral("请先勾选该分区，再双击选择刷写镜像。"));
        return;
    }
    const PartitionInfo& partition = m_partitions.at(row);
    const QString currentPath = row < m_partitionImagePaths.size()
        ? m_partitionImagePaths.at(row) : QString();
    const QString initialPath = currentPath.isEmpty()
        ? QString() : QFileInfo(currentPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("为分区 %1 选择镜像").arg(partition.name), initialPath,
        QStringLiteral("磁盘镜像 (*.img *.bin *.raw);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    QFile image(path);
    if (!image.open(QIODevice::ReadOnly) || image.size() <= 0) {
        QMessageBox::warning(this, QStringLiteral("选择镜像"),
                             QStringLiteral("镜像文件读取失败：%1").arg(image.errorString()));
        return;
    }
    quint64 payloadBytes = static_cast<quint64>(image.size());
    if (SparseImage::hasSparseMagic(image)) {
        SparseImageInfo sparseInfo;
        QString sparseError;
        if (!SparseImage::inspect(image, &sparseInfo, &sparseError)) {
            QMessageBox::warning(this, QStringLiteral("选择镜像"), sparseError);
            return;
        }
        payloadBytes = sparseInfo.expandedSize();
    }
    const quint64 bytesPerSector =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs)
            ? 4096ULL : 512ULL;
    const quint64 capacity = partition.sectorCount() >
            std::numeric_limits<quint64>::max() / bytesPerSector
        ? std::numeric_limits<quint64>::max()
        : partition.sectorCount() * bytesPerSector;
    if (payloadBytes > capacity) {
        QMessageBox::warning(
            this, QStringLiteral("镜像过大"),
            QStringLiteral("%1 展开后为 %2，超过分区 %3 的容量 %4。")
                .arg(QFileInfo(path).fileName(), formatBytes(payloadBytes),
                     partition.name, formatBytes(capacity)));
        return;
    }

    while (m_partitionImagePaths.size() < m_partitions.size()) {
        m_partitionImagePaths.push_back(QString());
    }
    m_partitionImagePaths[row] = QFileInfo(path).absoluteFilePath();
    if (auto* fileItem = m_partitionTable->item(row, 2)) {
        fileItem->setText(QFileInfo(path).fileName());
        fileItem->setToolTip(QFileInfo(path).absoluteFilePath());
    }
    if (auto* statusItem = m_partitionTable->item(row, 4)) {
        statusItem->setText(QStringLiteral("镜像已选择"));
        statusItem->setForeground(QColor(m_darkTheme ? QStringLiteral("#3ddc97")
                                                     : QStringLiteral("#138451")));
    }
    m_partitionTable->selectRow(row);
    appendLog(QStringLiteral("分区镜像已选择：%1 → %2（%3）")
                  .arg(QFileInfo(path).fileName(), partition.name,
                       formatBytes(payloadBytes)));
}

void MainWindow::chooseImageAndFlash()
{
    QVector<PartitionInfo> partitions;
    QStringList imagePaths;
    for (int row = 0; row < m_partitionTable->rowCount() && row < m_partitions.size(); ++row) {
        const QTableWidgetItem* check = m_partitionTable->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            partitions.push_back(m_partitions.at(row));
            imagePaths.push_back(row < m_partitionImagePaths.size()
                ? m_partitionImagePaths.at(row) : QString());
        }
    }
    if (partitions.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("写入分区"),
                                 QStringLiteral("请先勾选需要写入的分区。"));
        return;
    }

    bool allowProtected = false;
    if (!applyProtectionPolicy(&partitions, &imagePaths,
                               QStringLiteral("写入"), &allowProtected)) {
        appendLog(QStringLiteral("写入任务中仅包含保护分区，本次任务已结束"));
        return;
    }

    QStringList missing;
    QStringList mapping;
    for (int index = 0; index < partitions.size(); ++index) {
        const PartitionInfo& partition = partitions.at(index);
        const QString selectedPath = imagePaths.at(index);
        const QFileInfo image(selectedPath);
        if (selectedPath.isEmpty() || !image.isFile() || image.size() <= 0) {
            missing.push_back(partition.name);
            for (int row = 0; row < m_partitions.size(); ++row) {
                if (m_partitions.at(row).lun == partition.lun &&
                    m_partitions.at(row).firstLba == partition.firstLba) {
                    if (auto* statusItem = m_partitionTable->item(row, 4)) {
                        statusItem->setText(QStringLiteral("双击选择镜像"));
                        statusItem->setForeground(QColor(m_darkTheme
                            ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09")));
                    }
                    break;
                }
            }
            continue;
        }
        mapping.push_back(QStringLiteral("%1  →  %2")
                              .arg(image.fileName(), partition.name));
    }

    if (!missing.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("刷写镜像未选择"),
            QStringLiteral("以下已勾选分区还没有指定镜像：\n%1\n\n"
                           "请双击对应分区行选择镜像，然后再次点击写入分区。")
                .arg(missing.join(QStringLiteral("、"))));
        return;
    }

    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认写入"),
        QStringLiteral("即将执行 %1 个分区写入：\n\n%2\n\n该操作会覆盖原有数据。")
            .arg(partitions.size()).arg(mapping.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    setBusy(true);
    emit flashPartitionsRequested(partitions, imagePaths, allowProtected);
}

void MainWindow::chooseXmlAndFlash()
{
    QStringList selectedPaths = m_selectedXmlPaths;
    if (selectedPaths.isEmpty()) {
        selectedPaths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择 rawprogram XML（可多选）"), {},
            rawprogramXmlFilters());
    }
    if (selectedPaths.isEmpty()) return;

    const QStringList paths = expandRawprogramXmlSelection(selectedPaths);
    QString categoryError;
    if (!validateXmlCategory(paths, &categoryError)) {
        QMessageBox::warning(this, QStringLiteral("XML 类型"), categoryError);
        return;
    }

    RawProgramPlan plan;
    QString error;
    if (!RawProgramParser::parse(paths, &plan, &error)) {
        QMessageBox::warning(this, QStringLiteral("XML 刷机计划"), error);
        return;
    }
    m_selectedXmlPaths = plan.xmlPaths;

    int dynamicStarts = 0;
    for (const RawProgramEntry& entry : plan.entries) {
        if (!entry.startSectorResolved) ++dynamicStarts;
    }
    QString summary = QStringLiteral(
        "已选择 %1 个 XML，共 %2 个 program 条目。\n"
        "镜像数据约 %3。\n"
        "将按 XML 原始顺序直接写入各 LUN 和起始扇区。")
        .arg(plan.xmlPaths.size()).arg(plan.entries.size()).arg(formatBytes(plan.totalSourceBytes));
    if (plan.hasSplitSuper()) {
        summary += QStringLiteral("\n\n检测到 %1 个 super 条目，将逐段直刷，全程免合并。")
                       .arg(plan.superEntryCount);
    }
    if (dynamicStarts > 0) {
        summary += QStringLiteral("\n\n有 %1 个 NUM_DISK_SECTORS 条目，将按设备各 LUN 容量解析。")
                       .arg(dynamicStarts);
    }
    summary += QStringLiteral("\n\n开始执行 XML 刷机？");

    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认 XML 刷机"), summary,
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    bool allowProtected = !(m_protectPartitionsCheck &&
                            m_protectPartitionsCheck->isChecked());
    QStringList protectedNames;
    for (const RawProgramEntry& entry : std::as_const(plan.entries)) {
        if (!isProtectedPartitionMapping(entry.label, entry.filename)) continue;
        const QString name = normalizedPartitionName(
            !entry.label.isEmpty() ? entry.label : entry.filename);
        if (!protectedNames.contains(name, Qt::CaseInsensitive)) protectedNames.push_back(name);
    }
    if (!allowProtected && !protectedNames.isEmpty()) {
        QMessageBox confirmation(
            QMessageBox::Warning, QStringLiteral("XML 保护分区确认"),
            QStringLiteral("XML 计划包含以下保护分区：\n\n%1\n\n"
                           "默认动作会跳过这些分区，并锁定对应物理范围。")
                .arg(protectedNames.join(QStringLiteral("、"))),
            QMessageBox::NoButton, this);
        QPushButton* continueButton = confirmation.addButton(
            QStringLiteral("继续刷写保护分区"), QMessageBox::AcceptRole);
        QPushButton* skipButton = confirmation.addButton(
            QStringLiteral("跳过保护分区"), QMessageBox::RejectRole);
        confirmation.setDefaultButton(skipButton);
        confirmation.exec();
        allowProtected = confirmation.clickedButton() == continueButton;
        appendLog(allowProtected
            ? QStringLiteral("XML 保护分区二次确认已通过：%1")
                  .arg(protectedNames.join(QStringLiteral("、")))
            : QStringLiteral("XML 将跳过保护分区：%1")
                  .arg(protectedNames.join(QStringLiteral("、"))));
    }

    setBusy(true);
    emit flashXmlRequested(plan.xmlPaths, allowProtected);
}

void MainWindow::confirmErase()
{
    QVector<PartitionInfo> partitions = checkedPartitions();
    if (partitions.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("擦除分区"),
                                 QStringLiteral("请先勾选需要擦除的分区。"));
        return;
    }
    bool allowProtected = false;
    if (!applyProtectionPolicy(&partitions, nullptr,
                               QStringLiteral("擦除"), &allowProtected)) {
        appendLog(QStringLiteral("擦除任务中仅包含保护分区，本次任务已结束"));
        return;
    }
    quint64 totalBytes = 0;
    QStringList names;
    const quint64 bytesPerSector =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs)
            ? 4096ULL : 512ULL;
    for (const PartitionInfo& partition : partitions) {
        const quint64 bytes = partition.sectorCount() * bytesPerSector;
        totalBytes = bytes > std::numeric_limits<quint64>::max() - totalBytes
            ? std::numeric_limits<quint64>::max() : totalBytes + bytes;
        names.push_back(QStringLiteral("%1 (LUN %2)").arg(partition.name).arg(partition.lun));
    }
    const auto answer = QMessageBox::critical(
        this, QStringLiteral("确认擦除"),
        QStringLiteral("即将擦除 %1 个分区，共 %2：\n\n%3")
            .arg(partitions.size()).arg(formatBytes(totalBytes), names.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    setBusy(true);
    emit erasePartitionsRequested(partitions, allowProtected);
}

void MainWindow::confirmReboot(bool toEdl)
{
    if (!m_connected) return;
    const QString destination = toEdl ? QStringLiteral("9008 模式")
                                      : QStringLiteral("系统");
    if (QMessageBox::question(
            this, QStringLiteral("重启到%1").arg(destination),
            QStringLiteral("确认通过 Firehose 将设备重启到%1？").arg(destination),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在发送重启到%1命令…").arg(destination));
    emit rebootRequested(toEdl);
}

void MainWindow::applyTheme(bool dark)
{
    m_darkTheme = dark;
    QFile style(dark ? QStringLiteral(":/styles/dark.qss")
                     : QStringLiteral(":/styles/light.qss"));
    if (style.open(QIODevice::ReadOnly)) {
        qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    }
    if (m_themeButton) {
        m_themeButton->setText(dark ? QStringLiteral("●  深色模式")
                                    : QStringLiteral("○  浅色模式"));
        m_themeButton->setToolTip(dark ? QStringLiteral("点击切换到浅色模式")
                                       : QStringLiteral("点击切换到深色模式"));
    }
    QSettings settings;
    settings.setValue(QStringLiteral("appearance/darkTheme"), dark);
    if (m_partitionTable && m_partitionTable->rowCount() == m_partitions.size()) {
        for (int row = 0; row < m_partitions.size(); ++row) {
            const bool guarded = isProtectedPartitionName(m_partitions.at(row).name) &&
                m_protectPartitionsCheck && m_protectPartitionsCheck->isChecked();
            if (QTableWidgetItem* status = m_partitionTable->item(row, 4)) {
                status->setForeground(QColor(guarded
                    ? (dark ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09"))
                    : (dark ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))));
            }
        }
    }
    if (m_logView) rebuildLogView();
}

void MainWindow::appendLog(const QString& message)
{
    constexpr qsizetype kMaxLogRecords = 50000;
    constexpr qsizetype kTrimLogRecords = 5000;
    bool historyTrimmed = false;
    if (m_logHistory.size() >= kMaxLogRecords) {
        m_logHistory.remove(0, kTrimLogRecords);
        historyTrimmed = true;
    }

    const LogRecord record {
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
        message
    };
    m_logHistory.push_back(record);
    if (!shouldShowLog(message)) return;

    if (historyTrimmed) rebuildLogView();
    else m_logView->append(formatLogHtml(record));
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
}

bool MainWindow::shouldShowLog(const QString& message) const
{
    if (m_debugLogEnabled) return true;

    // Normal mode keeps task milestones and actionable errors while hiding the
    // high-frequency protocol trace. Every record remains in m_logHistory and
    // is restored immediately when Debug is enabled.
    if (message.startsWith(QStringLiteral("Firehose: INFO:"))) return false;
    if (message.startsWith(QStringLiteral("发送 Firehose 命令："))) return false;
    if (message.startsWith(QStringLiteral("VIP READ："))) return false;
    if (message.startsWith(QStringLiteral("VIP PROGRAM："))) return false;
    if (message.startsWith(QStringLiteral("VIP ERASE："))) return false;
    if (message.startsWith(QStringLiteral("[Sahara][DEBUG]"))) return false;
    if (message.startsWith(QStringLiteral("Firehose: ERROR: operation 0 on")) &&
        message.contains(QStringLiteral("is forbidden"), Qt::CaseInsensitive)) return false;
    if (message.contains(QStringLiteral("not get permission,line="),
                         Qt::CaseInsensitive)) return false;
    if (message.contains(QStringLiteral("begin boot log"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("end boot log"), Qt::CaseInsensitive) ||
        (message.startsWith(QStringLiteral("Firehose: ERROR: B -")) &&
         message.contains(QStringLiteral("usb: init start"), Qt::CaseInsensitive))) return false;

    // Some OPPO programmers emit these contradictory internal diagnostics even
    // when the following verify/configure stage succeeds.
    if (message.contains(QStringLiteral("DevprgRSAVerify verify signature"))) return false;
    if (message.contains(QStringLiteral("SpmiBus_ReadLong"))) return false;
    if (message.contains(QStringLiteral("devprg_pm_charger_set"))) return false;

    return true;
}

QString MainWindow::formatLogHtml(const LogRecord& record) const
{
    QString color = m_darkTheme ? QStringLiteral("#d8e1eb") : QStringLiteral("#27364a");
    if (record.message.contains(QStringLiteral("错误")) ||
        record.message.contains(QStringLiteral("失败"))) {
        color = m_darkTheme ? QStringLiteral("#ff6b72") : QStringLiteral("#c92f37");
    } else if (record.message.contains(QStringLiteral("完成")) ||
               record.message.contains(QStringLiteral("成功")) ||
               record.message.contains(QStringLiteral("就绪"))) {
        color = m_darkTheme ? QStringLiteral("#37db91") : QStringLiteral("#138451");
    } else if (record.message.contains(QStringLiteral("等待")) ||
               record.message.contains(QStringLiteral("正在"))) {
        color = m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09");
    }
    const QString timeColor = m_darkTheme ? QStringLiteral("#86a0b9")
                                          : QStringLiteral("#64748b");
    return QStringLiteral("<span style='color:%1'>%2</span>&nbsp;&nbsp;<span style='color:%3'>%4</span>")
        .arg(timeColor, record.time.toHtmlEscaped(), color,
             record.message.toHtmlEscaped());
}

void MainWindow::rebuildLogView()
{
    QStringList lines;
    lines.reserve(m_logHistory.size());
    for (const LogRecord& record : std::as_const(m_logHistory)) {
        if (shouldShowLog(record.message)) lines.push_back(formatLogHtml(record));
    }

    m_logView->setUpdatesEnabled(false);
    m_logView->setHtml(lines.join(QStringLiteral("<br>")));
    m_logView->setUpdatesEnabled(true);
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
}

void MainWindow::updateState(SessionState state, const QString& detail)
{
    if (state == SessionState::Ready) m_connected = true;
    if (state == SessionState::Disconnected || state == SessionState::Failed) m_connected = false;
    m_connectButton->setText(m_connected ? QStringLiteral("断开连接") : QStringLiteral("查找分区"));
    m_stateBanner->setText(detail);
    m_modeValue->setText(stateText(state));
    m_bottomStatus->setText(QStringLiteral("●  %1").arg(stateText(state)));
    m_bottomStatus->setProperty("connected", m_connected);
    m_bottomStatus->setProperty("failed", state == SessionState::Failed);
    m_bottomStatus->style()->unpolish(m_bottomStatus);
    m_bottomStatus->style()->polish(m_bottomStatus);
    m_protocolStatus->setText(state == SessionState::Failed
        ? QStringLiteral("协议通信状态：异常") : QStringLiteral("协议通信状态：正常"));
    m_protocolStatus->setProperty("failed", state == SessionState::Failed);
    m_protocolStatus->style()->unpolish(m_protocolStatus);
    m_protocolStatus->style()->polish(m_protocolStatus);
}

void MainWindow::updateProgress(quint64 completed, quint64 total)
{
    if (total == 0) {
        m_taskProgressTarget = 0;
        m_progressBar->setValue(0);
        m_progressText->setText(QStringLiteral("当前任务  0.0%"));
        m_progressText->setToolTip({});
        return;
    }

    const double ratio = qMin(1.0, static_cast<double>(completed) / static_cast<double>(total));
    m_taskProgressTarget = static_cast<int>(ratio * 1000.0);
    if (completed == 0) m_progressBar->setValue(0);
    m_progressText->setText(QStringLiteral("当前任务  %1%")
                                .arg(ratio * 100.0, 0, 'f', 1));
    m_progressText->setToolTip(QStringLiteral("%1 / %2")
                                   .arg(formatBytes(completed), formatBytes(total)));
    if (!m_progressAnimationTimer->isActive()) m_progressAnimationTimer->start();
}

void MainWindow::updateOverallProgress(quint64 completed, quint64 total)
{
    if (total == 0) {
        m_overallProgressTarget = 0;
        m_totalProgressBar->setValue(0);
        m_totalProgressText->setText(QStringLiteral("总体进度  0.0%"));
        m_totalProgressText->setToolTip({});
        m_speedTimer.invalidate();
        m_speedLastCompleted = 0;
        m_speedTotal = 0;
        m_speedLastUpdateMs = 0;
        m_smoothedBytesPerSecond = 0.0;
        m_speedText->setText(QStringLiteral("速率  0.00 MB/s"));
        return;
    }

    const bool transferChanged = !m_speedTimer.isValid() || total != m_speedTotal ||
                                 completed < m_speedLastCompleted;
    if (transferChanged) {
        m_speedTimer.start();
        m_speedLastCompleted = completed;
        m_speedTotal = total;
        m_speedLastUpdateMs = 0;
        m_smoothedBytesPerSecond = 0.0;
        m_speedText->setText(QStringLiteral("速率  0.00 MB/s"));
    } else if (total >= 64ULL * 1024ULL &&
               (m_speedTimer.elapsed() - m_speedLastUpdateMs >= 500 || completed == total)) {
        const qint64 elapsedMs = m_speedTimer.elapsed();
        const quint64 transferred = completed - m_speedLastCompleted;
        if (elapsedMs > 0 && transferred > 0) {
            const double average = static_cast<double>(transferred) * 1000.0 /
                                   static_cast<double>(elapsedMs);
            m_smoothedBytesPerSecond = m_smoothedBytesPerSecond <= 0.0
                ? average
                : m_smoothedBytesPerSecond * 0.80 + average * 0.20;
            m_speedText->setText(QStringLiteral("速率  %1 MB/s")
                                     .arg(m_smoothedBytesPerSecond / (1024.0 * 1024.0),
                                          0, 'f', 2));
        }
        m_speedLastUpdateMs = elapsedMs;
    }
    const double ratio = qMin(1.0, static_cast<double>(completed) /
                                    static_cast<double>(total));
    m_overallProgressTarget = static_cast<int>(ratio * 1000.0);
    if (completed == 0) m_totalProgressBar->setValue(0);
    m_totalProgressText->setText(QStringLiteral("总体进度  %1%")
                                     .arg(ratio * 100.0, 0, 'f', 1));
    m_totalProgressText->setToolTip(QStringLiteral("%1 / %2").arg(completed).arg(total));
    if (!m_progressAnimationTimer->isActive()) m_progressAnimationTimer->start();
}

void MainWindow::updateDeviceInfo(const DeviceInfo& info)
{
    m_portValue->setText(info.portName.isEmpty() ? QStringLiteral("—") : info.portName);
    const QString storageText = info.storageBytes > 0
        ? QStringLiteral("%1 GB").arg(
              static_cast<double>(info.storageBytes) /
                  (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
        : QStringLiteral("—");
    m_storageValue->setText(storageText);
    m_storageValue->setToolTip(QStringLiteral("存储类型：%1\n原始字节数：%2")
        .arg(info.storage.isEmpty() ? QStringLiteral("—") : info.storage)
        .arg(info.storageBytes));
    m_programmerValue->setText(info.programmer.isEmpty() ? QStringLiteral("—") :
        (info.vipMode.isEmpty() ? info.programmer : QStringLiteral("%1  ·  %2").arg(info.programmer, info.vipMode)));
    const QString detectedBrand = !info.brand.isEmpty() ? info.brand :
        (info.vipMode.contains(QStringLiteral("OPPO"), Qt::CaseInsensitive)
             ? QStringLiteral("OPLUS") : QStringLiteral("Qualcomm"));
    m_brandValue->setText(info.portName.isEmpty() ? QStringLiteral("—") : detectedBrand);
    QString chip = info.model;
    static const QRegularExpression chipName(
        QStringLiteral("((?:SM|MSM|SDM|APQ)[0-9]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch chipMatch = chipName.match(chip);
    if (chipMatch.hasMatch()) chip = chipMatch.captured(1).toUpper();
    else if (chip.isEmpty() && !info.msmId.isEmpty()) chip = info.msmId.toUpper();
    m_modelValue->setText(chip.isEmpty() ? QStringLiteral("—") : chip);
    const QString identity = !info.serial.isEmpty() ? info.serial : info.hwid;
    m_cpuidValue->setText(identity.isEmpty() ? QStringLiteral("—") : identity);
    m_systemValue->setText(info.storageProduct.isEmpty()
        ? QStringLiteral("—") : info.storageProduct);
    m_productNameValue->setText(info.productName.isEmpty()
        ? QStringLiteral("—") : info.productName);
    m_androidValue->setText(info.androidVersion.isEmpty()
        ? QStringLiteral("—") : info.androidVersion);
    m_versionValue->setText(info.systemVersion.isEmpty()
        ? QStringLiteral("—") : info.systemVersion);
    m_versionValue->setToolTip(info.buildFingerprint);
    m_securityPatchValue->setText(info.securityPatch.isEmpty()
        ? QStringLiteral("—") : info.securityPatch);
    m_stateBanner->setToolTip(info.portName.isEmpty() ? QString() :
        QStringLiteral("串口：%1\nProgrammer：%2\n模式：%3\nSahara：%4\nMSM ID：%5\nSerial：%6\nPK Hash：%7\n存储型号：%8\n设备名称：%9\nAndroid：%10\n系统版本：%11\n安全补丁：%12")
            .arg(info.portName, info.programmer, info.vipMode,
                 info.saharaVersion, info.msmId, info.serial, info.pkHash,
                 info.storageProduct, info.productName, info.androidVersion,
                 info.systemVersion, info.securityPatch));
}

void MainWindow::showPartitions(const QVector<PartitionInfo>& partitions)
{
    const QSignalBlocker tableBlocker(m_partitionTable);
    const QSignalBlocker selectAllBlocker(m_selectAllCheck);
    m_partitions = partitions;
    m_partitionImagePaths.clear();
    m_partitionImagePaths.reserve(partitions.size());
    for (int row = 0; row < partitions.size(); ++row) {
        m_partitionImagePaths.push_back(QString());
    }
    m_partitionTable->setRowCount(partitions.size());
    m_selectAllCheck->setChecked(false);
    m_emptyState->setText(QStringLiteral("选择 rawprogram XML 后，这里会显示分区镜像\n\n连接设备后可双击分区选择独立镜像"));
    m_emptyState->setVisible(partitions.isEmpty());
    const quint64 bytesPerSector =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs) ? 4096ULL : 512ULL;
    for (int row = 0; row < partitions.size(); ++row) {
        const PartitionInfo& p = partitions.at(row);
        const bool guarded = isProtectedPartitionName(p.name) &&
            m_protectPartitionsCheck && m_protectPartitionsCheck->isChecked();
        const QStringList columns{QString(), p.name, QStringLiteral("—"),
                                  formatBytes(p.sectorCount() * bytesPerSector),
                                  guarded ? QStringLiteral("保护已开启")
                                          : QStringLiteral("就绪")};
        for (int column = 0; column < columns.size(); ++column) {
            auto* item = new QTableWidgetItem(columns.at(column));
            if (column == 0) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
            }
            if (column == 4) {
                item->setForeground(QColor(guarded
                    ? (m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09"))
                    : (m_darkTheme ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))));
            }
            item->setToolTip(QStringLiteral("LUN %1，起始扇区 %2，扇区数 %3")
                                 .arg(p.lun).arg(p.firstLba).arg(p.sectorCount()));
            m_partitionTable->setItem(row, column, item);
        }
    }
    if (!partitions.isEmpty()) m_partitionTable->selectRow(0);
    updateCheckedCount();
}

void MainWindow::operationFinished(bool success, const QString& message)
{
    const bool startAutomaticGptRead = success && m_readGptAfterConnect && m_connected;
    const bool startVersionRead = success && m_connected &&
        message.startsWith(QStringLiteral("分区表读取完成")) &&
        (m_readVersionAfterGpt || (m_readInfoCheck && m_readInfoCheck->isChecked()));
    const bool completedDataReadOrWrite =
        message.startsWith(QStringLiteral("批量读取完成")) ||
        (message.startsWith(QStringLiteral("分区 ")) &&
         (message.contains(QStringLiteral("已备份")) ||
          message.endsWith(QStringLiteral("写入完成")))) ||
        message.startsWith(QStringLiteral("批量写入完成")) ||
        message.startsWith(QStringLiteral("XML 刷机完成"));
    const bool autoRebootAfterData = success && m_connected &&
        m_autoRebootCheck && m_autoRebootCheck->isChecked() &&
        completedDataReadOrWrite;
    if (!success || !m_connected) {
        m_readGptAfterConnect = false;
        m_readVersionAfterGpt = false;
    }
    setBusy(false);
    if (!success && !message.contains(QStringLiteral("停止"))) {
        QMessageBox::warning(this, QStringLiteral("EDL 操作"), message);
    }
    if (startAutomaticGptRead) {
        m_readGptAfterConnect = false;
        appendLog(QStringLiteral("连接完成，自动读取 GPT 分区表…"));
        setBusy(true);
        emit readGptRequested();
    } else if (startVersionRead) {
        m_readVersionAfterGpt = false;
        appendLog(QStringLiteral("分区表已就绪，自动读取系统版本信息…"));
        setBusy(true);
        emit readVersionInfoRequested();
    } else if (autoRebootAfterData) {
        appendLog(QStringLiteral("读写任务完成，按设置自动重启到系统…"));
        setBusy(true);
        emit rebootRequested(false);
    }
}

void MainWindow::setBusy(bool busy)
{
    if (busy && !m_busy) {
        m_speedTimer.invalidate();
        m_speedLastCompleted = 0;
        m_speedTotal = 0;
        m_speedLastUpdateMs = 0;
        m_smoothedBytesPerSecond = 0.0;
        if (m_speedText) m_speedText->setText(QStringLiteral("速率  0.00 MB/s"));
    }
    m_busy = busy;
    m_portCombo->setEnabled(!busy && !m_connected);
    m_storageCombo->setEnabled(!busy && !m_connected);
    m_modeCombo->setEnabled(!busy && !m_connected);
    m_chipsetCombo->setEnabled(!busy && !m_connected);
    m_programmerEdit->setEnabled(!busy && !m_connected);
    m_firmwareDirectoryEdit->setEnabled(!busy);
    m_firmwareBrowseButton->setEnabled(!busy);
    m_autoScanCheck->setEnabled(!busy && !m_connected);
    if (m_autoScanTimer) {
        if (!busy && !m_connected && m_autoScanCheck->isChecked()) m_autoScanTimer->start();
        else m_autoScanTimer->stop();
    }
    m_vipCheck->setEnabled(!busy && !m_connected);
    m_autoRebootCheck->setEnabled(!busy);
    m_generateXmlCheck->setEnabled(!busy);
    m_protectPartitionsCheck->setEnabled(!busy);
    m_readInfoCheck->setEnabled(!busy);
    m_vipDirectoryEdit->setEnabled(!busy && !m_connected && m_vipCheck->isChecked());
    m_vipBrowseButton->setEnabled(!busy && !m_connected && m_vipCheck->isChecked());
    m_connectButton->setEnabled(!busy);
    const bool ready = m_connected && !busy;
    const bool hasPort = m_portCombo && !m_portCombo->currentData().toString().isEmpty();
    m_readGptButton->setEnabled(!busy && (m_connected || hasPort));
    m_readVersionButton->setEnabled(!busy && (m_connected || hasPort));
    m_backupButton->setEnabled(ready && !m_partitions.isEmpty());
    m_flashButton->setEnabled(ready);
    m_eraseButton->setEnabled(ready && !m_partitions.isEmpty());
    m_rebootButton->setEnabled(ready);
    m_stopButton->setEnabled(busy);
    m_selectAllCheck->setEnabled(ready && !m_partitions.isEmpty());
}

QString MainWindow::formatBytes(quint64 bytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KiB"),
                                   QStringLiteral("MiB"), QStringLiteral("GiB"),
                                   QStringLiteral("TiB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, unit == 0 ? 'f' : 'f', unit == 0 ? 0 : 2).arg(units.at(unit));
}

} // namespace swt::edl
