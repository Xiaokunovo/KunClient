#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QVector>

class QWidget;

namespace kc::ui {

// ─────────────────────────────────────────────────────────────────────────────
//  FpsProbe —— 主屏常驻状态的**重绘**度量（探针，只在 --fps-probe 时构造）
//
//  用户口径「动画会有卡顿」。卡顿有两种完全不同的成因，必须分开量：
//    ① 帧太多：某个定时器 / 信号在反复 update()，把 CPU 占满（空闲也不停）；
//    ② 帧太慢：单帧绘制超预算（16.7ms @60Hz），肉眼就是一顿一顿。
//  只数「每帧耗时」分不出 ①，只数「帧率」分不出 ②，所以两个都测。
//
//  实现：装一个 QApplication 级事件过滤器，数 UpdateRequest / Paint 事件；
//  再包一层 QWidget::paintEvent 是做不到的（虚函数不能截），所以改用
//  「Paint 事件进出时间戳」—— QApplication 过滤器看到 Paint 时，绘制还没发生，
//  这里用 sendEvent 的前后夹时间来近似单帧成本。
//
//  🔴 探针本身必须是**零渲染开销**的：只做整数计数与一次 elapsed()，
//     不画东西、不打日志（避免自己成为卡顿源、污染测量结果）。
// ─────────────────────────────────────────────────────────────────────────────
class FpsProbe : public QObject {
public:
    explicit FpsProbe(QWidget* window, QObject* parent = nullptr);

    /// 开始采样 ms 毫秒，到时打印一行统计。
    void start(int ms);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void report();

    QWidget* m_window = nullptr;
    QElapsedTimer m_clock;
    bool m_running = false;

    int m_updateRequests = 0;   ///< 收到的 UpdateRequest（= 请求重绘的次数）
    int m_paints = 0;           ///< 实际发生的 Paint 事件数
    qint64 m_paintNanos = 0;    ///< 累计绘制耗时
    qint64 m_maxPaintNanos = 0; ///< 单帧最慢
    QVector<qint64> m_frameNanos;

    /// 按控件累计（用于定位「谁在喊重绘」）。键是控件指针，只用于本次采样。
    QHash<QWidget*, int> m_updateByWidget;
    QHash<QWidget*, int> m_paintByWidget;
};

} // namespace kc::ui
