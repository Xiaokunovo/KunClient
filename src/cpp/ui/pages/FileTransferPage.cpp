#include "ui/pages/FileTransferPage.h"

#include "kc/auth/MembershipService.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>
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

/// 设备路径的上级目录（等价 C# Path.GetDirectoryName，统一 '/' 分隔）。
QString deviceParentDir(const QString& path)
{
    QString p = path.trimmed();
    while (p.endsWith(QLatin1Char('/')) && p.size() > 1) p.chop(1);
    const int slash = p.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) return QStringLiteral("/");
    return p.left(slash + 1);
}

} // namespace

FileTransferPage::FileTransferPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("文件传输"));
    buildUi();
}

void FileTransferPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 2026-10-12：本页此前是「进度条 → 推送两行 → 拉取一行 → 安装一行 →
    // 浏览一行 → 文件列表 → 拖放区 → 日志」一整条平铺流 —— 推/拉/装/浏览
    // 四件事之间没有边界，进度条还贴在页面最上沿，拖放区连边框都没有。
    // 现在按功能收进卡片，拖放区补虚线框（QSS #dropZone）。
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

    // ── 卡 1：传输进度 + 存储余量行 ─────────────────────────────────────
    QVBoxLayout* progressCard = addCard(QStringLiteral("传输进度"));
    auto* progressRow = new QHBoxLayout;
    progressRow->setSpacing(10);
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setMaximumHeight(6);
    progressRow->addWidget(m_progressBar, 1);

    m_progressText = new QLabel(QStringLiteral("等待中"), this);
    m_progressText->setObjectName(QStringLiteral("statusValue"));
    progressRow->addWidget(m_progressText);

    auto* storageKey = new QLabel(QStringLiteral("存储剩余:"), this);
    storageKey->setObjectName(QStringLiteral("statusLabel"));
    progressRow->addWidget(storageKey);
    m_storageValue = new QLabel(QStringLiteral("-"), this);
    m_storageValue->setObjectName(QStringLiteral("statusValue"));
    progressRow->addWidget(m_storageValue);
    progressCard->addLayout(progressRow);

    // ── 卡 2：推送文件到设备 ───────────────────────────────────────────
    QVBoxLayout* pushLayout = addCard(QStringLiteral("推送到设备"));
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_pushLocal = new QLineEdit(this);
        m_pushLocal->setPlaceholderText(QStringLiteral("本地文件路径"));
        row->addWidget(m_pushLocal, 1);
        auto* browse = new QPushButton(QStringLiteral("选择文件"), this);
        browse->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择要推送的文件"), QString());
            if (!path.isEmpty()) m_pushLocal->setText(path);
        });
        row->addWidget(browse);
        pushLayout->addLayout(row);
    }
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_pushRemote = new QComboBox(this);
        m_pushRemote->setEditable(true);
        m_pushRemote->setInsertPolicy(QComboBox::NoInsert);
        m_pushRemote->setCurrentText(QStringLiteral("/sdcard/"));
        m_pushRemote->setMinimumContentsLength(24);
        row->addWidget(m_pushRemote, 1);
        auto* push = new QPushButton(QStringLiteral("推送"), this);
        push->setObjectName(QStringLiteral("primaryButton"));
        push->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(push);
        connect(push, &QPushButton::clicked, this, [this] {
            const QString local = m_pushLocal->text().trimmed();
            const QString remote = m_pushRemote->currentText().trimmed();
            if (local.isEmpty() || remote.isEmpty()) {
                error(QStringLiteral("请选择文件和目标路径"));
                return;
            }
            if (!QFileInfo::exists(local)) {
                error(QStringLiteral("本地文件不存在"));
                return;
            }
            pushFiles({ local });
        });
        row->addWidget(push);
        pushLayout->addLayout(row);
    }

    // ── 卡 3：从设备拉取 ───────────────────────────────────────────────
    QVBoxLayout* pullCard = addCard(QStringLiteral("从设备拉取"));
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* key = new QLabel(QStringLiteral("设备路径:"), this);
        key->setObjectName(QStringLiteral("statusLabel"));
        row->addWidget(key);
        m_pullRemote = new QComboBox(this);
        m_pullRemote->setEditable(true);
        m_pullRemote->setInsertPolicy(QComboBox::NoInsert);
        m_pullRemote->setCurrentText(QStringLiteral("/sdcard/"));
        m_pullRemote->setMinimumContentsLength(24);
        row->addWidget(m_pullRemote, 1);
        auto* pull = new QPushButton(QStringLiteral("拉取"), this);
        pull->setObjectName(QStringLiteral("primaryButton"));
        pull->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(pull);
        connect(pull, &QPushButton::clicked, this, [this] { pullFile(); });
        row->addWidget(pull);
        pullCard->addLayout(row);
    }

    // ── 卡 4：批量安装 APK ─────────────────────────────────────────────
    QVBoxLayout* apkCard = addCard(QStringLiteral("批量安装 APK"));
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_batchDir = new QLineEdit(this);
        m_batchDir->setPlaceholderText(QStringLiteral("包含 *.apk 的本地目录"));
        row->addWidget(m_batchDir, 1);
        auto* browse = new QPushButton(QStringLiteral("选择目录"), this);
        browse->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择 APK 目录"));
            if (!dir.isEmpty()) m_batchDir->setText(dir);
        });
        row->addWidget(browse);
        auto* install = new QPushButton(QStringLiteral("批量安装"), this);
        install->setObjectName(QStringLiteral("primaryButton"));
        install->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(install);
        connect(install, &QPushButton::clicked, this, [this] { batchInstall(); });
        row->addWidget(install);
        apkCard->addLayout(row);
    }

    // ── 卡 5：设备文件浏览（目录 + 列表）───────────────────────────────
    // 列表要吃掉剩余高度，所以这里手写一张带 stretch 的卡（addCard 的 body
    // 不带 stretch 参数）。
    auto* filesCard = new QFrame(this);
    filesCard->setObjectName(QStringLiteral("card"));
    filesCard->setFrameShape(QFrame::StyledPanel);
    auto* filesLayout = new QVBoxLayout(filesCard);
    filesLayout->setContentsMargins(14, 12, 14, 12);
    filesLayout->setSpacing(8);
    auto* filesHeader = new QLabel(QStringLiteral("设备文件"), filesCard);
    filesHeader->setObjectName(QStringLiteral("groupHeader"));
    filesLayout->addWidget(filesHeader);
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        m_devicePath = new QLineEdit(this);
        m_devicePath->setText(QStringLiteral("/sdcard/"));
        row->addWidget(m_devicePath, 1);
        auto* refresh = new QPushButton(QStringLiteral("刷新"), this);
        refresh->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(refresh);
        connect(refresh, &QPushButton::clicked, this, [this] { browseDevice(); });
        row->addWidget(refresh);
        auto* parent = new QPushButton(QStringLiteral("上级目录"), this);
        parent->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(parent);
        connect(parent, &QPushButton::clicked, this, [this] {
            m_devicePath->setText(deviceParentDir(m_devicePath->text()));
            browseDevice();
        });
        row->addWidget(parent);
        filesLayout->addLayout(row);
    }
    m_deviceFiles = new QListWidget(filesCard);
    m_deviceFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_deviceFiles->setUniformItemSizes(true);
    m_deviceFiles->setWordWrap(false);
    filesLayout->addWidget(m_deviceFiles, 1);
    layout->addWidget(filesCard, 2);

    // ── 拖放传输（补了虚线框，见 QSS #dropZone：此前没有任何规则，
    //    只有一个居中文字浮在白底上，看不出是个放置区）────────────────
    m_dropZone = new QLabel(QStringLiteral("拖放文件到此处推送"), this);
    m_dropZone->setObjectName(QStringLiteral("dropZone"));
    m_dropZone->setAlignment(Qt::AlignCenter);
    m_dropZone->setMinimumHeight(64);
    m_dropZone->setAcceptDrops(true);
    m_dropZone->installEventFilter(this);
    layout->addWidget(m_dropZone);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);
}

