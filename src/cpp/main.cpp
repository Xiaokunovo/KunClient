// ─────────────────────────────────────────────────────────────────────────────
//  KunClient — C++/Qt6 刷机工具入口
//
//  以 SWT-EDL 的 EDL 引擎为基底，扩展为完整的 KunClient 客户端。
//  GUI 模式（默认）+ 无头 CLI 模式（--cli，NDJSON stdin/stdout 协议）。
// ─────────────────────────────────────────────────────────────────────────────

#include "core/Types.h"
#include "kc/config/AppSettings.h"
#include "kc/config/ClientConfig.h"
#include "kc/tools/PlatformToolsManager.h" // 探针：--adb-takeover 验证 5037 抢占
#include "kc/version.h"
#include "ui/MainShell.h"
#include "ui/MainWindow.h" // 探针：--xml 调 loadFirmwareXmlPaths 需完整类型
#include "ui/pages/EdlFlashPage.h" // 探针：--xml 直接喂 rawprogram XML（免合并取证）
#include "ui/pages/MtkFlashPage.h" // 探针：--scatter 直接喂散列文件
#include "ui/dialogs/DisclaimerDialog.h"
#include "ui/dialogs/AnimatedDialog.h"
#include "ui/dialogs/ThemedMessageBox.h"
#include "ui/dialogs/EraseConfirmDialog.h"
#include "ui/dialogs/WelcomeDialog.h"   // 探针：--welcome / 首次启动引导
#include "ui/theme/ThemeManager.h"
#include "ui/dialogs/GlassDialog.h"   // 探针：--glassdialog 玻璃弹窗取证
#include "ui/widgets/AppIcons.h"      // 探针：--glassmenu 菜单行图标
#include "ui/widgets/BottomSheet.h"   // 探针：--sheet 底部抽屉取证
#include "ui/widgets/DynamicIsland.h" // 探针：--island 展开灵动岛（页面切换器）后抓图
#include "ui/widgets/GlassMenu.h"     // 探针：--glassmenu 液态玻璃菜单取证
#include "ui/widgets/IosSwitch.h"     // 探针：--ios-controls iOS 开关取证
#include "ui/widgets/IosGroupedList.h"    // 探针：--ios-gallery 分组列表
#include "ui/widgets/IosProgressRing.h"   // 探针：--ios-gallery 环形进度
#include "ui/widgets/IosSearchField.h"    // 探针：--ios-gallery 搜索框
#include "ui/widgets/IosSpinner.h"        // 探针：--ios-gallery 活动指示器
#include "ui/widgets/IosPageControl.h"    // 探针：--ios-gallery 页码点
#include "ui/widgets/IosStepper.h"        // 探针：--ios-gallery 步进器
#include "ui/widgets/IosBadge.h"          // 探针：--ios-gallery 角标
#include "ui/widgets/IosSwipeRow.h"       // 探针：--ios-gallery 侧滑删除行
#include "ui/widgets/SegmentedControl.h"  // 探针：--ios-controls 分段选择器取证
#include "ui/widgets/Toast.h"         // 探针：--toast 玻璃横幅取证

#include <QAction>
#include <QApplication>
#include <QColor>   // 强调色：启动时从 AppSettings 读回
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QComboBox>   // 探针：--edl-advanced 展开 #advancedCombo
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QLineEdit>   // 探针：--island-search 往灵动岛搜索框里塞字
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QSettings>
#include <QSslSocket>
#include <QTimer>

#include <cstdio>
#include <cstdlib>   // std::_Exit —— --preload 探针抓完图直接退出（绕开线程池等待）
#include <cstring>

namespace swt::edl {
/// headless（`--cli`）入口，实现在 cli/HeadlessMain.cpp。
int runHeadless(int argc, char** argv);
}

// ─────────────────────────────────────────────────────────────────────────────
//  启动计时探针（KC_TIMING=1）
//
//  「登录慢」被反复提起，但慢在冷启动、慢在握手、还是慢在预载窗，光看代码猜不准。
//  这套打印把关键节点的时间戳打在 stderr 上（不写日志文件、不影响正常启动）：
//    [boot]  0 ms  进程起步
//    [boot]  412 ms  QApplication 就绪
//    [boot]  580 ms  登录框可交互
//    [boot]  1830 ms 登录通过（→ 预载/主界面）
//    [boot]  2100 ms 主界面已显示
//  配合 HttpClient 的 [http] 行（每个请求的耗时 + 状态码）就能直接看出瓶颈在哪一段。
// ─────────────────────────────────────────────────────────────────────────────
namespace {
qint64 g_bootMs = 0;

void bootMark(const char* what)
{
    if (qEnvironmentVariableIsEmpty("KC_TIMING")) return;
    const qint64 ms = QDateTime::currentMSecsSinceEpoch() - g_bootMs;
    std::fprintf(stderr, "[boot] %5lld ms  %s\n", static_cast<long long>(ms), what);
    std::fflush(stderr);
}
} // namespace

