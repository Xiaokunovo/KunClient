#pragma once

#include "core/Types.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QStringList>
#include <QThread>

class QComboBox;
class QCheckBox;
class QDialog;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSerialPortInfo;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace kc::ui {
class LogView;
class XiaomiEdlPage;
}

namespace kc::devices { struct StorageBrandProbeOutcome; }
namespace kc::diag { struct FlashRecord; }

namespace swt::edl {

class EdlWorker;
struct RawProgramPlan;   // 刷机计划预演用（2026-10-31）

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// @param embedded 为 true 时表示被 KunClient MainShell 当子控件嵌入：
    ///   · 主题完全由外壳的 ThemeManager 驱动（ctor 不读 QSettings、不刷全局
    ///     样式表、不持久化）—— 否则嵌入窗自己的默认值（深色）会在注册页面时
    ///     把整个应用翻成深色，并与外壳的浅色默认相冲；
    ///   · 日志由外壳统一的 kc::ui::LogView 渲染（它自己接 ThemeManager::themeChanged
    ///     重绘）；本类的 m_darkTheme 只再决定分区表状态列等自绘颜色，由 applyTheme() 转发。
    explicit MainWindow(QWidget* parent = nullptr, bool embedded = false);
    ~MainWindow() override;

    /// 嵌入模式（KunClient MainShell 把本窗当子控件用时调用）：
    /// 隐藏自带标题栏（外壳已有标题栏，双标题栏是视觉冗余）。
    /// 独立运行（SWT-EDL 原用法 / --cli）时不调用，行为完全不变。
    void setEmbeddedMode(bool embedded);

    /// 应用主题（独立运行时自带全局样式表 + 持久化；嵌入时只切自己的色板，
    /// 全局样式表/QSettings 归外壳管）。自绘颜色（分区表状态列）随主题重刷，
    /// 日志由 LogView 的 recolor() 整篇重画。
    void applyTheme(bool dark);

    /// 直接指定 programmer 文件（与双击 programmer 框走同一条链路：
    /// 回填输入框 + 同目录 OPLUS VIP 签名包发现 + 日志）。
    /// KunClient 外壳的「云端引导库」下载完引导程序后从这里喂进来 ——
    /// m_programmerEdit 是私有的，不给这个入口外壳就只能另找子控件，更脆。

    /// 载入一组 rawprogram XML（与「选择刷机 XML」按钮同一条解析 + 表格填充链路，
    /// 只是跳过文件选择对话框）。GUI 探针用 —— `--xml <path>` 直接把包喂进 EDL 页，
    /// 分区表会渲染出「META 免合并」状态，免合并细节因此可在截图里取证。
    void loadFirmwareXmlPaths(const QStringList& selectedPaths);
    void setProgrammerPath(const QString& path);

    /// 启动 FDL（小米）进入 EDL 流程。
    /// 2026-10-17 用户要求把 FDL 从「重启设备 ▾」菜单里提出来，做成 EDL 页顶部
    /// 快捷条上的独立按钮 —— 它是一条完整的入口流程，藏在重启菜单里既不好找，
    /// 也和「重启」这个语义不搭。实现仍复用 openFdlFlow()（弹窗 + 自动开跑）。
    void startFdlFlow() { openFdlFlow(); }