void FileTransferPage::onActivated()
{
    refreshStorage();
}

// ─────────────────────────────────────────────────────────────────────────────
//  拖放
// ─────────────────────────────────────────────────────────────────────────────

bool FileTransferPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_dropZone) {
        if (event->type() == QEvent::DragEnter) {
            auto* drag = static_cast<QDragEnterEvent*>(event);
            if (drag->mimeData()->hasUrls()) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* drop = static_cast<QDropEvent*>(event);
            QStringList files;
            for (const QUrl& url : drop->mimeData()->urls()) {
                const QString local = url.toLocalFile();
                if (!local.isEmpty()) files.append(local);
            }
            if (!files.isEmpty()) pushFiles(files);
            return true;
        }
    }
    return BasePage::eventFilter(watched, event);
}

// ─────────────────────────────────────────────────────────────────────────────
//  操作触发（UI 线程）
// ─────────────────────────────────────────────────────────────────────────────

void FileTransferPage::pushFiles(const QStringList& files)
{
    if (m_busy.loadAcquire() != 0) return;
    const QString remote = m_pushRemote->currentText().trimmed();
    if (remote.isEmpty()) {
        error(QStringLiteral("请输入目标路径"));
        return;
    }
    // 按钮推送与拖放推送统一在此扣额度（拖放走本函数，两条口径一处收口，对齐 C#）。
    if (!kc::MembershipService::guardAndConsume(QStringLiteral("文件传输"),
                                                QStringLiteral("file.transfer")))
        return;
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, files, remote] { pushFilesCore(files, remote); }));
}

