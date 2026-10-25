#include "ui/widgets/SvgPath.h"

#include <QPointF>
#include <QtGlobal>

#include <cmath>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace kc::ui {
namespace {

constexpr qreal kTiny = 1e-12;

// ─────────────────────────────────────────────────────────────────────────────
//  词法：把 "M224,120v96a8,8,0,0,1-8,8H160Z" 拆成「命令字符 + 数值流」
//
//  SVG 的数字写法比想象中松：逗号/空白都可作分隔符，也可以**完全不写分隔符**
//  （`1-8` 是两个数 1 和 -8；`1.5.5` 是两个数 1.5 和 .5）。所以不能先 split，
//  必须逐字符扫。指数形式（1e-5）也要认，否则科学计数法的路径会解析歪。
// ─────────────────────────────────────────────────────────────────────────────
class Lexer {
public:
    explicit Lexer(const QString& src) : m_s(src) {}

    /// 跳过分隔符（空白 / 逗号）。
    void skipSeparators()
    {
        while (m_i < m_s.size()) {
            const QChar c = m_s.at(m_i);
            if (c.isSpace() || c == QLatin1Char(',')) ++m_i;
            else break;
        }
    }

    bool atEnd()
    {
        skipSeparators();
        return m_i >= m_s.size();
    }

    /// 下一个 token 是不是命令字母（区分「隐式重复参数」和「新命令」）。
    bool peekIsCommand()
    {
        skipSeparators();
        return m_i < m_s.size() && m_s.at(m_i).isLetter();
    }

    QChar takeCommand()
    {
        skipSeparators();
        return m_i < m_s.size() ? m_s.at(m_i++) : QChar();
    }

