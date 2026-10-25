#pragma once

#include <QObject>
#include <QString>

namespace swt::edl {

/// 在**独立线程**里阻塞读 stdin，逐行发出去。
///
/// 🔴 为什么必须单独开线程：主线程要跑事件循环才能把 worker 的信号投递出去。
/// 如果主线程去阻塞读 stdin，那么「指令是收到了，但所有事件都发不出去」——
/// 外部看到的就是「发一条指令没反应」，最难查的那种。
class StdinReader final : public QObject {
    Q_OBJECT

public:
    explicit StdinReader(QObject* parent = nullptr);

public slots:
    /// 在独立线程里跑（`QThread::started` → 这个槽）。读到 EOF 会发 [`eof`]。
    void run();

signals:
    void lineReceived(const QString& line);
    void eof();
};

} // namespace swt::edl
