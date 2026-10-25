#include "ui/pages/OnePlusFlashPage.h"

#include "kc/auth/MembershipService.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/ShellStatus.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"
#include "ui/widgets/ToolProgressBar.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include "ui/dialogs/ThemedMessageBox.h"
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QThreadPool>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

QString formatSeconds(qint64 started)
{
    return QStringLiteral("%1秒").arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0,
                                      0, 'f', 2);
}

} // namespace

OnePlusFlashPage::OnePlusFlashPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("一加刷写"));
    buildUi();
}

OnePlusFlashPage::~OnePlusFlashPage() = default;

void OnePlusFlashPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 2026-10-13：裸行平铺 → 按功能收进卡片（对齐 FastbootFlashPage 的 addCard 套路）。
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

    // ── 卡 1：刷写包（本地 + 云端）──────────────────────────────────────
    QVBoxLayout* packageCard = addCard(QStringLiteral("刷写包"));
    auto* localRow = new QHBoxLayout;
    localRow->setSpacing(8);
    m_localFileEdit = new QLineEdit(this);
    m_localFileEdit->setPlaceholderText(QStringLiteral("刷写包路径（.ofp / .ops / .ozip / .zip / .bin）"));
    m_browseButton = new QPushButton(QStringLiteral("浏览…"), this);
    m_browseButton->setCursor(Qt::PointingHandCursor);
    connect(m_browseButton, &QPushButton::clicked, this, [this] { browseLocal(); });
    localRow->addWidget(m_localFileEdit, 1);
    localRow->addWidget(m_browseButton);
    packageCard->addLayout(localRow);

    // 停更离线版（2.0.6）：「云端刷写包下载」整行已移除（服务器关停）。
    // 刷写包一律通过上方「本地刷写包」选择本地文件。

    // ── 卡 2：刷写操作（动作按钮 + 进度）────────────────────────────────
    QVBoxLayout* actionCard = addCard(QStringLiteral("刷写操作"));
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);

    m_startButton = new QPushButton(QStringLiteral("一键刷写"), this);
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_fastbootDButton = new QPushButton(QStringLiteral("重启FastbootD"), this);
    m_formatDataButton = new QPushButton(QStringLiteral("格式化数据"), this);
    m_extractButton = new QPushButton(QStringLiteral("提取Payload"), this);
    // 2026-10-20 合规下线：「擦除 FRP」按钮已移除。
    // FRP 是厂商防盗机制，工具不应提供针对它的专项操作 —— 服务端也把 oneplus.frp
    // 从功能目录里删了（旧版本调 /api/app/check 会拿到「未知功能项」而被拒绝）。
    for (QPushButton* b : { m_startButton, m_stopButton, m_fastbootDButton,
                            m_formatDataButton, m_extractButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        actionRow->addWidget(b);
    }
    actionRow->addStretch();
    actionCard->addLayout(actionRow);

    connect(m_startButton, &QPushButton::clicked, this, [this] { startFlash(); });
    connect(m_stopButton, &QPushButton::clicked, this, [this] { stopFlash(); });
    connect(m_fastbootDButton, &QPushButton::clicked, this, [this] { rebootFastbootD(); });
    connect(m_formatDataButton, &QPushButton::clicked, this, [this] { formatData(); });
    connect(m_extractButton, &QPushButton::clicked, this, [this] { extractPayload(); });

    m_progress = new ToolProgressBar(this);
    m_progressLabel = new QLabel(this);
    m_progressLabel->setObjectName(QStringLiteral("deviceHintLabel"));
    actionCard->addWidget(m_progress);
    actionCard->addWidget(m_progressLabel);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 1);
}

void OnePlusFlashPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
}

void OnePlusFlashPage::browseLocal()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择刷写包"), QString(),
        QStringLiteral("OPLUS刷写包 (*.ofp *.ops *.ozip *.zip);;Payload文件 (*.bin);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    m_localFileEdit->setText(path);
}

// ─────────────────────────────────────────────────────────────────────────────
//  云端下载：已随服务器功能移除（停更离线版 2.0.6）
// ─────────────────────────────────────────────────────────────────────────────

