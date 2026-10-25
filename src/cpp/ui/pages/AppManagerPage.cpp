#include "ui/pages/AppManagerPage.h"

#include "kc/auth/MembershipService.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include "ui/dialogs/ThemedMessageBox.h"
#include <QPushButton>
#include <QRunnable>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

constexpr int kColPkg = 0;

/// 把一段函数体丢进全局线程池（等价 C# 的 Task.Run）。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

} // namespace

AppManagerPage::AppManagerPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("应用管理"));
    buildUi();
}

void AppManagerPage::buildUi()
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

    // ── 搜索行：输入框 + 搜索按钮 + 显示系统应用 ────────────────────────
    auto* searchRow = new QHBoxLayout;
    searchRow->setSpacing(8);

    auto* searchLabel = new QLabel(QStringLiteral("搜索应用"), this);
    searchLabel->setObjectName(QStringLiteral("statusLabel"));
    searchRow->addWidget(searchLabel);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("输入包名关键字过滤"));
    m_searchEdit->setClearButtonEnabled(true);
    // C# 的 300ms 防抖：计时器只建一次、Tick 只挂一次，停止输入后才真正查询。
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(300);
    connect(m_searchDebounce, &QTimer::timeout, this, [this] { loadApps(); });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { m_searchDebounce->start(); });
    searchRow->addWidget(m_searchEdit, 1);

    m_refreshButton = new QPushButton(QStringLiteral("搜索"), this);
    m_refreshButton->setObjectName(QStringLiteral("primaryButton"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_refreshButton);
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        m_searchDebounce->stop();
        loadApps();
    });
    searchRow->addWidget(m_refreshButton);

    m_showSystem = new QCheckBox(QStringLiteral("显示系统应用"), this);
    connect(m_showSystem, &QCheckBox::toggled, this, [this] { loadApps(); });
    searchRow->addWidget(m_showSystem);
    layout->addLayout(searchRow);

    // ── 应用列表（包名一列）────────────────────────────────────────────
    m_table = new QTableWidget(this);
    m_table->setColumnCount(1);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("包名") });
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(kColPkg, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0) return;
        if (auto* item = m_table->item(row, kColPkg))
            onPackageSelected(item->text());
    });
    layout->addWidget(m_table, 3);

    // ── 应用信息读数行 ─────────────────────────────────────────────────
    auto* infoRow = new QHBoxLayout;
    infoRow->setSpacing(8);
    auto makeReading = [infoRow](const QString& label, QLabel** value) {
        auto* key = new QLabel(label, infoRow->parentWidget());
        key->setObjectName(QStringLiteral("statusLabel"));
        *value = new QLabel(QStringLiteral("-"), infoRow->parentWidget());
        (*value)->setObjectName(QStringLiteral("statusValue"));
        infoRow->addWidget(key);
        infoRow->addWidget(*value, 1);
    };
    makeReading(QStringLiteral("包名:"), &m_pkgValue);
    makeReading(QStringLiteral("版本:"), &m_versionValue);
    layout->addLayout(infoRow);

    // ── 操作按钮行（与 C# 相同 6 个 + 安装APK）──────────────────────────
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);

    // 前 5 个操作都需要选中包；安装APK 不需要（handler 内走文件选择器）。
    struct Action {
        QString label;
        bool needsSelection;
        std::function<void()> handler;
    };
    const QVector<Action> actions = {
        { QStringLiteral("冻结应用"), true, [this] {
             runPackageAction(QStringLiteral("shell pm disable-user --user 0 %1").arg(m_selectedPackage),
                              QStringLiteral("冻结: %1").arg(m_selectedPackage), 30000,
                              QStringLiteral("app.manage"));
         } },
        { QStringLiteral("解冻应用"), true, [this] {
             runPackageAction(QStringLiteral("shell pm enable %1").arg(m_selectedPackage),
                              QStringLiteral("解冻: %1").arg(m_selectedPackage), 30000,
                              QStringLiteral("app.manage"));
         } },
        { QStringLiteral("提取APK"), true, [this] {
             logView()->appendSection(QStringLiteral("提取: %1").arg(m_selectedPackage));
             setBusy(true);
             QThreadPool::globalInstance()->start(
                 new LambdaTask([this, pkg = m_selectedPackage] { extractApkCore(pkg); }));
         } },
        { QStringLiteral("清除数据"), true, [this] {
             // C# 口径：先 ensureAllowed（确认之前），确认通过后 worker 里 consume。
             if (!kc::MembershipService::ensureAllowed(QStringLiteral("应用管理"),
                                                       QStringLiteral("app.manage")))
                 return;
             if (ThemedMessageBox::warning(this, QStringLiteral("确认"),
                                      QStringLiteral("确定清除 %1 的数据？").arg(m_selectedPackage),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                 return;
             runPackageAction(QStringLiteral("shell pm clear %1").arg(m_selectedPackage),
                              QStringLiteral("清除数据: %1").arg(m_selectedPackage), 30000,
                              QStringLiteral("app.manage"));
         } },
        { QStringLiteral("卸载应用"), true, [this] {
             if (!kc::MembershipService::ensureAllowed(QStringLiteral("应用管理"),
                                                       QStringLiteral("app.manage")))
                 return;
             if (ThemedMessageBox::warning(this, QStringLiteral("确认"),
                                      QStringLiteral("确定卸载 %1？").arg(m_selectedPackage),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                 return;
             // C# 用的是 `adb uninstall {pkg}`（等价于对 user 0 的 pm uninstall）。
             runPackageAction(QStringLiteral("uninstall %1").arg(m_selectedPackage),
                              QStringLiteral("卸载: %1").arg(m_selectedPackage), 30000,
                              QStringLiteral("app.manage"));
         } },
        { QStringLiteral("备份APK"), true, [this] { backupApk(); } },
        { QStringLiteral("安装APK"), false, [this] { installApk(); } },
    };
    for (const Action& a : actions) {
        auto* b = new QPushButton(a.label, this);
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        connect(b, &QPushButton::clicked, this, [this, needsSelection = a.needsSelection,
                                                 handler = a.handler] {
            if (m_busy.loadAcquire() != 0) return;
            if (needsSelection && m_selectedPackage.isEmpty()) {
                error(QStringLiteral("请先在列表中选择一个应用"));
                return;
            }
            handler();
        });
        actionRow->addWidget(b);
    }
    actionRow->addStretch();
    layout->addLayout(actionRow);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);
}

void AppManagerPage::onActivated()
{
    loadApps();
}

// ─────────────────────────────────────────────────────────────────────────────
//  包列表
// ─────────────────────────────────────────────────────────────────────────────

void AppManagerPage::loadApps()
{
    if (m_busy.loadAcquire() != 0) return;
    setBusy(true);
    const QString searchFilter = m_searchEdit->text().trimmed();
    const bool showSystem = m_showSystem->isChecked();
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, searchFilter, showSystem] { loadAppsCore(searchFilter, showSystem); }));
}

