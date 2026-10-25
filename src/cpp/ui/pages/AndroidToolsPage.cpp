#include "ui/pages/AndroidToolsPage.h"

#include "kc/auth/MembershipService.h"
#include "kc/security/Digest.h"
#include "kc/tools/FirmwareChecksum.h"
#include "kc/tools/PlatformToolsManager.h"
#include "oplus/OfpExtractor.h"
#include "oplus/OpsExtractor.h"
#include "ui/dialogs/ThemedMessageBox.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/BottomSheet.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QThreadPool>
#include <QThread>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

/// 把一段函数体丢进全局线程池（等价 C# 的 Task.Run）。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

/// SuperMergeService 的 C++ 对应物（仅本页使用，故放匿名命名空间）。
struct SuperPartition {
    QString name;
    QString path;
    QString size;
    QString groupName;
};

struct SuperConfig {
    QString metaSize = QStringLiteral("65536");
    QVector<QPair<QString, QString>> devices;   // name : size
    QVector<QPair<QString, QString>> groups;    // name : maximumSize(可空)
    QVector<SuperPartition> partitions;
};

/// FindTool：先 exe 同目录 Tools/，再沿 PATH 找（与 C# SuperMergeService.FindTool 一致）。
QString findMergeTool(const QString& toolName)
{
    const QString local = QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("Tools/") + toolName);
    if (QFile::exists(local)) return local;

    const QStringList dirs =
        qEnvironmentVariable("PATH").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& dir : dirs) {
        const QString candidate = QDir(dir.trimmed()).filePath(toolName);
        if (QFile::exists(candidate)) return candidate;
    }
    return {};
}

bool runTool(const QString& program, const QStringList& args, const QString& workingDir,
             const std::function<void(const QString&)>& log)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setWorkingDirectory(workingDir);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(10000)) {
        log(QStringLiteral("Process error: 无法启动 %1").arg(program));
        return false;
    }
    while (process.waitForReadyRead(30000)) {
        for (const QString& line : QString::fromLocal8Bit(process.readAll()).split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty()) log(line.trimmed());
    }
    process.waitForFinished(10 * 60 * 1000);
    if (process.state() == QProcess::Running) {
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit) {
        log(QStringLiteral("Process error: %1 异常退出").arg(QFileInfo(program).fileName()));
        return false;
    }
    if (process.exitCode() != 0)
        log(QStringLiteral("[tool ERR] 退出码 %1").arg(process.exitCode()));
    return process.exitCode() == 0;
}

QString formatMergeSize(qint64 bytes)
{
    return QStringLiteral("%1 GB")
        .arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

} // namespace

AndroidToolsPage::AndroidToolsPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("Android工具箱"));
    buildUi();
}

AndroidToolsPage::~AndroidToolsPage() = default;