int main(int argc, char* argv[])
{
    g_bootMs = QDateTime::currentMSecsSinceEpoch();
    bootMark("进程起步");

    // 🔴 `--cli` 必须在构造 QApplication 之前判断：
    //    CLI 模式走 QCoreApplication（不需要窗口系统）。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cli") == 0) {
            return swt::edl::runHeadless(argc, argv);
        }
    }

    QApplication app(argc, argv);
    bootMark("QApplication 就绪");
    QApplication::setApplicationName(QStringLiteral("KunClient"));
    QApplication::setApplicationVersion(QString::fromLatin1(kc::kVersionString));
    QApplication::setOrganizationName(QStringLiteral("KunAwa"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/logo.png")));

    // ── 停更离线版：--netcheck / --checkupdate 探针已随联网功能一并移除 ──────
    //  服务器关停后这两个自检通道无意义，直接删掉（保留主题参数逻辑）。

    // 主题参数：--light / --dark
    QSettings settings;
    if (app.arguments().contains(QStringLiteral("--light"))) {
        settings.setValue(QStringLiteral("appearance/darkTheme"), false);
    } else if (app.arguments().contains(QStringLiteral("--dark"))) {
        settings.setValue(QStringLiteral("appearance/darkTheme"), true);
    }

    // 注册 Qt 元类型（跨线程信号投递需要）
    qRegisterMetaType<swt::edl::StorageType>();
    qRegisterMetaType<swt::edl::SessionState>();
    qRegisterMetaType<swt::edl::PartitionInfo>();
    qRegisterMetaType<QVector<swt::edl::PartitionInfo>>();
    qRegisterMetaType<swt::edl::DeviceInfo>();
    qRegisterMetaType<swt::edl::OplusVipPackage>();

    // 字体
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    app.setFont(font);

    // QSS 样式表
    // 🔴 默认主题是**浅色**（2026-10-11 用户指定）：没改过设置的新用户一律浅色。
    //    老用户若在设置里切过深色，QSettings 里已有值，不会被这里的默认值覆盖。
    // 🔴 必须走 ThemeManager::apply()：它会把 QSS 里的 %GLASS_CARD% / %GLASS_PANEL%
    //    占位符展开成数字。**不能**直接 setStyleSheet(原始文件文本) ——
    //    rgba(255,255,255,%GLASS_CARD%) 是语法错误，Qt 的 QSS 解析器会丢弃
    //    该行之后的**全部**规则（实测：卡片 99.2% 像素 alpha=0，只剩文件头
    //    几条通用规则生效；主窗口建起来前的 DisclaimerDialog / LoginDialog
    //    整个卡片背景全透）。取证见 build-about/probe_bg3.cpp（V1 vs V4）。
    const bool darkTheme = settings.value(QStringLiteral("appearance/darkTheme"), false).toBool();
    kc::ui::ThemeManager::instance().setDark(darkTheme);
    // 强调色必须在 apply() **之前**设：它决定 QSS 里 %ACCENT% 系列占位符的取值。
    // 空串 / 非法色 = 保持主题内置的 #0A84FF（setAccent 内部会忽略无效色）。
    kc::ui::ThemeManager::instance().setAccent(
        QColor(kc::AppSettings::current().accentColor));
    kc::ui::ThemeManager::instance().apply(app);
    bootMark("主题与样式表就绪");

    // 刷机引擎参数（COM 速率 / 线程核心 / 兼容模式）：线程数要尽早落到全局线程池，
    // 否则第一批后台任务（更新检查、驱动下载）会按默认线程数起跑。
    kc::AppSettings::current().applyRuntime();

    // ── 探针：--adb-takeover 跑一次 adb 服务接管并打印结论（不建窗口）──────────
    //  复现「5037 被别的 adb 版本占着」：先用 System32 里那个 adb 起 server，
    //  再跑本探针 —— 期望把服务端换成随包 adb。退出码 0 = 接管成功。
    if (app.arguments().contains(QStringLiteral("--adb-takeover"))) {
        kc::PlatformToolsManager tools([](const QString& line) {
            std::fprintf(stdout, "[tools] %s\n", line.toUtf8().constData());
        });

        const kc::PlatformToolsManager::AdbServerInfo before = tools.inspectAdbServer();
        std::fprintf(stdout, "before: pid=%lld ownedByUs=%d exe=%s\n",
                     static_cast<long long>(before.pid), before.ownedByUs ? 1 : 0,
                     before.exePath.toUtf8().constData());

        // 探针跳过冷却：要在一次运行里看到「杀旧 server → 起新 server」全过程。
        const kc::PlatformToolsManager::TakeoverResult result =
            tools.takeOverAdbServer(/*minIntervalMs=*/0);

        std::fprintf(stdout, "after : pid=%lld ownedByUs=%d\n"
                             "result: attempted=%d ok=%d killed=%d detail=%s\n",
                     static_cast<long long>(result.after.pid),
                     result.after.ownedByUs ? 1 : 0,
                     result.attempted ? 1 : 0, result.ok ? 1 : 0,
                     result.killedForeign ? 1 : 0,
                     result.detail.toUtf8().constData());
        std::fflush(stdout);
        return result.ok ? 0 : 1;
    }

    // 截图模式（用于探针 / CI）。⚠️ 参数与「模态窗抓取」定时器必须排在法务闸门**之前**：
    // 闸门未同意时会模态阻塞在 exec()，之后的任何代码都不会执行。
    const int screenshotIndex = app.arguments().indexOf(QStringLiteral("--screenshot"));
    const QString shotPath = screenshotIndex >= 0
        ? (screenshotIndex + 1 < app.arguments().size()
               ? app.arguments().at(screenshotIndex + 1)
               : QDir::current().filePath(QStringLiteral("KunClient-preview.png")))
        : QString();
    // ═══ 早期控件预览探针（2026-10-24）═══════════════════════════════════════
    //  🔴 放在登录 / 法务闸门**之前**：跳过登录的路径被鉴权改动影响时会直接退出，
    //     走不到 MainShell；这两个探针只依赖 QApplication + 主题，不受影响。
    // 总览：一屏摆下全部 iOS 组件（取证 / 走查用）。
    if (app.arguments().contains(QStringLiteral("--ios-gallery"))) {
        const bool dark = kc::ui::ThemeManager::instance().isDark();
        auto* host = new QWidget;
        host->setAttribute(Qt::WA_StyledBackground, true);
        host->setStyleSheet(dark ? QStringLiteral("background: #151A22;")
                                 : QStringLiteral("background: #EEF2F8;"));
        host->resize(560, 800);
        auto* lay = new QVBoxLayout(host);
        lay->setContentsMargins(20, 18, 20, 18);
        lay->setSpacing(14);

        // ① 分组列表
        auto* group = new kc::ui::IosGroupedList(host);
        group->setHeader(QStringLiteral("通用"));
        group->addRow(QStringLiteral("版本"), QString(), QStringLiteral("1.0.0"));
        auto* check = group->addRow(QStringLiteral("检查更新"), QString(), QString(), true);
        check->setClickable(true);
        check->onActivated = [] {};
        group->addToggleRow(QStringLiteral("自动更新"), true);
        group->addRow(QStringLiteral("清理缓存"), QStringLiteral("释放约 128 MB"), QString(), true);
        group->setFooter(QStringLiteral("开关与行点击均为自绘，不依赖 QSS 背景。"));
        lay->addWidget(group);

        // ①' 侧滑删除行（探针里直接展开到打开态，截图才看得见）
        auto* swipe = new kc::ui::IosSwipeRow(host);
        auto* swipeInner = new QWidget;
        swipeInner->setAttribute(Qt::WA_StyledBackground, true);
        swipeInner->setStyleSheet(dark
            ? QStringLiteral("background: rgba(255,255,255,0.06); border-radius: 12px;")
            : QStringLiteral("background: rgba(255,255,255,0.92); border-radius: 12px;"));
        auto* sl = new QHBoxLayout(swipeInner);
        sl->setContentsMargins(16, 0, 16, 0);
        sl->addSpacing(92);   // 侧滑 88px 后标题仍在可视区（真实行有图标占位）
        auto* slb = new QLabel(QStringLiteral("分区  super"), swipeInner);
        slb->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 13px;")
                               .arg(dark ? QStringLiteral("#E4EAF2")
                                         : QStringLiteral("#12161C")));
        sl->addWidget(slb, 1);
        swipe->setContent(swipeInner);
        swipe->setActionText(QStringLiteral("删除"));
        swipe->setMinimumHeight(48);
        lay->addWidget(swipe);
        QTimer::singleShot(250, swipe, [swipe] { swipe->setOffset(88); });

        // ② 分段选择器
        auto* seg = new kc::ui::SegmentedControl(host);
        seg->addItem(QStringLiteral("纯黑"));
        seg->addItem(QStringLiteral("品牌蓝"));
        seg->addItem(QStringLiteral("深色玻璃"));
        seg->setCurrentIndex(1);
        lay->addWidget(seg);

        // ③ 搜索框
        auto* search = new kc::ui::IosSearchField(host);
        search->setPlaceholderText(QStringLiteral("搜索页面…"));
        search->setText(QStringLiteral("分区"));
        lay->addWidget(search);

        // ④ 指示器 + 环形进度
        auto* row = new QWidget(host);
        row->setAttribute(Qt::WA_TranslucentBackground, true);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);
        auto* sp = new kc::ui::IosSpinner(row);
        sp->setDiameter(22);
        sp->start();
        rl->addWidget(sp);
        auto* ring = new kc::ui::IosProgressRing(row);
        ring->setDiameter(52);
        ring->setValue(0.42);
        rl->addWidget(ring);
        auto* ring2 = new kc::ui::IosProgressRing(row);
        ring2->setDiameter(52);
        ring2->setLineWidth(5);
        ring2->setValue(0.86);
        rl->addWidget(ring2);
        rl->addStretch(1);
        lay->addWidget(row);

        // ⑤ 步进器 + 角标 + 页码点
        auto* row2 = new QWidget(host);
        row2->setAttribute(Qt::WA_TranslucentBackground, true);
        auto* rl2 = new QHBoxLayout(row2);
        rl2->setContentsMargins(0, 0, 0, 0);
        rl2->setSpacing(18);
        auto* stepper = new kc::ui::IosStepper(row2);
        stepper->setRange(1, 64);
        stepper->setValue(8);
        rl2->addWidget(stepper, 0, Qt::AlignVCenter);
        auto* dots = new kc::ui::IosPageControl(row2);
        dots->setCount(5);
        dots->setCurrentIndex(2);
        rl2->addWidget(dots, 0, Qt::AlignVCenter);
        auto* badge = new kc::ui::IosBadge(row2);
        badge->setCount(12);
        rl2->addWidget(badge, 0, Qt::AlignVCenter);
        rl2->addStretch(1);
        lay->addWidget(row2);

        lay->addStretch(1);
        host->show();
        QTimer::singleShot(700, host, [host, shotPath] {
            host->grab().save(shotPath);
            QApplication::quit();
        });
        return app.exec();
    }

    if (app.arguments().contains(QStringLiteral("--ios-controls"))) {
        const bool dark = kc::ui::ThemeManager::instance().isDark();
        const QString fg = dark ? QStringLiteral("#E4EAF2") : QStringLiteral("#12161C");
        auto* host = new QWidget;
        host->setAttribute(Qt::WA_StyledBackground, true);
        host->setStyleSheet(dark ? QStringLiteral("background: #151A22;")
                                 : QStringLiteral("background: #F4F6FA;"));
        host->resize(520, 300);
        auto* lay = new QVBoxLayout(host);
        lay->setContentsMargins(26, 24, 26, 24);
        lay->setSpacing(16);

        auto* segLabel = new QLabel(QStringLiteral("外观配色"), host);
        segLabel->setStyleSheet(QStringLiteral(
            "background: transparent; color: %1; font-size: 13px;").arg(fg));
        lay->addWidget(segLabel);

        auto* seg = new kc::ui::SegmentedControl(host);
        seg->addItem(QStringLiteral("纯黑"));
        seg->addItem(QStringLiteral("品牌蓝"));
        seg->addItem(QStringLiteral("深色玻璃"));
        seg->setCurrentIndex(2);
        lay->addWidget(seg);

        lay->addSpacing(8);

        const QVector<QPair<QString, bool>> rows = {
            { QStringLiteral("展开时压暗背景"), true },
            { QStringLiteral("动效精简（低配机 / 嫌晃眼）"), false },
        };
        for (const auto& r : rows) {
            auto* row = new QWidget(host);
            row->setAttribute(Qt::WA_TranslucentBackground, true);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            auto* lb = new QLabel(r.first, row);
            lb->setStyleSheet(QStringLiteral(
                "background: transparent; color: %1; font-size: 13px;").arg(fg));
            auto* sw = new kc::ui::IosSwitch(row);
            sw->setCheckedSilently(r.second);
            rl->addWidget(lb, 1);
            rl->addWidget(sw, 0, Qt::AlignRight | Qt::AlignVCenter);
            lay->addWidget(row);
        }

        host->show();
        QTimer::singleShot(420, host, [host, shotPath] {
            host->grab().save(shotPath);
            QApplication::quit();
        });
        return app.exec();
    }

    if (app.arguments().contains(QStringLiteral("--actionsheet"))) {
        auto* host = new QWidget;
        host->setStyleSheet(kc::ui::ThemeManager::instance().isDark()
                                ? QStringLiteral("background: #1a2130;")
                                : QStringLiteral("background: #EDF1F7;"));
        host->resize(900, 700);
        host->show();
        QTimer::singleShot(300, host, [] {
            QVector<kc::ui::BottomSheet::Action> actions;
            actions.append({ QStringLiteral("重启到系统"), false, [] {} });
            actions.append({ QStringLiteral("重启到 9008"), false, [] {} });
            actions.append({ QStringLiteral("重启到 Recovery"), false, [] {} });
            actions.append({ QStringLiteral("重启到 FastbootD"), false, [] {} });
            kc::ui::BottomSheet::showActions(QStringLiteral("重启设备"), actions);
        });
        QTimer::singleShot(900, &app, [shotPath] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* s = qobject_cast<kc::ui::BottomSheet*>(w)) {
                    s->grab().save(shotPath);
                    break;
                }
            }
            QApplication::quit();
        });
        QTimer::singleShot(3500, &app, [] { std::exit(0); });
        return app.exec();
    }

    if (screenshotIndex >= 0) {
        // 条款对话框挡在 window.show() 之前时，抓「当前活动模态窗」（即 DisclaimerDialog）
        // —— 否则此定时器不动作，让主窗口创建后的定时器接管。两条路径最终都会 quit。
        QTimer::singleShot(800, &app, [&app, shotPath] {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                modal->grab().save(shotPath);
                QApplication::quit();
            }
        });
    }

    // ── --startup-check / --update-dialog 探针已随更新功能移除 ────────────────

    // ── 截图探针：--legal 单独弹「使用条款与隐私说明」并抓图 ─────────────────
    //  正常路径下这个弹窗只在「未同意当前条款版本」时出现，探针想复现就得改
    //  settings.json 的 legalAcceptedVersion（会污染用户配置）。这里直接开一个
    //  实例，与真实弹窗同一个类、同一套 QSS，拍出来就是用户看到的样子。
    if (app.arguments().contains(QStringLiteral("--legal"))) {
        kc::ui::DisclaimerDialog dlg(/*enforce=*/true);
        if (screenshotIndex >= 0) {
            QTimer::singleShot(700, &dlg, [&dlg, shotPath] {
                dlg.grab().save(shotPath);
                dlg.reject();   // 走退场动画，不写任何配置
            });
        }
        dlg.exec();
        return 0;
    }

    // ── 探针：--dialog-reopen 验证「弹窗关掉再打开还能看见」 ────────────────
    //  2026-10-21 的真实 bug：AnimatedDialog 退场把 windowOpacity 淡到 0 才 done()，
    //  而 m_inPlayed 一直是 true，第二次 show 时 playIn 直接 return —— 窗口在、全透明。
    //  这个探针把「开 → 关 → 再开」走一遍再抓图：图要是全黑/全透明，说明又退化了。
    if (app.arguments().contains(QStringLiteral("--dialog-reopen"))) {
        // 时间轴必须压进 800ms：上面那个全局截图定时器在 800ms 会 quit()，
        // 排在它后面的动作一律不执行（2026-10-22 踩过，探针「跑了但什么都没产出」）。
        // 🔴 必须关掉「最后一个窗口关闭即退出」：探针在 250ms 把唯一的窗口 reject 掉，
        //    默认行为会让整个 app 直接退出，后面 520ms 的「再打开」根本轮不到
        //    （2026-10-22 实测：探针跑完退出码 0，却没有任何输出）。
        app.setQuitOnLastWindowClosed(false);
        auto* dlg = new kc::ui::AnimatedDialog;
        dlg->setObjectName(QStringLiteral("dialogReopenProbe"));
        dlg->setWindowTitle(QStringLiteral("重开探针"));
        dlg->resize(360, 160);
        auto* label = new QLabel(QStringLiteral("第二次打开也要看得见"), dlg);
        label->setAlignment(Qt::AlignCenter);
        auto* lay = new QVBoxLayout(dlg);
        lay->addWidget(label);

        dlg->show();                                            // 第一次打开
        QTimer::singleShot(250, dlg, [dlg] { dlg->reject(); });  // 走完整退场（150ms）
        QTimer::singleShot(520, dlg, [dlg] {                     // 关完之后再开
            dlg->show();
            dlg->raise();
        });
        QTimer::singleShot(740, dlg, [dlg, shotPath, screenshotIndex] {
            QFile log(shotPath + QStringLiteral(".log"));
            log.open(QIODevice::WriteOnly | QIODevice::Truncate);
            log.write(QStringLiteral("visible=%1 opacity=%2\n")
                          .arg(int(dlg->isVisible()))
                          .arg(dlg->windowOpacity(), 0, 'f', 2)
                          .toUtf8());
            if (screenshotIndex >= 0) {
                const bool ok = dlg->grab().save(shotPath);
                log.write(QStringLiteral("saved=%1 path=%2\n")
                              .arg(int(ok), 0).arg(shotPath).toUtf8());
            }
            log.close();
            QApplication::quit();
        });
        app.exec();
        return 0;
    }

    // ── 截图探针：--welcome 单独弹「首次欢迎 + 引导」并逐页抓图 ──────────────
    //  真实路径下这个弹窗只在 onboarding/completed 未置位时出现一次，探针想复现
    //  就得清标记（会污染用户配置）。这里直接开实例、用 previewPage 逐页翻，
    //  一次跑完四张：<name>.png / <name>_2.png / _3 / _4 —— 与真实弹窗同类同 QSS。
    //  放在法务闸门之前：不依赖条款同意态、不依赖登录态。
    if (app.arguments().contains(QStringLiteral("--welcome"))) {
        kc::ui::WelcomeDialog dlg;
        if (screenshotIndex >= 0) {
            constexpr int kPages = 4;
            constexpr int kStep = 320;    // 翻页间隔：QStackedWidget 无过渡，留出排版时间即可
            for (int i = 0; i < kPages; ++i) {
                QTimer::singleShot(600 + i * kStep, &dlg, [&dlg, i] { dlg.previewPage(i); });
                QTimer::singleShot(600 + i * kStep + 240, &dlg, [&dlg, shotPath, i] {
                    const QString path =
                        i == 0 ? shotPath
                               : shotPath.left(shotPath.length() - 4)
                                     + QStringLiteral("_%1.png").arg(i + 1);
                    dlg.grab().save(path);
                });
            }
            QTimer::singleShot(600 + kPages * kStep + 200, &dlg, [&dlg] {
                dlg.reject();   // 走退场动画，不写 onboarding 标记
            });
        }
        dlg.exec();
        return 0;
    }

    // ── 启动法务闸门（对应 C# App.OnStartup：disclaimer → 主窗口）────────────
    //  未同意当前版本的条款就在创建 MainShell 之前强制阅读；点「不同意」或直接关窗
    //  都会 reject → 返回 0 退出程序，未同意不留任何口子（「告知—同意」环节）。
    //  同意后落盘 legalAcceptedVersion，下次启动不再弹。
    //
    //  KC_SKIP_LEGAL=1 环境变量可整体跳过本闸门（CI / 截图探针 / 自动化测试用，
    //  此旁路为**永久保留**的设计 —— 正式用户不受影响，探针不用再预置 settings.json）。
    const bool skipLegal = qEnvironmentVariable("KC_SKIP_LEGAL")
                               .compare(QLatin1String("1"), Qt::CaseInsensitive) == 0;
    // 临时诊断已撤（闸门三要素打印）。诊断历史见 .workbuddy/memory。
    if (!skipLegal
        && kc::AppSettings::current().legalAcceptedVersion
               < kc::ui::DisclaimerDialog::LegalVersion) {
        std::fprintf(stderr, "[gate] FIRING\n");
        kc::ui::DisclaimerDialog disclaimer(/*enforce=*/true);
        if (disclaimer.exec() != QDialog::Accepted) {
            return 0;   // 未同意 → 直接退出，不进主界面
        }
        kc::AppSettings::current().legalAcceptedVersion =
            kc::ui::DisclaimerDialog::LegalVersion;
        kc::AppSettings::current().save();
    }

    // ── 截图探针：--msgbox info|warning|critical|question ──────────────────
    //  统一弹窗（ThemedMessageBox）的视觉与动画没法用单元测试断言，只能抓图取证。
    //  放在法务闸门之后、登录之前：不依赖登录态，任何机器上都能跑。
    //  抓图时机取 600ms —— 入场动画 200ms 已播完，退场动画还没开始。
    {
        const int msgIndex = app.arguments().indexOf(QStringLiteral("--msgbox"));
        if (msgIndex >= 0 && msgIndex + 1 < app.arguments().size()) {
            const QString kind = app.arguments().at(msgIndex + 1);
            kc::ui::ThemedMessageBox::Severity severity = kc::ui::ThemedMessageBox::Severity::Info;
            QString title = QStringLiteral("提示");
            QString text = QStringLiteral("这是一条普通提示信息。");
            if (kind == QLatin1String("warning")) {
                severity = kc::ui::ThemedMessageBox::Severity::Warning;
                title = QStringLiteral("确认操作");
                text = QStringLiteral("该操作会写入设备分区，执行后无法撤销。\n请确认已备份重要数据。");
            } else if (kind == QLatin1String("critical")) {
                severity = kc::ui::ThemedMessageBox::Severity::Critical;
                title = QStringLiteral("账号已被封禁");
                text = QStringLiteral("封禁原因：系统代理指向本机 127.0.0.1:3067\n"
                                      "到期时间：2026-10-14 12:00\n\n"
                                      "如认为误封，可点「去申诉」提交证据流水。");
            } else if (kind == QLatin1String("question")) {
                severity = kc::ui::ThemedMessageBox::Severity::Question;
                title = QStringLiteral("删除应用");
                text = QStringLiteral("确定要卸载所选应用吗？应用数据将一并清除。");
            } else if (kind == QLatin1String("erase")) {
                // 擦除分区专用弹窗（EraseConfirmDialog）取证：带滚动分区列表，
                // 用样例 24 个分区把列表撑过 240px 上限，验证内部滚动。
                QStringList sample;
                for (int i = 0; i < 24; ++i) {
                    sample.append(QStringLiteral("partition_%1 (LUN %2)")
                                      .arg(i, 2, 10, QLatin1Char('0'))
                                      .arg(i % 4));
                }
                kc::ui::EraseConfirmDialog dlg(
                    nullptr, QStringLiteral("即将擦除 24 个分区，共 1118.90 GB。"), sample);
                if (screenshotIndex >= 0) {
                    QTimer::singleShot(600, &dlg, [&dlg, shotPath] {
                        dlg.grab().save(shotPath);
                        // 同时抓内层卡片：顶层 grab 在 WA_TranslucentBackground 下
                        // 丢背景，卡片自身 grab 才能验证 QSS 背景是否真的画出来了。
                        if (auto* card =
                                dlg.findChild<QFrame*>(QStringLiteral("msgRoot"))) {
                            card->grab().save(shotPath.left(shotPath.length() - 4)
                                              + QStringLiteral("_card.png"));
                        }
                        dlg.reject();
                    });
                }
                dlg.exec();
                return 0;
            }
            kc::ui::ThemedMessageBox box(
                severity, title, text,
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
            if (screenshotIndex >= 0) {
                QTimer::singleShot(600, &box, [&box, shotPath] {
                    box.grab().save(shotPath);
                    if (auto* card =
                            box.findChild<QFrame*>(QStringLiteral("msgRoot"))) {
                        card->grab().save(shotPath.left(shotPath.length() - 4)
                                          + QStringLiteral("_card.png"));
                    }
                    // 🔴 reject() 会走 AnimatedDialog::done 的退场动画路径，
                    //    正好把「关闭动画」也跑一遍；直接 quit() 会跳过它。
                    box.reject();
                });
            }
            box.exec();
            return 0;
        }
    }

    // ── 截图探针：--cloud-loader / --preload 已随联网功能移除 ────────────────
    //  停更离线版不再有云端引导库和启动预载。

    // ── --env-check 探针已随环境检测上报功能移除 ──────────────────────────

    // ── 停更离线版（2.0.6）：不再登录、不再检查更新、不再做环境检测上报 ─────
    //  作者退圈、服务器即将关停，这些联网功能全部移除。启动直接进主界面，
    //  本地刷机功能（EDL / Fastboot / MTK / 小米 / OPPO / 解包 / 分区）照常可用。

    kc::ui::MainShell window;
    bootMark("主界面已构造");
    window.show();
    bootMark("主界面已显示");

    // ── 首次启动的欢迎 + 引导 ────────────────────────────────────────────────
    //  用户口径：「仅仅是第一次打开，不是每个设备打开都要一次」——
    //  判据是本机 QSettings 的 onboarding/completed，写在弹窗自己手里：
    //  看完 / 点「跳过」都会置位，之后每次启动直接进主屏。
    //  · 放在 window.show() **之后**：主屏已经在后面铺好，弹窗像盖在上面；
    //  · 截图探针一律跳过 —— 否则每次抓图都只能拍到引导窗，主屏再也拍不到；
    //  · --welcome 已经在前面单独 return，不会走到这里。
    //  想重看：设置页「再看一次引导」（WelcomeDialog::resetCompleted）。
    if (screenshotIndex < 0 && !kc::ui::WelcomeDialog::isCompleted()) {
        kc::ui::WelcomeDialog welcome;
        welcome.exec();   // 关闭（Esc / 关窗）不置位 —— 下次还会提示，这是有意的
    }

    // 可选：--page <key> 落到指定页（探针 / 调试用，见 MainShell::navigateTo）。
    // 🔴 **可重复传**：`--page A --page B` 会依次跳 A→B，用来实测翻页过渡动画
    //    （只传一个时第一个就是初始页，previous=-1 不播动画，测不到切换）。
    QStringList navKeys;
    for (int i = 0; i + 1 < app.arguments().size(); ++i) {
        if (app.arguments().at(i) == QLatin1String("--page")) {
            navKeys.append(app.arguments().at(i + 1));
        }
    }
    // 导航步进间隔（探针用）。默认 400ms：翻页过渡 400ms，刚好接上。
    // 🔴 测转场帧率时调大（KC_NAV_GAP=2000）：冷启动那一下页面正在做首次激活
    //    （枚举设备 / 扫目录），主线程被占住 —— 量出来的是初始化开销，不是转场。
    int navGap = 400;
    if (const QByteArray g = qgetenv("KC_NAV_GAP"); !g.isEmpty()) {
        bool ok = false;
        const int v = g.toInt(&ok);
        if (ok) navGap = qBound(50, v, 20000);
    }
    if (!navKeys.isEmpty()) {
        window.navigateTo(navKeys.first());
        for (int i = 1; i < navKeys.size(); ++i) {
            QTimer::singleShot(navGap * i, &window, [&window, key = navKeys.at(i)] {
                window.navigateTo(key);
            });
        }
    }

    // 可选：--scroll <px|bottom> 把**当前页**的滚动区滚到指定位置（探针 / 调试用）。
    // 设置页内容比窗口高一大截，底部的卡片（安全与隐私 / 法务）不滚动永远拍不到，
    // 只能靠人眼打开程序手动滚 —— 等于这部分改动没有截图证据。
    // 取「当前可见的那个 QScrollArea」：隐藏页的滚动区 isVisible() 为 false，
    // 不会被误选（每个页面各有一个，findChild 只取第一个会拍到别的页）。
    {
        const int scrollIndex = app.arguments().indexOf(QStringLiteral("--scroll"));
        if (scrollIndex >= 0 && scrollIndex + 1 < app.arguments().size()) {
            const QString spec = app.arguments().at(scrollIndex + 1).trimmed();
            QTimer::singleShot(700, &window, [&window, spec] {
                const auto areas = window.findChildren<QScrollArea*>();
                for (QScrollArea* area : areas) {
                    if (!area->isVisible() || !area->verticalScrollBar()) continue;
                    int value = 0;
                    if (spec.compare(QStringLiteral("bottom"), Qt::CaseInsensitive) == 0) {
                        value = area->verticalScrollBar()->maximum();
                    } else {
                        bool ok = false;
                        value = spec.toInt(&ok);
                        if (!ok) return;
                    }
                    area->verticalScrollBar()->setValue(value);
                    return;
                }
            });
        }
    }

    // 可选：--scatter <路径> 让 MTK 页直接加载一个 scatter（探针 / 调试用，
    // 否则截图只能看到空表 —— 浏览文件对话框在无头探针里点不了）。
    {
        const int scatterIndex = app.arguments().indexOf(QStringLiteral("--scatter"));
        if (scatterIndex >= 0 && scatterIndex + 1 < app.arguments().size()) {
            const QString scatterPath = app.arguments().at(scatterIndex + 1);
            if (auto* mtk = window.findChild<kc::ui::MtkFlashPage*>()) {
                QTimer::singleShot(300, mtk,
                                   [mtk, scatterPath] { mtk->loadScatterFile(scatterPath); });
            }
        }
    }

    // 可选：--xml <路径> 让 EDL 刷写页直接载入一个 rawprogram XML（探针 / 调试用，
    // 同 --scatter 思路：跳过文件选择对话框，分区表会渲染出「META 免合并」状态，
    // 免合并细节因此可在截图里取证）。支持动态分区包（super.img 为空 / 缺失）→ 免合并。
    {
        const int xmlIndex = app.arguments().indexOf(QStringLiteral("--xml"));
        if (xmlIndex >= 0 && xmlIndex + 1 < app.arguments().size()) {
            const QString xmlPath = app.arguments().at(xmlIndex + 1);
            if (auto* edl = window.findChild<kc::ui::EdlFlashPage*>()) {
                QTimer::singleShot(300, edl, [edl, xmlPath] {
                    if (auto* mw = edl->edlWindow()) mw->loadFirmwareXmlPaths({xmlPath});
                });
            }
        }
    }

    // 可选：--tab <key> 切到指定页签（探针 / 调试用；key = flash / device /
    // advanced / cloud / settings / all）。排在所有 --page 跳转之后执行，
    // 且 navigate = true：当前页不在该页签就落到它的首个可见页。
    {
        const int tabIndex = app.arguments().indexOf(QStringLiteral("--tab"));
        if (tabIndex >= 0 && tabIndex + 1 < app.arguments().size()) {
            const QString tabKey = app.arguments().at(tabIndex + 1);
            const int tabDelay = 400 * qMax(1, navKeys.size()) + 200;
            QTimer::singleShot(tabDelay, &window, [&window, tabKey] {
                window.setActiveTab(tabKey, /*navigate=*/true);
            });
        }
    }

    // 可选：--island-opt theme=glass,columns=8,collapsed=clock,...
    // 探针：个性化七项在无头环境里没有别的取证办法（直接改 settings.json 会污染
    // 用户真实配置）。这里只改内存、**不 save()**，进程退出即丢弃。
    {
        const int optIndex = app.arguments().indexOf(QStringLiteral("--island-opt"));
        if (optIndex >= 0 && optIndex + 1 < app.arguments().size()) {
            const QString spec = app.arguments().at(optIndex + 1);
            kc::AppSettings& s = kc::AppSettings::current();
            for (const QString& pair : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const int eq = pair.indexOf(QLatin1Char('='));
                if (eq <= 0) continue;
                const QString key = pair.left(eq).trimmed();
                const QString value = pair.mid(eq + 1).trimmed();
                if (key == QLatin1String("theme")) s.islandTheme = value;
                else if (key == QLatin1String("radius")) s.islandRadius = value.toInt();
                else if (key == QLatin1String("collapsed")) s.islandCollapsed = value;
                else if (key == QLatin1String("visibility")) s.islandVisibility = value;
                else if (key == QLatin1String("columns")) s.islandColumns = value.toInt();
                else if (key == QLatin1String("dim"))
                    s.islandDim = (value != QLatin1String("0")
                                   && value != QLatin1String("false"));
                else if (key == QLatin1String("lite"))
                    s.islandLiteMotion = (value != QLatin1String("0")
                                          && value != QLatin1String("false"));
            }
            window.applyIslandSettings();
        }
    }

    // 可选：--island 展开灵动岛后抓图（探针 / 调试用）。
    // 灵动岛默认折叠，展开态是点出来的 —— 没有这个开关就拍不到展开面板。
    if (app.arguments().contains(QStringLiteral("--island"))) {
        if (auto* island = window.findChild<kc::ui::DynamicIsland*>()) {
            QTimer::singleShot(500, island, [island] { island->setExpanded(true); });
        }
    }

    // 可选：--island-search <文字> 往岛内搜索框里塞一段字（探针）。
    // 搜索是「输入即过滤 + 面板按新行数收缩」，无头环境里没有键盘，只能这么取证。
    {
        const int searchIndex = app.arguments().indexOf(QStringLiteral("--island-search"));
        if (searchIndex >= 0 && searchIndex + 1 < app.arguments().size()) {
            const QString text = app.arguments().at(searchIndex + 1);
            if (auto* island = window.findChild<kc::ui::DynamicIsland*>()) {
                QTimer::singleShot(560, island, [island, text] {
                    if (auto* edit = island->findChild<QLineEdit*>()) edit->setText(text);
                });
            }
        }
    }

    // 可选：--island-action <id> 模拟点一下灵动岛快捷动作条（探针）。
    // 信号本身可以按名字 invoke —— 不必为了取证在组件上加一个「测试专用」的公开槽。
    {
        const int actIndex = app.arguments().indexOf(QStringLiteral("--island-action"));
        if (actIndex >= 0 && actIndex + 1 < app.arguments().size()) {
            const QString id = app.arguments().at(actIndex + 1);
            if (auto* island = window.findChild<kc::ui::DynamicIsland*>()) {
                QTimer::singleShot(400, island, [island, id] {
                    QMetaObject::invokeMethod(island, "actionTriggered",
                                              Q_ARG(QString, id));
                });
            }
        }
    }

    // ═══ 液态玻璃新控件取证探针（2026-10-24）═══════════════════════════════════
    //  菜单 / Toast / 抽屉 / 玻璃弹窗都是**顶层窗口**，window.grab() 拍不到，
    //  必须各自 grab() 自身；同时要在下面的整窗抓图里让路，否则整窗抓图先 quit。

    // 可选：--glassmenu 弹出液态玻璃菜单并抓菜单自身。
    if (app.arguments().contains(QStringLiteral("--glassmenu"))) {
        QTimer::singleShot(700, &window, [&window, shotPath] {
            const bool dark = kc::ui::ThemeManager::instance().isDark();
            const QColor ic = dark ? QColor(255, 255, 255, 205) : QColor(20, 24, 30, 205);

            auto* menu = new kc::ui::GlassMenu(&window);
            menu->addItem(QIcon(kc::ui::appIconPixmap(kc::ui::AppIcon::Package, ic, 16)),
                          QStringLiteral("读取分区"), [] {});
            menu->addItem(QIcon(kc::ui::appIconPixmap(kc::ui::AppIcon::Folder, ic, 16)),
                          QStringLiteral("写入分区"), [] {});
            menu->addSeparator();
            menu->addItem(QIcon(kc::ui::appIconPixmap(kc::ui::AppIcon::Bolt, ic, 16)),
                          QStringLiteral("擦除分区"), [] {});
            menu->addItem(QIcon(), QStringLiteral("复制分区名"), [] {}, true);

            menu->popupAt(QRect(window.geometry().center(), QSize(0, 0)));
            QTimer::singleShot(420, menu, [menu, shotPath] {
                menu->grab().save(shotPath);
                menu->dismiss(false);
                menu->deleteLater();
                QApplication::quit();
            });
        });
    }

    // 可选：--toast 弹一条玻璃横幅并抓横幅自身。
    if (app.arguments().contains(QStringLiteral("--toast"))) {
        QTimer::singleShot(700, &window, [&window, shotPath] {
            kc::ui::Toast::show(QStringLiteral("设置已保存"), kc::ui::Toast::Kind::Success, 20000);
            QTimer::singleShot(520, &window, [shotPath] {
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (auto* t = qobject_cast<kc::ui::Toast*>(w)) {
                        t->grab().save(shotPath);
                        break;
                    }
                }
                QApplication::quit();
            });
        });
    }

    // 可选：--sheet 弹出底部玻璃抽屉并抓抽屉自身。
    if (app.arguments().contains(QStringLiteral("--sheet"))) {
        QTimer::singleShot(700, &window, [&window, shotPath] {
            kc::ui::BottomSheet::confirm(
                QStringLiteral("擦除分区"),
                QStringLiteral("即将擦除 system 分区，共 1.2 GB。此操作不可撤销。"),
                QStringLiteral("擦除"), [] {});
            QTimer::singleShot(700, &window, [shotPath] {
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (auto* s = qobject_cast<kc::ui::BottomSheet*>(w)) {
                        s->grab().save(shotPath);
                        break;
                    }
                }
                QApplication::quit();
            });
        });
    }

    // 可选：--glassdialog 弹一个玻璃弹窗并抓弹窗自身。
    if (app.arguments().contains(QStringLiteral("--glassdialog"))) {
        QTimer::singleShot(700, &window, [&window, shotPath] {
            auto* dlg = new kc::ui::GlassDialog(&window);
            dlg->setAttribute(Qt::WA_DeleteOnClose, true);
            dlg->setTitle(QStringLiteral("擦除分区"));
            auto* lbl = new QLabel(
                QStringLiteral("即将擦除 system 分区，共 1.2 GB。\n此操作不可撤销，请确认。"), dlg);
            lbl->setWordWrap(true);
            lbl->setStyleSheet(QStringLiteral(
                "background: transparent; color: %1; font-size: 12.5px;")
                .arg(kc::ui::ThemeManager::instance().isDark()
                         ? QStringLiteral("rgba(255,255,255,0.82)")
                         : QStringLiteral("rgba(18,22,28,0.82)")));
            dlg->bodyLayout()->addWidget(lbl);
            dlg->addButton(QStringLiteral("取消"), false, [dlg] { dlg->reject(); });
            dlg->addButton(QStringLiteral("擦除"), true, [dlg] { dlg->accept(); });
            dlg->show();
            QTimer::singleShot(520, dlg, [dlg, shotPath] {
                dlg->grab().save(shotPath);
                dlg->hide();
                QApplication::quit();
            });
            // 🔴 兜底：模态弹窗里 quit() 可能落不到主事件循环（探针曾卡死 6 分钟）。
            QTimer::singleShot(3500, &window, [] { std::exit(0); });
        });
    }

    // 可选：--controlcenter 打开控制中心那一页后抓图（探针 / 调试用）。
    if (app.arguments().contains(QStringLiteral("--controlcenter"))) {
        QTimer::singleShot(600, &window, [&window] { window.openControlCenter(); });
    }

    // 可选：--edl-advanced 展开「高级功能 ▾」下拉（探针 / 调试用）。
    // 2026-10-19 起高级功能是页签条右侧的下拉框（objectName = advancedCombo），
    // 不再是「按钮 + 模态弹窗」—— 旧探针按 quickLink 找按钮，早就找不到东西了。
    // 🔴 延时 900ms：EDL 窗是进页面后懒建的，500ms 时 findChild 还是空。
    if (app.arguments().contains(QStringLiteral("--edl-advanced"))) {
        QTimer::singleShot(900, &window, [&window] {
            if (auto* combo = window.findChild<QComboBox*>(QStringLiteral("advancedCombo")))
                combo->showPopup();
        });
    }

    // 可选：--edl-advanced-dialog 展开下拉并**抓弹出层自身**
    // （下拉弹层是独立顶层窗，主窗口 grab() 拍不到）。
    if (app.arguments().contains(QStringLiteral("--edl-advanced-dialog"))) {
        QTimer::singleShot(900, &window, [&window, shotPath] {
            auto* combo = window.findChild<QComboBox*>(QStringLiteral("advancedCombo"));
            if (!combo) {
                QApplication::quit();
                return;
            }
            combo->showPopup();
            QTimer::singleShot(400, &window, [combo, shotPath] {
                if (QWidget* popup = combo->view()->window()) popup->grab().save(shotPath);
                QApplication::quit();
            });
        });
    }

    // 可选：--edl-fdl-dialog 打开 FDL 流程窗并抓图（打开即自动开跑完整流程；
    // 无头环境没有 fastboot 设备，会停在「失败 / 未检测到设备」态，正好取证 UI）。
    if (app.arguments().contains(QStringLiteral("--edl-fdl-dialog"))) {
        QTimer::singleShot(500, &window, [&window, shotPath] {
            if (auto* menu = window.findChild<QMenu*>(QStringLiteral("rebootMenu"))) {
                for (QAction* action : menu->actions()) {
                    if (action->text().contains(QStringLiteral("FDL"))) {
                        action->trigger();
                        break;
                    }
                }
            }
            QTimer::singleShot(900, &window, [&window, shotPath] {
                if (auto* dlg = window.findChild<QDialog*>(QStringLiteral("fdlFlowDialog")))
                    dlg->grab().save(shotPath);
                QApplication::quit();
            });
        });
    }

    // --fdl-auth 探针已随 FDL 授权功能移除。

    // 可选：--edl-reboot-menu 弹出重启菜单并抓图（探针取证用：菜单是独立
    // 顶层弹窗，抓主窗口不含它，必须单独 grab 菜单自身）。
    if (app.arguments().contains(QStringLiteral("--edl-reboot-menu"))) {
        QTimer::singleShot(600, &window, [&window, shotPath] {
            auto* menu = window.findChild<QMenu*>(QStringLiteral("rebootMenu"));
            if (!menu) {
                QApplication::quit();
                return;
            }
            menu->popup(window.mapToGlobal(QPoint(420, 120)));
            QTimer::singleShot(300, menu, [menu, shotPath] {
                menu->grab().save(shotPath);
                menu->hide();
                QApplication::quit();
            });
        });
    }

    // 可选：--live-alpha <卡片,界面> 在**运行时**改玻璃参数并重放样式表
    // （探针 / 回归用，2026-10-17）。
    // 存在的唯一目的：验证「设置页滑条拖动即时生效」那条代码路径。
    // 启动时读配置生效 ≠ 运行时 refreshStyleSheet() 生效 —— 后者要给整棵树
    // 重新 polish 一遍，是另一条路。与不传参的同页截图逐像素比，有差异才算过。
    {
        const int liveIndex = app.arguments().indexOf(QStringLiteral("--live-alpha"));
        if (liveIndex >= 0 && liveIndex + 1 < app.arguments().size()) {
            const QStringList parts =
                app.arguments().at(liveIndex + 1).split(QLatin1Char(','));
            QTimer::singleShot(400, &window, [parts] {
                kc::AppSettings& s = kc::AppSettings::current();
                if (parts.size() > 0) s.cardOpacity = parts.at(0).toDouble();
                if (parts.size() > 1) s.uiTransparency = parts.at(1).toDouble();
                kc::ui::ThemeManager::instance().refreshStyleSheet();
            });
        }
    }

    // 可选：--swipe-preview <0~1> 把「主屏 → 控制中心」的横滑定格在指定进度
    // （探针：跟手转场的几何 / 投影 / 衬底有没有错位，只能看定格帧）。
    {
        const int swipeIndex = app.arguments().indexOf(QStringLiteral("--swipe-preview"));
        if (swipeIndex >= 0 && swipeIndex + 1 < app.arguments().size()) {
            const qreal p = app.arguments().at(swipeIndex + 1).toDouble();
            QTimer::singleShot(600, &window, [&window, p] { window.previewSwipe(p); });
        }
    }

    // 可选：--scroll <px> 把**当前页**的滚动区滚到指定位置（探针 / 调试用）。
    // 设置页与工具页都带 QScrollArea，内容比窗口高 —— 不滚动只能抓到第一屏，
    // 页面下半部分（新增的卡片）永远无法取证。
    // 取「当前可见」的那个 QScrollArea：页面栈里所有页都是主窗口的后代，
    // 直接 findChild 会摸到别的页（被隐藏，滚它没用）。
    {
        const int scrollIndex = app.arguments().indexOf(QStringLiteral("--scroll"));
        if (scrollIndex >= 0 && scrollIndex + 1 < app.arguments().size()) {
            const int offset = app.arguments().at(scrollIndex + 1).toInt();
            const int scrollDelay = 450 + navGap * qMax(0, navKeys.size() - 1);
            QTimer::singleShot(scrollDelay, &window, [&window, offset] {
                for (QScrollArea* area : window.findChildren<QScrollArea*>()) {
                    if (!area->isVisible()) continue;
                    area->verticalScrollBar()->setValue(offset);
                    break;
                }
            });
        }
    }

    // --cloud-panel / --cloud-panel-popup 探针已随云端引导库移除（停更离线版 2.0.6）。

    // 截图模式：抓主窗口（法务闸门已放行 / 已同意才会走到这里）。
    // 多次 --page 时把抓图推迟到**最后一次跳转之后**，否则抓到的是中间页。
    // 🔴 --edl-reboot-menu 自带抓图（抓菜单自身，主窗口 grab 不含弹窗菜单），
    //    这里必须让路 —— 否则 800ms 的整窗抓图抢先 quit，菜单特写永远拍不到
    //    （2026-10-16 探针实测：拍出来是整窗而不是菜单）。
    if (screenshotIndex >= 0 &&
        !app.arguments().contains(QStringLiteral("--edl-reboot-menu")) &&
        !app.arguments().contains(QStringLiteral("--edl-advanced-dialog")) &&
        !app.arguments().contains(QStringLiteral("--glassmenu")) &&
        !app.arguments().contains(QStringLiteral("--toast")) &&
        !app.arguments().contains(QStringLiteral("--sheet")) &&
        !app.arguments().contains(QStringLiteral("--actionsheet")) &&
        !app.arguments().contains(QStringLiteral("--glassdialog")) &&
        !app.arguments().contains(QStringLiteral("--cloud-panel-popup"))) {
        int grabDelay = 500 + navGap * qMax(0, navKeys.size() - 1) + 300;
        // KC_SHOT_DELAY：给「依赖异步资源就绪」的探针留额外余量。
        // 典型场景：视频壁纸要走 Media Foundation 解码，首帧到达往往在 800ms 之后，
        // 此时 Dock 的液态玻璃背板还在用「底色晕」凑 —— 拍出来是一块白板，
        // 看起来像玻璃没做，其实是拍早了。
        {
            bool ok = false;
            const int extra = qEnvironmentVariableIntValue("KC_SHOT_DELAY", &ok);
            if (ok && extra > 0) grabDelay += extra;
        }
        // --controlcenter 是普通跳页：600ms 起跳、转场 400ms；不推后就会拍到
        // 推入到一半的画面。
        if (app.arguments().contains(QStringLiteral("--controlcenter"))) grabDelay += 500;
        // --island 的展开补间 500ms 才起跑、340ms 到位。
        // 🔴 这里要留**很宽**的余量（实测 1100ms 仍只到 79%）：启动后头 1 秒
        //    事件循环基本被页面激活 / 设备枚举占满，两个定时器都会整体后移，
        //    而 QPropertyAnimation 的进度是按真实时间走的 —— 卡在 79% 时第三排
        //    卡片还是半透明，拍出来像「展开失败」。
        if (app.arguments().contains(QStringLiteral("--island"))) grabDelay += 1200;
        // --swipe-preview 同理：驱动本身在 600ms，但同样会被启动占用推后。
        if (app.arguments().contains(QStringLiteral("--swipe-preview"))) grabDelay += 900;
        // --scroll 的滚动定时器在 450ms 起跳，留 400ms 让重排 / 重绘落定。
        if (app.arguments().contains(QStringLiteral("--scroll"))) grabDelay += 400;
        // --cloud-panel：900ms 展开 + 一次索引请求（线上 ~0.3s，留足余量），
        // 之后还要等表格填充与布局重排 —— 不留这一档就只能拍到「正在获取…」。
        if (app.arguments().contains(QStringLiteral("--cloud-panel"))) grabDelay += 2200;
        // --edl-advanced：下拉在 900ms 才展开（EDL 窗懒建），主窗口抓图必须等它。
        if (app.arguments().contains(QStringLiteral("--edl-advanced"))) grabDelay += 1200;
        QTimer::singleShot(grabDelay, &window, [&window, shotPath] {
            window.grab().save(shotPath);
            QApplication::quit();
        });
    }
    return app.exec();
}
