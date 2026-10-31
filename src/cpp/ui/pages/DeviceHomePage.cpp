#include "ui/pages/DeviceHomePage.h"

#include "kc/devices/DeviceWatcher.h"
#include "kc/devices/UsbDeviceNotifier.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QScrollArea>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

/// 把一段函数体丢进全局线程池（等价 C# 的 Task.Run）。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

/// adb / fastboot 一律走**随包平台工具**（与其它页一致），随包缺失才回落 PATH。
/// 之前本页直接跑裸命令名，等于把「能不能检测到设备」交给用户的 PATH ——
/// 随包工具明明在，PATH 里没有就永远显示「未连接」。
QString toolPath(const QString& program)
{
    static const QString adb = ::kc::PlatformToolsManager().adbPath();
    static const QString fastboot = ::kc::PlatformToolsManager().fastbootPath();
    if (program == QLatin1String("adb") && QFile::exists(adb)) return adb;
    if (program == QLatin1String("fastboot") && QFile::exists(fastboot)) return fastboot;
    return program;
}

/// 起进程前的组件完整性检查：adb.exe 缺同目录 AdbWinApi.dll 时 Windows 会弹
/// 系统错误框，而本页每 5 秒探一轮 → 弹窗关了又冒。组件不齐直接当「没探到」。
bool toolReady(const QString& exePath)
{
    const QFileInfo info(exePath);
    if (!info.isAbsolute()) return true;   // 走 PATH 的裸命令名，无法预检
    return ::kc::PlatformToolsManager::ensureBinaryReady(exePath, nullptr);
}

/// 执行一条外部命令并返回合并后的 stdout（超时 / 失败返回空串）。
/// 与 C# RunCommand 等价：进程不存在时静默返回空（不抛）。
QString runCommand(const QString& program, const QStringList& args, int timeoutMs = 4000)
{
    const QString exe = toolPath(program);
    if (!toolReady(exe)) return {};

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(exe, args);
    if (!process.waitForStarted(1500)) return {};
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

bool processExists(const QString& program)
{
    const QString exe = toolPath(program);
    if (!toolReady(exe)) return false;

    QProcess process;
    process.start(exe, { QStringLiteral("--version") });
    return process.waitForStarted(800);
}

/// getprop 输出里按 key 取一行值（行格式：[key]: [value]）。
QString getProp(const QString& props, const QString& key)
{
    for (const QString& rawLine : props.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (!line.startsWith(QStringLiteral("[") + key)) continue;
        const int close = line.indexOf(QStringLiteral("]:"));
        if (close < 0) continue;
        return line.mid(close + 2).trimmed();
    }
    return {};
}

/// `MemTotal:        7657472 kB` → `7478 MB`（取 kB 数 / 1024）。
QString parseRamMb(const QString& memInfo)
{
    if (memInfo.isEmpty()) return {};
    const QStringList parts = memInfo.split(QRegularExpression(QStringLiteral("[\\s\\t]+")),
                                            Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        bool ok = false;
        const qint64 kb = part.toLongLong(&ok);
        if (ok && kb > 1024) return QStringLiteral("%1 MB").arg(kb / 1024);
    }
    return {};
}

/// `wm size` 输出 `Physical size: 1080x2400`（还可能有 Override 行）→ 取最后一组。
QString parseResolution(const QString& wmSize)
{
    if (wmSize.isEmpty()) return {};
    static const QRegularExpression re(QStringLiteral("(\\d{3,5})\\s*x\\s*(\\d{3,5})"));
    QRegularExpressionMatch last;
    auto it = re.globalMatch(wmSize);
    while (it.hasNext()) last = it.next();
    if (!last.hasMatch()) return {};
    return QStringLiteral("%1 × %2").arg(last.captured(1), last.captured(2));
}

} // namespace

DeviceHomePage::DeviceHomePage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("主页"));

    buildUi();

    // USB 插拔一次物理变化会连发多条 PnP 事件，去抖到 600ms 只触发一轮探测。
    m_usbDebounce = new QTimer(this);
    m_usbDebounce->setSingleShot(true);
    m_usbDebounce->setInterval(600);
    connect(m_usbDebounce, &QTimer::timeout, this, [this] { refreshAsync(); });

    // 自动刷新（5s 一轮），默认关闭（开关在快捷操作区）。
    m_autoRefresh = new QTimer(this);
    m_autoRefresh->setInterval(5000);
    connect(m_autoRefresh, &QTimer::timeout, this, [this] { refreshAsync(); });

    m_watcher = new DeviceWatcher(this);

    // USB 插拔钩子：设备切模式时自动重探（仅在页面可见时生效）。
    if (auto* notifier = UsbDeviceNotifier::instance()) {
        notifier->initialize();
        connect(notifier, &UsbDeviceNotifier::deviceChanged, this, [this] {
            if (!isVisible()) return;
            m_usbDebounce->start();
        });
    }
}

DeviceHomePage::~DeviceHomePage() = default;

