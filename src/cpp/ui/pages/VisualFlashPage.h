#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  VisualFlashPage — 可视刷写（移植自 C# Pages/VisualFlashPage，545 行）。
//
//  C# 的最终形态：已并入 FASTBOOT 页的模式切换。C++ 侧沿用 XiaomiEdl 的先例：
//  注册为隐藏页（不进页签条），由 FASTBOOT 页的「可视刷写 ›」按钮跳转 ——
//  入口语义等价（都在 Fastboot 流程里），实现省一套模式切换状态机。
//
//  功能清单（逐项对齐 C#）：
//   · 读取分区表 / 刷新（**优先 adb 读磁盘头 → GptParser 解析真 GPT**；
//     adb 不可用时退回 fastboot getvar all 的 partition-size 名字表）
//   · 分区表：勾选 + 右键菜单 读取分区 / 写入分区 / 擦除分区
//   · 刷写镜像：选镜像 + 目标分区 + 写入
//   · GPT 操作：备份 GPT（磁盘头原始扇区）/ 恢复 GPT（写回磁盘头）/ 生成 rawprogram.xml
//   · 会员额度口径与 C# 一致（edl.readtable / edl.backup / edl.gpt / edl.write / edl.erase）
//
//  ⚠️ fastboot 协议没有「分区 → 主机」的通用命令，`fastboot getvar all` 给的只是
//     引导器上报的名字 + 大小（没有起始 LBA / 类型 GUID），那不是 GPT。真 GPT 只能
//     从 Android 侧的块设备头 dd 出来（需 root），见 kc::AdbGptReader。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>

class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace kc::ui {

class NotificationBar;
class ToolProgressBar;

class VisualFlashPage : public BasePage {
    Q_OBJECT

public:
    explicit VisualFlashPage(QWidget* parent = nullptr);

    void onActivated() override;

private slots:
    void readPartTable();
    void backupGpt();
    void restoreGpt();
    void generateRawprogram();
    void browseImage();
    void writeSelected();
    void readPartition(const QString& name);
    void writePartition(const QString& name);
    void erasePartition(const QString& name);

private:
    struct PartitionInfo {
        bool selected = false;
        QString name;
        QString size;
        QString type;
        QString startLba;   // 起始扇区（只有 GPT 来源才有；getvar 兜底为空）
        QString guid;       // 类型 GUID（只有 GPT 来源才有）
    };

    void buildUi();
    void setBusy(bool busy);
    void applyPartitions(const QVector<PartitionInfo>& partitions);

    NotificationBar* m_notify = nullptr;
    QTableWidget* m_table = nullptr;
    QPushButton* m_readButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QLineEdit* m_imageEdit = nullptr;
    QLineEdit* m_targetEdit = nullptr;
    QPushButton* m_writeButton = nullptr;
    QPushButton* m_backupGptButton = nullptr;
    QPushButton* m_restoreGptButton = nullptr;
    QPushButton* m_rawprogramButton = nullptr;
    ToolProgressBar* m_progress = nullptr;
    QLabel* m_progressLabel = nullptr;

    QVector<PartitionInfo> m_partitions;
    QAtomicInt m_busy{0};
};

} // namespace kc::ui
