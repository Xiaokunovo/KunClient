#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AppManagerPage — 应用管理（ADB 包管理）
//
//  移植自 tests/src/KunClient.App/Pages/AppManagerPage.{xaml,xaml.cs}。
//  C# 原页核心（此处保留）：
//   · 包列表：`adb shell pm list packages -3`（勾选「显示系统应用」时去掉 -3）；
//   · 搜索：300ms 防抖（防每键一轮 adb 查询）+ 搜索按钮；过滤在客户端做；
//   · 应用信息：选中包后 `shell dumpsys package {pkg} | grep versionName` 取版本；
//   · 操作：冻结（pm disable-user --user 0）/ 解冻（pm enable）/
//     提取APK（pm path → pull）/ 清除数据（pm clear，需确认）/
//     卸载（adb uninstall，需确认）/ 备份APK（pm path → pull 到用户选的路径）；
//   · 安装APK：文件选择器 → `install -r "{apk}"`（文件传输页同款命令）。
//  会员额度对齐 C#：冻结/解冻/提取/备份 guardAndConsume("app.manage")，
//  清除数据/卸载先 ensureAllowed（确认前）再在 worker 里 consume。
//
//  所有 adb 调用走 QThreadPool（PlatformToolsManager 是同步阻塞实现），
//  控件更新过 runOnUi。忙碌闸门：任一操作进行中禁用全部动作按钮。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QVector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace kc::ui {

class NotificationBar;

class AppManagerPage : public BasePage {
    Q_OBJECT

public:
    explicit AppManagerPage(QWidget* parent = nullptr);

    void onActivated() override;

private:
    void buildUi();
    void loadApps();                      // UI 线程：起闸门 + 丢线程池
    void loadAppsCore(const QString& searchFilter, bool showSystem);
    void onPackageSelected(const QString& pkg);
    void queryVersionCore(const QString& pkg);
    void runPackageAction(const QString& adbArgs, const QString& desc, int timeoutMs = 30000,
                          const QString& quotaKey = {});
    void runPackageActionCore(const QString& adbArgs, const QString& desc, int timeoutMs,
                              const QString& quotaKey);
    void extractApkCore(const QString& pkg);
    void backupApk();
    void installApk();

    void setBusy(bool busy);

    NotificationBar* m_notify = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QCheckBox* m_showSystem = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_pkgValue = nullptr;
    QLabel* m_versionValue = nullptr;
    QPushButton* m_refreshButton = nullptr;

    QVector<QPushButton*> m_busyButtons;  // 忙碌时统一禁用的动作按钮
    QTimer* m_searchDebounce = nullptr;   // 搜索防抖（300ms，单次）
    QAtomicInt m_busy{0};
    QString m_selectedPackage;
};

} // namespace kc::ui