void AppManagerPage::loadAppsCore(const QString& searchFilter, bool showSystem)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    PlatformToolsManager tools;
    QStringList packages;
    if (!tools.ensureToolsExist()) {
        logError(QStringLiteral("[ADB] adb 工具不可用，请检查网络后重试"));
    } else {
        // C#：filter = showSystem ? "" : "-3"
        const QString args = showSystem
                                 ? QStringLiteral("shell pm list packages")
                                 : QStringLiteral("shell pm list packages -3");
        const auto r = tools.executeAdb(args, 30000);
        if (r.success) {
            for (const QString& rawLine : r.output.split(QLatin1Char('\n'))) {
                if (!rawLine.startsWith(QStringLiteral("package:"))) continue;
                const QString pkg = rawLine.mid(8).trimmed();
                if (pkg.isEmpty()) continue;
                packages.append(pkg);
            }
            packages.removeAll(QString());
        } else {
            logError(QStringLiteral("[ADB错误] 包列表查询失败: %1").arg(r.error.trimmed()));
        }
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this, packages, searchFilter] {
        // 客户端过滤（C# 同款：拿到全量后按搜索框 contains 过滤）。
        const QString needle = searchFilter.toLower();
        QStringList rows;
        for (const QString& pkg : packages) {
            if (needle.isEmpty() || pkg.toLower().contains(needle)) rows.append(pkg);
        }

        m_table->setRowCount(rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            auto* item = new QTableWidgetItem(rows.at(i));
            item->setForeground(ThemeManager::instance().color(ColorToken::Text));
            m_table->setItem(i, kColPkg, item);
        }
        logInfo(QStringLiteral("[应用] 共 %1 个包（过滤后 %2）").arg(packages.size()).arg(rows.size()));
        setBusy(false);
    });
}