void AndroidToolsPage::buildUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);

    // ── 左列：日志（对应 C# 左列「日志」分组框，占满剩余高度）────────────
    // 2026-10-12：日志列此前只有一个「日志」标题浮在同色空白上，左边的区域
    // 看起来像一块没画完的空洞 —— 收进卡片，与右列工具卡同一族视觉语言。
    auto* log = new LogView(this);
    registerLogView(log);
    log->setMinimumWidth(320);

    auto* logCard = new QFrame(this);
    logCard->setObjectName(QStringLiteral("card"));
    logCard->setFrameShape(QFrame::StyledPanel);
    auto* logColumn = new QVBoxLayout(logCard);
    logColumn->setContentsMargins(14, 12, 14, 12);
    logColumn->setSpacing(8);
    auto* logHeader = new QLabel(QStringLiteral("日志"), logCard);
    logHeader->setObjectName(QStringLiteral("groupHeader"));
    logColumn->addWidget(logHeader);
    logColumn->addWidget(log, 1);

    // ── 右列：工具分组框（可滚，对应 C# 右列 ScrollViewer）────────────────
    auto* toolsScroll = new QScrollArea(this);
    toolsScroll->setWidgetResizable(true);
    toolsScroll->setFrameShape(QFrame::NoFrame);
    toolsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* toolsHost = new QWidget;
    auto* tools = new QVBoxLayout(toolsHost);
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(10);

    // 每个功能一张卡 ───────────────────────────────────────────────────────
    // 2026-10-12：此前是「一行标题 + 一组控件」平铺在同一条竖直流里，块与块
    // 之间没有任何边界 —— 五件事（Super 合并 / OFP 解包 / Payload 转换 /
    // OCDT / TWRP）糊成一坨，标题和输入框混在一起看不出归属。现在每块收进
    // 一张卡片（与「设置」页的 settingsCard 同族），标题在卡内、操作在卡内。
    auto addGroup = [&](const QString& title) -> QVBoxLayout* {
        auto* card = new QFrame(toolsHost);
        card->setObjectName(QStringLiteral("card"));
        card->setFrameShape(QFrame::StyledPanel);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        auto* header = new QLabel(title, card);
        header->setObjectName(QStringLiteral("groupHeader"));
        cardLayout->addWidget(header);

        auto* box = new QVBoxLayout;
        box->setSpacing(6);
        cardLayout->addLayout(box);

        tools->addWidget(card);
        return box;
    };
    auto makeFileRow = [&](QVBoxLayout* group, QLineEdit** edit, const QString& placeholder) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        *edit = new QLineEdit(toolsHost);
        (*edit)->setPlaceholderText(placeholder);
        auto* browse = new QPushButton(QStringLiteral("选择"), toolsHost);
        browse->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(browse);
        row->addWidget(*edit, 1);
        row->addWidget(browse);
        group->addLayout(row);
        return browse;
    };

    // ① OPLUS Super 合并 ────────────────────────────────────────────────────
    auto* superGroup = addGroup(QStringLiteral("OPLUS Super 合并"));
    auto* superBrowse = makeFileRow(superGroup, &m_superEdit,
                                    QStringLiteral("Super 镜像（*.img / *.bin，仅校验用）"));
    m_superEdit->setObjectName(QStringLiteral("toolInput"));
    auto* mergeButton = new QPushButton(QStringLiteral("合并 Super"), toolsHost);
    mergeButton->setObjectName(QStringLiteral("primaryButton"));
    mergeButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(mergeButton);
    superGroup->addWidget(mergeButton);
    superGroup->addStretch();
    connect(superBrowse, &QPushButton::clicked, this, [this] { browseSuper(); });
    connect(mergeButton, &QPushButton::clicked, this, [this] { mergeSuper(); });

    // ② OFP / OPS 解包 ─────────────────────────────────────────────────────
    auto* ofpGroup = addGroup(QStringLiteral("OFP / OPS 解包"));
    auto* ofpBrowse = makeFileRow(ofpGroup, &m_ofpEdit,
                                  QStringLiteral("OPLUS 刷写包（*.ofp / *.ops）"));
    m_ofpEdit->setObjectName(QStringLiteral("toolInput"));
    auto* ofpRow = new QHBoxLayout;
    ofpRow->setSpacing(6);
    auto* unpackOfpButton = new QPushButton(QStringLiteral("解包 OFP"), toolsHost);
    unpackOfpButton->setObjectName(QStringLiteral("primaryButton"));
    auto* unpackOpsButton = new QPushButton(QStringLiteral("解包 OPS"), toolsHost);
    for (QPushButton* b : { unpackOfpButton, unpackOpsButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        ofpRow->addWidget(b);
    }
    ofpRow->addStretch();
    ofpGroup->addLayout(ofpRow);
    ofpGroup->addStretch();
    connect(ofpBrowse, &QPushButton::clicked, this, [this] { browseOfp(); });
    connect(unpackOfpButton, &QPushButton::clicked, this, [this] { unpackOfp(); });
    connect(unpackOpsButton, &QPushButton::clicked, this, [this] { unpackOps(); });

    // ③ Payload 转 Fastboot ────────────────────────────────────────────────
    auto* payloadGroup = addGroup(QStringLiteral("Payload 转 Fastboot"));
    auto* payloadBrowse = makeFileRow(payloadGroup, &m_payloadEdit,
                                      QStringLiteral("payload.bin（*.bin）"));
    m_payloadEdit->setObjectName(QStringLiteral("toolInput"));
    auto* convertButton = new QPushButton(QStringLiteral("转 Fastboot 脚本"), toolsHost);
    convertButton->setObjectName(QStringLiteral("primaryButton"));
    convertButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(convertButton);
    payloadGroup->addWidget(convertButton);
    payloadGroup->addStretch();
    connect(payloadBrowse, &QPushButton::clicked, this, [this] { browsePayload(); });
    connect(convertButton, &QPushButton::clicked, this, [this] { convertPayload(); });

    // ④ OCDT 生成 ──────────────────────────────────────────────────────────
    //  2026-10-19：原来只 adb 读一下机型再写个 XML（那不是 OCDT，真机刷进去无效）。
    //  现在按 YukiClient 的 TDCO 容器逐字节生成：平台 → 机型代号 → 项目号 → 变体
    //  → （可选）OSIG 备份，输出 8MB 联发科 / 128KB 高通整幅镜像。
    auto* ocdtGroup = addGroup(QStringLiteral("OCDT 生成"));
    m_ocdtPlatform = new QComboBox(toolsHost);
    m_ocdtPlatform->addItem(QStringLiteral("8MB 联发科"), QStringLiteral("8mb"));
    m_ocdtPlatform->addItem(QStringLiteral("128KB 高通"), QStringLiteral("128kb"));
    ocdtGroup->addWidget(m_ocdtPlatform);

    m_ocdtModel = new QLineEdit(toolsHost);
    m_ocdtModel->setObjectName(QStringLiteral("toolInput"));
    m_ocdtModel->setPlaceholderText(QStringLiteral("机型代号（如 PDEM10，可留空手填项目号）"));
    ocdtGroup->addWidget(m_ocdtModel);

    m_ocdtProjectId = new QLineEdit(toolsHost);
    m_ocdtProjectId->setObjectName(QStringLiteral("toolInput"));
    m_ocdtProjectId->setPlaceholderText(QStringLiteral("项目号（如 19065）"));
    ocdtGroup->addWidget(m_ocdtProjectId);

    m_ocdtVariant = new QComboBox(toolsHost);
    m_ocdtVariant->addItem(QStringLiteral("标准"), QStringLiteral("standard"));
    m_ocdtVariant->addItem(QStringLiteral("0x02 标记填充"), QStringLiteral("fill02"));
    m_ocdtVariant->addItem(QStringLiteral("geyixue 密钥填充"), QStringLiteral("geyixue_fill"));
    m_ocdtVariant->addItem(QStringLiteral("固定配置（PGZ110 / RMX3461）"),
                           QStringLiteral("mixed_projid"));
    ocdtGroup->addWidget(m_ocdtVariant);

    auto* ocdtOsigBrowse = makeFileRow(ocdtGroup, &m_ocdtOsigEdit,
                                       QStringLiteral("OSIG 备份镜像（可选，仅旧款联发科使用）"));
    m_ocdtOsigEdit->setObjectName(QStringLiteral("toolInput"));

    // 机型代号补全：内置 44 个欧加机型，选中即带出项目号与推荐变体。
    {
        QStringList models;
        for (const auto& entry : swt::edl::OcdtGenerator::modelTable()) {
            models << QString::fromLatin1(entry.model);
        }
        auto* completer = new QCompleter(models, this);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchStartsWith);
        m_ocdtModel->setCompleter(completer);
    }
    connect(m_ocdtModel, &QLineEdit::textChanged, this, [this](const QString& text) {
        int projectId = 0;
        if (!swt::edl::OcdtGenerator::tryGetProjectId(text, &projectId)) return;
        m_ocdtProjectId->setText(QString::number(projectId));
        const int index = m_ocdtVariant->findData(
            swt::edl::OcdtGenerator::variantKey(
                swt::edl::OcdtGenerator::recommendedVariant(text)));
        if (index >= 0) m_ocdtVariant->setCurrentIndex(index);
    });

    auto* ocdtRow = new QHBoxLayout;
    ocdtRow->setSpacing(6);
    auto* ocdtButton = new QPushButton(QStringLiteral("生成 OCDT"), toolsHost);
    ocdtButton->setObjectName(QStringLiteral("primaryButton"));
    auto* ocdtAnalyzeButton = new QPushButton(QStringLiteral("分析 OCDT"), toolsHost);
    for (QPushButton* b : { ocdtButton, ocdtAnalyzeButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        ocdtRow->addWidget(b);
    }
    ocdtRow->addStretch();
    ocdtGroup->addLayout(ocdtRow);
    ocdtGroup->addStretch();
    connect(ocdtOsigBrowse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 OSIG 备份镜像"), QString(),
            QStringLiteral("镜像文件 (*.img *.bin);;所有文件 (*.*)"));
        if (!file.isEmpty()) m_ocdtOsigEdit->setText(file);
    });
    connect(ocdtButton, &QPushButton::clicked, this, [this] { generateOcdt(); });
    connect(ocdtAnalyzeButton, &QPushButton::clicked, this, [this] { analyzeOcdt(); });

    // ⑤ TWRP 刷写 ──────────────────────────────────────────────────────────
    auto* twrpGroup = addGroup(QStringLiteral("TWRP 刷写"));
    auto* twrpRow = new QHBoxLayout;
    twrpRow->setSpacing(6);
    m_twrpPartitionEdit = new QLineEdit(toolsHost);
    m_twrpPartitionEdit->setObjectName(QStringLiteral("toolInput"));
    m_twrpPartitionEdit->setPlaceholderText(QStringLiteral("分区文件(以boot为例)"));
    auto* twrpBrowse = new QPushButton(QStringLiteral("选择文件"), toolsHost);
    twrpBrowse->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(twrpBrowse);
    twrpRow->addWidget(m_twrpPartitionEdit, 1);
    twrpRow->addWidget(twrpBrowse);
    twrpGroup->addLayout(twrpRow);

    auto* twrpActions = new QHBoxLayout;
    twrpActions->setSpacing(8);
    auto* readButton = new QPushButton(QStringLiteral("读取分区"), toolsHost);
    auto* writeButton = new QPushButton(QStringLiteral("写入分区"), toolsHost);
    writeButton->setObjectName(QStringLiteral("primaryButton"));
    auto* eraseButton = new QPushButton(QStringLiteral("擦除分区"), toolsHost);
    // 卡刷包直刷（移植自 TMFTool 的「Twrp FlashZip / 刷入卡刷包」）：
    // 推包到 /sdcard/Download 再交给 TWRP 的 `twrp install`，由 recovery 自己解包执行
    // updater-script —— 与「把 zip 解开再 dd 分区」是两回事，后者会漏掉脚本里的
    // 分区挂载 / 权限修复 / 增量 patch。
    auto* zipButton = new QPushButton(QStringLiteral("刷入卡刷包"), toolsHost);
    for (QPushButton* b : { readButton, writeButton, eraseButton, zipButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        twrpActions->addWidget(b);
    }
    twrpActions->addStretch();
    twrpGroup->addLayout(twrpActions);
    twrpGroup->addStretch();
    connect(twrpBrowse, &QPushButton::clicked, this, [this] { browseTwrpImage(); });
    connect(readButton, &QPushButton::clicked, this, [this] { twrpRead(); });
    connect(writeButton, &QPushButton::clicked, this, [this] { twrpWrite(); });
    connect(eraseButton, &QPushButton::clicked, this, [this] { twrpErase(); });
    connect(zipButton, &QPushButton::clicked, this, [this] { twrpFlashZip(); });

    // ⑥ 固件校验（MD5 / SHA-256）──────────────────────────────────────────
    //  移植自 TMFTool 的「MD5校验 / 校验Sha256 / 生成校验文件」。
    //  校验文件格式按 coreutils 走（`<hash>  <name>`），这样和网上发布的
    //  SHA256SUMS 能直接互认；「核对」按钮就是读那种文件逐条比对。
    auto* sumGroup = addGroup(QStringLiteral("固件校验（MD5 / SHA-256）"));
    auto* sumRow = new QHBoxLayout;
    sumRow->setSpacing(6);
    m_checksumEdit = new QLineEdit(toolsHost);
    m_checksumEdit->setObjectName(QStringLiteral("toolInput"));
    m_checksumEdit->setPlaceholderText(QStringLiteral("固件文件，或整个固件目录（批量）"));
    auto* sumFileButton = new QPushButton(QStringLiteral("选文件"), toolsHost);
    auto* sumDirButton = new QPushButton(QStringLiteral("选目录"), toolsHost);
    for (QPushButton* b : { sumFileButton, sumDirButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        sumRow->addWidget(b);
    }
    sumRow->insertWidget(0, m_checksumEdit, 1);
    sumGroup->addLayout(sumRow);

    auto* sumActions = new QHBoxLayout;
    sumActions->setSpacing(8);
    m_checksumAlgo = new QComboBox(toolsHost);
    m_checksumAlgo->setObjectName(QStringLiteral("toolInput"));
    m_checksumAlgo->addItem(QStringLiteral("SHA-256"));
    m_checksumAlgo->addItem(QStringLiteral("MD5"));
    m_checksumAlgo->addItem(QStringLiteral("两者都算"));
    m_checksumAlgo->setToolTip(QStringLiteral("SHA-256 抗碰撞，核对第三方校验文件时才用 MD5"));
    auto* sumRunButton = new QPushButton(QStringLiteral("开始校验"), toolsHost);
    sumRunButton->setObjectName(QStringLiteral("primaryButton"));
    auto* sumWriteButton = new QPushButton(QStringLiteral("生成校验文件"), toolsHost);
    auto* sumVerifyButton = new QPushButton(QStringLiteral("核对校验文件…"), toolsHost);
    for (QPushButton* b : { sumRunButton, sumWriteButton, sumVerifyButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        sumActions->addWidget(b);
    }
    sumActions->insertWidget(0, m_checksumAlgo);
    sumActions->addStretch();
    sumGroup->addLayout(sumActions);
    sumGroup->addStretch();
    connect(sumFileButton, &QPushButton::clicked, this, [this] { browseChecksumTarget(); });
    connect(sumDirButton, &QPushButton::clicked, this, [this] { browseChecksumDir(); });
    connect(sumRunButton, &QPushButton::clicked, this, [this] { runChecksum(); });
    connect(sumWriteButton, &QPushButton::clicked, this, [this] { writeChecksumFile(); });
    connect(sumVerifyButton, &QPushButton::clicked, this, [this] { verifyChecksumFile(); });

    tools->addStretch();

    toolsScroll->setWidget(toolsHost);

    // 左列 日志 / 右列 工具 —— C# 左列固定 316，这里给日志列一个相近的宽度感。
    layout->addWidget(logCard, 2);
    layout->addWidget(toolsScroll, 3);

    // 通知条（BasePage 依赖 registerNotifyBar）。
    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->insertWidget(0, m_notify);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI 线程入口
// ─────────────────────────────────────────────────────────────────────────────

void AndroidToolsPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons)
        if (b) b->setEnabled(!busy);
}

void AndroidToolsPage::browseSuper()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 Super 镜像"), QString(),
        QStringLiteral("Super镜像 (*.img *.bin)"));
    if (!file.isEmpty()) m_superEdit->setText(file);
}

