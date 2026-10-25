#include "ui/pages/XiaomiFastbootPage.h"

#include "kc/auth/MembershipService.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/ShellStatus.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include "ui/dialogs/ThemedMessageBox.h"
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
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

/// 三档刷写方式（与 C# ScriptOptions 完全一致）。
const QVector<QPair<QString, QString>>& scriptFiles()
{
    // title → file（wipes/locks 在 startFlash 里按下标判断）。
    static const QVector<QPair<QString, QString>> kScripts = {
        { QStringLiteral("全部清除"), QStringLiteral("flash_all.bat") },
        { QStringLiteral("保留用户数据"), QStringLiteral("flash_all_except_storage.bat") },
        { QStringLiteral("全部清除并回锁"), QStringLiteral("flash_all_lock.bat") },
    };
    return kScripts;
}

/// 统计脚本里的 fastboot flash 命令行数，作为进度分母（估算值，够用）。
int countFlashSteps(const QString& scriptPath)
{
    QFile f(scriptPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
    int count = 0;
    while (!f.atEnd()) {
        QString line = QString::fromLocal8Bit(f.readLine()).trimmed();
        if (line.startsWith(QStringLiteral("::")) ||
            line.startsWith(QStringLiteral("rem "), Qt::CaseInsensitive))
            continue;
        if (line.contains(QStringLiteral("fastboot"), Qt::CaseInsensitive) &&
            line.contains(QStringLiteral(" flash "), Qt::CaseInsensitive))
            ++count;
    }
    return count;
}

} // namespace

XiaomiFastbootPage::XiaomiFastbootPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("小米线刷"));
    buildUi();
    detectRom();
}

void XiaomiFastbootPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 2026-10-13：本页此前是「ROM 行 → 设备行 → 刷写方式 → 进度行」的裸行平铺，
    // 全部挤在同一条 10px 行距的竖流里。按功能收进卡片，对齐 FastbootFlashPage
    // 的 addCard 套路（QFrame#card + StyledPanel + #groupHeader 抬头，不造新类名）。
    auto addCard = [&](const QString& title) -> QVBoxLayout* {
        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("card"));
        card->setFrameShape(QFrame::StyledPanel);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        auto* header = new QLabel(title, card);
        header->setObjectName(QStringLiteral("groupHeader"));
        cardLayout->addWidget(header);

        auto* body = new QVBoxLayout;
        body->setSpacing(8);
        cardLayout->addLayout(body);
        layout->addWidget(card);
        return body;
    };

    // ── 卡 1：线刷包 ────────────────────────────────────────────────────
    QVBoxLayout* romCard = addCard(QStringLiteral("线刷包"));
    auto* romRow = new QHBoxLayout;
    romRow->setSpacing(8);
    m_romPathEdit = new QLineEdit(this);
    m_romPathEdit->setPlaceholderText(
        QStringLiteral("线刷包目录（根目录需含 flash_all.bat），或直接选官方 .tgz 自动解压"));
    connect(m_romPathEdit, &QLineEdit::textChanged, this,
            [this](const QString&) { detectRom(); });
    romRow->addWidget(m_romPathEdit, 1);

    m_browseFolderButton = new QPushButton(QStringLiteral("浏览目录"), this);
    m_browseTgzButton = new QPushButton(QStringLiteral("选择 .tgz"), this);
    m_rescanButton = new QPushButton(QStringLiteral("重新扫描"), this);
    for (QPushButton* b : { m_browseFolderButton, m_browseTgzButton, m_rescanButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        romRow->addWidget(b);
    }
    connect(m_browseFolderButton, &QPushButton::clicked, this, [this] { browseFolder(); });
    connect(m_browseTgzButton, &QPushButton::clicked, this, [this] { browseTgz(); });
    connect(m_rescanButton, &QPushButton::clicked, this, [this] { detectRom(); });
    romCard->addLayout(romRow);

    m_romInfoLabel = new QLabel(
        QStringLiteral("请选择解压后的线刷包目录（根目录需含 flash_all.bat），或直接选择官方 .tgz 线刷包自动解压"),
        this);
    m_romInfoLabel->setObjectName(QStringLiteral("statusLabel"));
    m_romInfoLabel->setWordWrap(true);
    romCard->addWidget(m_romInfoLabel);

    // ── 卡 2：设备 ──────────────────────────────────────────────────────
    QVBoxLayout* deviceCard = addCard(QStringLiteral("设备"));
    auto* deviceRow = new QHBoxLayout;
    deviceRow->setSpacing(8);
    m_deviceInfoLabel = new QLabel(
        QStringLiteral("未检测 · 请确认设备已进入 Fastboot 模式并安装驱动"), this);
    m_deviceInfoLabel->setWordWrap(true);
    m_detectButton = new QPushButton(QStringLiteral("检测设备"), this);
    m_detectButton->setObjectName(QStringLiteral("primaryButton"));
    m_detectButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_detectButton);
    connect(m_detectButton, &QPushButton::clicked, this, [this] { detectDevice(); });
    deviceRow->addWidget(m_deviceInfoLabel, 1);
    deviceRow->addWidget(m_detectButton);
    deviceCard->addLayout(deviceRow);

    // ── 卡 3：刷写（方式 + 进度）────────────────────────────────────────
    QVBoxLayout* flashCard = addCard(QStringLiteral("刷写"));
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);
    m_scriptCombo = new QComboBox(this);
    for (const auto& s : scriptFiles()) m_scriptCombo->addItem(s.first);
    actionRow->addWidget(new QLabel(QStringLiteral("刷写方式:"), this));
    actionRow->addWidget(m_scriptCombo);

    m_startButton = new QPushButton(QStringLiteral("开始刷写"), this);
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setEnabled(false);
    for (QPushButton* b : { m_startButton, m_stopButton }) {
        b->setCursor(Qt::PointingHandCursor);
        actionRow->addWidget(b);
    }
    m_busyButtons.append(m_startButton);
    actionRow->addStretch();
    connect(m_startButton, &QPushButton::clicked, this, [this] { startFlash(); });
    connect(m_stopButton, &QPushButton::clicked, this, [this] { stopFlash(); });
    flashCard->addLayout(actionRow);

    auto* progressRow = new QHBoxLayout;
    progressRow->setSpacing(8);
    m_stageLabel = new QLabel(QStringLiteral("空闲"), this);
    m_stageLabel->setObjectName(QStringLiteral("deviceModeLabel"));
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_pctLabel = new QLabel(QStringLiteral("0%"), this);
    m_pctLabel->setFixedWidth(40);
    progressRow->addWidget(m_stageLabel);
    progressRow->addWidget(m_progress, 1);
    progressRow->addWidget(m_pctLabel);
    flashCard->addLayout(progressRow);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);
}

void XiaomiFastbootPage::onActivated()
{
    detectRom();
}

// ─────────────────────────────────────────────────────────────────────────────
//  忙碌闸门
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiFastbootPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
    m_stopButton->setEnabled(busy);
    m_scriptCombo->setEnabled(!busy);
    m_romPathEdit->setEnabled(!busy);
}

void XiaomiFastbootPage::setProgress(int percent)
{
    runOnUi([this, percent] {
        m_progress->setValue(percent);
        m_pctLabel->setText(QStringLiteral("%1%").arg(percent));
    });
}