void DeviceHomePage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);

    // 页面内通知条（BasePage 通过 registerNotifyBar 取用）。
    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // ═════════ 2026-10-16 主页整体重做（iOS 27 玻璃风仪表盘）══════════════
    // 旧版是「状态卡 → 快捷卡 → 读数表 → 日志」四段满宽竖排：读数表用
    // QTableWidget 独占半屏、快捷按钮挤成两行，一屏看下去还是「一堆按钮」。
    // 新版重排成：
    //   ① 顶部双列 —— 左「设备状态」hero 卡（模式大字 + 提示 + 通道说明 +
    //      刷新入口），右「快捷操作」瓦片卡（5×2 玻璃瓦片）；
    //   ② 中部「设备信息」卡 —— 21 项读数由表格改为三列键值网格，一屏看完；
    //   ③ 底部「执行日志」卡。
    // ════════════════════════════════════════════════════════════════════

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(12);

    // ── ① 左：设备状态 hero 卡 ──────────────────────────────────────────
    auto* heroCard = new QFrame(this);
    heroCard->setObjectName(QStringLiteral("heroCard"));
    heroCard->setFrameShape(QFrame::StyledPanel);
    auto* heroLayout = new QVBoxLayout(heroCard);
    heroLayout->setContentsMargins(16, 14, 16, 14);
    heroLayout->setSpacing(8);

    auto* heroTitle = new QLabel(QStringLiteral("设备状态"), heroCard);
    heroTitle->setObjectName(QStringLiteral("groupHeader"));
    heroLayout->addWidget(heroTitle);

    auto* modeRow = new QHBoxLayout;
    modeRow->setSpacing(8);
    m_dot = new QLabel(QStringLiteral("●"), heroCard);
    m_dot->setObjectName(QStringLiteral("statusDot"));
    m_modeLabel = new QLabel(QStringLiteral("未连接"), heroCard);
    m_modeLabel->setObjectName(QStringLiteral("heroModeLabel"));
    modeRow->addWidget(m_dot);
    modeRow->addWidget(m_modeLabel, 1);
    heroLayout->addLayout(modeRow);

    m_hintLabel = new QLabel(heroCard);
    m_hintLabel->setObjectName(QStringLiteral("deviceHintLabel"));
    m_hintLabel->setWordWrap(true);
    heroLayout->addWidget(m_hintLabel);

    // 通道说明：让「自动选 adb / fastboot」这件事可见（原先在快捷卡右上角）。
    m_modeNote = new QLabel(QStringLiteral("未检测到设备"), heroCard);
    m_modeNote->setObjectName(QStringLiteral("statusLabel"));
    m_modeNote->setWordWrap(true);
    heroLayout->addWidget(m_modeNote);
    heroLayout->addStretch();

    auto* heroActions = new QHBoxLayout;
    heroActions->setSpacing(8);
    m_refreshButton = new QPushButton(QStringLiteral("立即刷新"), heroCard);
    m_refreshButton->setObjectName(QStringLiteral("primaryButton"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    // 手动刷新 = 用户明确要求重探一次：清掉粘滞，立刻反映真实状态。
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        m_stickiness.reset();
        refreshAsync();
    });
    m_autoRefreshButton = new QPushButton(QStringLiteral("开启自动刷新"), heroCard);
    m_autoRefreshButton->setObjectName(QStringLiteral("secondaryButton"));
    m_autoRefreshButton->setCursor(Qt::PointingHandCursor);
    m_autoRefreshButton->setToolTip(QStringLiteral("每 5 秒重新检测一次设备"));
    connect(m_autoRefreshButton, &QPushButton::clicked, this, [this] { toggleAutoRefresh(); });
    heroActions->addWidget(m_refreshButton);
    heroActions->addWidget(m_autoRefreshButton);
    heroActions->addStretch();
    heroLayout->addLayout(heroActions);

    topRow->addWidget(heroCard, 2);

    // ── ① 右：快捷操作瓦片卡 ────────────────────────────────────────────
    // 旧版 = ADB 快捷 ×5 + Fastboot 快捷 ×5 + 系统快捷 ×4：同一件事（重启系统/
    // Recovery/Bootloader）在 ADB 行和 Fastboot 行各写一遍，用户还得先判断自己在
    // 哪个模式。现在合成 10 个瓦片，点下去由 runDeviceOp 按当前模式决定下发 adb
    // 还是 fastboot；仅 Fastboot 存在的解锁 BL / 引导状态会明确挡回。
    auto* quickCard = new QFrame(this);
    quickCard->setObjectName(QStringLiteral("card"));
    quickCard->setFrameShape(QFrame::StyledPanel);
    auto* quickLayout = new QVBoxLayout(quickCard);
    quickLayout->setContentsMargins(16, 14, 16, 14);
    quickLayout->setSpacing(10);

    auto* quickHeaderRow = new QHBoxLayout;
    quickHeaderRow->setSpacing(8);
    auto* quickTitle = new QLabel(QStringLiteral("快捷操作"), quickCard);
    quickTitle->setObjectName(QStringLiteral("groupHeader"));
    quickHeaderRow->addWidget(quickTitle);
    quickHeaderRow->addStretch();
    quickLayout->addLayout(quickHeaderRow);

    auto* tileGrid = new QGridLayout;
    tileGrid->setHorizontalSpacing(8);
    tileGrid->setVerticalSpacing(8);
    int tileIndex = 0;
    const auto addTile = [&](const QString& label, const QString& tip,
                             const std::function<void()>& onClick) {
        auto* tile = new QPushButton(label, quickCard);
        tile->setObjectName(QStringLiteral("opTile"));
        tile->setCursor(Qt::PointingHandCursor);
        tile->setToolTip(tip);
        // 🔴 高度由 QSS 的 `min-height` 决定，**不要**用 setFixedHeight()：
        //    样式表里有 min-height 时 QStyleSheetStyle 会接管尺寸约束，
        //    setFixedHeight 会被无声忽略（2026-10-16 实测：设了 58px 仍渲染 20px）。
        connect(tile, &QPushButton::clicked, this, onClick);
        tileGrid->addWidget(tile, tileIndex / 5, tileIndex % 5);
        ++tileIndex;
        return tile;
    };

    addTile(QStringLiteral("重启系统"),
            QStringLiteral("ADB: adb reboot　/　Fastboot: fastboot reboot"),
            [this] { runDeviceOp(DeviceOp::Reboot); });
    addTile(QStringLiteral("重启Recovery"),
            QStringLiteral("ADB: adb reboot recovery　/　Fastboot: fastboot reboot recovery"),
            [this] { runDeviceOp(DeviceOp::RebootRecovery); });
    addTile(QStringLiteral("重启Bootloader"),
            QStringLiteral("ADB: adb reboot bootloader　/　Fastboot: fastboot reboot-bootloader"),
            [this] { runDeviceOp(DeviceOp::RebootBootloader); });
    addTile(QStringLiteral("重启EDL"),
            QStringLiteral("ADB: adb reboot edl　/　Fastboot: fastboot reboot-edl"),
            [this] { runDeviceOp(DeviceOp::RebootEdl); });
    addTile(QStringLiteral("重启FastbootD"),
            QStringLiteral("ADB: adb reboot fastboot　/　Fastboot: fastboot reboot fastboot"),
            [this] { runDeviceOp(DeviceOp::RebootFastbootD); });
    addTile(QStringLiteral("解锁BL"),
            QStringLiteral("仅 Fastboot 模式：fastboot oem unlock"),
            [this] { runDeviceOp(DeviceOp::UnlockBl); });
    addTile(QStringLiteral("查看引导状态"),
            QStringLiteral("仅 Fastboot 模式：fastboot getvar unlocked"),
            [this] { runDeviceOp(DeviceOp::BootState); });
    addTile(QStringLiteral("免BL进EDL"),
            QStringLiteral("ADB: adb reboot edl　/　Fastboot: fastboot oem edl + reboot edl"),
            [this] { runDeviceOp(DeviceOp::EnterEdl); });
    addTile(QStringLiteral("设备管理器"),
            QStringLiteral("打开 Windows 设备管理器（devmgmt.msc）"),
            [this] { openDeviceManager(); });
    addTile(QStringLiteral("命令行  ›"),
            QStringLiteral("跳到命令行页"),
            [this] { emit navigationRequested(QStringLiteral("CommandConsole")); });

    for (int c = 0; c < 5; ++c) tileGrid->setColumnStretch(c, 1);
    quickLayout->addLayout(tileGrid);
    quickLayout->addStretch();

    topRow->addWidget(quickCard, 3);
    layout->addLayout(topRow);

    // ── ② 设备信息卡（三列键值网格）────────────────────────────────────
    // stretch 4 : 2 —— 读数有 21 项、是主页的主体，日志是辅助；给少了末行会被
    // 卡内滚动切掉半行（2026-10-16 截图实测）。
    layout->addWidget(buildInfoCard(), 4);

    // ── ③ 执行日志卡 ────────────────────────────────────────────────────
    // 收进卡片给它一个身份，高度压到 2 份 stretch，空闲时不再占掉半个页面。
    auto* logCard = new QFrame(this);
    logCard->setObjectName(QStringLiteral("card"));
    logCard->setFrameShape(QFrame::StyledPanel);
    auto* logCardLayout = new QVBoxLayout(logCard);
    logCardLayout->setContentsMargins(16, 12, 16, 12);
    logCardLayout->setSpacing(8);
    auto* logHeader = new QLabel(QStringLiteral("执行日志"), logCard);
    logHeader->setObjectName(QStringLiteral("groupHeader"));
    logCardLayout->addWidget(logHeader);
    auto* log = new LogView(logCard);
    registerLogView(log);
    logCardLayout->addWidget(log, 1);
    layout->addWidget(logCard, 2);

    setModeLabel(Mode::None);
    clearInfo();
}