void AndroidToolsPage::mergeSuper()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = m_superEdit->text().trimmed();
    if (file.isEmpty() || !QFile::exists(file)) {
        error(QStringLiteral("请选择Super镜像"));
        return;
    }
    // C# 在文件校验后弹「选择Super镜像所在文件夹」。
    const QString romDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择Super镜像所在文件夹"));
    if (romDir.isEmpty()) {
        error(QStringLiteral("请选择Super镜像所在文件夹"));
        return;
    }
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, romDir] { mergeCore(romDir); }));
}

void AndroidToolsPage::browseOfp()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 OPLUS 刷写包"), QString(),
        QStringLiteral("OPLUS刷写包 (*.ofp *.ops)"));
    if (!file.isEmpty()) m_ofpEdit->setText(file);
}

void AndroidToolsPage::unpackOfp()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = m_ofpEdit->text().trimmed();
    if (file.isEmpty() || !QFile::exists(file)) {
        error(QStringLiteral("请选择OFP文件"));
        return;
    }
    const QString outputDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 OFP 解包目录"), QFileInfo(file).absolutePath());
    if (outputDir.isEmpty()) return;

    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, file, outputDir] { unpackOfpCore(file, outputDir); }));
}

void AndroidToolsPage::unpackOps()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = m_ofpEdit->text().trimmed();
    if (file.isEmpty() || !QFile::exists(file)) {
        error(QStringLiteral("请选择OPS文件"));
        return;
    }
    const QString outputDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 OPS 解包目录"), QFileInfo(file).absolutePath());
    if (outputDir.isEmpty()) return;

    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, file, outputDir] { unpackOpsCore(file, outputDir); }));
}

void AndroidToolsPage::browsePayload()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 Payload 文件"), QString(),
        QStringLiteral("Payload文件 (*.bin)"));
    if (!file.isEmpty()) m_payloadEdit->setText(file);
}

void AndroidToolsPage::convertPayload()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = m_payloadEdit->text().trimmed();
    if (file.isEmpty() || !QFile::exists(file)) {
        error(QStringLiteral("请选择payload.bin"));
        return;
    }
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, file] { convertPayloadCore(file); }));
}

void AndroidToolsPage::generateOcdt()
{
    if (m_busy.loadAcquire() != 0) return;

    swt::edl::OcdtPlatform platform = swt::edl::OcdtPlatform::Mtk8Mb;
    if (!swt::edl::OcdtGenerator::parsePlatform(m_ocdtPlatform->currentData().toString(),
                                               &platform)) {
        error(QStringLiteral("请选择 OCDT 平台"));
        return;
    }

    swt::edl::OcdtVariant variant = swt::edl::OcdtVariant::Standard;
    if (!swt::edl::OcdtGenerator::parseVariant(m_ocdtVariant->currentData().toString(),
                                               &variant)) {
        error(QStringLiteral("请选择 OCDT 配置变体"));
        return;
    }

    const QString modelCode = m_ocdtModel->text().trimmed();
    bool projectIdOk = false;
    const int projectId = m_ocdtProjectId->text().trimmed().toInt(&projectIdOk);
    if (!projectIdOk || projectId <= 0 || projectId > 0xFFFF) {
        error(QStringLiteral("请填写有效的项目号（1 ~ 65535）"));
        m_ocdtProjectId->setFocus();
        return;
    }

    // 机型代号只在表里时才提示「已识别」；填了表外代号不拦（有些机型确实没收录）。
    int knownProjectId = 0;
    const bool modelKnown =
        !modelCode.isEmpty()
        && swt::edl::OcdtGenerator::tryGetProjectId(modelCode, &knownProjectId);
    if (modelKnown && knownProjectId != projectId) {
        // iOS 底部上滑抽屉确认（玻璃 + 遮罩），取代居中实色弹窗。
        const bool go = BottomSheet::confirmSync(
            QStringLiteral("项目号与机型不一致"),
            QStringLiteral("机型 %1 在表里对应项目号 %2，当前填的是 %3。\n\n仍按 %3 生成吗？")
                .arg(modelCode)
                .arg(knownProjectId)
                .arg(projectId),
            QStringLiteral("继续生成"));
        if (!go) return;
    }

    const QString osigPath = m_ocdtOsigEdit->text().trimmed();
    if (!osigPath.isEmpty() && !QFile::exists(osigPath)) {
        error(QStringLiteral("OSIG 备份镜像不存在: %1").arg(osigPath));
        return;
    }

    // 默认文件名与参考实现一致：ocdt_{projId}_{8mb|128kb}.img
    const QString defaultPath = QDir(QDir::homePath()).filePath(
        swt::edl::OcdtGenerator::defaultFileName(projectId, platform));
    const QString dest = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存 OCDT 镜像"), defaultPath,
        QStringLiteral("OCDT 镜像 (*.img);;所有文件 (*.*)"));
    if (dest.isEmpty()) return;

    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, projectId, platform, variant, modelCode, osigPath, dest] {
            generateOcdtCore(projectId, platform, variant, modelCode, osigPath, dest);
        }));
}

