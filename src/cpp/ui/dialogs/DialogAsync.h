#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  runAsync —— 对话框里的异步辅助（ui/dialogs 共用）。
//
//  把一个**阻塞型**调用丢到 QThreadPool 上跑（kc::HttpClient / FdlApiClient /
//  UpdateService / ChunkedDownloader 都是 QEventLoop 阻塞模型，绝不能在 GUI 线程调），
//  完成后把结果经 QMetaObject::invokeMethod(context, ...) 回投到对话框线程。
//
//  🔴 context 生命周期：worker 持有 QPointer，回投前发现对话框已销毁就直接丢弃 ——
//     不会在析构后触碰任何成员（对应 C# 的 async/await 回 UI 线程序列化）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include <utility>

namespace kc::ui {

/// work() 在 worker 线程执行（可阻塞），返回值交给 done() 在 context 所在线程执行。
template <typename Work, typename Done>
void runAsync(QObject* context, Work&& work, Done&& done)
{
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start(
        [guard, work = std::forward<Work>(work),
         done = std::forward<Done>(done)]() mutable {
            auto result = work();                       // worker 线程：阻塞没关系
            if (guard.isNull()) return;                 // 对话框已经没了，别再投递
            QMetaObject::invokeMethod(
                guard, [&done, result = std::move(result)]() mutable {
                    done(std::move(result));            // GUI 线程：只碰 UI
                },
                Qt::QueuedConnection);
        });
}

} // namespace kc::ui
