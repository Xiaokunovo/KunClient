#include "ui/widgets/FpsProbe.h"

#include <QApplication>
#include <QEvent>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cstdio>

namespace kc::ui {

FpsProbe::FpsProbe(QWidget* window, QObject* parent)
    : QObject(parent), m_window(window)
{
}

void FpsProbe::start(int ms)
{
    if (m_running) return;
    m_running = true;

    m_updateRequests = 0;
    m_paints = 0;
    m_paintNanos = 0;
    m_maxPaintNanos = 0;
    m_frameNanos.clear();

    // 只监听本窗口子树：别的窗口（弹窗 / 玻璃菜单）的重绘不该算进来。
    if (m_window) m_window->installEventFilter(this);
    qApp->installEventFilter(this);

    m_clock.start();
    QTimer::singleShot(ms, this, [this] { report(); });
}

bool FpsProbe::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_running) return QObject::eventFilter(watched, event);

    // 只统计本窗口及其子控件的重绘 —— 目标就是「主屏常驻时到底刷了多少帧」。
    QWidget* w = qobject_cast<QWidget*>(watched);
    if (!w) return QObject::eventFilter(watched, event);
    if (m_window && w != m_window && !m_window->isAncestorOf(w))
        return QObject::eventFilter(watched, event);

    const QEvent::Type type = event->type();
    if (type == QEvent::UpdateRequest) {
        ++m_updateRequests;
        // 按控件累计 UpdateRequest：这是**谁在喊重绘**的直接证据。
        // 只留前若干名，避免 unordered_map 在热路径上反复扩容。
        if (m_updateByWidget.size() < 512 || m_updateByWidget.contains(w)) {
            ++m_updateByWidget[w];
        }
    } else if (type == QEvent::Paint) {
        ++m_paints;
        if (m_paintByWidget.size() < 512 || m_paintByWidget.contains(w)) {
            ++m_paintByWidget[w];
        }
    }
    return QObject::eventFilter(watched, event);
}

void FpsProbe::report()
{
    m_running = false;
    if (m_window) m_window->removeEventFilter(this);
    qApp->removeEventFilter(this);

    const qint64 elapsedMs = std::max<qint64>(1, m_clock.elapsed());
    const double seconds = elapsedMs / 1000.0;

    std::fprintf(stderr,
                 "[fps] window=%lldms  updateRequests=%d (%.1f/s)  paints=%d (%.1f/s)\n",
                 static_cast<long long>(elapsedMs), m_updateRequests,
                 m_updateRequests / seconds, m_paints, m_paints / seconds);

    // 按 UpdateRequest 排序，打印前 12 名 —— 这就是「谁在喊重绘」的答案。
    QVector<QPair<int, QString>> rows;
    rows.reserve(m_updateByWidget.size());
    for (auto it = m_updateByWidget.constBegin(); it != m_updateByWidget.constEnd(); ++it) {
        QWidget* w = it.key();
        const QString name = QStringLiteral("%1#%2")
                                 .arg(QString::fromLatin1(w->metaObject()->className()),
                                      w->objectName().isEmpty()
                                          ? QStringLiteral("(unnamed)")
                                          : w->objectName());
        rows.append({it.value(), name});
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::fprintf(stderr, "[fps] top updaters:\n");
    for (int i = 0; i < rows.size() && i < 12; ++i) {
        std::fprintf(stderr, "        %6d  %s\n", rows[i].first,
                     rows[i].second.toUtf8().constData());
    }

    std::fprintf(stderr, "[fps] verdict: %s\n",
                 m_paints / seconds > 30.0
                     ? "帧率过高 —— 有东西在常驻状态下持续重绘（见 top updaters）"
                     : "帧率正常 —— 卡顿不在常驻重绘，查转场或单帧耗时");
}

} // namespace kc::ui