void AppManagerPage::onPackageSelected(const QString& pkg)
{
    if (m_busy.loadAcquire() != 0) {
        m_selectedPackage = pkg;
        m_pkgValue->setText(pkg);
        return;
    }
    m_selectedPackage = pkg;
    m_pkgValue->setText(pkg);
    m_versionValue->setText(QStringLiteral("…"));
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, pkg] { queryVersionCore(pkg); }));
}

void AppManagerPage::queryVersionCore(const QString& pkg)
{
    PlatformToolsManager tools;
    QString version;
    if (tools.ensureToolsExist()) {
        // C#：shell dumpsys package {pkg} | grep versionName → 取 '=' 后的值。
        const auto r = tools.executeAdb(
            QStringLiteral("shell dumpsys package %1 | grep versionName").arg(pkg), 30000);
        if (r.success) {
            for (const QString& rawLine : r.output.split(QLatin1Char('\n'))) {
                const QString line = rawLine.trimmed();
                const int eq = line.indexOf(QLatin1Char('='));
                if (eq < 0) continue;
                version = line.mid(eq + 1).trimmed();
                if (!version.isEmpty()) break;
            }
        }
    }
    runOnUi([this, version] {
        m_versionValue->setText(version.isEmpty() ? QStringLiteral("-") : version);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  包操作（日志三明治：Section → 输出 → Footer，成败都收尾）
// ─────────────────────────────────────────────────────────────────────────────

void AppManagerPage::runPackageAction(const QString& adbArgs, const QString& desc, int timeoutMs,
                                      const QString& quotaKey)
{
    if (m_busy.loadAcquire() != 0 || m_selectedPackage.isEmpty()) {
        if (m_selectedPackage.isEmpty()) error(QStringLiteral("请先在列表中选择一个应用"));
        return;
    }
    logView()->appendSection(desc);
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, adbArgs, desc, timeoutMs, quotaKey] {
            runPackageActionCore(adbArgs, desc, timeoutMs, quotaKey);
        }));
}

