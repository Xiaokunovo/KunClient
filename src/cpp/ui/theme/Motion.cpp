#include "ui/theme/Motion.h"

#include <QGraphicsOpacityEffect>
#include <QHash>
#include <QPointer>
#include <QPropertyAnimation>
#include <QSettings>
#include <QByteArray>
#include <QTimer>
#include <QWidget>

#include <cmath>

namespace kc::ui {
namespace {

// 每个 widget 一份动画令牌。C# 用 ConditionalWeakTable；Qt 用指针索引的哈希，
// 令牌只增不减、按键为指针，widget 销毁后残留的极少数条目不影响正确性
// （isCurrentToken 只会在持有有效指针时查询）。
QHash<const QWidget*, quint64>& tokenTable() {
    static QHash<const QWidget*, quint64> t;
    return t;
}

void setFrames(QPropertyAnimation* anim, double from, double to, int ms, const QEasingCurve& curve) {
    anim->setDuration(ms);
    anim->setStartValue(from);
    anim->setEndValue(to);
    anim->setEasingCurve(curve);
}

/// 保证 widget 上装了 QGraphicsOpacityEffect，返回它（用于透明度补间）。
/// C# 的 windowOpacity 在 Qt 里等价物是 QGraphicsOpacityEffect.opacity（对子控件同样生效）。
QGraphicsOpacityEffect* ensureOpacityEffect(QWidget* w) {
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(w);
        eff->setOpacity(1.0);
        w->setGraphicsEffect(eff);
    }
    return eff;
}

} // namespace

QEasingCurve Motion::appleEase() {
    QEasingCurve c(QEasingCurve::BezierSpline);
    c.addCubicBezierSegment(QPointF(0.32, 0.72), QPointF(0.0, 1.0), QPointF(1.0, 1.0));
    return c;
}

QEasingCurve Motion::silkEase() {
    QEasingCurve c(QEasingCurve::BezierSpline);
    c.addCubicBezierSegment(QPointF(0.16, 1.0), QPointF(0.3, 1.0), QPointF(1.0, 1.0));
    return c;
}

QEasingCurve Motion::exitEase() {
    QEasingCurve c(QEasingCurve::BezierSpline);
    c.addCubicBezierSegment(QPointF(0.4, 0.0), QPointF(0.6, 1.0), QPointF(1.0, 1.0));
    return c;
}

QEasingCurve Motion::springEase() {
    // 第二控制点 y=1.56 > 1：Qt 的贝塞尔缓动不钳制，过冲段天然产生回弹。
    QEasingCurve c(QEasingCurve::BezierSpline);
    c.addCubicBezierSegment(QPointF(0.34, 1.56), QPointF(0.64, 1.0), QPointF(1.0, 1.0));
    return c;
}

QEasingCurve Motion::settleEase() {
    QEasingCurve c(QEasingCurve::BezierSpline);
    c.addCubicBezierSegment(QPointF(0.2, 0.9), QPointF(0.3, 1.0), QPointF(1.0, 1.0));
    return c;
}

quint64 Motion::nextToken(const QWidget* w) {
    if (!w) return 0;
    return ++tokenTable()[w];
}

bool Motion::isCurrentToken(const QWidget* w, quint64 token) {
    if (!w) return false;
    auto it = tokenTable().constFind(w);
    return it != tokenTable().constEnd() && it.value() == token;
}

void Motion::clearAnimation(QObject* target, const char* property, double finalValue) {
    if (!target || !property) return;
    for (auto* anim : target->findChildren<QPropertyAnimation*>()) {
        if (anim->propertyName() == QByteArray(property)) {
            anim->stop();
            anim->deleteLater();
        }
    }
    if (!std::isnan(finalValue)) {
        target->setProperty(property, finalValue);
    }
}

QPropertyAnimation* Motion::tween(QObject* target, const char* property, double from, double to,
                                  int ms, const QEasingCurve& curve) {
    if (!target || !property) return nullptr;
    auto* anim = new QPropertyAnimation(target, property, target);
    setFrames(anim, from, to, ms, curve);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    return anim;
}

