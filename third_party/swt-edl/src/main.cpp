#include "core/Types.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QSettings>
#include <QTimer>

#include <cstring>

namespace swt::edl {
/// headless（`--cli`）入口，实现在 src/cli/HeadlessMain.cpp。
int runHeadless(int argc, char** argv);
}

int main(int argc, char* argv[])
{
    // 🔴 `--cli` 必须在**构造 QApplication 之前**判断：
    //    CLI 模式走 QCoreApplication（不需要窗口系统）。反过来的话，
    //    在没有桌面/服务化的环境里 QApplication 会先起不来，CLI 也就跟着废了。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cli") == 0) {
            return swt::edl::runHeadless(argc, argv);
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("QST"));
    QApplication::setApplicationVersion(QStringLiteral("0.11.0"));
    QApplication::setOrganizationName(QStringLiteral("SWT"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/logo.png")));

    QSettings settings;
    if (app.arguments().contains(QStringLiteral("--light"))) {
        settings.setValue(QStringLiteral("appearance/darkTheme"), false);
    } else if (app.arguments().contains(QStringLiteral("--dark"))) {
        settings.setValue(QStringLiteral("appearance/darkTheme"), true);
    }

    qRegisterMetaType<swt::edl::StorageType>();
    qRegisterMetaType<swt::edl::SessionState>();
    qRegisterMetaType<swt::edl::PartitionInfo>();
    qRegisterMetaType<QVector<swt::edl::PartitionInfo>>();
    qRegisterMetaType<swt::edl::DeviceInfo>();
    qRegisterMetaType<swt::edl::OplusVipPackage>();

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    app.setFont(font);

    QFile style(QStringLiteral(":/styles/dark.qss"));
    if (style.open(QIODevice::ReadOnly)) {
        app.setStyleSheet(QString::fromUtf8(style.readAll()));
    }

    swt::edl::MainWindow window;
    window.show();

    const int screenshotIndex = app.arguments().indexOf(QStringLiteral("--screenshot"));
    if (screenshotIndex >= 0) {
        const QString path = screenshotIndex + 1 < app.arguments().size()
                                 ? app.arguments().at(screenshotIndex + 1)
                                 : QDir::current().filePath(QStringLiteral("SWT-EDL-preview.png"));
        QTimer::singleShot(500, &window, [&window, path] {
            window.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