void AndroidToolsPage::analyzeOcdt()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 OCDT 镜像"), QString(),
        QStringLiteral("OCDT 镜像 (*.img);;所有文件 (*.*)"));
    if (file.isEmpty()) return;

    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, file] { analyzeOcdtCore(file); }));
}

void AndroidToolsPage::browseTwrpImage()
{
    // C#：选完镜像顺手把文件名（去扩展名）填进分区名输入框。
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"), QString(),
        QStringLiteral("镜像文件 (*.img)"));
    if (file.isEmpty()) return;
    m_twrpPartitionEdit->setText(QFileInfo(file).completeBaseName());
    logInfo(QStringLiteral("[TWRP] 选择镜像: %1").arg(file));
}

void AndroidToolsPage::twrpRead()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString part = m_twrpPartitionEdit->text().trimmed();
    if (part.isEmpty()) {
        error(QStringLiteral("请输入分区名"));
        return;
    }
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, part] { twrpReadCore(part); }));
}

void AndroidToolsPage::twrpWrite()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString part = m_twrpPartitionEdit->text().trimmed();
    if (part.isEmpty()) {
        error(QStringLiteral("请输入分区名"));
        return;
    }
    // C#：先弹镜像对话框，再过会员闸门。
    const QString image = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"), QString(),
        QStringLiteral("镜像文件 (*.img)"));
    if (image.isEmpty()) return;
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, part, image] { twrpWriteCore(part, image); }));
}

void AndroidToolsPage::twrpErase()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString part = m_twrpPartitionEdit->text().trimmed();
    if (part.isEmpty()) {
        error(QStringLiteral("请输入分区名"));
        return;
    }
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, part] { twrpEraseCore(part); }));
}

// ─────────────────────────────────────────────────────────────────────────────
//  线程池执行体
// ─────────────────────────────────────────────────────────────────────────────