QWidget* DeviceHomePage::buildInfoCard()
{
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("card"));
    card->setFrameShape(QFrame::StyledPanel);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 12, 16, 12);
    cardLayout->setSpacing(8);

    auto* header = new QLabel(QStringLiteral("设备信息"), card);
    header->setObjectName(QStringLiteral("groupHeader"));
    cardLayout->addWidget(header);

    // 对齐 C# 21 项读数（合规：不采集序列号 / Android ID 等设备唯一标识）。
    const QStringList keys = {
        QStringLiteral("型号"),          // model
        QStringLiteral("Android 版本"),  // android
        QStringLiteral("版本号"),        // build
        QStringLiteral("品牌"),          // brand
        QStringLiteral("产品名"),        // product
        QStringLiteral("设备代号"),      // codename
        QStringLiteral("芯片平台"),      // platform
        QStringLiteral("内核版本"),      // kernel
        QStringLiteral("安全补丁"),      // patch
        QStringLiteral("基带版本"),      // baseband
        QStringLiteral("运行内存"),      // ram
        QStringLiteral("分辨率"),        // resolution
        QStringLiteral("屏幕密度"),      // density
        QStringLiteral("启动槽位"),      // slot
        QStringLiteral("A-B 分区"),      // ab
        QStringLiteral("电量"),          // battery
        QStringLiteral("电池温度"),      // batteryTemp
        QStringLiteral("存储空间"),      // storage
        QStringLiteral("Root 状态"),     // root
        QStringLiteral("引导状态"),      // bootloader
        QStringLiteral("USB 调试"),      // usbDebug
    };

    // 21 项读数一屏放不下时**卡内滚动**（不把整页拉长）—— 这是新版把表格换成
    // 网格后仍要保留的能力：窄窗口 / 高 DPI 下 3 列会换行。
    auto* scroll = new QScrollArea(card);
    scroll->setObjectName(QStringLiteral("infoScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* host = new QWidget;
    host->setObjectName(QStringLiteral("infoScrollHost"));
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(8);

    m_infoKeys = keys;
    m_infoValues.clear();
    m_infoGrid = grid;
    constexpr int kColumns = 3;
    for (int i = 0; i < keys.size(); ++i) {
        const QString& name = keys.at(i);
        auto* cell = new QWidget(host);
        auto* cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(2);
        auto* key = new QLabel(name, cell);
        key->setObjectName(QStringLiteral("infoKey"));
        auto* value = new QLabel(QStringLiteral("-"), cell);
        value->setObjectName(QStringLiteral("infoValue"));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        cellLayout->addWidget(key);
        cellLayout->addWidget(value);
        grid->addWidget(cell, i / kColumns, i % kColumns);
        m_infoValues.insert(name, value);
    }
    for (int c = 0; c < kColumns; ++c) grid->setColumnStretch(c, 1);
    // 末行下方留伸缩，读数不足一屏时不留一块拉伸的空行。
    grid->setRowStretch(keys.size() / kColumns + 1, 1);

    scroll->setWidget(host);
    cardLayout->addWidget(scroll, 1);
    return card;
}

void DeviceHomePage::onActivated()
{
    refreshAsync();
}

void DeviceHomePage::onDeactivated()
{
    if (m_usbDebounce) m_usbDebounce->stop();
}

void DeviceHomePage::refreshAsync()
{
    // 重入闸门：USB 插拔 / 可见性变化 / 手动刷新可能几乎同时打到，
    // 已有一轮在跑就把需求记成 pending，跑完再补一轮。
    if (m_detectRunning) {
        m_detectPending = true;
        return;
    }
    m_detectRunning = true;
    if (m_refreshButton) m_refreshButton->setEnabled(false);

    // 线程池跑检测（等价 C# 的 Task.Run），完成后回 UI 线程收尾。
    QPointer<DeviceHomePage> self(this);
    class DetectTask : public QRunnable {
    public:
        // 🔴 只持有 QPointer，**不存裸指针** —— 存了就会有人忍不住在判空前
        //    先用它（这正是 2026-10-30 那次崩溃的成因）。
        explicit DetectTask(QPointer<DeviceHomePage> guard) : m_guard(guard) {}
        void run() override
        {
            // 🔴 2026-10-30 崩溃修复（DeviceHomePage 约 12% 概率 exit=139）：
            //
            //    原代码是 `m_page->detect();` 打头，**然后**才 `m_guard.data()`
            //    判空 —— 顺序反了。任务在全局线程池里排队，页面完全可能在
            //    轮到这个任务之前就被销毁（切页 / 关窗 / 探针退出），
            //    此时裸指针已是野指针，detect() 一进去就段错误。
            //    守卫必须**先**查，查不到直接返回。
            DeviceHomePage* page = m_guard.data();
            if (!page) return;

            page->detect();

            // 排队回 UI 线程时同样要带守卫：从「排队」到「执行」之间还有窗口期，
            // 裸指针 lambda 在这段里会悬空。捕获 QPointer，真正执行时再取一次。
            QMetaObject::invokeMethod(
                page,
                [guard = m_guard] {
                    DeviceHomePage* p = guard.data();
                    if (!p) return;
                    p->m_detectRunning = false;
                    if (p->m_refreshButton) p->m_refreshButton->setEnabled(true);
                    if (p->m_detectPending) {
                        p->m_detectPending = false;
                        p->refreshAsync();
                    }
                },
                Qt::QueuedConnection);
        }

    private:
        QPointer<DeviceHomePage> m_guard;
    };
    QThreadPool::globalInstance()->start(new DetectTask(self));
}

void DeviceHomePage::detect()
{
    Mode mode = Mode::None;
    QHash<QString, QString> values;
    // 仅在「抢 adb 服务」真的发生 / 被拒时非空 —— 没探到设备时把原因摆到提示行，
    // 否则用户只能对着「未连接」猜是线的问题还是驱动的问题。
    QString takeoverHint;

    // ── ADB ────────────────────────────────────────────────────────────
    const bool haveAdb = processExists(QStringLiteral("adb"));
    if (haveAdb) {
        // `adb devices` 只有「序列号\tdevice」行才算在线。表头 `List of devices
        // attached` 里也含 device 字样，所以必须按行尾精确匹配。
        const auto probeAdbOnline = []() {
            const QString devices =
                runCommand(QStringLiteral("adb"), { QStringLiteral("devices") });
            const QStringList lines = devices.split(QLatin1Char('\n'));
            for (int i = 1; i < lines.size(); ++i) {
                if (lines.at(i).trimmed().endsWith(QStringLiteral("\tdevice"))) return true;
            }
            return false;
        };

        bool online = probeAdbOnline();

        // 探不到设备时，先怀疑「5037 被别的 adb 版本占着」：机器上常同时躺着
        // 好几个 adb.exe（System32 里被别的工具塞进去的旧版、Android Studio 的、
        // 其它刷机工具的）。adb server 是单例，谁先占住 5037 谁当服务端，后来者
        // 只能当客户端 —— 版本不同就报 server version doesn't match，直接连不上，
        // 于是设备明明插着却永远「未连接」。抢回服务端后再探一次。
        // （内部有 30s 冷却，不会在 1.5s 一轮的轮询里反复重启服务。）
        if (!online) {
            static ::kc::PlatformToolsManager tools;
            const ::kc::PlatformToolsManager::TakeoverResult takeover =
                tools.takeOverAdbServer();
            if (takeover.attempted) {
                takeoverHint = takeover.detail;
                if (takeover.ok) online = probeAdbOnline();
            }
        }

        if (online) {
            const QString buildType =
                runCommand(QStringLiteral("adb"),
                           { QStringLiteral("shell"), QStringLiteral("getprop"),
                             QStringLiteral("ro.build.type") });
            mode = (buildType == QStringLiteral("userdebug") || buildType == QStringLiteral("eng"))
                       ? Mode::Recovery
                       : Mode::Adb;
        }
    }

    // ── Fastboot ───────────────────────────────────────────────────────
    if (mode == Mode::None) {
        const QString fb = runCommand(QStringLiteral("fastboot"), { QStringLiteral("devices") });
        if (fb.contains(QStringLiteral("fastboot"))) mode = Mode::Fastboot;
    }

    // ── EDL 9008（DeviceWatcher 枚举，非侵入式）─────────────────────────
    if (mode == Mode::None) {
        const QList<::kc::DeviceInfo> watcherDevices = m_watcher->allDevices();
        for (const ::kc::DeviceInfo& dev : watcherDevices) {
            if (dev.type == ::kc::DeviceType::Edl9008) {
                mode = Mode::Edl;
                break;
            }
        }
    }

    // ── 连接状态粘滞 ────────────────────────────────────────────────────
    //  探测抖动（adb 服务刚起 / 模式切换中间态 / fastboot 重新枚举）会让状态
    //  标签来回闪。规则：状态成立后至少保持 10s，之后连续 4 次探不到才清除；
    //  探到**另一个**非 0 模式（真切换）立即采纳 —— 确定性事件不粘滞。
    //  🔴 必须在读设备信息**之前**定下有效模式：否则粘滞保住的 ADB 状态
    //     会跳过 getprop，标签显示 ADB 而读数全空。
    mode = static_cast<Mode>(m_stickiness.apply(static_cast<int>(mode),
                                                QDateTime::currentMSecsSinceEpoch()));

    // ── 设备信息（仅 ADB / Recovery 可读 getprop；一次 getprop 全量 dump
    //    本地解析，避免 C# 注释里说的「每项一次 adb 往返」拖慢刷新）───────
    if (mode == Mode::Adb || mode == Mode::Recovery) {
        values.insert(QStringLiteral("型号"),
                      runCommand(QStringLiteral("adb"),
                                 { QStringLiteral("shell"), QStringLiteral("getprop"),
                                   QStringLiteral("ro.product.model") }));
        values.insert(QStringLiteral("Android 版本"),
                      runCommand(QStringLiteral("adb"),
                                 { QStringLiteral("shell"), QStringLiteral("getprop"),
                                   QStringLiteral("ro.build.version.release") }));
        values.insert(QStringLiteral("版本号"),
                      runCommand(QStringLiteral("adb"),
                                 { QStringLiteral("shell"), QStringLiteral("getprop"),
                                   QStringLiteral("ro.build.display.id") }));
        values.insert(QStringLiteral("品牌"),
                      runCommand(QStringLiteral("adb"),
                                 { QStringLiteral("shell"), QStringLiteral("getprop"),
                                   QStringLiteral("ro.product.brand") }));

        const QString props =
            runCommand(QStringLiteral("adb"),
                       { QStringLiteral("shell"), QStringLiteral("getprop") });
        values.insert(QStringLiteral("产品名"), getProp(props, QStringLiteral("ro.product.name")));
        values.insert(QStringLiteral("设备代号"), getProp(props, QStringLiteral("ro.product.device")));
        values.insert(QStringLiteral("芯片平台"), getProp(props, QStringLiteral("ro.board.platform")));
        values.insert(QStringLiteral("安全补丁"),
                      getProp(props, QStringLiteral("ro.build.version.security_patch")));
        values.insert(QStringLiteral("基带版本"),
                      getProp(props, QStringLiteral("gsm.version.baseband")));
        values.insert(QStringLiteral("USB 调试"),
                      getProp(props, QStringLiteral("persist.sys.usb.config")));

        values.insert(QStringLiteral("内核版本"),
                      runCommand(QStringLiteral("adb"),
                                 { QStringLiteral("shell"), QStringLiteral("cat"),
                                   QStringLiteral("/proc/version") }));
        values.insert(QStringLiteral("运行内存"),
                      parseRamMb(runCommand(QStringLiteral("adb"),
                                            { QStringLiteral("shell"), QStringLiteral("grep"),
                                              QStringLiteral("MemTotal"), QStringLiteral("/proc/meminfo") })));
        values.insert(QStringLiteral("分辨率"),
                      parseResolution(runCommand(QStringLiteral("adb"),
                                                 { QStringLiteral("shell"), QStringLiteral("wm"),
                                                   QStringLiteral("size") })));

        const QString density = getProp(props, QStringLiteral("ro.sf.lcd_density")).trimmed();
        values.insert(QStringLiteral("屏幕密度"),
                      density.isEmpty() ? QString()
                                        : density + QStringLiteral(" dpi"));

        const QString slot = getProp(props, QStringLiteral("ro.boot.slot_suffix")).trimmed();
        values.insert(QStringLiteral("启动槽位"),
                      slot.compare(QStringLiteral("_a"), Qt::CaseInsensitive) == 0
                          ? QStringLiteral("A 槽")
                          : slot.compare(QStringLiteral("_b"), Qt::CaseInsensitive) == 0
                                ? QStringLiteral("B 槽")
                                : QStringLiteral("不适用（非 A/B）"));
        values.insert(QStringLiteral("A-B 分区"),
                      getProp(props, QStringLiteral("ro.build.ab_update")).trimmed()
                              == QStringLiteral("true")
                          ? QStringLiteral("支持")
                          : QStringLiteral("不支持"));

        // 一次 dumpsys battery 里同时抠出电量与温度（对齐 C#：不能抠到 level 就 break）。
        const QString battery =
            runCommand(QStringLiteral("adb"),
                       { QStringLiteral("shell"), QStringLiteral("dumpsys"),
                         QStringLiteral("battery") }, 15000);
        for (const QString& raw : battery.split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.startsWith(QStringLiteral("level:"), Qt::CaseInsensitive))
                values.insert(QStringLiteral("电量"),
                              line.mid(6).trimmed() + QStringLiteral("%"));
            else if (line.startsWith(QStringLiteral("temperature:"), Qt::CaseInsensitive))
                values.insert(QStringLiteral("电池温度"),
                              QStringLiteral("%1 °C")
                                  .arg(line.mid(12).trimmed().toInt() / 10.0, 0, 'f', 1));
        }

        values.insert(QStringLiteral("存储空间"),
                      runCommand(QStringLiteral("adb"),
                                 { QStringLiteral("shell"), QStringLiteral("df"),
                                   QStringLiteral("/data") }, 15000));

        const QString rootCheck =
            runCommand(QStringLiteral("adb"),
                       { QStringLiteral("shell"), QStringLiteral("su"), QStringLiteral("-c"),
                         QStringLiteral("id") }, 8000);
        values.insert(QStringLiteral("Root 状态"),
                      rootCheck.contains(QStringLiteral("uid=0")) ? QStringLiteral("已获取 Root")
                                                                  : QStringLiteral("未 Root"));

        // verifiedbootstate：green=已锁定，其余按已解锁显示（对齐 C#）。
        const QString bootState =
            runCommand(QStringLiteral("adb"),
                       { QStringLiteral("shell"), QStringLiteral("getprop"),
                         QStringLiteral("ro.boot.verifiedbootstate") });
        values.insert(QStringLiteral("引导状态"),
                      bootState.trimmed() == QStringLiteral("green") ? QStringLiteral("已锁定")
                                                                     : QStringLiteral("已解锁"));
    } else if (mode == Mode::Fastboot) {
        // 合规：不读 getvar serialno（设备唯一标识）。
        values.insert(QStringLiteral("产品名"),
                      runCommand(QStringLiteral("fastboot"),
                                 { QStringLiteral("getvar"), QStringLiteral("product") }));
    }

    const Mode finalMode = mode;
    const QHash<QString, QString> finalValues = values;
    const QString finalHint = takeoverHint;
    runOnUi([this, finalMode, finalValues, finalHint] {
        applyMode(finalMode);
        if (finalMode == Mode::Adb || finalMode == Mode::Recovery || finalMode == Mode::Fastboot)
            applyInfo(finalValues);
        else
            clearInfo();
        if (finalMode != Mode::None) {
            m_adbHint.clear();   // 连上了，之前那条接管结论就过期了
            const QLabel* modelLabel = m_infoValues.value(QStringLiteral("型号"));
            const QString model = modelLabel ? modelLabel->text() : QString();
            const QString device = model.isEmpty() || model == QStringLiteral("-")
                                       ? QStringLiteral("未知设备")
                                       : model;
            const QString modeText = m_modeLabel->text();
            setShellStatus(device, modeText);
        } else {
            // 「5037 被别的 adb 版本占着」和「线没插好」在界面上长得一模一样，
            // 有接管结论就把它顶到提示行，别让用户对着「未连接」瞎猜。
            if (!finalHint.isEmpty()) m_adbHint = finalHint;
            if (m_hintLabel && !m_adbHint.isEmpty()) m_hintLabel->setText(m_adbHint);
            setShellStatus(QStringLiteral("未连接设备"), QStringLiteral("未连接"));
        }
    });
}