    /// 读一个数。失败返回 0 并把 ok 置假（调用方据此判断是否该停）。
    qreal number(bool* ok = nullptr)
    {
        skipSeparators();
        const int start = m_i;
        if (m_i < m_s.size()
            && (m_s.at(m_i) == QLatin1Char('+') || m_s.at(m_i) == QLatin1Char('-')))
            ++m_i;
        while (m_i < m_s.size() && m_s.at(m_i).isDigit()) ++m_i;
        if (m_i < m_s.size() && m_s.at(m_i) == QLatin1Char('.')) {
            ++m_i;
            while (m_i < m_s.size() && m_s.at(m_i).isDigit()) ++m_i;
        }
        if (m_i < m_s.size()
            && (m_s.at(m_i) == QLatin1Char('e') || m_s.at(m_i) == QLatin1Char('E'))) {
            ++m_i;
            if (m_i < m_s.size()
                && (m_s.at(m_i) == QLatin1Char('+') || m_s.at(m_i) == QLatin1Char('-')))
                ++m_i;
            while (m_i < m_s.size() && m_s.at(m_i).isDigit()) ++m_i;
        }
        const bool good = m_i > start;
        if (ok) *ok = good;
        return good ? m_s.mid(start, m_i - start).toDouble() : 0.0;
    }

private:
    const QString& m_s;
    int m_i = 0;
};

/// 两个向量之间的夹角（带符号），SVG 规范 F.6.5.4 用的那个。
qreal vectorAngle(qreal ux, qreal uy, qreal vx, qreal vy)
{
    const qreal len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    if (len < kTiny) return 0.0;
    qreal a = std::acos(qBound(-1.0, (ux * vx + uy * vy) / len, 1.0));
    if (ux * vy - uy * vx < 0) a = -a;
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
//  椭圆弧 → 三次贝塞尔
//
//  SVG 的 A 命令给的是**端点参数化**（起点、两个半径、旋转角、大小弧标志、
//  扫向标志、终点），而绘图后端要的是**中心参数化**。中间这步换算就是规范
//  F.6.5 的公式，抄错了会得到镜像或反方向的弧 —— 图标会直接变形，很好发现。
//
//  换算完再把弧按 ≤90° 切段，每段用一段三次贝塞尔逼近（单段贝塞尔的误差在
//  90° 以内约 0.03%，肉眼不可见；这也是各家 SVG 渲染器的通行做法）。
// ─────────────────────────────────────────────────────────────────────────────
void arcTo(QPainterPath& path, qreal rx, qreal ry, qreal rotationDeg,
           bool largeArc, bool sweep, const QPointF& end)
{
    const QPointF start = path.currentPosition();

    // 半径为 0 时按规范退化成直线。
    if (std::abs(rx) < kTiny || std::abs(ry) < kTiny) {
        path.lineTo(end);
        return;
    }
    rx = std::abs(rx);
    ry = std::abs(ry);

    const qreal phi = rotationDeg * M_PI / 180.0;
    const qreal cosPhi = std::cos(phi);
    const qreal sinPhi = std::sin(phi);

    // 把起点/终点挪到「以中点为原点、且消掉椭圆旋转」的坐标系里。
    const qreal dx2 = (start.x() - end.x()) / 2.0;
    const qreal dy2 = (start.y() - end.y()) / 2.0;
    const qreal x1p =  cosPhi * dx2 + sinPhi * dy2;
    const qreal y1p = -sinPhi * dx2 + cosPhi * dy2;

    // 半径小到画不出这段弧时，规范要求等比放大到刚好够。
    const qreal lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const qreal k = std::sqrt(lambda);
        rx *= k;
        ry *= k;
    }

    const qreal rx2 = rx * rx;
    const qreal ry2 = ry * ry;
    qreal numerator = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    if (numerator < 0.0) numerator = 0.0;   // 浮点误差兜底
    const qreal denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
    qreal coef = denominator > kTiny ? std::sqrt(numerator / denominator) : 0.0;
    if (largeArc == sweep) coef = -coef;    // 两个标志相同则取另一侧圆心

    const qreal cxp =  coef * (rx * y1p / ry);
    const qreal cyp = -coef * (ry * x1p / rx);
    const qreal cx = cosPhi * cxp - sinPhi * cyp + (start.x() + end.x()) / 2.0;
    const qreal cy = sinPhi * cxp + cosPhi * cyp + (start.y() + end.y()) / 2.0;

    const qreal ux = (x1p - cxp) / rx;
    const qreal uy = (y1p - cyp) / ry;
    const qreal vx = (-x1p - cxp) / rx;
    const qreal vy = (-y1p - cyp) / ry;

    const qreal theta1 = vectorAngle(1.0, 0.0, ux, uy);
    qreal dTheta = vectorAngle(ux, uy, vx, vy);
    if (!sweep && dTheta > 0.0)      dTheta -= 2.0 * M_PI;
    else if (sweep && dTheta < 0.0)  dTheta += 2.0 * M_PI;

    // 椭圆上参数角 t 处的点，以及它对 t 的导数（用来定贝塞尔控制点）。
    const auto pointAt = [&](qreal t) {
        const qreal ct = std::cos(t);
        const qreal st = std::sin(t);
        return QPointF(cx + rx * ct * cosPhi - ry * st * sinPhi,
                       cy + rx * ct * sinPhi + ry * st * cosPhi);
    };
    const auto derivativeAt = [&](qreal t) {
        const qreal ct = std::cos(t);
        const qreal st = std::sin(t);
        return QPointF(-rx * st * cosPhi - ry * ct * sinPhi,
                       -rx * st * sinPhi + ry * ct * cosPhi);
    };

    const int segments = qMax(1, static_cast<int>(std::ceil(std::abs(dTheta) / (M_PI / 2.0))));
    const qreal delta = dTheta / segments;
    const qreal alpha = 4.0 / 3.0 * std::tan(delta / 4.0);

    qreal t = theta1;
    for (int i = 0; i < segments; ++i) {
        const qreal tNext = t + delta;
        const QPointF p0 = pointAt(t);
        const QPointF p1 = pointAt(tNext);
        path.cubicTo(p0 + alpha * derivativeAt(t),
                     p1 - alpha * derivativeAt(tNext),
                     p1);
        t = tNext;
    }
}

} // namespace