    /// 打开「刷机记录」窗（查看 / 导出 / 清空）。纯本地，不依赖连接。
    /// 放在 public 段：外壳（MainShell 的辅助触控菜单 / 灵动岛）要直接调它 ——
    /// Qt6 的 QMetaObject::invokeMethod 对「无参 + 无返回值」的字符串反射调用
    /// 已经没有合适重载了（会报 no matching function），只能走普通成员函数。
    void showFlashHistory();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    /// 首次显示时预取云端引导库索引（见实现里的注释：不放构造函数是因为
    /// 单测会建窗但从不 show，构造函数里发请求会把测试拖死）。
    void showEvent(QShowEvent* event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

signals:
    void connectRequested(const QString& portName, const QString& programmerPath,
                          swt::edl::StorageType storage,
                          const swt::edl::OplusVipPackage& vipPackage);
    void disconnectRequested();
    void readGptRequested();
    void readVersionInfoRequested();
    void backupPartitionsRequested(const QVector<swt::edl::PartitionInfo>& partitions,
                                   const QString& outputDirectory, bool generateXml);
    void flashPartitionsRequested(const QVector<swt::edl::PartitionInfo>& partitions,
                                  const QStringList& imagePaths, bool allowProtected);
    void flashXmlRequested(const QStringList& xmlPaths, bool allowProtected,
                           bool keepUserData);
    void erasePartitionsRequested(const QVector<swt::edl::PartitionInfo>& partitions,
                                  bool allowProtected);
    void rebootRequested(bool toEdl);
    /// 写 BCB（boot-recovery / boot-fastboot）后重启 —— Recovery / FastbootD 入口。
    void rebootWithBcbRequested(const QString& bcbCommand);
    void factoryResetRequested();
    void enableOemUnlockRequested();
    void backupBasebandRequested(const QString& outputDirectory);
    void readStorageInfoRequested();
    void eraseLunRequested(quint32 lun);
    void writeGptRequested(const QString& path, quint32 lun);
    void readGptToFileRequested(const QString& path, quint32 lun);
    void stopRequested();

private slots:
    void refreshPorts();
    void chooseProgrammer();
    void chooseFirmwareDirectory();
    /// 「自动选择」：选一个刷机包目录，自动发现其中的全部 rawprogram XML
    /// （目录本身 / IMAGES 子目录，标准与非标准一并纳入）后直接载入。
    void autoSelectFirmwareXml();
    void chooseVipPackage();
    void toggleConnection();
    void chooseBackup();
    void choosePartitionImage(int row);
    void chooseImageAndFlash();
    void chooseXmlAndFlash();
    void preflightXmlPlan();
    void confirmErase();
    void confirmReboot(bool toEdl = false);
    void confirmRebootToRecovery();
    void confirmRebootToFastbootD();
    void confirmFactoryReset();
    void confirmEnableOemUnlock();
    void chooseBasebandBackup();
    void requestStorageInfo();
    void confirmEraseLun();
    void writeGptFromFile();
    void readGptToFile();
    /// 「高级功能 ▾ → 字库牌子」：用本页选中的串口 + 引导程序读一次存储芯片
    /// （厂商 / 类型 / 芯片序列号），全程只发 NOP，不写任何分区。
    /// 2026-10-21 从独立页 StorageBrandPage 搬进来，日志与结果都进 EDL 日志。
    void probeStorageBrand();
    void finishStorageBrandProbe(const kc::devices::StorageBrandProbeOutcome& out);
    /// 把当前分区表导出成 CSV（csv=true）或等宽 TXT。右键菜单与「高级功能 ▾」共用。
    void exportPartitionTable(bool csv);
    /// 记一条刷机记录（在 operationFinished 里调用；kind 为空则不记）。
    /// 只有「用户真正发起过的操作」才记 —— 连接/断开/GPT 读取这类不算刷机。
    void recordFlashOutcome(bool success, const QString& message);
    /// 发起一次可记录的操作：打现场快照（类型 / 来源包 / 分区摘要 / 起始时刻）。
    /// 三个入参都可以为空串（例如「读取存储信息」没有分区概念）。
    void beginFlashOperation(const QString& kind, const QString& package,
                             const QString& partitions);
    /// 分区摘要：「boot, system, vendor 等 3 个」/「69 个分区」。
    /// 刷机前自动备份（2026-10-31）：把 GPT + 关键分区读一份到本地。
    /// 返回 false = 备份失败，调用方必须**中止**后续写入（不能带着虚假的安全感刷）。
    /// 设置里关掉时直接返回 true（用户自担风险）。
    bool ensurePreFlashBackup();
    /// 一键重跑上次刷机（2026-10-31）：按最近一条可重放记录重新执行。
    /// 会先校验设备身份与来源包，不一致就拒绝（绝不静默换设备刷）。
    void replayLastFlash();
    /// 固件包预检拦截（2026-10-31）：发现 Error 就**中止**刷机并说明原因。
    /// 返回 false = 不要继续刷。
    bool ensureFirmwarePrecheckPasses(const QStringList& xmlPaths);
    /// 刷机计划预演（2026-10-31）：算出会写哪些扇区、有无重叠，让用户确认。
    /// 干净的计划直接放过（不打扰）；有重叠/警告/保护分区才弹确认。
    bool confirmDryRun(const swt::edl::RawProgramPlan& plan);
    /// 分区表对比（2026-10-31）：把当前表与基线快照比，列出增/删/改。
    /// 没有基线时先把当前表存为基线。
    void diffPartitionTables();
    /// 导出设备报告（2026-10-31）：TXT / JSON 两种格式。
    void exportDeviceReport();
    static QString summarizePartitions(const QVector<PartitionInfo>& partitions);
    void appendLog(const QString& message);
    void updateState(swt::edl::SessionState state, const QString& detail);
    void setBusy(bool busy);
    void updateProgress(quint64 completed, quint64 total);
    void updateOverallProgress(quint64 completed, quint64 total);
    void updateDeviceInfo(const swt::edl::DeviceInfo& info);
    void showPartitions(const QVector<swt::edl::PartitionInfo>& partitions);
    void operationFinished(bool success, const QString& message);

private:
    QWidget* buildHeader();
    QWidget* buildLeftCard();
    /// 刷新「高级功能」下拉里各条目的可用状态（未连接 / 忙碌 / 分区表未就绪时置灰）。
    /// 2026-10-19 起高级功能是**下拉选择框**（页签条右侧），不再有独立弹窗 ——
    /// 原来那层 application-modal 弹窗会把里面按钮打开的确认框压在后面看不见。
    void updateAdvancedItems();
    /// 打开 FDL（小米）进入 EDL 流程窗，并**自动开跑**完整流程。
    /// 2026-10-16 用户要求：FDL 不再占独立页面，改由右下角重启列表触发。
    void openFdlFlow();
    /// 停更离线版（2.0.6）：云端引导库已随服务器关停移除。
    /// 保留下拉控件（m_chipsetCombo）本身，但只含第 0 项（官方高通芯片）；
    /// 引导程序改由下方「引导程序」输入框 + 浏览按钮选本地文件。
    void ensureCloudLoaderList();
    QWidget* buildRightCard();
    QWidget* makeInfoCell(const QString& title, QLabel** valueLabel);
    PartitionInfo selectedPartition(bool* ok = nullptr) const;
    QVector<PartitionInfo> checkedPartitions() const;
    void updateCheckedCount();
    void refreshPartitionRowStatus(int row);
    bool applyProtectionPolicy(QVector<PartitionInfo>* partitions,
                               QStringList* imagePaths, const QString& operation,
                               bool* allowProtected);
    bool loadVipPackage(bool showDialog);
    static QString formatBytes(quint64 bytes);
    /// 串口指纹：判断某个串口是不是 Qualcomm 9008（EDL）端口。
    /// 优先看 VID 0x05C6 + PID 0x9008（最可靠），描述串关键字兜底。
    static bool isEdlPort(const QSerialPortInfo& port);
    /// 在 parent 下建「KunClient-回读-时间戳」目录（chooseBackup 与基带备份共用）。
    QString createBackupDirectory(const QString& parent, bool* ok);

    QThread m_workerThread;
    EdlWorker* m_worker = nullptr;
    bool m_connected = false;
    bool m_busy = false;
    bool m_brandProbeRunning = false;   // 字库牌子探测进行中（防重入）
    bool m_cloudLoaderLoaded = false;
    bool m_readGptAfterConnect = false;
    bool m_readVersionAfterGpt = false;
    QVector<PartitionInfo> m_partitions;
    /// 分区表对比的基线快照（2026-10-31）。空 = 还没拍过快照。
    QVector<PartitionInfo> m_gptBaseline;
    QStringList m_selectedXmlPaths;
    QStringList m_partitionImagePaths;

    // ── 刷机记录（kc::diag::FlashHistory）的现场信息 ─────────────────────
    //  为什么在 UI 侧攒而不是 worker 侧：worker 只知道「写了几个分区」，不知道
    //  用户是从哪个 XML / 哪个镜像点进来的、设备是什么型号 —— 那些在点按钮那一刻
    //  才齐全。所以每次发起操作时打一份快照，operationFinished 里连同成败一起落盘。
    /// 本次操作的开始时间（QElapsedTimer 不跨任务复用，这里存绝对毫秒）。
    qint64 m_operationStartedMs = 0;
    /// 操作类型（写入 / 擦除 / 读取 / XML 刷机 …），空 = 不是可记录的操作。
    QString m_operationKind;
    /// 来源包摘要（XML 文件名 / 镜像名 / 输出目录）。
    QString m_operationPackage;
    /// 分区摘要（"boot, system 等 3 个" / "69 个分区"）。
    QString m_operationPartitions;
    /// 设备摘要（"COM7 · 小米 23127PN0CC · 0x1234abcd"），连接时刷新。
    QString m_deviceSummary;

    QComboBox* m_portCombo = nullptr;
    QComboBox* m_storageCombo = nullptr;
    QComboBox* m_modeCombo = nullptr;
    /// 云端引导入口 2026-10-18 已并入 m_chipsetCombo 的「云端引导…」条目，
    /// 独立的 m_cloudLoaderButton 删除（用户要求：按钮删掉、放进选择框里）。
    /// 最终形态：索引拉回来后把条目**直接追加进这个下拉**（按品牌分组）。
    QComboBox* m_chipsetCombo = nullptr;
    QLineEdit* m_programmerEdit = nullptr;
    QLineEdit* m_firmwareDirectoryEdit = nullptr;
    QCheckBox* m_vipCheck = nullptr;
    QCheckBox* m_autoScanCheck = nullptr;
    QCheckBox* m_autoRebootCheck = nullptr;
    QCheckBox* m_generateXmlCheck = nullptr;
    QCheckBox* m_protectPartitionsCheck = nullptr;
    /// 「保留用户数据」：XML 刷写时跳过 userdata / metadata（移植自 TMFTool 的「保数据」）。
    QCheckBox* m_keepUserDataCheck = nullptr;
    QCheckBox* m_readInfoCheck = nullptr;
    QLineEdit* m_vipDirectoryEdit = nullptr;
    QPushButton* m_vipBrowseButton = nullptr;
    QPushButton* m_firmwareBrowseButton = nullptr;
    /// 「自动选择」：选目录 → 自动发现全部 rawprogram XML（2026-10-17）。
    QPushButton* m_autoXmlButton = nullptr;
    QPushButton* m_connectButton = nullptr;
    QPushButton* m_readGptButton = nullptr;
    QPushButton* m_readVersionButton = nullptr;
    QPushButton* m_backupButton = nullptr;
    QPushButton* m_flashButton = nullptr;
    QPushButton* m_xmlFlashButton = nullptr;
    QPushButton* m_edlPreflightButton = nullptr;
    QPushButton* m_eraseButton = nullptr;
    QPushButton* m_rebootButton = nullptr;
    QMenu* m_rebootMenu = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_debugLogButton = nullptr;
    QPushButton* m_themeButton = nullptr;
    // ── 左卡顶部与「高级功能」下拉（2026-10-19：弹窗 → 下拉选择框）───────────
    QPushButton* m_flashTabButton = nullptr;      ///< 静态「刷机界面」标题（不可点）
    /// 「高级功能 ▾」下拉：条目 = 恢复出厂 / OEM 解锁 / 基带备份 / 存储信息 /
    /// LUN 擦除 / GPT 读写；选中即执行，执行完复位回第 0 项。
    QComboBox* m_advancedCombo = nullptr;
    /// FDL 流程窗：懒创建常驻；内容是一个 kc::ui::XiaomiEdlPage。
    QDialog* m_fdlDialog = nullptr;
    kc::ui::XiaomiEdlPage* m_fdlPage = nullptr;
    /// 擦除磁盘 / GPT 读写的目标 LUN（下拉在页签条上，高级功能入口左侧）。
    QComboBox* m_lunCombo = nullptr;
    QCheckBox* m_selectAllCheck = nullptr;
    QLabel* m_checkedCountLabel = nullptr;
    QLabel* m_emptyState = nullptr;
    QTableWidget* m_partitionTable = nullptr;
    /// 刷写日志视图 —— 外壳统一的 kc::ui::LogView（C# PageLog 的 C++ 移植），
    /// 不再是本窗私有的 QTextEdit + 自写 HTML 着色（2026-10-13 用户要求
    /// 刷写日志统一走 C# 版式）。
    kc::ui::LogView* m_logView = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QProgressBar* m_totalProgressBar = nullptr;
    QLabel* m_progressText = nullptr;
    QLabel* m_totalProgressText = nullptr;
    QLabel* m_speedText = nullptr;
    QLabel* m_stateBanner = nullptr;
    QLabel* m_portValue = nullptr;
    QLabel* m_storageValue = nullptr;
    QLabel* m_programmerValue = nullptr;
    QLabel* m_modeValue = nullptr;
    QLabel* m_brandValue = nullptr;
    QLabel* m_modelValue = nullptr;
    QLabel* m_cpuidValue = nullptr;
    QLabel* m_systemValue = nullptr;
    QLabel* m_productNameValue = nullptr;
    QLabel* m_androidValue = nullptr;
    QLabel* m_versionValue = nullptr;
    QLabel* m_securityPatchValue = nullptr;
    QLabel* m_bottomStatus = nullptr;
    QLabel* m_protocolStatus = nullptr;
    /// 页脚三格所在整条（2026-10-16 嵌入时整条收起，读数并入外壳状态栏）。
    QFrame* m_footerFrame = nullptr;
    QTimer* m_autoScanTimer = nullptr;
    QTimer* m_progressAnimationTimer = nullptr;
    int m_taskProgressTarget = 0;
    int m_overallProgressTarget = 0;
    QElapsedTimer m_speedTimer;
    quint64 m_speedLastCompleted = 0;
    quint64 m_speedTotal = 0;
    qint64 m_speedLastUpdateMs = 0;
    double m_smoothedBytesPerSecond = 0.0;
    QWidget* m_headerWidget = nullptr;   // 自带标题栏（嵌入模式下隐藏）
    bool m_embedded = false;             // 被 MainShell 嵌入：主题/样式表归外壳驱动
    bool m_darkTheme = true;
    QString m_lastPortSnapshot;
    /// 自动连接防抖：同一个端口只自动连一次，避免拔插抖动时反复重连。
    QString m_autoConnectedPort;
    /// 用户手动点过「断开」后置 true：不再自动把端口抢回去，直到下次手动连接。
    bool m_autoConnectSuppressed = false;
    OplusVipPackage m_vipPackage;
};

} // namespace swt::edl