void AppManagerPage::runPackageActionCore(const QString& adbArgs, const QString& desc, int timeoutMs,
                                          const QString& quotaKey)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    bool ok = false;

    // 会员额度（对齐 C#）：有 quotaKey 的操作先扣一份，扣不动就收手（清数据/卸载的
    // ensureAllowed 已在 UI 线程确认前做过，这里 consume 等价于 C# 的确认后扣费）。
    if (!quotaKey.isEmpty()
        && !kc::MembershipService::guardAndConsume(QStringLiteral("应用管理"), quotaKey)) {
        logError(QStringLiteral("[应用] 会员额度校验未通过，操作已取消"));
        logView()->appendFooter(QStringLiteral("失败, 耗时: 0.00秒"));
        runOnUi([this] { setBusy(false); });
        return;
    }

    logInfo(QStringLiteral("[ADB] %1: adb %2").arg(desc, adbArgs));
    logView()->beginOperation(QStringLiteral("adb %1").arg(adbArgs));

    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logView()->appendOperation(QStringLiteral("adb %1").arg(adbArgs), false);
        logError(QStringLiteral("[ADB] adb 工具不可用，请检查网络后重试"));
        error(QStringLiteral("adb 工具不可用，请检查网络后重试"));
    } else {
        const auto r = tools.executeAdb(adbArgs, timeoutMs);
        logView()->appendOperation(QStringLiteral("adb %1").arg(adbArgs), r.success);
        if (!r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
        if (!r.error.trimmed().isEmpty()) logWarning(QStringLiteral("[ADB错误] %1").arg(r.error.trimmed()));
        ok = r.success;
        if (ok) success(QStringLiteral("%1 完成").arg(desc));
        else error(QStringLiteral("%1 失败").arg(desc));
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this] { setBusy(false); });
}

void AppManagerPage::extractApkCore(const QString& pkg)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    // 会员额度（对齐 C# BtnExtractApk_Click）：一份额度换一次提取。
    if (!kc::MembershipService::guardAndConsume(QStringLiteral("应用管理"),
                                                QStringLiteral("app.manage"))) {
        logError(QStringLiteral("[应用] 会员额度校验未通过，提取已取消"));
        logView()->appendFooter(QStringLiteral("失败, 耗时: 0.00秒"));
        runOnUi([this] { setBusy(false); });
        return;
    }

    logView()->beginOperation(QStringLiteral("adb shell pm path %1").arg(pkg));
    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logView()->appendOperation(QStringLiteral("adb shell pm path %1").arg(pkg), false);
        logError(QStringLiteral("[ADB] adb 工具不可用，请检查网络后重试"));
    } else {
        const auto r = tools.executeAdb(QStringLiteral("shell pm path %1").arg(pkg), 30000);
        logView()->appendOperation(QStringLiteral("adb shell pm path %1").arg(pkg), r.success);
        QString apkPath;
        if (r.success) {
            // C#：取第一行，去掉 "package:" 前缀。
            for (const QString& rawLine : r.output.split(QLatin1Char('\n'))) {
                if (rawLine.startsWith(QStringLiteral("package:"))) {
                    apkPath = rawLine.mid(8).trimmed();
                    break;
                }
            }
        }
        if (apkPath.isEmpty()) {
            logWarning(QStringLiteral("[应用] 未能取得 APK 路径"));
        } else {
            const QString pullArgs = QStringLiteral("pull \"%1\" .").arg(apkPath);
            logView()->beginOperation(QStringLiteral("adb %1").arg(pullArgs));
            const auto pull = tools.executeAdb(pullArgs, 600000);
            logView()->appendOperation(QStringLiteral("adb %1").arg(pullArgs), pull.success);
            if (!pull.output.trimmed().isEmpty()) logInfo(pull.output.trimmed());
            if (!pull.error.trimmed().isEmpty()) logWarning(QStringLiteral("[ADB错误] %1").arg(pull.error.trimmed()));
            if (pull.success) success(QStringLiteral("提取: %1 完成（APK 已保存到工具目录）").arg(pkg));
            else error(QStringLiteral("提取: %1 失败").arg(pkg));
        }
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this] { setBusy(false); });
}

