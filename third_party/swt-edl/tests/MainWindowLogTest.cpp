#include "ui/MainWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollBar>
#include <QTextEdit>
#include <QTextDocument>
#include <QTableWidget>
#include <QThread>

#include <iostream>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    swt::edl::MainWindow window;
    auto* logView = window.findChild<QTextEdit*>(QStringLiteral("logView"));
    auto* debugButton = window.findChild<QPushButton*>(QStringLiteral("debugButton"));
    auto* themeButton = window.findChild<QPushButton*>(QStringLiteral("themeButton"));
    if (!logView || !debugButton || !themeButton) {
        std::cerr << "Log controls were not created\n";
        return 1;
    }

    QPushButton* clearButton = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("清空")) {
            clearButton = button;
            break;
        }
    }
    if (!clearButton) {
        std::cerr << "Clear button was not created\n";
        return 2;
    }
    clearButton->click();

    const QString noisy = QStringLiteral("Firehose: INFO: protocol fixture");
    const QString vipFallbackNoise = QStringLiteral(
        "Firehose: ERROR: operation 0 on BackupGPT:6:2 is forbidden on external network");
    const QString important = QStringLiteral("XML 1/1：boot → boot，LUN 0");
    const QString error = QStringLiteral("Firehose: ERROR: operation fixture");
    for (const QString& message : {noisy, vipFallbackNoise, important, error}) {
        if (!QMetaObject::invokeMethod(&window, "appendLog", Qt::DirectConnection,
                                       Q_ARG(QString, message))) {
            std::cerr << "appendLog invocation failed\n";
            return 3;
        }
    }

    QString visible = logView->toPlainText();
    if (visible.contains(noisy) || visible.contains(vipFallbackNoise) ||
        !visible.contains(important) || !visible.contains(error)) {
        std::cerr << "Normal log filtering result is incorrect\n";
        return 4;
    }

    debugButton->click();
    QApplication::processEvents();
    visible = logView->toPlainText();
    if (!debugButton->isChecked() || !visible.contains(noisy) ||
        !visible.contains(vipFallbackNoise) ||
        !visible.contains(important) || !visible.contains(error)) {
        std::cerr << "Debug log replay result is incorrect\n";
        return 5;
    }

    debugButton->click();
    QApplication::processEvents();
    visible = logView->toPlainText();
    if (debugButton->isChecked() || visible.contains(noisy) ||
        visible.contains(vipFallbackNoise) ||
        !visible.contains(important) || !visible.contains(error)) {
        std::cerr << "Returning to quiet log mode failed\n";
        return 6;
    }

    clearButton->click();
    debugButton->click();
    QApplication::processEvents();
    if (!logView->toPlainText().isEmpty()) {
        std::cerr << "Clearing the log did not clear retained history\n";
        return 7;
    }

    const QString initialThemeText = themeButton->text();
    const QString initialStyle = app.styleSheet();
    themeButton->click();
    QApplication::processEvents();
    if (themeButton->text() == initialThemeText || app.styleSheet() == initialStyle) {
        std::cerr << "Light/dark theme switch did not update the UI\n";
        return 8;
    }
    themeButton->click();
    QApplication::processEvents();
    if (themeButton->text() != initialThemeText || app.styleSheet() != initialStyle) {
        std::cerr << "Theme switch did not restore the original theme\n";
        return 9;
    }

    clearButton->click();
    for (int index = 0; index < 250; ++index) {
        const QString line = QStringLiteral("流式日志测试 %1").arg(index);
        if (!QMetaObject::invokeMethod(&window, "appendLog", Qt::DirectConnection,
                                       Q_ARG(QString, line))) {
            return 10;
        }
    }
    QApplication::processEvents();
    if (!logView->toPlainText().contains(QStringLiteral("流式日志测试 249")) ||
        logView->document()->blockCount() < 250 ||
        logView->verticalScrollBar()->value() != logView->verticalScrollBar()->maximum()) {
        std::cerr << "Streaming log rendering did not keep up or auto-scroll\n";
        return 11;
    }

    auto* taskProgress = window.findChild<QProgressBar*>(QStringLiteral("taskProgressBar"));
    auto* overallProgress = window.findChild<QProgressBar*>(QStringLiteral("overallProgressBar"));
    if (!taskProgress || !overallProgress) return 12;
    auto waitForProgress = [&app](QProgressBar* bar, int expected) {
        for (int attempt = 0; attempt < 100 && bar->value() != expected; ++attempt) {
            app.processEvents();
            QThread::msleep(10);
        }
        app.processEvents();
        return bar->value() == expected;
    };
    if (!QMetaObject::invokeMethod(&window, "updateOverallProgress", Qt::DirectConnection,
                                   Q_ARG(quint64, 25), Q_ARG(quint64, 100)) ||
        !waitForProgress(overallProgress, 250) || taskProgress->value() != 0) {
        std::cerr << "Overall progress changed the child progress bar\n";
        return 13;
    }
    if (!QMetaObject::invokeMethod(&window, "updateProgress", Qt::DirectConnection,
                                   Q_ARG(quint64, 50), Q_ARG(quint64, 100)) ||
        !waitForProgress(taskProgress, 500) || overallProgress->value() != 250) {
        std::cerr << "Child progress changed the overall progress bar\n";
        return 14;
    }

    auto* speedText = window.findChild<QLabel*>(QStringLiteral("speedText"));
    constexpr quint64 speedTotal = 100ULL * 1024ULL * 1024ULL;
    if (!speedText ||
        !QMetaObject::invokeMethod(&window, "updateOverallProgress", Qt::DirectConnection,
                                   Q_ARG(quint64, 0), Q_ARG(quint64, speedTotal))) {
        return 15;
    }
    QThread::msleep(550);
    if (!QMetaObject::invokeMethod(&window, "updateOverallProgress", Qt::DirectConnection,
                                   Q_ARG(quint64, 10ULL * 1024ULL * 1024ULL),
                                   Q_ARG(quint64, speedTotal)) ||
        speedText->text() == QStringLiteral("速率  0.00 MB/s")) {
        std::cerr << "Overall transfer rate was not calculated\n";
        return 16;
    }
    const QString stableSpeed = speedText->text();
    if (!QMetaObject::invokeMethod(&window, "updateProgress", Qt::DirectConnection,
                                   Q_ARG(quint64, 0), Q_ARG(quint64, 4096)) ||
        speedText->text() != stableSpeed) {
        std::cerr << "Child task reset the overall transfer rate\n";
        return 17;
    }

    QPushButton* firmwareButton = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("选择资料")) {
            firmwareButton = button;
            break;
        }
    }
    auto* partitionTable = window.findChild<QTableWidget*>(QStringLiteral("partitionTable"));
    if (!firmwareButton || !partitionTable ||
        window.metaObject()->indexOfSlot("choosePartitionImage(int)") < 0) {
        std::cerr << "XML selector or partition image binding controls are missing\n";
        return 18;
    }
    if (!QMetaObject::invokeMethod(
            &window, "updateState", Qt::DirectConnection,
            Q_ARG(swt::edl::SessionState, swt::edl::SessionState::Ready),
            Q_ARG(QString, QStringLiteral("EDL 已就绪"))) ||
        !QMetaObject::invokeMethod(&window, "operationFinished", Qt::DirectConnection,
                                   Q_ARG(bool, true),
                                   Q_ARG(QString, QStringLiteral("设备连接成功"))) ||
        !firmwareButton->isEnabled()) {
        std::cerr << "XML selector was disabled in the connected idle state\n";
        return 19;
    }

    swt::edl::DeviceInfo info;
    info.portName = QStringLiteral("COM3");
    info.storage = QStringLiteral("UFS");
    info.storageBytes = static_cast<quint64>(
        226.49 * 1024.0 * 1024.0 * 1024.0);
    info.storageProduct = QStringLiteral("KLUFG8RHHD-B0G1");
    info.brand = QStringLiteral("OPLUS");
    info.model = QStringLiteral("SM8250 (Snapdragon 865)");
    info.serial = QStringLiteral("fdbc1aed");
    info.productName = QStringLiteral("OPPO Find X8");
    info.androidVersion = QStringLiteral("16");
    info.systemVersion = QStringLiteral("CPH9999_16.0.1");
    info.securityPatch = QStringLiteral("2026-08-05");
    if (!QMetaObject::invokeMethod(&window, "updateDeviceInfo", Qt::DirectConnection,
                                   Q_ARG(swt::edl::DeviceInfo, info))) {
        return 20;
    }
    bool brandVisible = false;
    bool chipVisible = false;
    bool serialVisible = false;
    bool storageVisible = false;
    bool storageProductVisible = false;
    bool productNameVisible = false;
    bool androidVisible = false;
    bool versionVisible = false;
    bool patchVisible = false;
    for (QLabel* label : window.findChildren<QLabel*>()) {
        brandVisible = brandVisible || label->text() == QStringLiteral("OPLUS");
        chipVisible = chipVisible || label->text() == QStringLiteral("SM8250");
        serialVisible = serialVisible || label->text() == QStringLiteral("fdbc1aed");
        storageVisible = storageVisible || label->text() == QStringLiteral("226.49 GB");
        storageProductVisible = storageProductVisible ||
            label->text() == QStringLiteral("KLUFG8RHHD-B0G1");
        productNameVisible = productNameVisible ||
            label->text() == QStringLiteral("OPPO Find X8");
        androidVisible = androidVisible || label->text() == QStringLiteral("16");
        versionVisible = versionVisible ||
            label->text() == QStringLiteral("CPH9999_16.0.1");
        patchVisible = patchVisible ||
            label->text() == QStringLiteral("2026-08-05");
    }
    if (!brandVisible || !chipVisible || !serialVisible || !storageVisible ||
        !storageProductVisible || !productNameVisible || !androidVisible ||
        !versionVisible || !patchVisible) {
        std::cerr << "Device identity or storage details were not rendered\n";
        return 21;
    }

    bool versionButtonVisible = false;
    bool protectionEnabled = false;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        versionButtonVisible = versionButtonVisible ||
            button->text() == QStringLiteral("读取版本");
    }
    for (QCheckBox* checkBox : window.findChildren<QCheckBox*>()) {
        protectionEnabled = protectionEnabled ||
            (checkBox->text() == QStringLiteral("保护分区") && checkBox->isChecked());
    }
    if (!versionButtonVisible || !protectionEnabled) {
        std::cerr << "Version reader or protected partition control is missing\n";
        return 24;
    }

    swt::edl::PartitionInfo bootPartition;
    bootPartition.name = QStringLiteral("boot_a");
    bootPartition.firstLba = 1024;
    bootPartition.lastLba = 2047;
    QVector<swt::edl::PartitionInfo> fixturePartitions{bootPartition};
    if (!QMetaObject::invokeMethod(&window, "showPartitions", Qt::DirectConnection,
                                   Q_ARG(QVector<swt::edl::PartitionInfo>,
                                         fixturePartitions))) {
        return 22;
    }
    partitionTable->item(0, 0)->setCheckState(Qt::Checked);
    QApplication::processEvents();
    if (partitionTable->item(0, 4)->text() != QStringLiteral("双击选择镜像")) {
        std::cerr << "Checked partition did not request an explicit image mapping\n";
        return 23;
    }

    std::cout << "MainWindow explicit partition mapping, device info, log and progress tests passed\n";
    return 0;
}