void DeviceHomePage::applyInfo(const QHash<QString, QString>& values)
{
    for (const QString& key : m_infoKeys) {
        QLabel* value = m_infoValues.value(key);
        if (!value) continue;
        const QString text = values.value(key).trimmed();
        value->setText(text.isEmpty() ? QStringLiteral("-") : text);
    }
}

void DeviceHomePage::applyMode(Mode mode)
{
    m_mode = mode;
    setModeLabel(mode);
    const bool online = mode != Mode::None;
    m_hintLabel->setText(online ? QStringLiteral("设备已就绪")
                                : QStringLiteral("插入设备后自动检测（ADB / Fastboot / EDL 9008）"));

    // 快捷操作卡右上角的通道提示：让「自动选 adb / fastboot」这件事可见。
    if (m_modeNote) {
        QString note;
        switch (mode) {
        case Mode::Adb:      note = QStringLiteral("当前 ADB 模式 · 操作走 adb"); break;
        case Mode::Recovery: note = QStringLiteral("当前 Recovery 模式 · 操作走 adb"); break;
        case Mode::Fastboot: note = QStringLiteral("当前 Fastboot 模式 · 操作走 fastboot"); break;
        case Mode::Edl:      note = QStringLiteral("当前 EDL(9008) · 请用刷机页"); break;
        case Mode::None:
        default:             note = QStringLiteral("未检测到设备 · 插入后自动选通道"); break;
        }
        m_modeNote->setText(note);
    }
}