void FileTransferPage::pullFile()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString remote = m_pullRemote->currentText().trimmed();
    if (remote.isEmpty()) {
        error(QStringLiteral("请输入设备文件路径"));
        return;
    }
    const QString local = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存到"), QFileInfo(remote).fileName());
    if (local.isEmpty()) return;

    if (!kc::MembershipService::guardAndConsume(QStringLiteral("文件传输"),
                                                QStringLiteral("file.transfer")))
        return;
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, remote, local] { pullCore(remote, local); }));
}

void FileTransferPage::batchInstall()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString dir = m_batchDir->text().trimmed();
    if (dir.isEmpty() || !QFileInfo::exists(dir)) {
        error(QStringLiteral("请选择有效的APK目录"));
        return;
    }
    if (!kc::MembershipService::guardAndConsume(QStringLiteral("文件传输"),
                                                QStringLiteral("file.transfer")))
        return;
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, dir] { batchInstallCore(dir); }));
}

void FileTransferPage::browseDevice()
{
    if (m_busy.loadAcquire() != 0) return;
    QString path = m_devicePath->text().trimmed();
    if (path.isEmpty()) path = QStringLiteral("/sdcard/");
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, path] { browseCore(path); }));
}

void FileTransferPage::refreshStorage()
{
    if (m_busy.loadAcquire() != 0) return;
    QThreadPool::globalInstance()->start(new LambdaTask([this] { storageCore(); }));
}

// ─────────────────────────────────────────────────────────────────────────────
//  线程池执行体
// ─────────────────────────────────────────────────────────────────────────────

void FileTransferPage::runAdbCore(const QString& adbArgs, const QString& desc, int timeoutMs)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    logView()->beginOperation(QStringLiteral("adb %1").arg(adbArgs));
    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logView()->appendOperation(QStringLiteral("adb %1").arg(adbArgs), false);
        logError(QStringLiteral("[ADB] adb 不可用，请检查网络后重试"));
        runOnUi([this, desc] {
            error(QStringLiteral("adb 不可用，请检查网络后重试"));
            setProgress(false, QStringLiteral("adb 不可用"));
        });
    } else {
        runOnUi([this, desc] { setProgress(true, desc); });
        const auto r = tools.executeAdb(adbArgs, timeoutMs);
        logView()->appendOperation(QStringLiteral("adb %1").arg(adbArgs), r.success);
        if (!r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
        if (!r.error.trimmed().isEmpty()) logWarning(r.error.trimmed());
        if (!r.success) {
            logError(QStringLiteral("[ADB错误] %1 失败").arg(desc));
            runOnUi([this, desc] { error(QStringLiteral("%1 失败").arg(desc)); });
        }
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this, desc] {
        setProgress(false, QStringLiteral("%1 完成").arg(desc));
        setBusy(false);
    });
}

