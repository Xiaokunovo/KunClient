#include "cli/HeadlessController.h"
#include "cli/StdinReader.h"

#include <QCoreApplication>
#include <QThread>

#include <cstdlib>

namespace swt::edl {

/// `--cli` 的入口：与 GUI 完全分叉，**不构造 QApplication、不建 MainWindow**。
///
/// 为什么不用 QApplication：CLI 模式下不需要任何窗口系统，用 QCoreApplication
/// 能少一层依赖（某些无桌面/服务化场景下 QApplication 直接起不来）。
int runHeadless(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SWT-EDL"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.11.0"));

    // 自定义类型要跨线程排队投递，必须先注册（GUI 侧在 main.cpp 里做，这里同样要做一次）
    qRegisterMetaType<StorageType>();
    qRegisterMetaType<SessionState>();
    qRegisterMetaType<PartitionInfo>();
    qRegisterMetaType<QVector<PartitionInfo>>();
    qRegisterMetaType<DeviceInfo>();
    qRegisterMetaType<OplusVipPackage>();

    HeadlessController controller;
    if (!controller.start()) {
        return 2;
    }

    // stdin 放独立线程读（见 StdinReader 注释：主线程要被事件循环占着）
    QThread stdinThread;
    StdinReader reader;
    reader.moveToThread(&stdinThread);
    QObject::connect(&stdinThread, &QThread::started, &reader, &StdinReader::run);
    QObject::connect(&reader, &StdinReader::lineReceived, &controller,
                     &HeadlessController::handleLine, Qt::QueuedConnection);
    QObject::connect(&reader, &StdinReader::eof, &controller,
                     &HeadlessController::handleEof, Qt::QueuedConnection);
    stdinThread.start();

    const int rc = app.exec();

    // 🔴 不能走「stdinThread.quit() → wait() → 让 QThread 自然析构」这条路，否则
    //    进程会**以 0xC0000602（STATUS_FAIL_FAST_EXCEPTION / abort）退出**：
    //    · 读 stdin 的线程正阻塞在 `_read()` 上，`QThread::quit()` 只退事件循环、
    //      叫不醒一次阻塞式读，所以 `wait(2000)` 必然超时；
    //    · 随后 `~QThread` 发现「线程仍在运行」→ qFatal → abort()。
    //    实测（同一个 exe，同一份指令）：stdin 保持打开时 rc=0xC0000602；
    //    stdin 先被关闭（读线程遇到 EOF 自己退出）时 rc=0。
    //    危害：父进程（探针 / KunClient）会把「正常收尾」误判成「CLI 崩了」。
    //
    //    正确做法 = 先把**设备侧**收尾做完（这一步是必需的、且能同步完成），
    //    然后直接 `std::_Exit` —— 进程马上就结束了，没必要再去 join 一条
    //    注定叫不醒的读线程，也不该让它的析构把退出码带歪。
    controller.stop();      // 幂等；让 worker 先断开设备
    std::_Exit(rc);         // 不再返回：跳过 QThread 析构与静态析构
}

} // namespace swt::edl
