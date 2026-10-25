#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ControlCenterPage —— 控制中心（2026-10-18 从「顶部下拉面板」改成整页）
//
//  用户口径：「灵动岛下来改为切换页面的控制台……这个页面向左滑是控制中心，
//  默认显示为主页，然后把现在这个删掉」——即原来的下拉抽屉（ui/widgets/
//  ControlCenter）整体废弃，控制中心变成主屏**左边/右边的一页**：
//
//    主屏 ──向左滑──▶ 控制中心 ──向右滑──▶ 主屏
//
//  所以这一页不再是「浮在内容上的玻璃板」，而是页面栈里的普通页（继承
//  BasePage）：顶栏的「‹ 控制台」返回钮、转场叠层、状态栏全都自动复用。
//
//  内容与旧面板一一对应（8 块开关原样保留）：
//    · 状态条：设备 / 模式 / 协议 / 链路 四枚只读芯片（数据来自 ShellStatus）；
//    · 磁贴区：4 × 2 共 8 枚开关（深色 / 液态玻璃 / 背景模糊 / 背景动效 /
//      自动识别 9008 / 日志详细 / 检查更新 / 恢复默认外观）；
//    · 滑杆区：卡片不透明度、背景不透明度 —— 拖动即时生效，松手落盘；
//    · 快捷入口：主屏 / Qualcomm / 设置 / 关于。
//
//  🔴 磁贴位置归 QGridLayout 管，进场动画只能用 Motion::fadeIn（纯透明度）。
//     走 Motion::reveal 的话它内部 move() 出来的位移会被下一次重排抹掉，
//     中途还会闪一下错位（2026-10-17 在旧面板上实测踩过）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QColor>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QLabel;
class QPainter;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace kc::ui {

/// 控制中心里的一枚磁贴（自绘：矢量图标 + 标题 + 开关状态）。
class CcTile : public QWidget {
    Q_OBJECT

public:
    /// 磁贴图标。🔴 **不要用字体字形**（◐ / ⚡ / ⟳ 这类码位）：本机实测 14pt 下
    /// 全部落成一个小圆点（字体里没有这些码位，回退成一个点），磁贴看起来像坏了。
    /// 一律用 QPainter 画矢量 —— 任何机器、任何字体下长得都一样。
    enum class Icon { Theme, Glass, Blur, Motion, Port, Log, Update, Reset };

    /// action = true 表示「按一下执行一次」（检查更新 / 恢复默认外观），
    /// 不显示开/关状态，也不保持高亮。
    CcTile(Icon icon, const QString& title, bool action = false, QWidget* parent = nullptr);

    void setActive(bool active);
    bool isActive() const { return m_active; }
    void setHint(const QString& hint);   // 第二行小字（默认「开 / 关」）

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    /// 把图标画进 box（正方形）里，单色描边/填充，随激活态换色。
    void drawIcon(QPainter& p, const QRectF& box, const QColor& color) const;

    Icon m_icon;
    QString m_title;
    QString m_hint;
    bool m_action = false;
    bool m_active = false;
    bool m_hover = false;
};

class ControlCenterPage : public BasePage {
    Q_OBJECT

public:
    explicit ControlCenterPage(QWidget* parent = nullptr);

    /// 状态芯片（外壳 syncShellStatus 时喂进来）。
    void setStatus(const QString& device, const QString& mode, const QString& protocol,
                   const QString& link, const QString& linkState);

    /// 把 AppSettings / QSettings / ThemeManager 的真实状态重新同步到磁贴与滑杆上。
    /// 外壳在应用完某个动作之后必须调一次 —— 本页自己不持有任何设置的真值。
    void syncTiles();

    /// 页面被外壳显示：磁贴错峰点亮（「逐格通电」）。
    void onActivated() override;

signals:
    /// 用户在页里点了某个动作。action 取值见 .cpp 里的 kAction* 常量：
    /// theme / glass / blur / anim / autoport / debuglog / update / reset /
    /// card:<v> / bg:<v> / nav:<key>
    void actionTriggered(const QString& action);

private:
    void buildUi();
    /// 进场时 8 块磁贴「从左到右、从上到下」错峰淡入。
    void animateTilesIn();

    QLabel* m_chipDevice = nullptr;
    QLabel* m_chipMode = nullptr;
    QLabel* m_chipProtocol = nullptr;
    QLabel* m_chipLink = nullptr;

    CcTile* m_tileTheme = nullptr;
    CcTile* m_tileGlass = nullptr;
    CcTile* m_tileBlur = nullptr;
    CcTile* m_tileAnim = nullptr;
    CcTile* m_tileAutoPort = nullptr;
    CcTile* m_tileDebugLog = nullptr;
    CcTile* m_tileUpdate = nullptr;
    CcTile* m_tileReset = nullptr;

    QSlider* m_cardSlider = nullptr;
    QLabel* m_cardValue = nullptr;
    QSlider* m_bgSlider = nullptr;
    QLabel* m_bgValue = nullptr;
};

} // namespace kc::ui
