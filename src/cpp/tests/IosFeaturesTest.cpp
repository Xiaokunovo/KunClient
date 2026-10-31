// 2026-10-30 iOS 玩法六件套的结构回归。
//
// 这一组测试专门钉「设置页里的开关是不是**当场**生效」——
// 六项里有三项最初是「构造时读一次 AppSettings」实现的（主屏小组件 / 快捷指令页），
// 于是设置页写着「改完立刻生效」、实际却要重启。人眼很难发现（重启后一切正常），
// 只有把「改设置 → 立刻查控件」写成断言才拦得住。
//
// 另外两项（悬浮球 / 待机）也顺手验一下开关的显隐口径。
//
// 不依赖 show()：MainShell 的构造已经建好全部子控件，applyIosFeatures() 是
// Q_INVOKABLE，直接反射调用即可。

#include "kc/config/AppSettings.h"
#include "ui/MainShell.h"
#include "ui/pages/ConsolePage.h"
#include "ui/pages/ShortcutsPage.h"
#include "ui/widgets/AssistiveTouch.h"
#include "ui/widgets/ConsoleWidgets.h"
#include "ui/widgets/StandByOverlay.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMetaObject>
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

/// 让外壳重读一遍 AppSettings（设置页 applyIosLive() 走的就是这条路）。
void applyIos(kc::ui::MainShell& shell)
{
    QMetaObject::invokeMethod(&shell, "applyIosFeatures", Qt::DirectConnection);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    kc::ui::MainShell shell;

    auto* console = shell.findChild<kc::ui::ConsolePage*>();
    check(console != nullptr, "console page exists");
    if (!console) return 1;

    // ── ① 主屏小组件：关掉后卡片条必须**当场**隐藏，且不留空隙 ────────────
    {
        kc::AppSettings& s = kc::AppSettings::current();
        s.consoleWidgets = true;
        applyIos(shell);
        auto* bar = console->widgetsBar();
        check(bar != nullptr, "widgets bar exists when enabled");
        check(bar && bar->isVisibleTo(console), "widgets bar is visible when enabled");

        s.consoleWidgets = false;
        applyIos(shell);
        // 🔴 关键断言：卡片条自己 hidden **还不够** —— 它下面那 14px 间距也要一起收，
        //    否则主屏图标上方会留下一道来路不明的空隙。
        check(bar && !bar->isVisibleTo(console), "widgets bar hides immediately when disabled");
        if (bar && bar->parentWidget()) {
            check(!bar->parentWidget()->isVisibleTo(console),
                  "the whole widgets container (card + gap) hides, leaving no dead space");
        }

        // 再打开：懒建路径要能重新拿到数据与材质（不能是白板）。
        s.consoleWidgets = true;
        applyIos(shell);
        check(console->widgetsBar() && console->widgetsBar()->isVisibleTo(console),
              "widgets bar comes back without a restart");
    }

    // ── ② 快捷指令页：关掉后立刻从主屏消失，且**退出专注模式也不许复活** ──
    // 🔴 判据是「主屏图标网格里有没有这个 key」，不是 page->isHidden()：
    //    QStackedWidget 里的非当前页 isHidden() 恒为 true，拿它判断会误报。
    {
        kc::AppSettings& s = kc::AppSettings::current();
        auto* shortcuts = shell.findChild<kc::ui::ShortcutsPage*>();
        check(shortcuts != nullptr, "shortcuts page is registered");

        const QString key = QStringLiteral("Shortcuts");

        s.shortcutsPageVisible = true;
        s.focusFlashMode = false;
        applyIos(shell);
        check(console->entryKeys().contains(key), "shortcuts icon shows on home when enabled");

        s.shortcutsPageVisible = false;
        applyIos(shell);
        check(!console->entryKeys().contains(key), "shortcuts icon hides immediately when disabled");

        // 进一次专注模式再退出：refreshFocusFilter() 会把非刷机页统一置 hidden，
        // 退出时如果不看设置就会把这一页「恢复」出来 —— 这是最初的真实 bug。
        s.focusFlashMode = true;
        applyIos(shell);
        s.focusFlashMode = false;
        applyIos(shell);
        check(!console->entryKeys().contains(key),
              "shortcuts icon stays gone after a focus-mode round trip");

        s.shortcutsPageVisible = true;
        applyIos(shell);
        check(console->entryKeys().contains(key), "shortcuts icon comes back when re-enabled");
    }

    // ── ③ 辅助触控悬浮球：懒建 + 显隐 ────────────────────────────────────
    {
        kc::AppSettings& s = kc::AppSettings::current();
        s.assistiveTouch = true;
        s.focusFlashMode = false;
        applyIos(shell);
        auto* ball = shell.findChild<kc::ui::AssistiveTouch*>(QStringLiteral("assistiveTouch"));
        check(ball != nullptr, "assistive ball is created on demand");
        check(ball && !ball->isHidden(), "assistive ball is visible when enabled");

        // 专注模式必须把它收掉（它会挡住刷机进度）。
        s.focusFlashMode = true;
        applyIos(shell);
        check(ball && ball->isHidden(), "assistive ball hides in focus mode");

        s.focusFlashMode = false;
        s.assistiveTouch = false;
        applyIos(shell);
        check(ball && ball->isHidden(), "assistive ball hides when disabled");
    }

    // ── ⑤ 待机自动进入：秒数直接决定计时器跑不跑 ──────────────────────────
    {
        kc::AppSettings& s = kc::AppSettings::current();
        // 0 = 从不：待机层不能因为「没操作」自己弹出来。这里不去读计时器本身
        // （它是私有成员），而是把秒数设成 0、等到超过任何可能的短超时，
        // 再确认待机层仍然不可见 —— 用户看得见的结果才是要钉的东西。
        s.standbyIdleSeconds = 0;
        s.focusFlashMode = false;
        applyIos(shell);
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        auto* standby = shell.findChild<kc::ui::StandByOverlay*>(
            QStringLiteral("standByOverlay"));
        check(standby == nullptr || standby->isHidden(),
              "standby overlay does not auto-appear when idle seconds is 0");
    }

    // ── 收尾：把内存设置恢复成默认，别污染同一进程里后面的用例 ────────────
    {
        kc::AppSettings& s = kc::AppSettings::current();
        s.assistiveTouch = false;
        s.standbyIdleSeconds = 0;
        s.focusFlashMode = false;
        s.consoleWidgets = true;
        s.shortcutsPageVisible = true;
    }

    if (failures == 0)
        std::cout << "IosFeaturesTest: all checks passed\n";
    else
        std::cerr << "IosFeaturesTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