void OnePlusFlashPage::stopFlash()
{
    m_flashCancel.storeRelease(1);
    m_progress->setValue(0);
    m_progressLabel->setText(QStringLiteral("已停止"));
    logInfo(QStringLiteral("[欧加线刷] 刷写已停止"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  一键刷写（对应 C# BtnStartFlash_Click，按扩展名分流）
// ─────────────────────────────────────────────────────────────────────────────

void OnePlusFlashPage::startFlash()
{
    if (m_busy.loadAcquire() != 0) return;

    const QString file = m_localFileEdit->text().trimmed();
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        error(QStringLiteral("请选择有效的刷写包文件"));
        return;
    }

    const QString ext = QFileInfo(file).suffix().toLower();
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("OnePlus 一键刷写"),
                                              QStringLiteral("oneplus.flash")))
        return;
    if (ThemedMessageBox::warning(this, QStringLiteral("确认刷写"),
                             QStringLiteral("确认开始刷写？\n文件: %1").arg(file),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!kc::MembershipService::consume(QStringLiteral("oneplus.flash"))) return;

    logView()->appendSection(QStringLiteral("线刷刷写"));
    m_progress->setValue(0);
    m_progressLabel->setText(QStringLiteral("刷写中..."));
    setBusy(true);
    m_flashCancel.storeRelease(0);

    QThreadPool::globalInstance()->start(new LambdaTask([this, file] {
        ShellStatus::beginTask(QStringLiteral("线刷刷写"));
        flashCore(file);
        ShellStatus::endTask();
    }));
}

void OnePlusFlashPage::flashCore(const QString& file)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const QString ext = QFileInfo(file).suffix().toLower();
    bool ok = false;

    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logError(QStringLiteral("[欧加线刷] 平台工具不可用，请检查网络后重试"));
        logView()->appendFooter(QStringLiteral("失败, 耗时: %1").arg(formatSeconds(started)));
        runOnUi([this] {
            m_progressLabel->setText(QStringLiteral("刷写失败"));
            setBusy(false);
        });
        return;
    }

    if (ext == QStringLiteral("ofp") || ext == QStringLiteral("ops")) {
        // 对应 C#：`adb edl "<file>"`。
        logInfo(QStringLiteral("[欧加线刷] 使用edl刷写 %1").arg(QFileInfo(file).fileName()));
        logView()->beginOperation(QStringLiteral("adb edl %1").arg(QFileInfo(file).fileName()));
        const auto r = tools.executeAdb(QStringLiteral("edl \"%1\"").arg(file), 60 * 60 * 1000);
        logView()->appendOperation(QStringLiteral("adb edl %1").arg(QFileInfo(file).fileName()), r.success);
        ok = r.success;
        if (ok) {
            logSuccess(QStringLiteral("[欧加线刷] 刷写完成"));
        } else {
            logError(QStringLiteral("[欧加线刷] 刷写失败: %1").arg(r.error));
        }
    } else if (ext == QStringLiteral("ozip") || ext == QStringLiteral("zip")) {
        // 本地解压（C# 用 ZipFile；Qt 侧用系统自带 bsdtar —— Windows 10+ 的 tar.exe 支持 zip）。
        logInfo(QStringLiteral("[欧加线刷] 解压刷写包..."));
        const QString extractDir = QDir::temp().filePath(
            QStringLiteral("KunClient_oneplus_%1")
                .arg(QDateTime::currentMSecsSinceEpoch(), 8, 16, QLatin1Char('0')));
        QDir().mkpath(extractDir);

        QProcess unzip;
        unzip.setProgram(QStringLiteral("tar"));
        unzip.setNativeArguments(QStringLiteral("-xf \"%1\" -C \"%2\"").arg(file, extractDir));
        unzip.start();
        if (!unzip.waitForStarted(5000) || !unzip.waitForFinished(30 * 60 * 1000) ||
            unzip.exitCode() != 0) {
            logError(QStringLiteral("[欧加线刷] 解压失败: %1")
                         .arg(QString::fromLocal8Bit(unzip.readAllStandardError()).trimmed()));
            logView()->appendFooter(QStringLiteral("失败, 耗时: %1").arg(formatSeconds(started)));
            runOnUi([this] {
                m_progressLabel->setText(QStringLiteral("解压失败"));
                setBusy(false);
            });
            return;
        }

        const QString payloadBin = QDir(extractDir).filePath(QStringLiteral("payload.bin"));
        if (QFileInfo::exists(payloadBin)) {
            logInfo(QStringLiteral("[欧加线刷] 检测到payload.bin，使用fastboot刷写..."));
            ok = flashPayloadCore(extractDir);
        } else {
            const QStringList imgs =
                QDir(extractDir).entryList({ QStringLiteral("*.img") }, QDir::Files);
            if (!imgs.isEmpty()) {
                logInfo(QStringLiteral("[欧加线刷] 找到 %1 个镜像，逐个刷写...").arg(imgs.size()));
                for (int i = 0; i < imgs.size(); ++i) {
                    if (m_flashCancel.loadAcquire() != 0) { ok = false; break; }
                    const QString imgName = QFileInfo(imgs.at(i)).completeBaseName();
                    const QString imgPath = QDir(extractDir).filePath(imgs.at(i));
                    const auto r = tools.executeFastboot(
                        QStringLiteral("flash %1 \"%2\"").arg(imgName, imgPath), 600000);
                    if (!r.success)
                        logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
                    const double pct = static_cast<double>(i + 1) / imgs.size() * 100.0;
                    ShellStatus::report(std::nullopt, imgName, std::nullopt, std::nullopt, pct, -1.0);
                    runOnUi([this, pct] { m_progress->setValue(pct); });
                }
                ok = !m_flashCancel.loadAcquire();
                if (ok) logSuccess(QStringLiteral("[欧加线刷] 刷写完成"));
            } else {
                logError(QStringLiteral("[欧加线刷] 未找到可刷写的镜像文件"));
            }
        }
    } else if (ext == QStringLiteral("bin")) {
        logInfo(QStringLiteral("[欧加线刷] 使用fastboot刷写payload.bin..."));
        ok = flashPayloadCore(file);
    } else {
        logError(QStringLiteral("[欧加线刷] 不认识的刷写包类型: .%1").arg(ext));
    }

    logView()->appendFooter(QStringLiteral("%1, 耗时: %2")
                                .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"),
                                     formatSeconds(started)));
    runOnUi([this, ok] {
        if (ok) {
            m_progress->setValue(100);
            m_progressLabel->setText(QStringLiteral("刷写完成"));
            success(QStringLiteral("刷写完成"));
        } else {
            m_progressLabel->setText(QStringLiteral("刷写失败"));
        }
        setBusy(false);
    });
}