void DeviceHomePage::setModeLabel(Mode mode)
{
    const auto& theme = ThemeManager::instance();
    QString text;
    QColor color;
    switch (mode) {
    case Mode::Adb:
        text = QStringLiteral("ADB 模式");
        color = theme.color(ColorToken::LogSuccess);
        break;
    case Mode::Recovery:
        text = QStringLiteral("Recovery 模式");
        color = theme.color(ColorToken::LogWarning);
        break;
    case Mode::Fastboot:
        text = QStringLiteral("Fastboot 模式");
        color = theme.color(ColorToken::LogWarning);
        break;
    case Mode::Edl:
        text = QStringLiteral("EDL 模式 (9008)");
        color = theme.color(ColorToken::LogError);
        break;
    case Mode::None:
    default:
        text = QStringLiteral("未连接");
        color = theme.color(ColorToken::TextMuted);
        break;
    }
    m_modeLabel->setText(text);

    QPalette pal = m_modeLabel->palette();
    pal.setColor(QPalette::WindowText, color);
    m_modeLabel->setPalette(pal);
    QPalette dotPal = m_dot->palette();
    dotPal.setColor(QPalette::WindowText, color);
    m_dot->setPalette(dotPal);
}

void DeviceHomePage::clearInfo()
{
    for (const QString& key : m_infoKeys) {
        if (QLabel* value = m_infoValues.value(key)) value->setText(QStringLiteral("-"));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  快捷操作（线程池执行；日志三明治：Section 起头 → Footer 收尾）
// ─────────────────────────────────────────────────────────────────────────────

void DeviceHomePage::runShellAsync(const QString& program, const QStringList& args,
                                   const QString& section, const QString& doneHint)
{
    logView()->appendSection(section);
    QPointer<DeviceHomePage> self(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, self, program, args, doneHint] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        const QString output = runCommand(program, args, 15000);
        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        runOnUi([this, self, output, doneHint, seconds] {
            if (!output.trimmed().isEmpty()) logInfo(output.trimmed());
            logInfo(doneHint);
            success(doneHint);
            logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
        });
    }));
}

