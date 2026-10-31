#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ConsoleWidgets —— 主屏小组件条（iOS 主屏 Widgets，2026-10-29）
//
//  为什么加：主屏现在是「16 个图标 + Dock」，功能上够用，但**没有一处显示信息**
//  —— 想看一眼「设备连了没 / 上次刷成没成 / 现在几点」，得先进设备主页或者翻日志。
//  iOS 主屏的招牌恰恰是图标上方那排小组件：一屏之内既能点、也能读。
//
//  三块卡（从左到右，等宽）：
//    ① 设备：芯片 / 模式 / 链路，未连接时整块压暗；
//    ② 最近刷机：取 kc::diag::FlashHistory 的第一条（操作 · 成败 · 多久前）；
//    ③ 时钟：HH:mm + 日期 + 星期。
//
//  视觉：**真液态玻璃**（GlassSurface，与 Dock 同一套材质）。背板由外壳从壁纸
//  帧裁出来喂进来 —— 小组件浮在壁纸上，自己抓窗口会把图标一起抓进去。
//
//  🔴 卡高固定 92、总宽按列宽算（3 × 208 + 2 × 12），不随窗口拉伸：
//     iOS 的小组件是网格单位，不是流式布局；拉伸会让三块卡在宽屏上变成三条
//     长长的空条，反而更难看。
// ─────────────────────────────────────────────────────────────────────────────

#include <QImage>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

namespace kc::ui {

class GlassSurface;

class ConsoleWidgets : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleWidgets(QWidget* parent = nullptr);

    /// 外壳喂背板：**与本体等尺寸**的「它背后那块真实画面」（未处理的原图）。
    /// 传空图 = 退回 GlassSurface 内置的环境光兜底。
    void setBackdrop(const QImage& backdrop);

    /// 重读液态玻璃参数并套到三块卡上（设置页保存后由外壳调用）。
    void refreshGlass();

    /// 重新拉一次数据（外壳在 ShellStatus 变化 / 切回主屏 / 刷机记录变化时调）。
    void refreshData();

    /// 秒级时钟开关：主屏不可见时停掉，别在后台白跳。
    void setClockRunning(bool running);

    /// 外壳摆几何用：本控件自然尺寸（宽 = 3 卡 + 间距，高 = 卡高）。
    static QSize preferredSize();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// 建一块玻璃卡：caption 在上、value / sub 两行值由调用方持有。
    GlassSurface* makeCard(const QString& caption, QLabel** value, QLabel** sub);
    void relayout();

    QVector<GlassSurface*> m_cards;
    QLabel* m_deviceValue = nullptr;
    QLabel* m_deviceSub = nullptr;
    QLabel* m_flashValue = nullptr;
    QLabel* m_flashSub = nullptr;
    QLabel* m_clockValue = nullptr;
    QLabel* m_clockSub = nullptr;
    QTimer* m_clockTimer = nullptr;
    bool m_clockWanted = false;
};

} // namespace kc::ui