QPainterPath svgPathFromString(const QString& d)
{
    QPainterPath path;
    // 图标库的字形按 nonzero 规则设计：必须显式设，否则重叠子路径会被挖洞。
    path.setFillRule(Qt::WindingFill);

    Lexer lex(d);
    QChar command;
    QPointF current;       // 当前点
    QPointF subpathStart;  // 当前子路径起点（Z 之后要回到这里）

    // 相对命令的参考点始终是**命令开始时**的当前点，所以读参数时不能改 current。
    //
    // 🔴 x / y 必须**分两条语句**读。写成 QPointF(lex.number(), lex.number()) 是错的：
    //    C++ 对函数实参的求值顺序是未规定的，GCC 实际从右往左求值 —— 于是 y 先被读走、
    //    x 拿到第二个数，整个路径的坐标轴被对调。对称字形（齿轮、四宫格）看着还像样，
    //    非对称的（房子、CPU）直接变成一坨，极难一眼看出是求值顺序的锅。
    const auto readPoint = [&](bool relative) {
        const qreal x = lex.number();
        const qreal y = lex.number();
        const QPointF p(x, y);
        return relative ? current + p : p;
    };

    while (true) {
        lex.skipSeparators();
        if (lex.atEnd()) break;

        if (lex.peekIsCommand()) {
            command = lex.takeCommand();
        } else if (command.isNull()) {
            break;   // 数字先于任何命令出现 —— 非法数据，停
        } else if (command == QLatin1Char('M')) {
            command = QLatin1Char('L');   // 隐式重复：M 之后多余的坐标按 L 处理
        } else if (command == QLatin1Char('m')) {
            command = QLatin1Char('l');
        }

        const bool relative = command.isLower();
        const QChar op = command.toUpper();

        if (op == QLatin1Char('Z')) {
            path.closeSubpath();
            current = subpathStart;
            command = QChar();   // Z 后面不允许裸数字，置空以免下一轮死循环
            continue;
        }

        if (op == QLatin1Char('M')) {
            const QPointF p = readPoint(relative);
            path.moveTo(p);
            current = subpathStart = p;
            continue;
        }

        if (op == QLatin1Char('L')) {
            current = readPoint(relative);
            path.lineTo(current);
            continue;
        }

        if (op == QLatin1Char('H')) {
            const qreal x = lex.number();
            current = QPointF(relative ? current.x() + x : x, current.y());
            path.lineTo(current);
            continue;
        }

        if (op == QLatin1Char('V')) {
            const qreal y = lex.number();
            current = QPointF(current.x(), relative ? current.y() + y : y);
            path.lineTo(current);
            continue;
        }

        if (op == QLatin1Char('C')) {
            const QPointF c1 = readPoint(relative);
            const QPointF c2 = readPoint(relative);
            const QPointF e  = readPoint(relative);
            path.cubicTo(c1, c2, e);
            current = e;
            continue;
        }

        if (op == QLatin1Char('A')) {
            const qreal rx = lex.number();
            const qreal ry = lex.number();
            const qreal rot = lex.number();
            const bool large = lex.number() != 0.0;
            const bool sweep = lex.number() != 0.0;
            // 同上：端点坐标分两条语句读，别塞进同一个构造函数调用。
            const qreal ex = lex.number();
            const qreal ey = lex.number();
            QPointF e(ex, ey);
            if (relative) e += current;
            arcTo(path, rx, ry, rot, large, sweep, e);
            current = e;
            continue;
        }

        // 未实现的命令（S/Q/T 等）：吞掉一个数避免原地打转，然后继续。
        lex.number();
    }

    return path;
}

} // namespace kc::ui