void DeviceHomePage::runAdbReboot(const QString& args)
{
    // 与 C# RunReboot 同口径：一次重启就是一次操作，同步返回即结束。
    runShellAsync(QStringLiteral("adb"), args.split(QLatin1Char(' ')),
                  QStringLiteral("重启设备 (adb %1)").arg(args),
                  QStringLiteral("已发送: adb %1").arg(args));
}

void DeviceHomePage::runFastbootOp(const QString& args)
{
    runShellAsync(QStringLiteral("fastboot"), args.split(QLatin1Char(' ')),
                  QStringLiteral("Fastboot 操作 (fastboot %1)").arg(args),
                  QStringLiteral("已发送: fastboot %1").arg(args));
}

void DeviceHomePage::runDeviceOp(DeviceOp op)
{
    // 免 BL 进 EDL 是两段式（fastboot 先 oem edl 再 reboot edl），已有实现，复用。
    if (op == DeviceOp::EnterEdl) {
        enterEdl();
        return;
    }

    const bool adbMode = (m_mode == Mode::Adb || m_mode == Mode::Recovery);
    const bool fastbootMode = (m_mode == Mode::Fastboot);

    // EDL(9008) 下这两条通道都不通 —— 说清楚，别让用户以为是按钮坏了。
    if (m_mode == Mode::Edl) {
        logWarning(QStringLiteral("[主页] 设备处于 EDL(9008) 模式，快捷操作不可用"));
        error(QStringLiteral("设备已经在 EDL(9008) 模式了，请到「刷机」组用 Qualcomm 页进行刷写。"));
        return;
    }

    // 只有 Fastboot 通道才有的两个动作。
    if (op == DeviceOp::UnlockBl || op == DeviceOp::BootState) {
        if (!fastbootMode) {
            const QString where = (m_mode == Mode::None)
                                      ? QStringLiteral("未检测到设备")
                                      : QStringLiteral("当前不是 Fastboot 模式");
            logWarning(QStringLiteral("[主页] %1，该操作仅 Fastboot 支持").arg(where));
            error(QStringLiteral("%1。解锁 BL / 读取引导状态只能在 Fastboot 模式下执行，"
                                 "请先点「重启Bootloader」。").arg(where));
            return;
        }
        if (op == DeviceOp::BootState) {
            queryBootState();
        } else {
            runFastbootOp(QStringLiteral("oem unlock"));
        }
        return;
    }

    // 同一个动作在两条通道里的命令行（各自 CLI 的公开命令）。
    QString adbArgs;
    QString fbArgs;
    switch (op) {
    case DeviceOp::Reboot:
        adbArgs = QStringLiteral("reboot");
        fbArgs = QStringLiteral("reboot");
        break;
    case DeviceOp::RebootRecovery:
        adbArgs = QStringLiteral("reboot recovery");
        fbArgs = QStringLiteral("reboot recovery");
        break;
    case DeviceOp::RebootBootloader:
        adbArgs = QStringLiteral("reboot bootloader");
        fbArgs = QStringLiteral("reboot-bootloader");   // fastboot 用连字符形式
        break;
    case DeviceOp::RebootEdl:
        adbArgs = QStringLiteral("reboot edl");
        fbArgs = QStringLiteral("reboot-edl");
        break;
    case DeviceOp::RebootFastbootD:
        adbArgs = QStringLiteral("reboot fastboot");
        fbArgs = QStringLiteral("reboot fastboot");     // bootloader → fastbootd
        break;
    case DeviceOp::UnlockBl:
    case DeviceOp::BootState:
    case DeviceOp::EnterEdl:
        return;   // 上面已处理
    }

    if (fastbootMode) {
        runFastbootOp(fbArgs);
    } else {
        // ADB / Recovery / 未检测到（未检测到时沿用历史口径：照样发 adb，
        // 失败信息会自己落到日志里，比点不动更容易排查）。
        Q_UNUSED(adbMode);
        runAdbReboot(adbArgs);
    }
}