void XiaomiFastbootPage::setStage(const QString& text)
{
    runOnUi([this, text] {
        if (m_stageLabel) m_stageLabel->setText(text);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  ROM 选择 / 检测
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiFastbootPage::browseFolder()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择线刷包目录（根目录需含 flash_all.bat）"));
    if (dir.isEmpty()) return;
    m_romPathEdit->setText(dir);
}

void XiaomiFastbootPage::browseTgz()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString archive = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择小米官方线刷包（.tgz）"), QString(),
        QStringLiteral("小米线刷包 (*.tgz *.tar.gz);;所有文件 (*.*)"));
    if (archive.isEmpty()) return;

    // a.tgz → a；a.tar.gz → a.tar（再去一次尾巴）。
    QString destName = QFileInfo(archive).completeBaseName();
    if (destName.endsWith(QStringLiteral(".tar"), Qt::CaseInsensitive))
        destName.chop(4);
    const QString dest = QFileInfo(archive).absolutePath() + QStringLiteral("/") + destName;

    QDir destDir(dest);
    if (destDir.exists() && !destDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        error(QStringLiteral("解压目录已存在且非空，请先手动清理或重命名：\n%1").arg(dest));
        return;
    }

    setBusy(true);
    m_progress->setRange(0, 0);   // 忙碌指示
    setStage(QStringLiteral("正在解压线刷包…"));
    logView()->appendSection(QStringLiteral("解压线刷包"));
    logInfo(QStringLiteral("[小米线刷] 解压 %1").arg(archive));
    logInfo(QStringLiteral("[小米线刷] 目标目录 %1").arg(dest));
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QThreadPool::globalInstance()->start(new LambdaTask(
        [this, archive, dest, started] {
            PlatformToolsManager tools;
            const auto r = tools.extractTarGz(
                archive, dest, [this](const QString& line) { logInfo(QStringLiteral("[tar] %1").arg(line)); });
            const bool ok = r.success;
            runOnUi([this, ok, dest, r, started] {
                m_progress->setRange(0, 100);
                setProgress(0);
                setStage(QStringLiteral("空闲"));
                setBusy(false);
                const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
                logView()->appendFooter(
                    QStringLiteral("%1, 耗时: %2秒")
                        .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"),
                             QString::number(seconds, 'f', 2)));
                if (!ok) {
                    logError(QStringLiteral("[小米线刷] 解压失败: %1").arg(r.error));
                    error(QStringLiteral("解压失败: %1").arg(r.error));
                    return;
                }
                logSuccess(QStringLiteral("[小米线刷] 解压完成"));
                // 官方包有的带一层根目录，有的直接铺在根上：根上没有 flash_all.bat
                // 且只有唯一子目录时进入子目录。
                QString root = dest;
                if (!QFile::exists(root + QStringLiteral("/flash_all.bat"))) {
                    const QStringList dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    const QStringList files = QDir(root).entryList(QDir::Files);
                    if (dirs.size() == 1 && files.isEmpty())
                        root = root + QStringLiteral("/") + dirs.first();
                }
                m_romPathEdit->setText(root);
                success(QStringLiteral("解压完成，已加载线刷包"));
            });
        }));
}

void XiaomiFastbootPage::detectRom()
{
    if (!m_romPathEdit) return;
    m_romDir = m_romPathEdit->text().trimmed();
    if (m_romDir.isEmpty()) {
        m_romInfoLabel->setText(
            QStringLiteral("请选择解压后的线刷包目录（根目录需含 flash_all.bat），或直接选择官方 .tgz 线刷包自动解压"));
        return;
    }
    if (!QDir(m_romDir).exists()) {
        m_romInfoLabel->setText(QStringLiteral("目录不存在，请重新选择"));
        return;
    }

    int found = 0;
    QStringList summary;
    for (const auto& s : scriptFiles()) {
        const bool has = QFile::exists(m_romDir + QStringLiteral("/") + s.second);
        if (has) ++found;
        summary.append(QStringLiteral("%1 %2").arg(s.second, has ? QStringLiteral("有") : QStringLiteral("缺")));
    }

    const QString imagesDir = m_romDir + QStringLiteral("/images");
    int imageCount = -1;
    if (QDir(imagesDir).exists()) {
        imageCount = QDir(imagesDir).entryList(QStringList{ QStringLiteral("*.img"), QStringLiteral("*.mbn"),
                                                             QStringLiteral("*.bin") },
                                               QDir::Files)
                         .size();
    }

    QString text = found > 0
        ? QStringLiteral("刷写脚本 %1/3 可用（%2）").arg(found).arg(summary.join(QStringLiteral(" / ")))
        : QStringLiteral("未找到 flash_all 系列脚本，所选目录可能不是线刷包");
    text += imageCount >= 0 ? QStringLiteral(" ｜ images 镜像 %1 个").arg(imageCount)
                            : QStringLiteral(" ｜ 未找到 images 目录");

    m_romInfoLabel->setText(text);
    logInfo(QStringLiteral("[小米线刷] ROM 检测: %1").arg(text));
}

