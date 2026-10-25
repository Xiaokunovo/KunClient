#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Toast —— iOS 风格玻璃横幅通知
//
//  顶部居中滑入，停留 2~3 秒后淡出。用来替代「零散的 QMessageBox / 状态栏
//  一句话」—— 成功/失败/提示这类**不需要用户决策**的反馈，弹窗太重。
//
//    · 玻璃底：GlassPanel（背板来自 MainShell 壁纸）；
//    · 胶囊形：圆角 = 高度÷2（不是 999px —— Qt 会退化成直角）；
//    · 图标：矢量圆点（✓ / ! / × / i），不用字体字形；
//    · 队列：正在显示时再来的通知排队，一条一条放，不叠罗汉。
//
//  用法：
//      Toast::show("已保存设置", Toast::Kind::Success);
//      Toast::show("设备未连接", Toast::Kind::Warning, 3000);
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/widgets/GlassPanel.h"

#include <QQueue>
#include <QString>

class QTimer;

namespace kc::ui {

class Toast : public GlassPanel {
    Q_OBJECT

public:
    enum class Kind { Info, Success, Warning, Error };

    /// 弹一条通知（线程安全仅限 UI 线程）。
    static void show(const QString& text, Kind kind = Kind::Info, int durationMs = 2600);

    /// 立即清空（切主题 / 退出时用）。
    static void clearAll();

private:
    explicit Toast(QWidget* parent = nullptr);
    static Toast* ensure();

    void present(const QString& text, Kind kind, int durationMs);
    void dismiss(bool animate);
    void advance();

    struct Entry {
        QString text;
        Kind kind = Kind::Info;
        int durationMs = 2600;
    };

    QQueue<Entry> m_queue;
    QTimer* m_timer = nullptr;

    QString m_text;
    Kind m_kind = Kind::Info;
    int m_durationMs = 2600;
    bool m_busy = false;
    bool m_dismissing = false;

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace kc::ui