void DeviceHomePage::queryBootState()
{
    // 合规：只读设备级引导状态（fastboot getvar unlocked），不取任何唯一标识。
    logView()->appendSection(QStringLiteral("引导状态"));
    QPointer<DeviceHomePage> self(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, self] {
        const QString unlocked = runCommand(QStringLiteral("fastboot"),
                                            { QStringLiteral("getvar"), QStringLiteral("unlocked") });
        runOnUi([this, self, unlocked] {
            if (unlocked.contains(QStringLiteral("yes"), Qt::CaseInsensitive)) {
                logInfo(QStringLiteral("[Fastboot] Bootloader 状态: 已解锁"));
            } else if (!unlocked.isEmpty()) {
                logInfo(QStringLiteral("[Fastboot] Bootloader 状态: 已锁定"));
            } else {
                logWarning(QStringLiteral("[Fastboot] 未检测到 fastboot 设备"));
            }
        });
    }));
}

void DeviceHomePage::enterEdl()
{
    // 对齐 C# BtnEnterEdl_Click：只用公开文档里的两条命令。
    // 不实现「读 token 免解 BL」——那是绕过设备鉴权（厂商密钥签名），本工具不做。
    QPointer<DeviceHomePage> self(this);
    if (m_mode == Mode::Adb || m_mode == Mode::Recovery) {
        runShellAsync(QStringLiteral("adb"), { QStringLiteral("reboot"), QStringLiteral("edl") },
                      QStringLiteral("进入 EDL (adb reboot edl)"),
                      QStringLiteral("已发送 adb reboot edl，请查看设备管理器是否出现 9008 端口"));
        return;
    }
    if (m_mode == Mode::Fastboot) {
        logView()->appendSection(QStringLiteral("进入 EDL (fastboot)"));
        QThreadPool::globalInstance()->start(new LambdaTask([this, self] {
            const qint64 started = QDateTime::currentMSecsSinceEpoch();
            const QString oemOut = runCommand(QStringLiteral("fastboot"),
                                              { QStringLiteral("oem"), QStringLiteral("edl") }, 15000);
            runOnUi([this, self, oemOut] {
                if (!oemOut.trimmed().isEmpty()) logInfo(oemOut.trimmed());
                logInfo(QStringLiteral("已发送: fastboot oem edl"));
            });
            const QString rebootOut = runCommand(QStringLiteral("fastboot"),
                                                 { QStringLiteral("reboot"), QStringLiteral("edl") }, 15000);
            const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
            runOnUi([this, self, rebootOut, seconds] {
                if (!rebootOut.trimmed().isEmpty()) logInfo(rebootOut.trimmed());
                logInfo(QStringLiteral("已发送: fastboot reboot edl"));
                success(QStringLiteral("已依次尝试 fastboot oem edl / reboot edl。若设备管理器没有出现 9008 端口，"
                                       "说明该机型已封锁命令，需深度刷机线或短接测试点。"));
                logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
            });
        }));
        return;
    }
    logWarning(QStringLiteral("[主页] 未检测到在线设备，无法进入 EDL"));
    error(QStringLiteral("没有检测到 ADB / Fastboot 设备，请先连接手机"));
}