/// C# FlashPayloadAsync：flash_all.bat → rawprogram*.xml → 逐个 *.img。
bool OnePlusFlashPage::flashPayloadCore(const QString& payloadPath)
{
    const QString payloadDir = QFileInfo(payloadPath).absolutePath();

    const QString flashAllBat = QDir(payloadDir).filePath(QStringLiteral("flash_all.bat"));
    if (QFileInfo::exists(flashAllBat)) {
        logInfo(QStringLiteral("[欧加线刷] 执行 flash_all.bat..."));
        PlatformToolsManager tools;
        const auto r = tools.executeScriptStreaming(flashAllBat, payloadDir,
                                                    [this](const QString& line) {
                                                        if (!line.trimmed().isEmpty())
                                                            logInfo(line.trimmed());
                                                    });
        if (r.exitCode != 0) {
            logError(QStringLiteral("[欧加线刷] flash_all.bat 失败，退出码 %1（详见上方脚本输出）")
                         .arg(r.exitCode));
            return false;
        }
        logSuccess(QStringLiteral("[欧加线刷] flash_all.bat 执行完成"));
        return true;
    }

    // rawprogram*.xml（照 C# 的命令口径移植）。
    const QStringList rawprograms =
        QDir(payloadDir).entryList({ QStringLiteral("rawprogram*.xml") }, QDir::Files);
    if (!rawprograms.isEmpty()) {
        PlatformToolsManager tools;
        for (const QString& xml : rawprograms) {
            if (m_flashCancel.loadAcquire() != 0) return false;
            logInfo(QStringLiteral("[欧加线刷] 刷写: %1").arg(xml));
            const QString xmlPath = QDir(payloadDir).filePath(xml);
            const auto r = tools.executeFastboot(
                QStringLiteral("flash rawprogram \"%1\"").arg(xmlPath), 600000);
            if (!r.success)
                logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
        }
        return true;
    }

    // 逐个 *.img。
    const QStringList imgs = QDir(payloadDir).entryList({ QStringLiteral("*.img") }, QDir::Files);
    PlatformToolsManager tools;
    for (int i = 0; i < imgs.size(); ++i) {
        if (m_flashCancel.loadAcquire() != 0) return false;
        const QString name = QFileInfo(imgs.at(i)).completeBaseName();
        const QString imgPath = QDir(payloadDir).filePath(imgs.at(i));
        logInfo(QStringLiteral("[欧加线刷] flash %1...").arg(name));
        const auto r = tools.executeFastboot(
            QStringLiteral("flash %1 \"%2\"").arg(name, imgPath), 600000);
        if (!r.success)
            logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
        const double pct = static_cast<double>(i + 1) / imgs.size() * 100.0;
        runOnUi([this, pct] { m_progress->setValue(pct); });
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  单命令按钮（对应 C# RunAdbCommand / RunFastbootCommand）
// ─────────────────────────────────────────────────────────────────────────────

void OnePlusFlashPage::rebootFastbootD()
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("重启到FastbootD模式"));
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this] { runAdbCore(QStringLiteral("reboot fastboot"), QStringLiteral("重启到FastbootD模式")); }));
}

