#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FastbootFlashPage — 通用 Fastboot 刷写页
//
//  移植自 tests/src/KunClient.App/Pages/FastbootFlashPage.{xaml,xaml.cs}。
//  C# 原页核心流程（此处保留）：
//   · 「分区管理」表格：boot / init_boot / vendor_boot / dtbo / recovery /
//     system / custom 共 7 行；勾选 + 双击选镜像；custom 行的分区名可编辑；
//   · 写入选中 / 擦除选中（逐分区顺序下发 fastboot flash / erase）；
//   · 读取分区表：fastboot getvar all → partition-size 填「大小」列，
//     顺带刷左列设备读数（序列号 / 安全启动 / 设备代号 / 解锁状态 / 槽位）；
//   · 引导锁（oem unlock / oem lock / oem device-info）、格式化数据（-w）、
//     擦除 cache / metadata、getvar 系列；
//   · 重启三连：reboot bootloader / reboot / reboot-edl。
//  C# 的模式切换（分区管理 ↔ 可视刷写）改为跳转式：顶部「可视刷写 ›」
//  发 navigationRequested("VisualFlash") 进隐藏页（与 FDL 入口同款）。
//
//  所有 fastboot 调用走 QThreadPool（PlatformToolsManager 是同步阻塞实现），
//  日志经 LogView 的线程安全 append* 直写，控件更新过 runOnUi。
//  忙碌闸门：任一操作进行中禁用全部动作按钮与表格。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QTableWidget;
class QTableWidgetItem;

namespace kc::ui {

class NotificationBar;

class FastbootFlashPage : public BasePage {
    Q_OBJECT

public:
signals:
    /// 请求外壳跳转到指定页面 —— 顶部「可视刷写 ›」入口发 navigationRequested("VisualFlash")。
    void navigationRequested(const QString& key);

public:
    explicit FastbootFlashPage(QWidget* parent = nullptr);

    void onActivated() override;

private:
    /// 表格里的一行（与 C# FastbootPartitionRow 对应；表格行号 = m_rows 下标）。
    struct Row {
        QString key;        // 稳定键：boot / init_boot / … / custom
        QString name;       // 分区名（custom 行跟随名称单元格）
        QString filePath;   // 已选镜像路径（空 = 未选）
    };

    void buildUi();
    void buildPartitionRows();
    void syncRowToTable(int index);

    void pickImageFor(int tableRow);
    void pickRomDir();               // 选固件目录 → 逐行按 <分区>.img/.bin/.mbn 匹配填充
    void flashSinglePartition(const QString& partition, const QString& image);  // 6+1 快速刷写共用
    void onItemChanged(QTableWidgetItem* item);
    void onCellDoubleClicked(int row, int column);
    void applyFilter(const QString& text);

    void refreshDevice();
    void readPartitionTable();
    void flashSelected();
    void eraseSelected();
    void runFastbootCommand(const QString& args, const QString& desc, int timeoutMs = 120000,
                            const QString& quotaKey = {}, bool refreshAfter = false);

    // ── 高级刷写（移植自 YukiClient GenericTab 的 fastboot 侧能力）────────────
    void disableVbmeta();                              ///< 三选一参数 → 连刷 vbmeta 三兄弟
    void scanCowPartitions();                          ///< getvar all → 列出 -cow 逻辑分区
    void deleteCowPartitions(const QStringList& names);///< 二次确认后逐个 delete-logical-partition

    // ── 线程池执行体（阻塞实现，禁止在 UI 线程调用）────────────────────────
    bool runFastbootCore(const QString& args, const QString& desc, int timeoutMs);
    void refreshDeviceCore();
    void readPartitionTableCore();
    void flashSelectedCore(const QVector<int>& targets, bool adbMode);
    void eraseSelectedCore(const QVector<int>& targets);
    /// ADB 模式刷入：push 到 /data/local/tmp → dd 到 /dev/block/by-name/<分区>（失败回落 su -c）。
    bool flashViaAdb(const QString& partition, const QString& image, QString* error);
    void scanCowCore();
    void deleteCowCore(const QStringList& names);
    void disableVbmetaCore(const QString& flags, const QStringList& images);

    void setBusy(bool busy);
    void applyDeviceReadings(const QString& output);
    void applyPartitionSizes(const QString& output);

    NotificationBar* m_notify = nullptr;
    QLabel* m_modeLabel = nullptr;
    QLabel* m_serialValue = nullptr;
    QLabel* m_secureValue = nullptr;
    QLabel* m_productValue = nullptr;
    QLabel* m_unlockedValue = nullptr;
    QLabel* m_slotValue = nullptr;
    QTableWidget* m_table = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLineEdit* m_romDirEdit = nullptr;     // 固件目录（目录匹配填充用）
    QPushButton* m_refreshButton = nullptr;
    QCheckBox* m_adbMode = nullptr;        // ADB 模式（push + dd），需要 root / Fastbootd

    QVector<Row> m_rows;
    QVector<QPushButton*> m_busyButtons;   // 忙碌时统一禁用的动作按钮

    QAtomicInt m_busy{0};
    bool m_updatingTable = false;          // 程序性写表格时屏蔽 itemChanged
};

} // namespace kc::ui