// ─────────────────────────────────────────────────────────────────────────────
//  设备检测
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiFastbootPage::detectDevice()
{
    if (m_busy.loadAcquire() != 0) return;
    setBusy(true);
    logView()->appendSection(QStringLiteral("检测设备"));
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, started] { detectDeviceCore(started); }));
}

void XiaomiFastbootPage::detectDeviceCore(qint64 started)
{
    PlatformToolsManager tools(
        [this](const QString& msg) { logInfo(QStringLiteral("· %1").arg(msg)); });
    if (!tools.ensureToolsExist()) {
        logError(QStringLiteral("[小米线刷] fastboot 工具不可用，请检查网络后重试"));
        error(QStringLiteral("fastboot 工具不可用，请检查网络后重试"));
        runOnUi([this] { setBusy(false); });
        return;
    }

    const auto r = tools.executeFastboot(QStringLiteral("devices"), 15000);
    const QString text = r.output + QStringLiteral("\n") + r.error;
    QString serial;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.endsWith(QStringLiteral("fastboot"))) {
            serial = line.section(QLatin1Char(' '), 0, 0);
            break;
        }
    }

    if (serial.isEmpty()) {
        logWarning(QStringLiteral("[小米线刷] 未检测到 Fastboot 设备"));
        runOnUi([this] {
            m_deviceInfoLabel->setText(
                QStringLiteral("未检测到 Fastboot 设备 · 请确认设备已进入 Fastboot 模式并安装驱动"));
            setBusy(false);
        });
        const double failSeconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("失败, 耗时: %1秒").arg(failSeconds, 0, 'f', 2));
        return;
    }

    const auto getVar = [&](const QString& key) {
        const auto v = tools.executeFastboot(QStringLiteral("getvar %1").arg(key), 10000);
        const QString out = v.output + QStringLiteral("\n") + v.error;
        for (const QString& raw : out.split(QLatin1Char('\n'))) {
            QString line = raw.trimmed();
            if (line.startsWith(QStringLiteral("(bootloader)"), Qt::CaseInsensitive))
                line = line.mid(12).trimmed();
            if (line.startsWith(key + QStringLiteral(":"))) {
                QString val = line.mid(key.size() + 1).trimmed();
                if (val.isEmpty() || val == QStringLiteral("(null)")) val = QStringLiteral("-");
                return val;
            }
        }
        return QStringLiteral("-");
    };

    const QString product = getVar(QStringLiteral("product"));
    const QString anti = getVar(QStringLiteral("anti"));
    const QString unlocked = getVar(QStringLiteral("unlocked"));
    const QString slot = getVar(QStringLiteral("current-slot"));
    const QString unlockedText = unlocked == QStringLiteral("yes") ? QStringLiteral("已解锁")
                                 : unlocked == QStringLiteral("no") ? QStringLiteral("未解锁")
                                                                    : QStringLiteral("未知");

    const QString info = QStringLiteral("序列号 %1 ｜ 机型 %2 ｜ 反回滚 %3 ｜ Bootloader %4 ｜ 槽位 %5")
                             .arg(serial, product, anti, unlockedText, slot);
    logSuccess(QStringLiteral("[小米线刷] 设备已连接: %1（%2）").arg(serial, product));

    runOnUi([this, info] {
        m_deviceInfoLabel->setText(info);
        setBusy(false);
    });
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    setShellStatus(serial, QStringLiteral("Fastboot 模式"), product);
}