void AndroidToolsPage::mergeCore(const QString& romDir)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this] { logView()->appendSection(QStringLiteral("合并 Super")); });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    auto log = [this](const QString& msg) {
        runOnUi([this, msg] { logInfo(QStringLiteral("[Super] %1").arg(msg)); });
    };

    // ── FindSuperDefJson：romDir/META/super_def.*.json ─────────────────────
    const QDir metaDir(QDir(romDir).filePath(QStringLiteral("META")));
    if (!metaDir.exists()) {
        runOnUi([this] { logError(QStringLiteral("[Super] META folder not found")); });
        runOnUi([this] {
            logError(QStringLiteral("[工具] 未找到super_def.*.json配置文件"));
            error(QStringLiteral("未找到super_def.*.json配置文件"));
        });
        runOnUi([this, started] {
            logView()->appendFooter(
                QStringLiteral("失败, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
        return;
    }
    const QStringList defs = metaDir.entryList({ QStringLiteral("super_def.*.json") }, QDir::Files);
    if (defs.isEmpty()) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 未找到super_def.*.json配置文件"));
            error(QStringLiteral("未找到super_def.*.json配置文件"));
        });
        runOnUi([this, started] {
            logView()->appendFooter(
                QStringLiteral("失败, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
        return;
    }
    const QString jsonPath = metaDir.filePath(defs.first());
    log(QStringLiteral("Found config: %1").arg(defs.first()));

    // ── ParseSuperDefJson ───────────────────────────────────────────────────
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        runOnUi([this] { logError(QStringLiteral("[工具] JSON配置解析失败")); });
        runOnUi([this, started] {
            logView()->appendFooter(
                QStringLiteral("失败, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
    SuperConfig config;
    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("super_meta")))
        config.metaSize = root.value(QStringLiteral("super_meta"))
                              .toObject()
                              .value(QStringLiteral("size"))
                              .toString(QStringLiteral("65536"));
    for (const QJsonValue& v : root.value(QStringLiteral("block_devices")).toArray()) {
        const QJsonObject dev = v.toObject();
        config.devices.append(
            { dev.value(QStringLiteral("name")).toString(QStringLiteral("super")),
              dev.value(QStringLiteral("size")).toString() });
    }
    for (const QJsonValue& v : root.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject grp = v.toObject();
        config.groups.append(
            { grp.value(QStringLiteral("name")).toString(),
              grp.value(QStringLiteral("maximum_size")).toString() });
    }
    for (const QJsonValue& v : root.value(QStringLiteral("partitions")).toArray()) {
        const QJsonObject part = v.toObject();
        SuperPartition p;
        p.name = part.value(QStringLiteral("name")).toString();
        p.path = part.value(QStringLiteral("path")).toString();
        p.size = part.value(QStringLiteral("size")).toString();
        p.groupName = part.value(QStringLiteral("group_name")).toString();
        if (!p.name.isEmpty()) config.partitions.append(p);
    }
    log(QStringLiteral("Parsed config: %1 partitions").arg(config.partitions.size()));
    runOnUi([this, count = config.partitions.size()] {
        logInfo(QStringLiteral("[Super] 分区数: %1").arg(count));
    });

    // ── 找工具并拷进 ROM 目录（与 C# MergeSuperAsync 一致）──────────────────
    const QString simg2img = findMergeTool(QStringLiteral("simg2img.exe"));
    const QString lpmake = findMergeTool(QStringLiteral("lpmake.exe"));
    if (simg2img.isEmpty() || lpmake.isEmpty()) {
        log(simg2img.isEmpty() ? QStringLiteral("simg2img.exe not found")
                               : QStringLiteral("lpmake.exe not found"));
        runOnUi([this, started] {
            logError(QStringLiteral("[工具] Super合并失败"));
            error(QStringLiteral("Super合并失败：找不到 simg2img.exe / lpmake.exe"));
            logView()->appendFooter(
                QStringLiteral("失败, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
        return;
    }
    const QString localSimg2img = QDir(romDir).filePath(QStringLiteral("simg2img.exe"));
    const QString localLpmake = QDir(romDir).filePath(QStringLiteral("lpmake.exe"));
    QFile::remove(localSimg2img);
    QFile::copy(simg2img, localSimg2img);
    QFile::remove(localLpmake);
    QFile::copy(lpmake, localLpmake);
    log(QStringLiteral("Tools copied to IMAGES folder"));

    // ── Stage 1：img → raw ──────────────────────────────────────────────────
    log(QStringLiteral("Stage 1: Converting img to raw..."));
    QVector<QPair<SuperPartition, QString>> rawParts;
    bool cancelled = false;
    for (const SuperPartition& part : config.partitions) {
        if (part.path.isEmpty()) continue;
        const QString imgPath = QDir(romDir)
                                    .filePath(QString(part.path)
                                                  .replace(QLatin1Char('/'), QLatin1Char('\\')));
        if (!QFile::exists(imgPath)) {
            log(QStringLiteral("Warning: %1 not found, skipping").arg(part.path));
            continue;
        }
        const QString rawFileName =
            QFileInfo(imgPath).completeBaseName() + QStringLiteral(".raw");
        const QString rawPath = QDir(romDir).filePath(rawFileName);
        log(QStringLiteral("Converting %1...").arg(QFileInfo(imgPath).fileName()));
        QFile::remove(rawPath);
        if (runTool(localSimg2img,
                    { QDir::toNativeSeparators(imgPath), QDir::toNativeSeparators(rawPath) },
                    romDir, log)
            && QFile::exists(rawPath)) {
            rawParts.append({ part, rawPath });
            log(QStringLiteral("Converted: %1 -> %2")
                    .arg(QFileInfo(imgPath).fileName(), rawFileName));
        } else {
            log(QStringLiteral("Failed to convert: %1").arg(QFileInfo(imgPath).fileName()));
        }
        // 让主线程还有机会响应（本核心在 worker 线程，短暂让出即可）。
        QThread::msleep(1);
    }
    if (rawParts.isEmpty()) {
        log(QStringLiteral("No raw files generated, aborting merge"));
        cancelled = true;
    }

    QString outputPath;
    bool mergeOk = false;
    if (!cancelled) {
        // ── Stage 2：lpmake 合并 ────────────────────────────────────────────
        log(QStringLiteral("Stage 2: Merging raw files to super.img..."));
        outputPath = QDir(romDir).filePath(QStringLiteral("super.img"));
        QFile::remove(outputPath);

        QStringList args;
        args << QStringLiteral("--metadata-size") << config.metaSize;
        args << QStringLiteral("--metadata-slots") << QStringLiteral("2");
        for (const auto& dev : config.devices)
            args << QStringLiteral("--device") << QStringLiteral("%1:%2").arg(dev.first, dev.second);
        for (const auto& grp : config.groups) {
            if (!grp.second.isEmpty())
                args << QStringLiteral("--group")
                     << QStringLiteral("%1:%2").arg(grp.first, grp.second);
        }
        for (const auto& pair : rawParts) {
            args << QStringLiteral("--partition")
                 << QStringLiteral("%1:readonly:%2:%3")
                        .arg(pair.first.name, pair.first.size, pair.first.groupName);
            args << QStringLiteral("--image")
                 << QStringLiteral("%1=%2").arg(pair.first.name, pair.second);
        }
        args << QStringLiteral("--output") << outputPath;

        log(QStringLiteral("Running lpmake with %1 partitions...").arg(rawParts.size()));
        mergeOk = runTool(localLpmake, args, romDir, log) && QFile::exists(outputPath);
        if (mergeOk) {
            const qint64 size = QFileInfo(outputPath).size();
            log(QStringLiteral("Super merge completed! Size: %1").arg(formatMergeSize(size)));
            log(QStringLiteral("Output: %1").arg(outputPath));
            log(QStringLiteral("Cleaning up temporary raw files..."));
            for (const auto& pair : rawParts) QFile::remove(pair.second);
        } else {
            log(QStringLiteral("lpmake failed to create super.img"));
        }
    }

    QFile::remove(localSimg2img);
    QFile::remove(localLpmake);
    log(QStringLiteral("Tools cleaned up"));

    const bool success = mergeOk;
    runOnUi([this, ok = success, outputPath, started] {
        if (ok) {
            logSuccess(QStringLiteral("[工具] Super合并完成!"));
            this->success(QStringLiteral("Super合并完成!"));
            logDetail(QStringLiteral("[Super] 输出: %1").arg(outputPath));
        } else {
            logError(QStringLiteral("[工具] Super合并失败"));
        }
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::convertPayloadCore(const QString& payloadFile)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this] { logView()->appendSection(QStringLiteral("Payload 转 Fastboot")); });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    runOnUi([this, name = QFileInfo(payloadFile).fileName()] {
        logInfo(QStringLiteral("[工具] Payload转Fastboot: %1").arg(name));
    });

    const QString outputDir =
        QDir(QFileInfo(payloadFile).absolutePath()).filePath(QStringLiteral("fastboot_scripts"));
    QDir().mkpath(outputDir);

    PlatformToolsManager tools([this](const QString& msg) {
        runOnUi([this, msg] { logInfo(msg); });
    });

    // 与 C# 一致：先试 adb shell payload_dumper（设备端解包）。
    const QString adbArgs = QStringLiteral("shell payload_dumper --out \"%1\" \"%2\"")
                                .arg(QDir::toNativeSeparators(outputDir),
                                     QDir::toNativeSeparators(payloadFile));
    const auto r = tools.executeAdb(adbArgs, 10 * 60 * 1000);

    bool ok = false;
    QString generatedScript;
    if (r.success && !r.output.trimmed().isEmpty()) {
        ok = true;
        runOnUi([this, outputDir] {
            logSuccess(QStringLiteral("[工具] Payload转换完成: %1").arg(outputDir));
        });
    } else {
        // C# 回落：读 payload.bin 头 4KB，命中已知分区名就生成 flash_all.sh。
        QFile payload(payloadFile);
        if (payload.open(QIODevice::ReadOnly)) {
            const QByteArray head = payload.read(4096);
            payload.close();
            const QString content = QString::fromLatin1(head);

            const QStringList knownParts = {
                QStringLiteral("boot"),     QStringLiteral("system"),
                QStringLiteral("vendor"),   QStringLiteral("product"),
                QStringLiteral("system_ext"), QStringLiteral("dtbo"),
                QStringLiteral("recovery"), QStringLiteral("init_boot"),
                QStringLiteral("modem"),
            };
            QString script = QStringLiteral("#!/bin/bash\n");
            script += QStringLiteral("# Auto-generated flash script from payload.bin\n");
            script += QStringLiteral("set -e\n\n");
            for (const QString& part : knownParts)
                if (content.contains(part, Qt::CaseInsensitive))
                    script += QStringLiteral("fastboot flash %1 %1.img\n").arg(part);
            script += QStringLiteral("fastboot reboot\n");

            generatedScript = QDir(outputDir).filePath(QStringLiteral("flash_all.sh"));
            QFile out(generatedScript);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(script.toUtf8());
                out.close();
                ok = true;
            }
        }
        if (ok) {
            runOnUi([this, generatedScript] {
                logSuccess(QStringLiteral("[工具] 已生成刷写脚本: %1").arg(generatedScript));
            });
        }
    }

    runOnUi([this, ok, started] {
        if (!ok) logError(QStringLiteral("[工具] Payload转换错误"));
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::unpackOfpCore(const QString& sourcePath, const QString& outputDirectory)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this, name = QFileInfo(sourcePath).fileName()] {
        logView()->appendSection(QStringLiteral("解包 OFP: %1").arg(name));
    });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    runOnUi([this, source = QDir::toNativeSeparators(sourcePath),
             target = QDir::toNativeSeparators(outputDirectory)] {
        logInfo(QStringLiteral("[OFP] 源文件: %1").arg(source));
        logInfo(QStringLiteral("[OFP] 输出目录: %1").arg(target));
    });

    swt::edl::OfpExtractResult result;
    QString errorText;
    const bool ok = swt::edl::OfpExtractor::extract(
        sourcePath, outputDirectory, &result, &errorText,
        [this](int percent, const QString& message) {
            if (message.isEmpty()) return;
            runOnUi([this, percent, message] {
                logInfo(QStringLiteral("[OFP] %1%  %2").arg(percent).arg(message));
            });
        },
        [this](const QString& line) {
            runOnUi([this, line] { logInfo(line); });
        });

    QPointer<AndroidToolsPage> guard(this);
    runOnUi([this, guard, ok, errorText, result, started] {
        if (!guard) return;
        if (!ok) {
            logError(QStringLiteral("[OFP] 解包失败: %1").arg(errorText));
            error(QStringLiteral("OFP 解包失败"));
        } else {
            logSuccess(QStringLiteral("[OFP] %1 解包完成: %2 个文件, 共 %3 MB")
                           .arg(swt::edl::OfpExtractResult::formatName(result.format))
                           .arg(result.fileCount)
                           .arg(result.totalBytes / 1048576.0, 0, 'f', 1));
            const qsizetype shown = qMin<qsizetype>(result.files.size(), 12);
            for (qsizetype i = 0; i < shown; ++i) {
                logInfo(QStringLiteral("  · %1").arg(result.files.at(i)));
            }
            if (result.files.size() > shown) {
                logInfo(QStringLiteral("  · … 另有 %1 个文件")
                            .arg(result.files.size() - shown));
            }
            success(QStringLiteral("OFP 解包完成"));
        }
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::unpackOpsCore(const QString& sourcePath, const QString& outputDirectory)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this, name = QFileInfo(sourcePath).fileName()] {
        logView()->appendSection(QStringLiteral("解包 OPS: %1").arg(name));
    });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    runOnUi([this, source = QDir::toNativeSeparators(sourcePath),
             target = QDir::toNativeSeparators(outputDirectory)] {
        logInfo(QStringLiteral("[OPS] 源文件: %1").arg(source));
        logInfo(QStringLiteral("[OPS] 输出目录: %1").arg(target));
    });

    swt::edl::OpsExtractResult result;
    QString errorText;
    const bool ok = swt::edl::OpsExtractor::extract(
        sourcePath, outputDirectory, &result, &errorText,
        [this](int percent, const QString& message) {
            if (message.isEmpty()) return;
            runOnUi([this, percent, message] {
                logInfo(QStringLiteral("[OPS] %1%  %2").arg(percent).arg(message));
            });
        },
        [this](const QString& line) {
            runOnUi([this, line] { logInfo(line); });
        });

    QPointer<AndroidToolsPage> guard(this);
    runOnUi([this, guard, ok, errorText, result, started] {
        if (!guard) return;
        if (!ok) {
            logError(QStringLiteral("[OPS] 解包失败: %1").arg(errorText));
            error(QStringLiteral("OPS 解包失败"));
        } else {
            logSuccess(QStringLiteral("[OPS] MBox %1 解包完成: %2 个镜像, 共 %3 MB")
                           .arg(result.mboxVersion)
                           .arg(result.fileCount)
                           .arg(result.totalBytes / 1048576.0, 0, 'f', 1));
            const qsizetype shown = qMin<qsizetype>(result.files.size(), 12);
            for (qsizetype i = 0; i < shown; ++i) {
                logInfo(QStringLiteral("  · %1").arg(result.files.at(i)));
            }
            if (result.files.size() > shown) {
                logInfo(QStringLiteral("  · … 另有 %1 个镜像")
                            .arg(result.files.size() - shown));
            }
            success(QStringLiteral("OPS 解包完成"));
        }
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::generateOcdtCore(int projectId, swt::edl::OcdtPlatform platform,
                                        swt::edl::OcdtVariant variant, const QString& modelCode,
                                        const QString& osigPath, const QString& outputPath)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this] { logView()->appendSection(QStringLiteral("生成 OCDT")); });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    using swt::edl::OcdtGenerator;
    using swt::edl::OcdtPlatform;

    runOnUi([this, projectId, platform, variant, modelCode] {
        logInfo(QStringLiteral("[OCDT] 平台=%1  项目号=%2  变体=%3%4")
                    .arg(OcdtGenerator::platformKey(platform))
                    .arg(projectId)
                    .arg(OcdtGenerator::variantKey(variant))
                    .arg(modelCode.isEmpty()
                             ? QString()
                             : QStringLiteral("  机型=%1").arg(modelCode)));
    });

    // OSIG 备份（可选）：读不到不中断，按「没有备份」处理并在日志里说明。
    QByteArray osigBackup;
    const bool osigRequested = !osigPath.isEmpty();
    if (osigRequested) {
        QFile backup(osigPath);
        if (backup.open(QIODevice::ReadOnly)) {
            osigBackup = backup.readAll();
            backup.close();
        } else {
            runOnUi([this, message = backup.errorString()] {
                logWarning(QStringLiteral("[OCDT] OSIG 备份读取失败，按无备份处理: %1")
                               .arg(message));
            });
        }
    }

    QString errorText;
    const bool ok = OcdtGenerator::generate(platform, projectId, variant, outputPath,
                                            modelCode, osigBackup, &errorText);
    const qsizetype osigSize = osigBackup.size();

    QPointer<AndroidToolsPage> guard(this);
    runOnUi([this, guard, ok, errorText, outputPath, projectId, platform, osigRequested,
             osigSize, started] {
        if (!guard) return;
        if (!ok) {
            logError(QStringLiteral("[OCDT] 生成失败: %1").arg(errorText));
            error(QStringLiteral("OCDT 生成失败"));
        } else {
            logSuccess(QStringLiteral("[OCDT] 已生成: %1（%2 字节）")
                           .arg(outputPath)
                           .arg(QFileInfo(outputPath).size()));

            if (platform == OcdtPlatform::Mtk8Mb && !OcdtGenerator::needsOsigBackup(projectId)) {
                logInfo(QStringLiteral("[OCDT] 该机型不在「旧款联发科」名单里，OSIG 块留零（正常）"));
            } else if (platform == OcdtPlatform::Mtk8Mb && !osigRequested) {
                logWarning(QStringLiteral(
                    "[OCDT] 旧款联发科机型未提供 OSIG 备份：本次写的是空 OSIG"
                    "（Version=0、无签名），刷入前请确认目标机是否需要有效签名"));
            } else if (platform == OcdtPlatform::Mtk8Mb
                       && osigSize < OcdtGenerator::OsigBackupMinLength) {
                logWarning(QStringLiteral("[OCDT] OSIG 备份只有 %1 字节（需 ≥ %2），"
                                          "按参考实现未写入 OSIG 块")
                               .arg(osigSize)
                               .arg(OcdtGenerator::OsigBackupMinLength));
            }
        }
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::analyzeOcdtCore(const QString& imagePath)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const QString report = swt::edl::OcdtGenerator::analyze(imagePath);
    const QStringList lines = report.split(QLatin1Char('\n'));

    QPointer<AndroidToolsPage> guard(this);
    runOnUi([this, guard, lines, imagePath, started] {
        if (!guard) return;
        logView()->appendSection(
            QStringLiteral("分析 OCDT: %1").arg(QFileInfo(imagePath).fileName()));
        for (const QString& line : lines) {
            if (line.trimmed().isEmpty()) continue;
            if (line.startsWith(QStringLiteral("[错误]"))) {
                logError(line);
            } else {
                logInfo(line);
            }
        }
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::twrpReadCore(const QString& part)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this, part] {
        logView()->appendSection(QStringLiteral("读取分区 (TWRP): %1").arg(part));
    });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    PlatformToolsManager tools;
    // 命令与 C# 逐字一致（含 su -c 的单引号包裹）。
    const auto r = tools.executeAdb(
        QStringLiteral("shell su -c 'dd if=/dev/block/by-name/%1 of=/sdcard/%1.img'").arg(part),
        10 * 60 * 1000);
    if (r.success) {
        // C# 是 `adb pull /sdcard/{part}.img .`（落到当前目录）；这里落到 exe 同目录。
        const QString dest = QCoreApplication::applicationDirPath();
        tools.executeAdb(QStringLiteral("pull /sdcard/%1.img \"%2\"").arg(part, dest),
                         10 * 60 * 1000);
        runOnUi([this, part, dest, started] {
            logSuccess(QStringLiteral("[TWRP] 读取完成: %1.img（已 pull 到 %2）").arg(part, dest));
            logView()->appendFooter(
                QStringLiteral("完成, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    } else {
        runOnUi([this, part, err = r.error, started] {
            logError(QStringLiteral("[TWRP] 读取失败: %1").arg(err));
            logView()->appendFooter(
                QStringLiteral("完成, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }
}

void AndroidToolsPage::twrpWriteCore(const QString& part, const QString& imageFile)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this, part] {
        logView()->appendSection(QStringLiteral("写入分区 (TWRP): %1").arg(part));
    });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    PlatformToolsManager tools;
    runOnUi([this, name = QFileInfo(imageFile).fileName()] {
        logInfo(QStringLiteral("[TWRP] 推送 %1...").arg(name));
    });
    tools.executeAdb(QStringLiteral("push \"%1\" /sdcard/%2.img")
                         .arg(QDir::toNativeSeparators(imageFile), part),
                     30 * 60 * 1000);

    runOnUi([this, part] { logInfo(QStringLiteral("[TWRP] 写入分区 %1...").arg(part)); });
    const auto r = tools.executeAdb(
        QStringLiteral("shell su -c 'dd if=/sdcard/%1.img of=/dev/block/by-name/%1'").arg(part),
        10 * 60 * 1000);

    const bool ok = r.success;
    runOnUi([this, part, ok, err = r.error, started] {
        if (ok) logSuccess(QStringLiteral("[TWRP] 写入完成: %1").arg(part));
        else logError(QStringLiteral("[TWRP] 写入失败: %1").arg(err));
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::twrpEraseCore(const QString& part)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this, part] {
        logView()->appendSection(QStringLiteral("擦除分区 (TWRP): %1").arg(part));
    });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    PlatformToolsManager tools;
    const auto r = tools.executeAdb(
        QStringLiteral("shell su -c 'dd if=/dev/zero of=/dev/block/by-name/%1 bs=1 count=1'")
            .arg(part),
        10 * 60 * 1000);

    const bool ok = r.success;
    runOnUi([this, part, ok, err = r.error, started] {
        if (ok) logSuccess(QStringLiteral("[TWRP] 擦除完成: %1").arg(part));
        else logError(QStringLiteral("[TWRP] 擦除失败: %1").arg(err));
        logView()->appendFooter(
            QStringLiteral("完成, 耗时: %1秒")
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

void AndroidToolsPage::twrpFlashZip()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择卡刷包"), QString(),
        QStringLiteral("卡刷包 (*.zip)"));
    if (file.isEmpty()) return;
    if (ThemedMessageBox::warning(
            this, QStringLiteral("刷入卡刷包"),
            QStringLiteral("将把卡刷包推到 /sdcard/Download 并交给 TWRP 执行 `twrp install`，"
                           "由 recovery 自己解包运行 updater-script。\n\n"
                           "设备必须**正处于 TWRP**（Android 系统里跑会直接失败）。\n"
                           "卡刷包内容不受本工具校验，请确认来源可信。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, file] { twrpFlashZipCore(file); }));
}

void AndroidToolsPage::twrpFlashZipCore(const QString& zipFile)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    runOnUi([this] { logView()->appendSection(QStringLiteral("刷入卡刷包")); });

    if (!MembershipService::guardAndConsume(QStringLiteral("Android 工具"),
                                            QStringLiteral("android.tools"))) {
        runOnUi([this] {
            logError(QStringLiteral("[工具] 会员额度校验未通过，已取消"));
            setBusy(false);
        });
        return;
    }

    PlatformToolsManager tools;
    const QString name = QFileInfo(zipFile).fileName();
    const QString remote = QStringLiteral("/sdcard/Download/%1").arg(name);

    runOnUi([this, name] {
        logInfo(QStringLiteral("[TWRP] 推送卡刷包 %1 → /sdcard/Download …").arg(name));
    });
    const auto push = tools.executeAdb(
        QStringLiteral("push \"%1\" %2").arg(QDir::toNativeSeparators(zipFile), remote),
        30 * 60 * 1000);
    if (!push.success) {
        runOnUi([this, err = push.error, started] {
            logError(QStringLiteral("[TWRP] 推送失败: %1").arg(err));
            logView()->appendFooter(
                QStringLiteral("失败, 耗时: %1秒")
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
        return;
    }

    runOnUi([this, remote] {
        logInfo(QStringLiteral("[TWRP] twrp install %1（由 recovery 执行脚本，耗时随包体）…")
                    .arg(remote));
    });
    const auto r = tools.executeAdb(
        QStringLiteral("shell twrp install %1").arg(remote), 30 * 60 * 1000);

    const bool ok = r.success;
    runOnUi([this, name, ok, out = r.output.trimmed(), err = r.error, started] {
        if (ok) {
            logSuccess(QStringLiteral("[TWRP] 卡刷包刷入完成: %1").arg(name));
        } else {
            logError(QStringLiteral("[TWRP] 刷入失败: %1").arg(err.isEmpty() ? out : err));
            // 最常见的原因是设备不在 TWRP（在 Android 里没有 twrp 这个命令）。
            logDetail(QStringLiteral("      请确认设备当前处于 TWRP recovery；"
                                     "在 Android 系统内执行会报 unknown command。"));
        }
        logView()->appendFooter(
            QStringLiteral("%1, 耗时: %2秒")
                .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  固件校验（MD5 / SHA-256）—— 移植自 TMFTool 的「MD5校验 / 校验Sha256 /
//  生成校验文件」。三条口径与 TMF 对齐：
//    · 单文件 / 整目录两种入口；
//    · 校验文件按 coreutils 格式写（`<hash>  <name>`），能被 sha256sum -c 直接吃；
//    · 「核对」读第三方校验文件逐条比对，区分 一致 / 不一致 / 文件缺失。
//  与 TMF 的差别：**不把结果上传云端**（TMF 有「上传日志」到自有服务器），
//  校验是纯本地操作，结果只进本页日志。
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// 校验时要跳过的「本身就是校验文件」的名字，避免把 SHA256SUMS 也算进去。
bool isChecksumArtifact(const QString& fileName)
{
    const QString lower = fileName.toLower();
    return lower == QLatin1String("md5sums") || lower == QLatin1String("sha256sums")
        || lower.endsWith(QLatin1String(".md5")) || lower.endsWith(QLatin1String(".sha256"))
        || lower.endsWith(QLatin1String(".sha1"));
}

QString humanSize(qint64 bytes)
{
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 1).arg(QLatin1String(units[unit]));
}

} // namespace

void AndroidToolsPage::browseChecksumTarget()
{
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要校验的固件"), QString(),
        QStringLiteral("固件 (*.img *.bin *.mbn *.elf *.zip *.ofp *.ops *.tar *.gz);;所有文件 (*.*)"));
    if (file.isEmpty()) return;
    m_checksumEdit->setText(QDir::toNativeSeparators(file));
}

void AndroidToolsPage::browseChecksumDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择固件目录（批量校验）"), QString());
    if (dir.isEmpty()) return;
    m_checksumEdit->setText(QDir::toNativeSeparators(dir));
}

void AndroidToolsPage::runChecksum()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString target = m_checksumEdit->text().trimmed();
    if (target.isEmpty()) {
        error(QStringLiteral("请先选择固件文件或目录"));
        return;
    }
    const int mode = m_checksumAlgo->currentIndex();
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, target, mode] { checksumCore(target, mode, false); }));
}

void AndroidToolsPage::writeChecksumFile()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString target = m_checksumEdit->text().trimmed();
    if (target.isEmpty()) {
        error(QStringLiteral("请先选择固件文件或目录"));
        return;
    }
    const int mode = m_checksumAlgo->currentIndex();
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, target, mode] { checksumCore(target, mode, true); }));
}

void AndroidToolsPage::verifyChecksumFile()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择校验文件"), QString(),
        QStringLiteral("校验文件 (*.md5 *.sha256 *.sha1 MD5SUMS SHA256SUMS);;所有文件 (*.*)"));
    if (file.isEmpty()) return;
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, file] { verifyChecksumCore(file); }));
}

void AndroidToolsPage::checksumCore(const QString& path, int algoMode, bool writeFile)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const QFileInfo info(path);
    const bool isDir = info.isDir();
    runOnUi([this, path, writeFile] {
        logView()->appendSection(writeFile ? QStringLiteral("生成校验文件")
                                           : QStringLiteral("固件校验"));
        logInfo(QStringLiteral("[校验] 目标: %1").arg(QDir::toNativeSeparators(path)));
    });

    if (!info.exists()) {
        runOnUi([this] {
            logError(QStringLiteral("[校验] 目标不存在"));
            setBusy(false);
        });
        return;
    }

    // 目标清单：单文件就是它自己；目录则取**第一层**文件（固件包都是一层的），
    // 跳过子目录与校验文件自身 —— 递归容易把缓存/日志目录一起算进去。
    QStringList files;
    if (isDir) {
        const QFileInfoList entries = QDir(path).entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& entry : entries) {
            if (entry.size() <= 0) continue;
            if (isChecksumArtifact(entry.fileName())) continue;
            files.push_back(entry.absoluteFilePath());
        }
    } else {
        files.push_back(info.absoluteFilePath());
    }

    if (files.isEmpty()) {
        runOnUi([this] {
            logWarning(QStringLiteral("[校验] 目录里没有可校验的文件"));
            setBusy(false);
        });
        return;
    }

    QVector<kc::crypto::HashAlgo> algos;
    if (algoMode == 1) {
        algos.push_back(kc::crypto::HashAlgo::Md5);
    } else if (algoMode == 2) {
        algos.push_back(kc::crypto::HashAlgo::Sha256);
        algos.push_back(kc::crypto::HashAlgo::Md5);
    } else {
        algos.push_back(kc::crypto::HashAlgo::Sha256);
    }

    // 结果按算法分开收集：写校验文件时要各写一份（SHA256SUMS / MD5SUMS）。
    QHash<int, QStringList> linesByAlgo;
    int okCount = 0;
    int failCount = 0;
    qint64 totalBytes = 0;

    for (const QString& file : files) {
        const QFileInfo fi(file);
        for (kc::crypto::HashAlgo algo : algos) {
            QString err;
            // 大文件每 25% 报一次进度，避免几 GB 的镜像「看起来卡住」。
            qint64 lastStep = -1;
            const QString digest = kc::crypto::hashFile(
                file, algo, &err, [this, &lastStep, &fi, algo](qint64 done, qint64 total) {
                    if (total < 256LL * 1024 * 1024) return;   // 小文件不刷屏
                    const qint64 step = total > 0 ? done * 4 / total : 0;
                    if (step == lastStep) return;
                    lastStep = step;
                    runOnUi([this, name = fi.fileName(), algo, done, total] {
                        logDetail(QStringLiteral("[校验] %1 %2 … %3%")
                                      .arg(kc::crypto::hashAlgoName(algo).toUpper(),
                                           name,
                                           QString::number(total > 0 ? done * 100 / total : 0)));
                    });
                });
            if (digest.isEmpty()) {
                ++failCount;
                runOnUi([this, name = fi.fileName(), algo, err] {
                    logError(QStringLiteral("[校验] %1 %2 失败: %3")
                                 .arg(kc::crypto::hashAlgoName(algo).toUpper(), name, err));
                });
                continue;
            }
            ++okCount;
            linesByAlgo[static_cast<int>(algo)].push_back(
                kc::tools::formatChecksumLine(digest, fi.fileName()));
            runOnUi([this, name = fi.fileName(), algo, digest, size = fi.size()] {
                logSuccess(QStringLiteral("[%1] %2  (%3)")
                               .arg(kc::crypto::hashAlgoName(algo).toUpper(), name,
                                    humanSize(size)));
                logDetail(QStringLiteral("      %1").arg(digest));
            });
        }
        totalBytes += fi.size();
    }

    // 写校验文件：单文件 → `<文件名>.sha256`；目录 → 同目录下的 SHA256SUMS / MD5SUMS。
    QStringList written;
    if (writeFile) {
        for (auto it = linesByAlgo.constBegin(); it != linesByAlgo.constEnd(); ++it) {
            const auto algo = static_cast<kc::crypto::HashAlgo>(it.key());
            const QString base = isDir ? QString()
                                       : QFileInfo(files.first()).fileName();
            const QString target = QDir(isDir ? path : info.absolutePath())
                                       .filePath(kc::tools::checksumFileName(base, algo));
            QFile out(target);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                runOnUi([this, target, e = out.errorString()] {
                    logError(QStringLiteral("[校验] 写校验文件失败: %1（%2）")
                                 .arg(QDir::toNativeSeparators(target), e));
                });
                continue;
            }
            const QString text = it.value().join(QLatin1Char('\n')) + QLatin1Char('\n');
            out.write(text.toUtf8());
            out.close();
            written.push_back(target);
            runOnUi([this, target, count = it.value().size()] {
                logSuccess(QStringLiteral("[校验] 已写出 %1 条 → %2")
                               .arg(count)
                               .arg(QDir::toNativeSeparators(target)));
            });
        }
    }

    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - started;
    runOnUi([this, okCount, failCount, totalBytes, elapsed, written] {
        if (failCount > 0)
            logWarning(QStringLiteral("[校验] %1 项成功 / %2 项失败").arg(okCount).arg(failCount));
        else
            logSuccess(QStringLiteral("[校验] 全部通过（%1 项）").arg(okCount));
        QString footer = QStringLiteral("共 %1，耗时 %2 秒")
                             .arg(humanSize(totalBytes))
                             .arg(elapsed / 1000.0, 0, 'f', 2);
        if (!written.isEmpty()) {
            footer += QStringLiteral("；校验文件 %1 个").arg(written.size());
            notify(QStringLiteral("校验文件已生成"), 1, 4000);
        } else if (failCount == 0) {
            notify(QStringLiteral("校验通过"), 1, 3000);
        } else {
            notify(QStringLiteral("有 %1 项校验失败").arg(failCount), 2, 5000);
        }
        logView()->appendFooter(footer);
        setBusy(false);
    });
}

void AndroidToolsPage::verifyChecksumCore(const QString& checksumFile)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const QFileInfo info(checksumFile);
    runOnUi([this, checksumFile] {
        logView()->appendSection(QStringLiteral("核对校验文件"));
        logInfo(QStringLiteral("[核对] %1").arg(QDir::toNativeSeparators(checksumFile)));
    });

    QFile in(checksumFile);
    if (!in.open(QIODevice::ReadOnly)) {
        runOnUi([this, e = in.errorString()] {
            logError(QStringLiteral("[核对] 打不开校验文件: %1").arg(e));
            setBusy(false);
        });
        return;
    }
    const QVector<kc::tools::ChecksumRecord> records =
        kc::tools::parseChecksumText(in.readAll());
    in.close();

    if (records.isEmpty()) {
        runOnUi([this] {
            logWarning(QStringLiteral("[核对] 没有解析出任何有效记录（格式应为 `<hash>  <文件名>`）"));
            setBusy(false);
        });
        return;
    }

    const QDir baseDir = info.absoluteDir();
    int matched = 0;
    int mismatched = 0;
    int missing = 0;

    for (const kc::tools::ChecksumRecord& record : records) {
        // 算法由摘要长度反推：32 位十六进制 = MD5，64 位 = SHA-256。
        const auto algo = record.hash.size() == 32 ? kc::crypto::HashAlgo::Md5
                                                   : kc::crypto::HashAlgo::Sha256;
        const QString target = baseDir.filePath(record.name);
        if (!QFileInfo::exists(target)) {
            ++missing;
            runOnUi([this, name = record.name] {
                logWarning(QStringLiteral("[核对] 文件缺失: %1").arg(name));
            });
            continue;
        }
        QString err;
        const QString actual = kc::crypto::hashFile(target, algo, &err);
        if (actual.isEmpty()) {
            ++mismatched;
            runOnUi([this, name = record.name, err] {
                logError(QStringLiteral("[核对] %1 计算失败: %2").arg(name, err));
            });
            continue;
        }
        if (actual.compare(record.hash, Qt::CaseInsensitive) == 0) {
            ++matched;
            runOnUi([this, name = record.name, algo] {
                logSuccess(QStringLiteral("[核对] 一致: %1（%2）")
                               .arg(name, kc::crypto::hashAlgoName(algo).toUpper()));
            });
        } else {
            ++mismatched;
            runOnUi([this, name = record.name, expect = record.hash, actual] {
                logError(QStringLiteral("[核对] 不一致: %1").arg(name));
                logDetail(QStringLiteral("      期望 %1").arg(expect));
                logDetail(QStringLiteral("      实际 %1").arg(actual));
            });
        }
    }

    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - started;
    runOnUi([this, matched, mismatched, missing, elapsed] {
        const bool allOk = (mismatched == 0 && missing == 0);
        if (allOk)
            logSuccess(QStringLiteral("[核对] %1 项全部一致").arg(matched));
        else
            logWarning(QStringLiteral("[核对] 一致 %1 / 不一致 %2 / 缺失 %3")
                           .arg(matched)
                           .arg(mismatched)
                           .arg(missing));
        notify(allOk ? QStringLiteral("校验文件核对通过")
                     : QStringLiteral("有 %1 项不通过").arg(mismatched + missing),
               allOk ? 1 : 2, 5000);
        logView()->appendFooter(
            QStringLiteral("耗时 %1 秒").arg(elapsed / 1000.0, 0, 'f', 2));
        setBusy(false);
    });
}

} // namespace kc::ui
