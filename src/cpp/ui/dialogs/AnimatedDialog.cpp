#include "ui/dialogs/AnimatedDialog.h"

#include "ui/theme/Motion.h"

#include <QPointer>
#include <QAbstractAnimation>
#include <QPropertyAnimation>
#include <QShowEvent>
#include <QTimer>

namespace kc::ui {

AnimatedDialog::AnimatedDialog(QWidget* parent)
    : QDialog(parent)
{
}

void AnimatedDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    // 🔴 2026-10-21：**复用同一个弹窗时，第二次打开会完全看不见**。
    //    原因：退场动画把 windowOpacity 一路淡到 0 才调 QDialog::done()，
    //    而 m_inPlayed 一直为真 —— playIn() 进门就 return，没人把透明度拉回 1，
    //    窗口明明 show 了，却是一整块全透明（用户口径「关闭后再次点击，弹窗不显示」）。
    //    所以每次 show 都要：停掉可能还在跑的旧动画 → 复位两个标志 → 显式恢复不透明度 → 重新播入场。
    //    只停**作用在本窗口上**的动画（入场/退场那两条），别误伤子控件自己的动画
    //    （比如加载指示器那种常驻动画，停掉就再也转不起来了）。
    for (QPropertyAnimation* anim : findChildren<QPropertyAnimation*>()) {
        if (anim->targetObject() == this) anim->stop();
    }
    m_closing = false;
    m_inPlayed = false;
    setWindowOpacity(1.0);
    playIn();
}

void AnimatedDialog::playIn()
{
    if (m_inPlayed) return;
    m_inPlayed = true;

    if (!Motion::animationsEnabled()) {
        setWindowOpacity(1.0);   // 关掉动画也要保证可见（上一轮退场可能把它留在 0）
        return;
    }

    const QPoint endPos = pos();
    const QPoint startPos(endPos.x(), m_riseEnabled ? endPos.y() + 12 : endPos.y());

    // 基值先落到起点：动画启动前读到的就是起点，不会先以原样亮一下再淡入。
    setWindowOpacity(0.0);
    if (m_riseEnabled) move(startPos);

    QPointer<AnimatedDialog> guard(this);
    auto finish = [guard, endPos]() {
        if (!guard) return;
        guard->setWindowOpacity(1.0);
        guard->move(endPos);
    };

    auto* fade = Motion::tween(this, "windowOpacity", 0.0, 1.0, 200, Motion::silkEase());

    if (!m_riseEnabled) {
        // 只淡入不位移（登录框口径）：靠 fade 的 finished + 兜底定时器复位即可。
        QObject::connect(fade, &QAbstractAnimation::finished, this, finish);
        QTimer::singleShot(200 + Motion::TimeoutFudgeMs, this, [guard, finish]() {
            if (!guard) return;
            finish();
        });
        return;
    }

    // 🔴 pos 不能走 Motion::tween：那个助手只收 qreal，而 pos 是 QPoint，
    //    拿 double 去插 QPoint 属性会静默失效（动画"在跑"但窗口不动）。
    //    位移动画必须显式给 QPoint 的起终点，让 QVariantAnimation 按 QPoint 插值。
    auto* rise = new QPropertyAnimation(this, "pos", this);
    rise->setDuration(200);
    rise->setStartValue(startPos);
    rise->setEndValue(endPos);
    rise->setEasingCurve(Motion::silkEase());

    QObject::connect(fade, &QAbstractAnimation::finished, this, finish);
    // 兜底：动画被外部因素打断（例如期间再次 show / 主题切换重排）也要把状态复位。
    QTimer::singleShot(200 + Motion::TimeoutFudgeMs, this, [guard, finish]() {
        if (!guard) return;
        finish();
    });

    rise->start(QAbstractAnimation::DeleteWhenStopped);
}

void AnimatedDialog::done(int r)
{
    if (m_closing || !Motion::animationsEnabled() || !isVisible()) {
        QDialog::done(r);
        return;
    }

    m_closing = true;
    const QPoint startPos = pos();
    const QPoint endPos(startPos.x(), startPos.y() + 8);

    auto* fade = Motion::tween(this, "windowOpacity", 1.0, 0.0, 150, Motion::exitEase());
    auto* sink = new QPropertyAnimation(this, "pos", this);
    sink->setDuration(150);
    sink->setStartValue(startPos);
    sink->setEndValue(endPos);
    sink->setEasingCurve(Motion::exitEase());

    // 🔴 关键：把真正的 QDialog::done 推到动画结束之后。done() 会退出 exec() 的
    //    嵌套事件循环，在循环里同步播动画等于一帧都不走。
    QPointer<AnimatedDialog> guard(this);
    auto closeNow = [guard, r]() {
        if (!guard) return;
        // m_closing 已为真，这里再走一次 done() 会直接落到 QDialog::done。
        guard->done(r);
    };
    QObject::connect(fade, &QAbstractAnimation::finished, this, closeNow, Qt::QueuedConnection);
    QTimer::singleShot(150 + Motion::TimeoutFudgeMs, this, closeNow);
}

} // namespace kc::ui
