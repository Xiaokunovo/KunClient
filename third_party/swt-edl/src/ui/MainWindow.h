#pragma once

#include "core/Types.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QStringList>
#include <QThread>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTimer;

namespace swt::edl {

class EdlWorker;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
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
    void flashXmlRequested(const QStringList& xmlPaths, bool allowProtected);
    void erasePartitionsRequested(const QVector<swt::edl::PartitionInfo>& partitions,
                                  bool allowProtected);
    void rebootRequested(bool toEdl);
    void stopRequested();

private slots:
    void refreshPorts();
    void chooseProgrammer();
    void chooseFirmwareDirectory();
    void chooseVipPackage();
    void toggleConnection();
    void chooseBackup();
    void choosePartitionImage(int row);
    void chooseImageAndFlash();
    void chooseXmlAndFlash();
    void confirmErase();
    void confirmReboot(bool toEdl = false);
    void appendLog(const QString& message);
    void updateState(swt::edl::SessionState state, const QString& detail);
    void updateProgress(quint64 completed, quint64 total);
    void updateOverallProgress(quint64 completed, quint64 total);
    void updateDeviceInfo(const swt::edl::DeviceInfo& info);
    void showPartitions(const QVector<swt::edl::PartitionInfo>& partitions);
    void operationFinished(bool success, const QString& message);

private:
    struct LogRecord {
        QString time;
        QString message;
    };

    QWidget* buildHeader();
    QWidget* buildLeftCard();
    QWidget* buildRightCard();
    QWidget* makeInfoCell(const QString& title, QLabel** valueLabel);
    PartitionInfo selectedPartition(bool* ok = nullptr) const;
    QVector<PartitionInfo> checkedPartitions() const;
    void updateCheckedCount();
    void refreshPartitionRowStatus(int row);
    bool applyProtectionPolicy(QVector<PartitionInfo>* partitions,
                               QStringList* imagePaths, const QString& operation,
                               bool* allowProtected);
    void setBusy(bool busy);
    bool loadVipPackage(bool showDialog);
    bool shouldShowLog(const QString& message) const;
    QString formatLogHtml(const LogRecord& record) const;
    void rebuildLogView();
    void applyTheme(bool dark);
    static QString formatBytes(quint64 bytes);

    QThread m_workerThread;
    EdlWorker* m_worker = nullptr;
    bool m_connected = false;
    bool m_busy = false;
    bool m_readGptAfterConnect = false;
    bool m_readVersionAfterGpt = false;
    QVector<PartitionInfo> m_partitions;
    QStringList m_selectedXmlPaths;
    QStringList m_partitionImagePaths;

    QComboBox* m_portCombo = nullptr;
    QComboBox* m_storageCombo = nullptr;
    QComboBox* m_modeCombo = nullptr;
    QComboBox* m_chipsetCombo = nullptr;
    QLineEdit* m_programmerEdit = nullptr;
    QLineEdit* m_firmwareDirectoryEdit = nullptr;
    QCheckBox* m_vipCheck = nullptr;
    QCheckBox* m_autoScanCheck = nullptr;
    QCheckBox* m_autoRebootCheck = nullptr;
    QCheckBox* m_generateXmlCheck = nullptr;
    QCheckBox* m_protectPartitionsCheck = nullptr;
    QCheckBox* m_readInfoCheck = nullptr;
    QLineEdit* m_vipDirectoryEdit = nullptr;
    QPushButton* m_vipBrowseButton = nullptr;
    QPushButton* m_firmwareBrowseButton = nullptr;
    QPushButton* m_connectButton = nullptr;
    QPushButton* m_readGptButton = nullptr;
    QPushButton* m_readVersionButton = nullptr;
    QPushButton* m_backupButton = nullptr;
    QPushButton* m_flashButton = nullptr;
    QPushButton* m_xmlFlashButton = nullptr;
    QPushButton* m_eraseButton = nullptr;
    QPushButton* m_rebootButton = nullptr;
    QMenu* m_rebootMenu = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_debugLogButton = nullptr;
    QPushButton* m_themeButton = nullptr;
    QCheckBox* m_selectAllCheck = nullptr;
    QLabel* m_checkedCountLabel = nullptr;
    QLabel* m_emptyState = nullptr;
    QTableWidget* m_partitionTable = nullptr;
    QTextEdit* m_logView = nullptr;
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
    QTimer* m_autoScanTimer = nullptr;
    QTimer* m_progressAnimationTimer = nullptr;
    int m_taskProgressTarget = 0;
    int m_overallProgressTarget = 0;
    QElapsedTimer m_speedTimer;
    quint64 m_speedLastCompleted = 0;
    quint64 m_speedTotal = 0;
    qint64 m_speedLastUpdateMs = 0;
    double m_smoothedBytesPerSecond = 0.0;
    QVector<LogRecord> m_logHistory;
    bool m_debugLogEnabled = false;
    bool m_darkTheme = true;
    QString m_lastPortSnapshot;
    OplusVipPackage m_vipPackage;
};

} // namespace swt::edl
