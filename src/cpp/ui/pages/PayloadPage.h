#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PayloadPage — payload.bin 解析 / 分区提取页
//
//  移植自 tests/src/KunClient.App/Pages/PayloadPage.{xaml,xaml.cs}。
//  ⚠️ C# 页并没有用 KunFlashTool.Core.PayloadBinParser，而是页内手写的解析：
//    · 8 字节 magic（u32 LE = 0x00444150，不匹配则字节反转再试）；
//    · 8 字节 header_size + 8 字节 entry_count（i64 LE）；
//    · 每个表项：u32 nameLen + ASCII 名称（去 \0）+ i64 size + i64 offset，
//      然后按 4 字节对齐补读填充；
//    · magic 不匹配 / 解析异常 → 备用「名称检测模式」：在前 1MB 里扫
//      boot/system/vendor/… 已知分区名的 ASCII 串（此时无 size，提取会跳过）。
//     这里逐字对照移植，不改字段口径。
//
//  提取在 QThreadPool 上顺序读写（81 920 字节缓冲），进度喂 ToolProgressBar；
//  搜索过滤沿用 C# 的 220ms 防抖。忙碌闸门同 FastbootFlashPage。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class QTableWidgetItem;

namespace kc::ui {

class NotificationBar;
class ToolProgressBar;

class PayloadPage : public BasePage {
    Q_OBJECT

public:
    explicit PayloadPage(QWidget* parent = nullptr);

private:
    /// 表格一行（与 C# PayloadPartitionInfo 对应）。
    struct PartitionInfo {
        bool selected = false;
        QString name;
        QString sizeText;
        qint64 offset = 0;
        qint64 dataLength = 0;
        int index = 0;   // 「#」列序号，从 1 开始
    };

    void buildUi();
    void refreshTable();
    void applyFilter(const QString& text);

    void browsePayload();
    void pickSaveDir();
    void parsePayload();
    void parsePayloadFile(const QString& file);   // UI 线程（只读头部，开销极小）
    /// 标准 Android OTA（CrAU + protobuf manifest）走这里，与下面的 PAD 私有容器区分开。
    void parseStandardPayload(const QString& file);
    void parseAsRawPayload(const QString& file);  // 备用名称检测模式
    void extractSelected();
    void extractAll();
    void startExtract(QVector<PartitionInfo> partitions, const QString& outputDir);
    void extractCore(QVector<PartitionInfo> partitions, const QString& outputDir);
    /// CrAU 提取：按 manifest 的 operations（REPLACE / BZ / XZ / ZSTD / ZERO）重建镜像。
    void extractStandardCore(const QVector<PartitionInfo>& partitions, const QString& outputDir);
    void setBusy(bool busy);

    NotificationBar* m_notify = nullptr;
    QLineEdit* m_payloadEdit = nullptr;
    QLineEdit* m_saveDirEdit = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QLabel* m_infoLabel = nullptr;
    QTableWidget* m_table = nullptr;
    ToolProgressBar* m_progress = nullptr;
    QLabel* m_progressLabel = nullptr;
    QPushButton* m_parseButton = nullptr;
    QPushButton* m_extractSelectedButton = nullptr;
    QPushButton* m_extractAllButton = nullptr;
    QPushButton* m_selectAllButton = nullptr;
    QPushButton* m_deselectAllButton = nullptr;
    QPushButton* m_browseButton = nullptr;
    QPushButton* m_pickDirButton = nullptr;

    QVector<PartitionInfo> m_partitions;
    QString m_payloadPath;
    QTimer* m_filterDebounce = nullptr;
    /// 当前解析的是标准 CrAU payload（提取走 PayloadParser，不走 PAD 的裸偏移拷贝）。
    bool m_standardPayload = false;

    QAtomicInt m_busy{0};
    bool m_updatingTable = false;
};

} // namespace kc::ui