void FileTransferPage::pushFilesCore(const QStringList& files, const QString& remote)
{
    // C# DropZone：一批文件逐个 push，每个一条区段；进度行夹在中间。
    int succeeded = 0;
    for (int i = 0; i < files.size(); ++i) {
        const QString name = QFileInfo(files.at(i)).fileName();
        const QString desc = QStringLiteral("推送: %1 -> %2").arg(name, remote);
        logView()->appendSection(desc);

        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        const QString args = QStringLiteral("push \"%1\" \"%2\"").arg(files.at(i), remote);
        logView()->beginOperation(QStringLiteral("adb %1").arg(args));

        PlatformToolsManager tools;
        bool ok = false;
        if (!tools.ensureToolsExist()) {
            logView()->appendOperation(QStringLiteral("adb %1").arg(args), false);
            logError(QStringLiteral("[ADB] adb 不可用，请检查网络后重试"));
        } else {
            runOnUi([this, i, total = files.size(), name] {
                setProgress(true, QStringLiteral("推送中 (%1/%2): %3").arg(i + 1).arg(total).arg(name));
            });
            const auto r = tools.executeAdb(args, 600000);
            logView()->appendOperation(QStringLiteral("adb %1").arg(args), r.success);
            if (!r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
            if (!r.error.trimmed().isEmpty()) logWarning(r.error.trimmed());
            ok = r.success;
        }
        if (ok) ++succeeded;

        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    }

    const bool allOk = succeeded == files.size();
    runOnUi([this, succeeded, total = files.size(), remote, allOk] {
        setProgress(false, QStringLiteral("已推送 %1/%2 个文件到 %3").arg(succeeded).arg(total).arg(remote));
        if (allOk) success(QStringLiteral("已推送 %1 个文件到 %2").arg(total).arg(remote));
        else error(QStringLiteral("推送结束：成功 %1 / %2").arg(succeeded).arg(total));
        rememberHistory(m_pushRemote, remote);
        refreshStorage();
        setBusy(false);
    });
}

void FileTransferPage::pullCore(const QString& remote, const QString& local)
{
    runAdbCore(QStringLiteral("pull \"%1\" \"%2\"").arg(remote, local),
               QStringLiteral("拉取: %1 -> %2").arg(remote, local), 600000);
    // 历史记录要摸控件，过 runOnUi（runAdbCore 的收尾已在队列里，此处排队在其后）。
    runOnUi([this, remote] { rememberHistory(m_pullRemote, remote); });
}

void FileTransferPage::batchInstallCore(const QString& dir)
{
    // C#：Directory.GetFiles(dir, "*.apk")，逐个 install -r，按输出含 Success 计数。
    QDir apkDir(dir);
    const QFileInfoList apkFiles =
        apkDir.entryInfoList({ QStringLiteral("*.apk") }, QDir::Files, QDir::Name);
    if (apkFiles.isEmpty()) {
        runOnUi([this] {
            error(QStringLiteral("目录下没有 APK 文件"));
            setProgress(false, QStringLiteral("目录下没有 APK 文件"));
            setBusy(false);
        });
        return;
    }

    logInfo(QStringLiteral("[传输] 找到 %1 个APK文件").arg(apkFiles.size()));
    PlatformToolsManager tools;
    const bool haveTools = tools.ensureToolsExist();
    int succeeded = 0;

    for (int i = 0; i < apkFiles.size(); ++i) {
        const QString name = apkFiles.at(i).fileName();
        runOnUi([this, i, total = apkFiles.size(), name] {
            setProgress(true, QStringLiteral("安装中 (%1/%2): %3").arg(i + 1).arg(total).arg(name));
        });

        const QString desc = QStringLiteral("安装: %1").arg(name);
        logView()->appendSection(desc);
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        const QString args = QStringLiteral("install -r \"%1\"").arg(apkFiles.at(i).absoluteFilePath());
        logView()->beginOperation(QStringLiteral("adb %1").arg(args));

        bool ok = false;
        if (!haveTools) {
            logView()->appendOperation(QStringLiteral("adb %1").arg(args), false);
            logError(QStringLiteral("[ADB] adb 不可用，请检查网络后重试"));
        } else {
            const auto r = tools.executeAdb(args, 600000);
            logView()->appendOperation(QStringLiteral("adb %1").arg(args), r.success);
            if (!r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
            if (!r.error.trimmed().isEmpty()) logWarning(r.error.trimmed());
            ok = r.success && r.output.contains(QStringLiteral("Success"), Qt::CaseInsensitive);
            if (!ok) logWarning(QStringLiteral("[传输] %1 安装可能失败").arg(name));
        }
        if (ok) ++succeeded;

        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    }

    runOnUi([this, succeeded, total = apkFiles.size()] {
        setProgress(false, QStringLiteral("安装结束：成功 %1 / %2").arg(succeeded).arg(total));
        if (succeeded == total) success(QStringLiteral("全部 %1 个 APK 安装完成").arg(total));
        else error(QStringLiteral("安装结束：成功 %1 / %2").arg(succeeded).arg(total));
        setBusy(false);
    });
}

void FileTransferPage::browseCore(const QString& path)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    logView()->appendSection(QStringLiteral("浏览目录: %1").arg(path));
    logView()->beginOperation(QStringLiteral("adb shell ls -la \"%1\"").arg(path));

    PlatformToolsManager tools;
    QStringList lines;
    if (!tools.ensureToolsExist()) {
        logView()->appendOperation(QStringLiteral("adb shell ls -la \"%1\"").arg(path), false);
        logError(QStringLiteral("[ADB] adb 不可用，请检查网络后重试"));
    } else {
        const auto r = tools.executeAdb(
            QStringLiteral("shell ls -la \"%1\"").arg(path), 600000);
        logView()->appendOperation(QStringLiteral("adb shell ls -la \"%1\"").arg(path), r.success);
        if (r.success && !r.output.trimmed().isEmpty()) {
            for (const QString& raw : r.output.split(QLatin1Char('\n'))) {
                const QString line = raw.trimmed();
                if (!line.isEmpty()) lines.append(line);
            }
        } else if (!r.error.trimmed().isEmpty()) {
            logWarning(r.error.trimmed());
        }
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this, lines] {
        m_deviceFiles->clear();
        const QColor fg = ThemeManager::instance().color(ColorToken::Text);
        for (const QString& line : lines) {
            auto* item = new QListWidgetItem(line);
            item->setForeground(fg);
            m_deviceFiles->addItem(item);
        }
        setBusy(false);
    });
}