// ─────────────────────────────────────────────────────────────────────────────
//  刷写
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiFastbootPage::startFlash()
{
    if (m_busy.loadAcquire() != 0) return;

    const int idx = m_scriptCombo->currentIndex();
    const auto& scripts = scriptFiles();
    const QPair<QString, QString> option = scripts.value(idx < 0 ? 0 : idx);
    const QString scriptPath = m_romDir + QStringLiteral("/") + option.second;
    const bool wipesData = idx != 1;        // 三档里只有「保留用户数据」不清数据
    const bool locksBl = idx == 2;

    if (m_romDir.isEmpty() || !QDir(m_romDir).exists()) {
        error(QStringLiteral("请先选择线刷包目录"));
        return;
    }
    if (!QFile::exists(scriptPath)) {
        error(QStringLiteral("刷写脚本不存在: %1\n请确认所选目录是官方线刷包，或换一种刷写方式")
                  .arg(option.second));
        return;
    }

    // 二次确认（回锁档带砖头警告，与 C# 一致）。
    QString confirmText;
    if (locksBl)
        confirmText = QStringLiteral("「%1」将清除全部用户数据并回锁 Bootloader。\n"
                                     "回锁后若刷写中断可能变砖，且数据无法恢复。确定继续？")
                          .arg(option.first);
    else if (wipesData)
        confirmText = QStringLiteral("「%1」将清除全部用户数据，确定继续？").arg(option.first);
    else
        confirmText = QStringLiteral("「%1」保留用户数据刷写。若系统版本变化仍可能出现异常，"
                                     "建议提前备份。确定继续？")
                          .arg(option.first);
    if (ThemedMessageBox::warning(this, QStringLiteral("确认刷写"), confirmText,
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    // 二次确认必须在扣额度之前（与 C# 一致：确认通过后才 GuardAndConsume）。
    if (!kc::MembershipService::guardAndConsume(QStringLiteral("小米线刷"),
                                                QStringLiteral("fastboot.flash")))
        return;

    setBusy(true);
    setProgress(0);
    setStage(QStringLiteral("准备刷写…"));
    m_cancel.storeRelease(0);
    logView()->appendSection(QStringLiteral("线刷刷写（%1）").arg(option.first));
    logInfo(QStringLiteral("[小米线刷] 开始刷写: %1（%2）").arg(option.first, option.second));
    logInfo(QStringLiteral("[小米线刷] ROM 目录: %1").arg(m_romDir));
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, scriptPath, title = option.first, romDir = m_romDir, started] {
            ShellStatus::beginTask(QStringLiteral("线刷刷写"));
            flashCore(scriptPath, title, romDir);
            ShellStatus::endTask();
            runOnUi([this] { setBusy(false); });
        }));
}

void XiaomiFastbootPage::stopFlash()
{
    if (m_busy.loadAcquire() == 0) return;
    logWarning(QStringLiteral("[小米线刷] 正在停止刷写进程…"));
    setStage(QStringLiteral("正在停止…"));
    m_cancel.storeRelease(1);
}