void OnePlusFlashPage::formatData()
{
    if (m_busy.loadAcquire() != 0) return;
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("Fastboot 擦除 / 格式化"),
                                              QStringLiteral("fastboot.erase")))
        return;
    if (ThemedMessageBox::warning(this, QStringLiteral("确认"),
                             QStringLiteral("格式化数据会清除所有用户数据，确定？"),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!kc::MembershipService::consume(QStringLiteral("fastboot.erase"))) return;
    logView()->appendSection(QStringLiteral("格式化数据"));
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this] { runFastbootCore(QStringLiteral("-w"), QStringLiteral("格式化数据")); }));
}

void OnePlusFlashPage::extractPayload()
{
    if (m_busy.loadAcquire() != 0) return;

    const QString file = m_localFileEdit->text().trimmed();
    if (file.isEmpty() || !file.endsWith(QStringLiteral(".bin"), Qt::CaseInsensitive)) {
        error(QStringLiteral("请选择payload.bin文件"));
        return;
    }

    if (!kc::MembershipService::ensureAllowed(QStringLiteral("Payload 分区提取"),
                                              QStringLiteral("payload.extract")))
        return;
    const QString parentDir =
        QFileDialog::getExistingDirectory(this, QStringLiteral("选择提取目录"));
    if (parentDir.isEmpty()) return;
    if (!kc::MembershipService::consume(QStringLiteral("payload.extract"))) return;

    logView()->appendSection(QStringLiteral("提取 Payload"));
    logInfo(QStringLiteral("[欧加线刷] 提取Payload: %1").arg(file));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, file, parentDir] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        const QString extractDir = QDir(parentDir).filePath(QStringLiteral("payload_extracted"));
        QDir().mkpath(extractDir);
        PlatformToolsManager tools;
        tools.ensureToolsExist();
        const auto r = tools.executeAdb(
            QStringLiteral("payload unpack -o \"%1\" -i \"%2\"").arg(extractDir, file), 600000);
        if (r.success) {
            logSuccess(QStringLiteral("[欧加线刷] 提取完成: %1").arg(extractDir));
            runOnUi([this] { m_progressLabel->setText(QStringLiteral("提取完成")); });
        } else {
            logError(QStringLiteral("[欧加线刷] 提取失败: %1").arg(r.error));
            runOnUi([this] { m_progressLabel->setText(QStringLiteral("提取失败")); });
        }
        logView()->appendFooter(QStringLiteral("%1, 耗时: %2")
                                    .arg(r.success ? QStringLiteral("完成") : QStringLiteral("失败"),
                                         formatSeconds(started)));
        runOnUi([this] { setBusy(false); });
    }));
}

void OnePlusFlashPage::runAdbCore(const QString& args, const QString& desc)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    bool ok = false;
    logInfo(QStringLiteral("[ADB] %1: adb %2").arg(desc, args));
    PlatformToolsManager tools;
    if (tools.ensureToolsExist()) {
        const auto r = tools.executeAdb(args, 120000);
        ok = r.success;
        if (ok && !r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
        if (!r.error.trimmed().isEmpty()) logWarning(QStringLiteral("[ADB错误] %1").arg(r.error.trimmed()));
    } else {
        logError(QStringLiteral("[ADB] 平台工具不可用，请检查网络后重试"));
    }
    logView()->appendFooter(QStringLiteral("%1, 耗时: %2")
                                .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"),
                                     formatSeconds(started)));
    runOnUi([this] { setBusy(false); });
}

void OnePlusFlashPage::runFastbootCore(const QString& args, const QString& desc)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    bool ok = false;
    logInfo(QStringLiteral("[Fastboot] %1: fastboot %2").arg(desc, args));
    PlatformToolsManager tools;
    if (tools.ensureToolsExist()) {
        const auto r = tools.executeFastboot(args, 120000);
        ok = r.success;
        if (ok && !r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
        if (!r.error.trimmed().isEmpty())
            logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
    } else {
        logError(QStringLiteral("[Fastboot] 平台工具不可用，请检查网络后重试"));
    }
    logView()->appendFooter(QStringLiteral("%1, 耗时: %2")
                                .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"),
                                     formatSeconds(started)));
    runOnUi([this] { setBusy(false); });
}

} // namespace kc::ui