QPropertyAnimation* Motion::settle(QObject* target, const char* property, double from, double to,
                                   int ms, double overshoot) {
    if (!target || !property) return nullptr;
    const double peak = to + (to - from) * overshoot;
    auto* anim = new QPropertyAnimation(target, property, target);
    anim->setDuration(ms);
    anim->setKeyValueAt(0.0, from);
    anim->setKeyValueAt(0.72, peak);
    anim->setKeyValueAt(1.0, to);
    anim->setEasingCurve(appleEase());
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    return anim;
}

void Motion::start(QAbstractAnimation* anim) {
    if (!anim) return;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void Motion::reveal(QWidget* w, int durationMs, double slideFrom, int delayMs) {
    if (!w) return;

    // 已经在完全可见状态：只保证终态，不重播 ——
    // 同一个提示条被连续 Show 两次（内容更新）时不会闪一下。
    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
    if (w->isVisible() && (!eff || eff->opacity() >= 0.999) && delayMs <= 0) {
        if (eff) eff->setOpacity(1.0);
        return;
    }

    const quint64 token = nextToken(w);
    const QPoint endPos = w->pos();
    const QPoint startPos(endPos.x(), endPos.y() + static_cast<int>(slideFrom));

    // 基值必须也设成起点：延迟期间动画不参与计算，读到的就是基值，
    // 只写关键帧 From 的话延迟期元素会以原样先亮一下再淡入（"闪一下"）。
    eff = ensureOpacityEffect(w);
    eff->setOpacity(0.0);
    w->move(startPos);
    w->show();
    w->raise();

    QPointer<QWidget> guard(w);
    auto finish = [guard, token, endPos]() {
        if (!guard) return;
        if (!isCurrentToken(guard, token)) return; // 已被顶替 → 不回写
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(guard->graphicsEffect()))
            e->setOpacity(1.0);
        guard->move(endPos);
    };

    auto* fade = new QPropertyAnimation(eff, "opacity", w);
    setFrames(fade, 0.0, 1.0, durationMs, silkEase());
    auto* glide = new QPropertyAnimation(w, "pos", w);
    glide->setDuration(durationMs);
    glide->setStartValue(startPos);
    glide->setEndValue(endPos);
    glide->setEasingCurve(silkEase());

    QObject::connect(fade, &QAbstractAnimation::finished, w, finish);
    QTimer::singleShot(delayMs + durationMs + TimeoutFudgeMs, w, [guard, token, finish]() {
        if (!guard || !isCurrentToken(guard, token)) return;
        finish();
    });

    if (delayMs > 0) {
        QTimer::singleShot(delayMs, w, [fade, glide]() {
            fade->start(QAbstractAnimation::DeleteWhenStopped);
            glide->start(QAbstractAnimation::DeleteWhenStopped);
        });
    } else {
        fade->start(QAbstractAnimation::DeleteWhenStopped);
        glide->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void Motion::conceal(QWidget* w, int durationMs, double slideTo, int delayMs, bool collapseAfter) {
    if (!w) return;
    if (!w->isVisible()) return;

    const quint64 token = nextToken(w);
    const QPoint startPos = w->pos();
    const QPoint endPos(startPos.x(), startPos.y() + static_cast<int>(slideTo));

    auto* eff = ensureOpacityEffect(w);
    eff->setOpacity(1.0);

    QPointer<QWidget> guard(w);
    auto finish = [guard, token, collapseAfter]() {
        if (!guard) return;
        if (!isCurrentToken(guard, token)) return;
        if (collapseAfter) {
            if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(guard->graphicsEffect()))
                e->setOpacity(1.0); // 复位成"下次直接赋值也看得见"
            guard->hide();
        }
    };

    auto* fade = new QPropertyAnimation(eff, "opacity", w);
    setFrames(fade, 1.0, 0.0, durationMs, exitEase());
    auto* glide = new QPropertyAnimation(w, "pos", w);
    glide->setDuration(durationMs);
    glide->setStartValue(startPos);
    glide->setEndValue(endPos);
    glide->setEasingCurve(exitEase());

    QObject::connect(fade, &QAbstractAnimation::finished, w, finish);
    QTimer::singleShot(delayMs + durationMs + TimeoutFudgeMs, w, [guard, token, collapseAfter]() {
        if (!guard || !isCurrentToken(guard, token)) return;
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(guard->graphicsEffect())) {
            if (collapseAfter) e->setOpacity(1.0);
        }
        if (collapseAfter) guard->hide();
    });

    if (delayMs > 0) {
        QTimer::singleShot(delayMs, w, [fade, glide]() {
            fade->start(QAbstractAnimation::DeleteWhenStopped);
            glide->start(QAbstractAnimation::DeleteWhenStopped);
        });
    } else {
        fade->start(QAbstractAnimation::DeleteWhenStopped);
        glide->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void Motion::revealWidget(QWidget* w, int durationMs) { reveal(w, durationMs); }

void Motion::concealWidget(QWidget* w, int durationMs, bool collapseAfter) {
    conceal(w, durationMs, -10.0, 0, collapseAfter);
}

void Motion::revealX(QWidget* w, int dx, int durationMs) {
    if (!w) return;
    if (!animationsEnabled()) return;
    if (dx == 0) return;

    const quint64 token = nextToken(w);
    const QPoint endPos = w->pos();
    const QPoint startPos(endPos.x() + dx, endPos.y());

    auto* eff = ensureOpacityEffect(w);
    eff->setOpacity(0.0);
    w->move(startPos);
    w->show();
    w->raise();

    QPointer<QWidget> guard(w);
    auto finish = [guard, token, endPos]() {
        if (!guard) return;
        if (!isCurrentToken(guard, token)) return; // 已被顶替 → 不回写
        // 🔴 收尾要把透明度特效**摘掉**：整页常驻 QGraphicsOpacityEffect 会让这一页
        //    永远走合成路径（渲染开销 + 部分原生绘制优化失效），而它此时已经没用了。
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(guard->graphicsEffect())) {
            e->setOpacity(1.0);
            guard->setGraphicsEffect(nullptr);
        }
        guard->move(endPos);
    };

    auto* fade = new QPropertyAnimation(eff, "opacity", w);
    setFrames(fade, 0.0, 1.0, durationMs, silkEase());
    auto* glide = new QPropertyAnimation(w, "pos", w);
    glide->setDuration(durationMs);
    glide->setStartValue(startPos);
    glide->setEndValue(endPos);
    glide->setEasingCurve(appleEase());

    QObject::connect(fade, &QAbstractAnimation::finished, w, finish);
    QTimer::singleShot(durationMs + TimeoutFudgeMs, w, [guard, token, finish]() {
        if (!guard || !isCurrentToken(guard, token)) return;
        finish();
    });

    fade->start(QAbstractAnimation::DeleteWhenStopped);
    glide->start(QAbstractAnimation::DeleteWhenStopped);
}

void Motion::fadeIn(QWidget* w, int durationMs, int delayMs) {
    if (!w) return;

    // 关动画 / 零时长：直接落到终态。收尾同样要摘特效 ——
    // 留一个 opacity<1 的特效挂在那儿，下次直接 setVisible(true) 就会看见半透明控件。
    const bool instant = !animationsEnabled() || durationMs <= 0 || delayMs < 0;
    if (instant) {
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect())) {
            e->setOpacity(1.0);
            w->setGraphicsEffect(nullptr);
        }
        w->show();
        return;
    }

    // 🔴 这里**故意不做**「已经全亮就直接返回」的短路（reveal 里有）：
    //    fadeIn 的语义就是「从 0 走到 1」，换文案后的交叉淡化必须真的重播一次。
    //    要不要播由调用方判断（页名比对新旧文字、瓦片只看打开动作）。
    const quint64 token = nextToken(w);
    auto* eff = ensureOpacityEffect(w);
    eff->setOpacity(0.0);   // 基值也置 0：延迟期间读到的就是起点态，不会先亮一下
    w->show();

    QPointer<QWidget> guard(w);
    auto finish = [guard, token]() {
        if (!guard) return;
        if (!isCurrentToken(guard, token)) return;   // 已被顶替 → 不回写
        // 与 revealX 一致：收尾把透明度特效摘掉。常驻 QGraphicsOpacityEffect 会让
        // 这个控件永远走合成路径（渲染开销 + 部分原生绘制优化失效），而它此时已经没用了。
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(guard->graphicsEffect())) {
            e->setOpacity(1.0);
            guard->setGraphicsEffect(nullptr);
        }
    };

    auto* fade = new QPropertyAnimation(eff, "opacity", w);
    setFrames(fade, 0.0, 1.0, durationMs, silkEase());
    QObject::connect(fade, &QAbstractAnimation::finished, w, finish);
    QTimer::singleShot(delayMs + durationMs + TimeoutFudgeMs, w, [guard, token, finish]() {
        if (!guard || !isCurrentToken(guard, token)) return;
        finish();
    });

    if (delayMs > 0) {
        QTimer::singleShot(delayMs, w, [fade]() {
            fade->start(QAbstractAnimation::DeleteWhenStopped);
        });
    } else {
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void Motion::fadeOut(QWidget* w, int durationMs, bool hideAfter) {
    if (!w) return;

    const bool instant = !animationsEnabled() || durationMs <= 0;
    if (instant) {
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect())) {
            e->setOpacity(1.0);
            w->setGraphicsEffect(nullptr);
        }
        if (hideAfter) w->hide();
        return;
    }
    if (!w->isVisible()) {
        if (hideAfter) w->hide();
        return;
    }

    const quint64 token = nextToken(w);
    auto* eff = ensureOpacityEffect(w);
    eff->setOpacity(1.0);

    QPointer<QWidget> guard(w);
    auto finish = [guard, token, hideAfter]() {
        if (!guard) return;
        if (!isCurrentToken(guard, token)) return;
        // 🔴 先复位成 1.0 再 hide：留着 opacity=0 的控件，下次 show() 出来是隐形的
        //    （这个坑 NotificationBar 的 conceal 里已经踩过一次）。
        if (auto* e = qobject_cast<QGraphicsOpacityEffect*>(guard->graphicsEffect())) {
            e->setOpacity(1.0);
            guard->setGraphicsEffect(nullptr);
        }
        if (hideAfter) guard->hide();
    };

    auto* fade = new QPropertyAnimation(eff, "opacity", w);
    setFrames(fade, 1.0, 0.0, durationMs, exitEase());
    QObject::connect(fade, &QAbstractAnimation::finished, w, finish);
    QTimer::singleShot(durationMs + TimeoutFudgeMs, w, [guard, token, finish]() {
        if (!guard || !isCurrentToken(guard, token)) return;
        finish();
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

bool Motion::animationsEnabled() {
    // 探针旁路：KC_NO_ANIM=1 时整体关掉动画。截图要的是**确定性**——
    // 带动画时抓到的帧落在哪一阶段全看时序，同一个命令两次抓图可能不一样。
    if (qEnvironmentVariableIsSet("KC_NO_ANIM")
        && qEnvironmentVariable("KC_NO_ANIM") != QLatin1String("0")) {
        return false;
    }
    // QSettings 的键由 ThemeManager/SettingsPage 落（appearance/animations）。
    // 这里不 include QSettings，避免 Motion 反向依赖配置层：用 QCoreApplication
    // 的 organizationName/applicationName 取同一份存储。
    const bool on = QSettings().value(QStringLiteral("appearance/animations"), true).toBool();
    return on;
}

void Motion::setAnimationsEnabled(bool on) {
    QSettings().setValue(QStringLiteral("appearance/animations"), on);
}

} // namespace kc::ui