void AppManagerPage::backupApk()
{
    if (m_busy.loadAcquire() != 0 || m_selectedPackage.isEmpty()) return;
    const QString pkg = m_selectedPackage;

    // 对齐 C# BtnBackupApk_Click：pm path → pull 到用户选的路径。
    // （C++ 先弹保存框再进 worker，pm path 与 pull 都在线程池里串着做，时序等价。）
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("备份 APK"), pkg + QStringLiteral(".apk"),
        QStringLiteral("APK 文件 (*.apk);;所有文件 (*.*)"));
    if (target.isEmpty()) return;

    logView()->appendSection(QStringLiteral("备份: %1").arg(pkg));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, pkg, target] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();

        logView()->beginOperation(QStringLiteral("adb shell pm path %1").arg(pkg));
        PlatformToolsManager tools;
        if (!tools.ensureToolsExist()) {
            logView()->appendOperation(QStringLiteral("adb shell pm path %1").arg(pkg), false);
            logError(QStringLiteral("[ADB] adb 工具不可用，请检查网络后重试"));
            logView()->appendFooter(QStringLiteral("失败, 耗时: 0.00秒"));
            runOnUi([this] { setBusy(false); });
            return;
        }
        const auto r = tools.executeAdb(QStringLiteral("shell pm path %1").arg(pkg), 30000);
        logView()->appendOperation(QStringLiteral("adb shell pm path %1").arg(pkg), r.success);
        QString apkPath;
        if (r.success) {
            for (const QString& rawLine : r.output.split(QLatin1Char('\n'))) {
                if (rawLine.startsWith(QStringLiteral("package:"))) {
                    apkPath = rawLine.mid(8).trimmed();
                    break;
                }
            }
        }
        if (apkPath.isEmpty()) {
            logWarning(QStringLiteral("[应用] 未能取得 APK 路径"));
            logView()->appendFooter(QStringLiteral("失败, 耗时: 0.00秒"));
            runOnUi([this] { setBusy(false); });
            return;
        }

        const QString pullArgs = QStringLiteral("pull \"%1\" \"%2\"").arg(apkPath, target);
        logView()->beginOperation(QStringLiteral("adb %1").arg(pullArgs));
        const auto pull = tools.executeAdb(pullArgs, 600000);
        logView()->appendOperation(QStringLiteral("adb %1").arg(pullArgs), pull.success);
        if (!pull.output.trimmed().isEmpty()) logInfo(pull.output.trimmed());
        if (!pull.error.trimmed().isEmpty()) logWarning(QStringLiteral("[ADB错误] %1").arg(pull.error.trimmed()));
        if (pull.success) success(QStringLiteral("备份: %1 完成（%2）").arg(pkg, target));
        else error(QStringLiteral("备份: %1 失败").arg(pkg));

        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
        runOnUi([this] { setBusy(false); });
    }));
}

void AppManagerPage::installApk()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 APK 文件"), QString(),
        QStringLiteral("APK 文件 (*.apk);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    const QString name = QFileInfo(path).fileName();
    logView()->appendSection(QStringLiteral("安装: %1").arg(name));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, path, name] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        const QString args = QStringLiteral("install -r \"%1\"").arg(path);
        logInfo(QStringLiteral("[ADB] 安装: %1").arg(args));
        logView()->beginOperation(QStringLiteral("adb %1").arg(args));

        PlatformToolsManager tools;
        bool ok = false;
        if (!tools.ensureToolsExist()) {
            logView()->appendOperation(QStringLiteral("adb %1").arg(args), false);
            logError(QStringLiteral("[ADB] adb 工具不可用，请检查网络后重试"));
            error(QStringLiteral("adb 工具不可用，请检查网络后重试"));
        } else {
            const auto r = tools.executeAdb(args, 600000);
            logView()->appendOperation(QStringLiteral("adb %1").arg(args), r.success);
            if (!r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
            if (!r.error.trimmed().isEmpty()) logWarning(QStringLiteral("[ADB错误] %1").arg(r.error.trimmed()));
            // C# 批量安装的判定口径：输出含 Success 算成功。
            ok = r.success && r.output.contains(QStringLiteral("Success"), Qt::CaseInsensitive);
            if (ok) success(QStringLiteral("%1 安装完成").arg(name));
            else error(QStringLiteral("%1 安装失败").arg(name));
        }

        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
        runOnUi([this] { setBusy(false); });
    }));
}

void AppManagerPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
    m_table->setEnabled(!busy);
}

} // namespace kc::ui
