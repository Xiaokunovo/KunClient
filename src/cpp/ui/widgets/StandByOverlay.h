#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  StandByOverlay —— 待机模式覆盖层（iOS StandBy 的桌面版）
//
//  它是**一整屏海报**，不是普通控件：盖满主窗口、绝对定位、不参与任何布局。
//  下面几条口径都是踩过的坑，改之前先读：
//
//   · **不用布局管理器**：时钟 / 三块卡 / 底部提示的相对位置要随窗口高度成比例
//     走（640×420 与 1600×900 都得好看）。QVBoxLayout 在矮窗口里会把 96px 的
//     时钟压成一条，而「按比例绝对定位」一个 resizeEvent 就说清了。
//
//   · **整层的淡入靠自绘 alpha，不靠 windowOpacity / QGraphicsOpacityEffect**：
//     本控件是**子控件**，windowOpacity 对子控件无效（那是顶层窗的属性）；
//     QGraphicsOpacityEffect 挂在自绘容器上又会把整层先渲染到离屏位图再合成，
//     和 paintEvent 里的径向渐变 / 半透明叠色搅在一起，边缘会发灰。所以用一个
//     qreal 属性（Q_PROPERTY fade）给 QPropertyAnimation 驱动，paintEvent 里
//     所有颜色乘 m_alpha —— 最稳，且逐帧只改颜色、不重建控件。
//     （卡里的**叶子标签**另说：它们各自挂一个 QGraphicsOpacityEffect，见
//      .cpp 里 makeLabel 的注释 —— 叶子是独立的一次小合成，不参与本层自绘。）
//
//  🔴 待机层**永远是深底**（连浅色主题下也是），这是「熄灯」语义：浅色主题下
//     铺白底会像程序崩了。所以这里的取色不走 ThemeManager 的卡片色，只借它的
//     isDark() 区分两档深灰（差几个灰阶而已）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

namespace kc::ui {

/// 一块读数卡（自绘卡底 + 卡内标题 / 大值 / 可选副标题）。定义在 .cpp 内，无 Q_OBJECT。
class ReadoutCard;

/// 待机模式覆盖层（iOS StandBy 的桌面版）。
///
/// 为什么值得做：刷机时人经常一等等十分钟，屏幕就那么亮着、界面也没信息量。
/// 待机模式把「现在几点 + 这台设备什么状态 + 上次刷成没成」放大到一整屏，
/// 既是屏保也是仪表盘；点一下任意处就回到工作界面，不打断任何任务。
///
/// 视觉：整层用**极暗**的玻璃（不是纯黑）—— 纯黑在浅色主题下像程序崩了；
/// 主时钟 96px 细字重 + 秒级跳动，右上角日期 / 星期，下方三块读数卡
/// （设备 / 链路 / 上次刷机），底部一行「点击任意处返回」。
class StandByOverlay : public QWidget {
    Q_OBJECT
    /// 整层不透明度。由 QPropertyAnimation 驱动 —— 见文件头「为什么不用
    /// windowOpacity / QGraphicsOpacityEffect」。外部一般不用碰。
    Q_PROPERTY(qreal fade READ fade WRITE setFade)

public:
    explicit StandByOverlay(QWidget* parent = nullptr);

    /// 显示待机层：启动秒级时钟、刷新读数、淡入。
    void showOverlay();
    void hideOverlay();

    /// 外壳可主动喂读数（不喂则自己从 ShellStatus 读）。
    void setReadings(const QString& device, const QString& mode, const QString& link,
                     const QString& task, int percent);

    /// 整层不透明度 0~1（0 = 全透明，1 = 完全显形）。
    qreal fade() const { return m_alpha; }
    void setFade(qreal value);

signals:
    void dismissed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// 跟着宿主窗口一起长：外壳即使忘了在 resizeEvent 里同步 geometry 也不会露馅。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void tick();
    void rebuildTexts();
    /// 按当前尺寸重摆时钟 / 日期 / 三块卡 / 底部提示（绝对定位）。
    void relayout();
    /// 把 m_alpha 分发到三块卡（卡底自绘用）与三块卡里的值标签。
    void applyAlphaToChildren();

    QTimer* m_clock = nullptr;
    QLabel* m_timeLabel = nullptr;
    QLabel* m_dateLabel = nullptr;
    /// 三块卡的「大值」标签。**归卡所有**（ReadoutCard 构造时造、自己摆位置），
    /// 这里只是留一份指针给淡入用 —— 不要 delete / reparent 它们。
    QLabel* m_deviceValue = nullptr;
    QLabel* m_linkValue = nullptr;
    QLabel* m_lastValue = nullptr;
    QLabel* m_hintLabel = nullptr;
    /// 卡 3 的副标题（上次刷机时间 MM-dd HH:mm）；另两块卡没有副标题。
    QLabel* m_lastSubtitle = nullptr;
    /// 三块读数卡的容器（自绘卡底；卡内标签由卡自己管，见 .cpp 的 ReadoutCard）。
    ReadoutCard* m_deviceCard = nullptr;
    ReadoutCard* m_linkCard = nullptr;
    ReadoutCard* m_lastCard = nullptr;
    QString m_device, m_mode, m_link, m_task;
    int m_percent = -1;
    bool m_haveReadings = false;

    /// 整层不透明度。paintEvent 与三块卡的绘制都乘它。
    qreal m_alpha = 0.0;
    /// 淡入淡出的代际号：动画收尾时对不上，说明这一轮已被新的 show/hide 顶替，
    /// 直接让位（否则「淡出中途又被叫起来」会被收尾回调强行 hide 掉）。
    int m_fadeGen = 0;
    /// 显示前的焦点控件。退出时还回去 —— 待机层自己要吃 Esc / Space，不还焦点
    /// 的话回到工作界面后键盘导航是断的。
    QPointer<QWidget> m_prevFocus;
};

} // namespace kc::ui
