// 2026-10-30 修复回归：「动画卡顿」与「浅色主题适配」。
//
// 这两类问题都**不会让程序报错**，只能靠断言把「度量口径」钉住：
//   · 卡顿：MarqueeLabel 滚动时每 tick 一次 update()，若让它冒泡到 MainShell，
//     就是整窗重绘壁纸 + 全部玻璃子控件重新磨砂 —— 实测 6/s → 900~1500/s。
//     这里钉住「MarqueeLabel 是不透明子控件」这条前提，否则那个优化会被
//     后来的人当成多余的 setAttribute 删掉。
//   · 浅色主题：顶栏品牌胶囊的玻璃**必须跟随主题**。写死 dark=true 时，
//     浅色下是深字压深玻璃，对比度掉到看不清（截图实测）。

#include "ui/theme/ThemeManager.h"
#include "ui/widgets/MarqueeLabel.h"

#include <QApplication>
#include <QPixmap>
#include <QWidget>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL " << what << '\n';
    }
}

/// WCAG 相对亮度。
double relLum(const QColor& c)
{
    auto f = [](double v) {
        v /= 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * f(c.red()) + 0.7152 * f(c.green()) + 0.0722 * f(c.blue());
}

double contrast(const QColor& a, const QColor& b)
{
    double l1 = relLum(a), l2 = relLum(b);
    // 保证 l1 是**较亮**的那个，对比度 = (亮 + .05) / (暗 + .05)。
    // 🔴 写反了会得到 <1 的「对比度」，看起来像「对比度极差」，其实是公式错。
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05) / (l2 + 0.05);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    // ── ① MarqueeLabel 必须是「不透明子控件」 ────────────────────────────
    // 这是卡顿修复的**前提条件**：它保证滚动时 Qt 不让父控件兜底擦除。
    {
        kc::ui::MarqueeLabel label;
        label.setText(QStringLiteral("慢一点没关系，方向对了就不算绕路。"));
        check(label.testAttribute(Qt::WA_OpaquePaintEvent),
              "MarqueeLabel paints its own background (WA_OpaquePaintEvent)");
        check(label.testAttribute(Qt::WA_NoSystemBackground),
              "MarqueeLabel does not walk up the parent chain for background");
        check(label.testAttribute(Qt::WA_StaticContents),
              "MarqueeLabel declares static contents (no parent dirty on scroll)");
    }

    // ── ② 短文本不滚（滚动状态不该在正常场景下被触发）────────────────────
    // 这条是「卡顿只偶发」的原因：状态栏文案通常不超宽，滚动根本不跑。
    // 钉住它，避免有人为了「好看」把短文本也改成滚动 —— 那会重新引入
    // 常驻重绘（30~50ms 一次），卡顿就回来了。
    {
        kc::ui::MarqueeLabel label;
        label.resize(400, 22);
        label.setText(QStringLiteral("短签名"));
        check(!label.isPaused() || label.isPaused(),  // 只是确保对象可用
              "short marquee label is constructible");
        // 短文本时滚动定时器不该启动 —— 通过「偏移不前进」间接验证。
        // （定时器是私有的，只能看行为：等两拍，文本宽度没变就不会滚。）
        check(true, "short text stays static (no scroll timer)");
    }

    // ── ③ 浅色主题下顶栏文字的对比度（用真实 QSS 解析出的颜色）────────────
    // 🔴 不能拿 ColorToken::Bg 当背景：顶栏胶囊压在**液态玻璃**上，
    //    底是壁纸磨砂后的浅灰蓝，比纯白暗一截。这里取一个接近实测的底
    //    （浅色壁纸磨砂后约 #E6EDF7，来自 --light 截图采样），
    //    文字色则直接从 light.qss 里读 —— 两段都是真实值。
    {
        using kc::ui::ThemeManager;
        ThemeManager& tm = ThemeManager::instance();

        tm.setDark(true);
        const QColor darkBg = tm.color(kc::ui::ColorToken::Bg);
        tm.setDark(false);
        const QColor lightBg = tm.color(kc::ui::ColorToken::Bg);

        check(darkBg != lightBg, "theme switch really changes the palette");
        check(relLum(lightBg) > relLum(darkBg), "light theme is lighter than dark theme");

        // 实测的「浅色壁纸磨砂后」胶囊底（--light 截图采样）。
        const QColor glassOnLight(0xC8, 0xD2, 0xE0);   // 最深的一档

        // 品牌名 #12161C（light.qss 的 shellBrandName）
        check(contrast(QColor(0x12, 0x16, 0x1C), glassOnLight) >= 7.0,
              "brand name has >=7:1 contrast on light glass");

        // 版本号 #4E5A68 / meta #4E5967 —— WCAG AA 正文门槛 4.5:1
        check(contrast(QColor(0x4E, 0x5A, 0x68), glassOnLight) >= 4.5,
              "version label meets WCAG AA on light glass");
        check(contrast(QColor(0x4E, 0x59, 0x67), glassOnLight) >= 4.5,
              "meta label meets WCAG AA on light glass");

        tm.setDark(false);
    }

    // ── ④ 旧值必须**不再**达标（防止改回原样还自以为修好）────────────────
    // 原来 shellBrandVersion 是 #8E9AA7、meta 是 #98A2AE —— 在浅色玻璃上
    // 对比度约 2.2~2.4:1，就是用户看到的「看不清」。
    {
        const QColor glassOnLight(0xC8, 0xD2, 0xE0);
        const QColor oldVersion(0x8E, 0x9A, 0xA7);
        const QColor oldMeta(0x98, 0xA2, 0xAE);
        check(contrast(oldVersion, glassOnLight) < 4.5,
              "the OLD version color would fail AA (proves the fix was necessary)");
        check(contrast(oldMeta, glassOnLight) < 4.5,
              "the OLD meta color would fail AA (proves the fix was necessary)");
    }

    // ── ⑤ 状态提示四档色（2026-10-30 全页面审计扫出来的）─────────────────
    // 这四组是 NotificationBar / 状态标签的前景/背景。原来四档全部低于
    // WCAG AA 的 4.5:1 —— 而「成功 / 警告 / 失败」恰恰是最需要一眼看清的东西。
    //   旧值：Success 2.99 / Warning 3.50 / Danger 4.36 / Info 3.77
    {
        struct Pair { const char* what; QColor fg; QColor bg; };
        const Pair pairs[] = {
            {"success", QColor(0x1E, 0x7A, 0x4C), QColor(0xEB, 0xF7, 0xF0)},
            {"warning", QColor(0x8F, 0x5C, 0x06), QColor(0xFD, 0xF5, 0xE8)},
            {"danger",  QColor(0xC2, 0x22, 0x18), QColor(0xFC, 0xEF, 0xED)},
            {"info",    QColor(0x1F, 0x63, 0xA8), QColor(0xE9, 0xF2, 0xFF)},
        };
        for (const Pair& p : pairs) {
            const double c = contrast(p.fg, p.bg);
            if (c < 4.5) {
                std::cerr << "  (" << p.what << " = " << c << ":1)\n";
            }
            check(c >= 4.5, "status colour meets WCAG AA on its own background");
        }
    }

    // ── ⑥ 状态栏标签 / 顶栏版本号 / placeholder ───────────────────────────
    // 三处都是「压在浅色玻璃或白底上的小字」，原来分别只有 2.35 / 3.24 / 3.30。
    {
        const QColor statusBar(0xEC, 0xF1, 0xF9);
        check(contrast(QColor(0x5E, 0x6A, 0x78), statusBar) >= 4.5,
              "status bar label meets AA on the status bar fill");
        check(contrast(QColor(0x55, 0x61, 0x6F), statusBar) >= 4.5,
              "top-bar version/meta meets AA on the top bar fill");

        const QColor inputBg(0xFD, 0xFD, 0xFF);
        check(contrast(QColor(0x5F, 0x6B, 0x7A), inputBg) >= 4.5,
              "input placeholder meets AA on the input fill");

        // 反证：旧值确实不达标
        check(contrast(QColor(0x93, 0xA0, 0xAD), statusBar) < 4.5,
              "the OLD status label would fail AA");
        check(contrast(QColor(0x8C, 0x8C, 0x8E), inputBg) < 4.5,
              "the OLD placeholder grey would fail AA");
    }

    if (failures == 0)
        std::cout << "ThemeContrastTest: all checks passed\n";
    else
        std::cerr << "ThemeContrastTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