void DeviceHomePage::openDeviceManager()
{
    // devmgmt.msc 是 mmc 管理单元，必须走 Shell 执行（QProcess::startDetached 走
    // ShellExecute 语义由 QDesktopServices 提供，等价 C# 的 UseShellExecute = true）。
    if (QDesktopServices::openUrl(QUrl(QStringLiteral("file:///C:/Windows/System32/devmgmt.msc")))) {
        logInfo(QStringLiteral("[系统] 已打开设备管理器"));
        success(QStringLiteral("已打开设备管理器"));
    } else {
        const QString message = QStringLiteral("打开设备管理器失败：可手动运行 devmgmt.msc");
        logError(QStringLiteral("[系统] %1").arg(message));
        error(message);
    }
}

void DeviceHomePage::toggleAutoRefresh()
{
    if (m_autoRefreshOn) {
        m_autoRefresh->stop();
        m_autoRefreshButton->setText(QStringLiteral("开启自动刷新"));
        m_autoRefreshOn = false;
        logInfo(QStringLiteral("[主页] 自动刷新已关闭"));
    } else {
        m_autoRefresh->start();
        m_autoRefreshButton->setText(QStringLiteral("关闭自动刷新"));
        m_autoRefreshOn = true;
        logInfo(QStringLiteral("[主页] 自动刷新已开启 (5秒间隔)"));
    }
}

} // namespace kc::ui