void XiaomiFastbootPage::flashCore(const QString& scriptPath, const QString& scriptTitle,
                                   const QString& romDir)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const auto cancelled = [this] { return m_cancel.loadAcquire() != 0; };

    PlatformToolsManager tools(
        [this](const QString& msg) { logInfo(QStringLiteral("· %1").arg(msg)); });
    if (!tools.ensureToolsExist()) {
        logError(QStringLiteral("[小米线刷] fastboot 工具不可用，请检查网络后重试"));
        error(QStringLiteral("fastboot 工具不可用，请检查网络后重试"));
        setStage(QStringLiteral("刷写失败"));
        logView()->appendFooter(QStringLiteral("失败, 耗时: 0.00秒"));
        return;
    }

    // 设备必须在线：没有 fastboot 设备就不起脚本。
    const auto dev = tools.executeFastboot(QStringLiteral("devices"), 15000);
    if (!dev.success ||
        !(dev.output + dev.error).contains(QStringLiteral("fastboot"), Qt::CaseInsensitive)) {
        error(QStringLiteral("未检测到 Fastboot 设备，请确认设备已进入 Fastboot 模式并安装驱动"));
        setStage(QStringLiteral("刷写失败"));
        logView()->appendFooter(QStringLiteral("失败, 耗时: 0.00秒"));
        return;
    }

    int estSteps = countFlashSteps(scriptPath);
    if (estSteps <= 0) {
        logWarning(QStringLiteral("[小米线刷] 未从脚本解析到 fastboot 刷写命令，进度将按完成事件推进"));
        estSteps = 1;
    }
    logInfo(QStringLiteral("[小米线刷] 预计刷写步骤 %1 步").arg(estSteps));

    // 行分类（等价 C# OnScriptLine，LogView append* 线程安全，直接在工作线程调）。
    int doneSteps = 0;
    bool failedSeen = false;
    const auto onLine = [&](const QString& line) {
        if (line.contains(QStringLiteral("FAILED"), Qt::CaseInsensitive)) {
            failedSeen = true;
            logError(line);
            setStage(QStringLiteral("出现失败步骤"));
            return;
        }
        if (line.contains(QStringLiteral("Finished. Total time"))) {
            ++doneSteps;
            setProgress(qMin(100, doneSteps * 100 / estSteps));
            ShellStatus::report(std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                qMin(100.0, doneSteps * 100.0 / estSteps), -1.0);
            logInfo(line);
        } else if (line.contains(QStringLiteral("Sending '")) ||
                   line.contains(QStringLiteral("Writing '"))) {
            setStage(line.trimmed());
            // Sending/Writing 'xxx' → 状态栏分区名同步推进。
            const int q1 = line.indexOf(QLatin1Char('\''));
            const int q2 = line.indexOf(QLatin1Char('\''), q1 + 1);
            if (q1 >= 0 && q2 > q1)
                ShellStatus::report(std::nullopt, line.mid(q1 + 1, q2 - q1 - 1), std::nullopt,
                                    std::nullopt, std::nullopt, -1.0);
            logInfo(line);
        } else {
            logInfo(line);
        }
    };

    logInfo(QStringLiteral("[小米线刷] 启动脚本：%1（工作目录 = ROM 目录）").arg(scriptTitle));
    const auto r = tools.executeScriptStreaming(scriptPath, romDir, onLine, cancelled);

    if (r.cancelled) {
        logWarning(QStringLiteral("[小米线刷] 已手动停止，设备可能处于半刷写状态，建议重新完整刷写"));
        error(QStringLiteral("刷写已停止"));
        setStage(QStringLiteral("已停止"));
        return;
    }
    if (r.exitCode == 0 && !failedSeen) {
        setProgress(100);
        setStage(QStringLiteral("刷写完成"));
        logSuccess(QStringLiteral("[小米线刷] 线刷完成"));
        success(QStringLiteral("线刷完成，设备将自动重启"));
    } else {
        setStage(QStringLiteral("刷写失败"));
        const QString reason = failedSeen ? QStringLiteral("脚本输出含 FAILED")
                                          : QStringLiteral("退出码 %1").arg(r.exitCode);
        logError(QStringLiteral("[小米线刷] 线刷失败（%1），请检查日志中的红字与镜像/机型是否匹配")
                     .arg(reason));
        error(QStringLiteral("线刷失败（%1）").arg(reason));
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(
        QStringLiteral("%1, 耗时: %2秒")
            .arg((r.exitCode == 0 && !failedSeen && !r.cancelled) ? QStringLiteral("完成")
                                                                  : QStringLiteral("失败"),
                 QString::number(seconds, 'f', 2)));
}

} // namespace kc::ui
