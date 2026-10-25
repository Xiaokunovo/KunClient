#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkFlashPage — MTK 刷写页（导航名 "MTK"，挂在「刷机」组）
//
//  信息架构对齐上游 SP Flash Tool 的 DownloadWidget（蓝本 UI/Src/DownloadWidget.cpp）：
//  scatter 加载 → 分区表（勾选/名称/起始/大小/镜像）→ 连接设备 → 下载/读回。
//
//  能力边界（2026-10-13 二期，代码注释与界面提示同口径）：
//   · 设备发现 / BROM 握手 / HW code 识别 —— 已实现；
//   · DA 引导（SEND_DA / JUMP_DA / BOOT_TO）—— 已实现（需 DA 文件）；
//   · 分区刷写 / 读回 —— 走 DA2 的 XFlash（WRITE_DATA / READ_DATA），全速；
//   · 擦除 / 格式化 —— 已提供（DA FORMAT，走「擦除选中」+ 二次确认）。
//   · RPMB / efuse / SLA —— 仍未提供（SLA 只在引导时告警，不执行签名握手）。
//
//  线程模型（2026-10-13 重构，见 MtkWorker.h）：
//   一个**长驻工作线程**持有已引导的会话（DA 引导几分钟，绝不每分区重做），
//   UI 只投递作业、取消走原子标志。不再用 QThreadPool 每次开/关端口。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"
#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QHash>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QThread;

namespace kc::mtk {
class MtkWorker;
struct MtkUsbDevice;
} // namespace kc::mtk

namespace kc::ui {

class NotificationBar;

class MtkFlashPage : public BasePage {
    Q_OBJECT

public:
    explicit MtkFlashPage(QWidget* parent = nullptr);
    ~MtkFlashPage() override;

    void onActivated() override;

    /// 探针/自动化用：直接加载一个 scatter 文件（等价于点「浏览…」后选中该文件）。
    /// 内部已切 UI 线程（探针可能在跳页后立刻调用）。
    void loadScatterFile(const QString& path);

    /// 探针/自动化用：直接设定 DA 文件路径。
    void setDaFile(const QString& path);

private:
    // ── UI 构建 ──────────────────────────────────────────────────────────
    void buildUi();
    void applyHeaderLayout();         // 表头列名 + 列宽策略（空表也要有列名）
    void refreshTable();
    void syncRowToTable(int index);
    void applyFilter(const QString& text);

    // ── scatter 流程 ─────────────────────────────────────────────────────
    void pickScatterFile();
    void autoMatchImages();           // 按 file_name 在 scatter 目录内解析/兜底查找
    void pickImageFor(int tableRow);  // 双击镜像单元格手动指定
    void pickDaFile();
    QString preloaderPath() const;    // 从 scatter 的 PRELOADER 分区取（EMI 用）

    // ── 设备流程（worker 线程）───────────────────────────────────────────
    void ensureWorker();              // 惰性建线程 + worker
    void stopWorker();                // 停机（在 worker 线程释放会话）
    void scanDevicesOnly();           // 轻量扫描：只刷新设备标签
    void connectDevice();             // 扫描 → 打开 → 握手 → 读 HW code
    void disconnectDevice();          // 关会话 + 清状态

    // ── 传输流程（worker 线程）───────────────────────────────────────────
    void downloadSelected();
    void readbackSelected();
    void eraseSelected();             // 擦除（DA FORMAT）：不可回滚，走二次确认
    bool confirmProtected(const QVector<int>& rows,
                          const QString& verb = QStringLiteral("写入")); // 保护分区闸门
    void setAllChecks(bool checked);  // 全选 / 全不选（表格勾选列批量开关）
    void setBusy(bool busy);
    void applyDeviceReading(const kc::mtk::MtkUsbDevice& device, const QString& hwCodeText);
    void updateActionButtons();

    QString formatSize(qint64 bytes) const;
    QString formatAddr(uint64_t addr) const;

    NotificationBar* m_notify = nullptr;

    // scatter 行
    QLineEdit* m_scatterEdit = nullptr;
    QPushButton* m_browseButton = nullptr;
    QPushButton* m_reparseButton = nullptr;

    // DA 行
    QLineEdit* m_daEdit = nullptr;
    QPushButton* m_daBrowseButton = nullptr;

    // 设备读数行
    QLabel* m_chipValue = nullptr;
    QLabel* m_storageValue = nullptr;
    QLabel* m_deviceValue = nullptr;
    QLabel* m_hwCodeValue = nullptr;
    QPushButton* m_connectButton = nullptr;
    QPushButton* m_disconnectButton = nullptr;

    // 动作行
    QPushButton* m_downloadButton = nullptr;
    QPushButton* m_readbackButton = nullptr;
    QPushButton* m_eraseButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_selectAllButton = nullptr;
    QPushButton* m_selectNoneButton = nullptr;
    QLineEdit* m_filterEdit = nullptr;

    QTableWidget* m_table = nullptr;

    QVector<QPushButton*> m_busyButtons;

    // ── 状态 ────────────────────────────────────────────────────────────
    kc::mtk::ScatterInfo m_scatter;
    QString m_scatterPath;
    QString m_daPath;
    QHash<QString, QString> m_fileOverride; // 分区名 → 手动指定的镜像路径
    kc::mtk::DeviceStage m_deviceStage = kc::mtk::DeviceStage::Disconnected;
    QString m_connectedComPort;
    uint16_t m_hwCode = 0;
    bool m_daReady = false;

    QThread* m_workerThread = nullptr;
    kc::mtk::MtkWorker* m_worker = nullptr;

    QAtomicInt m_busy{0};
    bool m_updatingTable = false;
};

} // namespace kc::ui