void FileTransferPage::storageCore()
{
    PlatformToolsManager tools;
    QString freeText;
    if (tools.ensureToolsExist()) {
        // df 默认 1K 块：Filesystem 1K-blocks Used Available Use% Mounted on。
        const auto r = tools.executeAdb(
            QStringLiteral("shell df /storage/emulated/0"), 15000);
        if (r.success) {
            for (const QString& raw : r.output.split(QLatin1Char('\n'))) {
                if (!raw.contains(QStringLiteral("/storage/emulated"))) continue;
                const QStringList cols =
                    raw.trimmed().split(QRegularExpression(QStringLiteral("\\s+")));
                if (cols.size() < 4) continue;
                bool ok = false;
                const qint64 availKb = cols.at(3).toLongLong(&ok);
                if (!ok || availKb < 0) continue;
                const double gb = availKb / 1024.0 / 1024.0;
                freeText = QStringLiteral("%1 GB 可用").arg(gb, 0, 'f', 2);
                break;
            }
        }
    }
    if (freeText.isEmpty()) freeText = QStringLiteral("未知");
    runOnUi([this, freeText] { m_storageValue->setText(freeText); });
}

// ─────────────────────────────────────────────────────────────────────────────
//  忙碌闸门 / 进度 / 历史记录
// ─────────────────────────────────────────────────────────────────────────────

void FileTransferPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
}

void FileTransferPage::setProgress(bool busy, const QString& text)
{
    if (busy) {
        m_progressBar->setRange(0, 0);   // 不确定模式（C# IsIndeterminate）
    } else {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(100);
    }
    m_progressText->setText(text);
}

void FileTransferPage::rememberHistory(QComboBox* combo, const QString& text)
{
    if (!combo || text.isEmpty()) return;
    const int existing = combo->findText(text);
    if (existing >= 0) combo->removeItem(existing);
    combo->insertItem(0, text);
    while (combo->count() > 10) combo->removeItem(combo->count() - 1);
    combo->setCurrentText(text);
}

} // namespace kc::ui
