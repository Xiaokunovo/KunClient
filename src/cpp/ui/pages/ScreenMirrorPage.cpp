#include "ui/pages/ScreenMirrorPage.h"

#include "kc/auth/MembershipService.h"
#include "kc/config/AppStatePaths.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QStandardPaths>
#include <QStyle>
#include <QThread>
#include <QThreadPool>
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

// 递归找 scrcpy.exe（C# Directory.GetFiles(..., AllDirectories) 等价）。
QString findScrcpyExe(const QString& dir)
{
    if (!QDir(dir).exists()) return {};
    QDirIterator it(dir, { QStringLiteral("scrcpy.exe") }, QDir::Files,
                    QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

// C# StartScrcpyAsync 里 taskkill 不可用（process.Kill 只杀自己），
// 这里按任务要求杀整棵进程树：scrcpy 会带起 adb 子进程，只杀根会留尾巴。
void killProcessTree(qint64 pid)
{
    if (pid <= 0) return;
    QProcess::startDetached(QStringLiteral("taskkill"),
                            { QStringLiteral("/PID"), QString::number(pid),
                              QStringLiteral("/T"), QStringLiteral("/F") });
}

/// 状态值着色：复用 QSS 里 `QLabel#statusValue[linkState="ok"/"failed"]` 那两条规则
/// （外壳状态栏也是这套口径，视觉上统一）。tone 传空串 = 回到普通灰。
/// 🔴 属性驱动的选择器改完必须 unpolish/polish，否则不重绘（项目既有约定）。
void setTone(QLabel* label, const QString& tone)
{
    if (!label) return;
    label->setProperty("linkState", tone);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

/// 纯参数拼装（与 C# BuildArguments 逐条对应；scrcpy 2.0+ 的选项名）。
/// OTG+无线 的告警由调用方补（这里是纯函数，不碰日志）。
QStringList buildScrcpyArgs(const MirrorSettings& s,
                            const QString& crop, const QString& recordPath,
                            bool fullscreen)
{
    QStringList args;

    if (s.resolution != QStringLiteral("原始分辨率")) {
        const QStringList parts = s.resolution.split(QLatin1Char('x'));
        if (parts.size() == 2)
            args << QStringLiteral("--max-size") << parts.first();
    }

    if (!s.bitrate.isEmpty())
        args << QStringLiteral("--video-bit-rate") << s.bitrate;

    if (!s.fps.isEmpty())
        args << QStringLiteral("--max-fps") << s.fps;

    if (s.audio) args << QStringLiteral("--audio-codec=aac");
    else args << QStringLiteral("--no-audio");

    if (s.stayAwake) args << QStringLiteral("--stay-awake");

    if (!crop.isEmpty()) args << (QStringLiteral("--crop=") + crop);

    if (!recordPath.isEmpty()) {
        args << QStringLiteral("--record") << recordPath;
    } else if (fullscreen) {
        args << QStringLiteral("--fullscreen");
    }

    if (s.mode == QStringLiteral("usb")) {
        if (s.otg) {
            args << QStringLiteral("--otg");
        } else if (recordPath.isEmpty()) {
            args << QStringLiteral("--turn-screen-off");
        }
    } else if (s.mode == QStringLiteral("wifi")) {
        const QString ip = s.ip.trimmed();
        if (!ip.isEmpty() && ip != QStringLiteral("192.168.")) {
            // scrcpy 2.0 起不接受「位置参数 = IP:端口」，必须用 -s/--serial。
            const QString host = ip.contains(QLatin1Char(':')) ? ip : ip + QStringLiteral(":5555");
            args << QStringLiteral("-s") << host;
        }
    }
    return args;
}

} // namespace

ScreenMirrorPage::ScreenMirrorPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("屏幕镜像"));
    buildUi();
}

void ScreenMirrorPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 2026-10-12：本页此前是 5 行控件的平铺流 —— 运行状态、参数、四个开关、
    // 启动按钮、无线 IP、镜像快捷键之间完全没有边界（「下载 scrcpy」还和四个
    // 复选框挤在同一行）。现在按功能收进 4 张卡：设备与 scrcpy / 投屏参数 /
    // 投屏控制 / 镜像快捷操作。
    //
    // 2026-10-18（用户反馈「投屏用户使用不舒服」）三处重排：
    //   ① 参数行原来是「键: 值」三组水平平铺、下拉全部 addWidget(combo, 1) 拉满，
    //      1280 宽下变成三个超长空白框 —— 改 2×2 网格，两列等宽、下拉只占半行；
    //   ② 控制卡原来 5 个控件挤一行，「设备IP + 连接」被推到最右和主按钮割裂
    //      —— 拆成「主操作」「无线连接」两行，IP 和连接按钮成组；
    //   ③ scrcpy 的绝对路径直接铺在状态行里（C:/Users/Administrator/… 60+ 字符），
    //      把整行挤变形 —— 状态行只留「已就绪 / 不可用」，路径进 tooltip。
    auto addCard = [&](const QString& title) -> QVBoxLayout* {
        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("card"));
        card->setFrameShape(QFrame::StyledPanel);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 12, 14, 12);
        cardLayout->setSpacing(8);

        auto* header = new QLabel(title, card);
        header->setObjectName(QStringLiteral("groupHeader"));
        cardLayout->addWidget(header);

        auto* body = new QVBoxLayout;
        body->setSpacing(8);
        cardLayout->addLayout(body);
        layout->addWidget(card);
        return body;
    };

    // ── 卡 1：设备与 scrcpy 环境（状态 + 就绪情况 + 下载入口）────────────
    // 状态行只放「读数」，不放路径：绝对路径 60+ 字符会把整行挤变形，
    // 完整路径挂到 tooltip（要复制路径的话右键状态值即可）。
    QVBoxLayout* envCard = addCard(QStringLiteral("设备与 scrcpy"));
    auto makeKey = [this](const QString& text) {
        auto* key = new QLabel(text, this);
        key->setObjectName(QStringLiteral("statusLabel"));
        return key;
    };

    auto* stateRow = new QHBoxLayout;
    stateRow->setSpacing(8);
    stateRow->addWidget(makeKey(QStringLiteral("投屏状态")));
    m_stateLabel = new QLabel(QStringLiteral("未运行"), this);
    m_stateLabel->setObjectName(QStringLiteral("statusValue"));
    QFont stateFont = m_stateLabel->font();
    stateFont.setBold(true);
    m_stateLabel->setFont(stateFont);
    stateRow->addWidget(m_stateLabel);
    stateRow->addSpacing(16);
    stateRow->addWidget(makeKey(QStringLiteral("scrcpy")));
    m_scrcpyLabel = new QLabel(QStringLiteral("检测中"), this);
    m_scrcpyLabel->setObjectName(QStringLiteral("statusValue"));
    stateRow->addWidget(m_scrcpyLabel);
    stateRow->addStretch();

    // 「下载 scrcpy」原来跟 4 个复选框在同一行（和「录制音频」这种参数混在一起），
    // 它其实是 scrcpy 环境的兜底安装入口 —— 归到状态行才对。
    // 就绪后按钮隐藏（常态下这行只剩两个读数，最干净）；失败时改成「重试下载」。
    m_downloadButton = new QPushButton(QStringLiteral("下载 scrcpy"), this);
    m_downloadButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_downloadButton);
    connect(m_downloadButton, &QPushButton::clicked, this, [this] { ensureScrcpy(); });
    stateRow->addWidget(m_downloadButton);
    envCard->addLayout(stateRow);

    // ── 卡 2：投屏参数（画质三项 + 行为四项）────────────────────────────
    QVBoxLayout* paramCard = addCard(QStringLiteral("投屏参数"));

    auto* qualityCaption = new QLabel(QStringLiteral("画质"), this);
    qualityCaption->setObjectName(QStringLiteral("statusLabel"));
    paramCard->addWidget(qualityCaption);

    // 三项排一行：左「分辨率」中「比特率」右「帧率」，各自自然宽度，
    // 剩余宽度交给末尾空列吃掉。原来三组水平平铺 + addWidget(combo, 1)，
    // 1280 宽下每个下拉都被拉到 ~350px，读起来像三个空白输入框。
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setContentsMargins(0, 0, 0, 0);
    auto addField = [&](int col, const QString& label, QComboBox** combo,
                        const QStringList& items) {
        auto* key = makeKey(label);
        *combo = new QComboBox(this);
        (*combo)->addItems(items);
        // 上下限都卡住：内容最长的「原始分辨率」约 110px，给到 150 就够；
        // 再宽就是空白，会把「比特率 / 帧率」的标签推远，读起来断成三段。
        (*combo)->setMinimumWidth(150);
        (*combo)->setMaximumWidth(220);
        (*combo)->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        grid->addWidget(key, 0, col * 2);
        grid->addWidget(*combo, 0, col * 2 + 1);
    };
    addField(0, QStringLiteral("分辨率"), &m_resolution,
             { QStringLiteral("原始分辨率"), QStringLiteral("1920x1080"),
               QStringLiteral("1280x720"), QStringLiteral("800x480") });
    addField(1, QStringLiteral("比特率"), &m_bitrate,
             { QStringLiteral("8M"), QStringLiteral("4M"),
               QStringLiteral("16M"), QStringLiteral("32M") });
    addField(2, QStringLiteral("帧率"), &m_fps,
             { QStringLiteral("60"), QStringLiteral("30"), QStringLiteral("25") });
    // 末尾空列（第 6 列）吃掉剩余宽度：三组「标签 + 下拉」保持自然尺寸、靠左排。
    grid->setColumnStretch(6, 1);
    paramCard->addLayout(grid);

    auto* behaviorCaption = new QLabel(QStringLiteral("行为"), this);
    behaviorCaption->setObjectName(QStringLiteral("statusLabel"));
    paramCard->addWidget(behaviorCaption);

    auto* checkRow = new QHBoxLayout;
    checkRow->setSpacing(14);
    m_audio = new QCheckBox(QStringLiteral("录制音频"), this);
    m_audio->setChecked(true);
    m_crop = new QCheckBox(QStringLiteral("裁剪屏幕"), this);
    m_crop->setToolTip(QStringLiteral("按设备实际分辨率与状态栏/导航栏高度自动算 --crop，去掉黑边"));
    m_stayAwake = new QCheckBox(QStringLiteral("保持唤醒"), this);
    m_stayAwake->setChecked(true);
    m_otg = new QCheckBox(QStringLiteral("OTG控制"), this);
    m_otg->setToolTip(QStringLiteral("仅 USB 连接有效；无线模式下会被忽略"));
    for (QCheckBox* c : { m_audio, m_crop, m_stayAwake, m_otg }) checkRow->addWidget(c);
    checkRow->addStretch();
    paramCard->addLayout(checkRow);

    // ── 卡 3：投屏控制（主操作一行 / 无线连接一行）───────────────────────
    QVBoxLayout* controlCard = addCard(QStringLiteral("投屏控制"));

    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);
    auto* usbButton = new QPushButton(QStringLiteral("USB 投屏"), this);
    usbButton->setObjectName(QStringLiteral("primaryButton"));
    usbButton->setToolTip(QStringLiteral("通过 USB 数据线投屏（启动时自动关闭设备屏幕）"));
    auto* wifiButton = new QPushButton(QStringLiteral("无线投屏"), this);
    wifiButton->setObjectName(QStringLiteral("primaryButton"));
    wifiButton->setToolTip(QStringLiteral("需要先在下方填 IP 并点「连接」"));
    for (QPushButton* b : { usbButton, wifiButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        actionRow->addWidget(b);
    }
    // 只在「新开一次投屏」时扣额度；截图/录屏等是对已在跑会话的复用，
    // 所以校验放这两个按钮上，不放进 startScrcpy（对齐 C# 注释口径）。
    connect(usbButton, &QPushButton::clicked, this, [this] {
        if (!kc::MembershipService::guardAndConsume(QStringLiteral("屏幕镜像"),
                                                    QStringLiteral("mirror.scrcpy")))
            return;
        startScrcpy(QStringLiteral("usb"), {}, false);
    });
    connect(wifiButton, &QPushButton::clicked, this, [this] {
        // 🔴 IP 空着直接开跑等于没指定目标：buildScrcpyArgs 在 wifi 分支里
        //    「ip 为空就不加 -s」，scrcpy 会退回去抓**第一台 USB 设备** ——
        //    用户以为在无线投屏，其实投的是插着线的那台，或者干脆报「no devices」。
        //    先拦住并聚焦到输入框，比让它跑一遍再报错清楚。
        if (m_deviceIp->text().trimmed().isEmpty()) {
            error(QStringLiteral("请先填写设备 IP：手机「无线调试」页里可以查看"), 5000);
            m_deviceIp->setFocus();
            return;
        }
        if (!kc::MembershipService::guardAndConsume(QStringLiteral("屏幕镜像"),
                                                    QStringLiteral("mirror.scrcpy")))
            return;
        startScrcpy(QStringLiteral("wifi"), {}, false);
    });

    auto* stopButton = new QPushButton(QStringLiteral("关闭镜像"), this);
    stopButton->setCursor(Qt::PointingHandCursor);
    connect(stopButton, &QPushButton::clicked, this, [this] { stopScrcpy(); });
    actionRow->addWidget(stopButton);
    actionRow->addStretch();
    controlCard->addLayout(actionRow);

    // 「设备IP + 连接」原来被推到同一行的最右端，和 USB/无线/关闭三个主按钮
    // 混在一起分不清谁管谁。单独一行 + 左侧「无线连接」标签 + 右侧用法提示。
    auto* wifiRow = new QHBoxLayout;
    wifiRow->setSpacing(8);
    wifiRow->addWidget(makeKey(QStringLiteral("无线连接")));
    m_deviceIp = new QLineEdit(this);
    m_deviceIp->setPlaceholderText(QStringLiteral("设备 IP，如 192.168.1.20"));
    m_deviceIp->setClearButtonEnabled(true);
    m_deviceIp->setFixedWidth(180);
    wifiRow->addWidget(m_deviceIp);
    auto* connectButton = new QPushButton(QStringLiteral("连接"), this);
    connectButton->setCursor(Qt::PointingHandCursor);
    connectButton->setToolTip(QStringLiteral("执行 adb connect <IP>:5555"));
    m_busyButtons.append(connectButton);
    connect(connectButton, &QPushButton::clicked, this, [this] { connectWifi(); });
    wifiRow->addWidget(connectButton);
    auto* wifiHint = new QLabel(QStringLiteral("手机上先开启「无线调试」并查看 IP"), this);
    wifiHint->setObjectName(QStringLiteral("statusLabel"));
    wifiRow->addSpacing(6);
    wifiRow->addWidget(wifiHint);
    wifiRow->addStretch();
    controlCard->addLayout(wifiRow);

    // ── 卡 4：镜像快捷操作 ─────────────────────────────────────────────
    QVBoxLayout* quickCard = addCard(QStringLiteral("镜像快捷操作"));
    auto* quickRow = new QHBoxLayout;
    quickRow->setSpacing(8);
    struct Quick {
        QString label;
        std::function<void()> handler;
        /// 组内序号：0~2 是「画面」类，3+ 是「导航」类，中间插一段间距。
        int group = 0;
    };
    const QVector<Quick> quicks = {
        { QStringLiteral("截图"), [this] { screenshot(); }, 0 },
        { QStringLiteral("录屏"), [this] {
             const QString path = QFileDialog::getSaveFileName(
                 this, QStringLiteral("保存录屏文件"),
                 QStringLiteral("scrcpy_%1.mp4")
                     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
                 QStringLiteral("MP4 视频 (*.mp4);;MKV 视频 (*.mkv)"));
             if (path.isEmpty()) return;
             logInfo(QStringLiteral("[投屏] 开始录制到 %1").arg(path));
             startScrcpy(m_lastMode, path, false);
         }, 0 },
        { QStringLiteral("全屏"), [this] { startScrcpy(m_lastMode, {}, true); }, 0 },
        { QStringLiteral("息屏投屏"), [this] {
             if (m_mirroring.loadAcquire() == 0) {
                 notify(QStringLiteral("投屏未启动：USB 投屏启动时会自动关闭设备屏幕"),
                        static_cast<int>(NotifyLevel::Info));
                 logInfo(QStringLiteral("[投屏] 息屏投屏：USB 模式启动 scrcpy 时会自动执行 --turn-screen-off"));
                 return;
             }
             runKeyevent(26, QStringLiteral("息屏"));
         }, 1 },
        { QStringLiteral("返回"), [this] { runKeyevent(4, QStringLiteral("返回")); }, 1 },
        { QStringLiteral("Home"), [this] { runKeyevent(3, QStringLiteral("Home")); }, 1 },
        { QStringLiteral("最近任务"), [this] { runKeyevent(187, QStringLiteral("最近任务")); }, 1 },
    };
    int lastGroup = 0;
    for (const Quick& q : quicks) {
        // 画面类（截图/录屏/全屏）与导航类（息屏/返回/Home/最近任务）之间留白：
        // 七个同款按钮平铺成一条，扫视时找不到分界。
        if (q.group != lastGroup) {
            quickRow->addSpacing(14);
            lastGroup = q.group;
        }
        auto* b = new QPushButton(q.label, this);
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        connect(b, &QPushButton::clicked, this, [this, handler = q.handler] {
            if (m_busy.loadAcquire() != 0) return;
            handler();
        });
        quickRow->addWidget(b);
    }
    quickRow->addStretch();
    quickCard->addLayout(quickRow);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    log->setMinimumHeight(120);
    registerLogView(log);
    layout->addWidget(log, 1);
}

void ScreenMirrorPage::onActivated()
{
    ensureScrcpy();
}

void ScreenMirrorPage::onDeactivated()
{
    // C# Unloaded → StopScrcpy（切走页面即停，避免无主投屏挂着）。
    stopScrcpy();
}

// ─────────────────────────────────────────────────────────────────────────────
//  scrcpy 就绪（探测 → 下载 → 解压 → 探测）
// ─────────────────────────────────────────────────────────────────────────────

void ScreenMirrorPage::ensureScrcpy()
{
    if (m_isDownloading) return;
    if (!m_scrcpyPath.isEmpty() && QFile::exists(m_scrcpyPath)) {
        // 就绪：状态行只留「已就绪」，完整路径挂 tooltip —— 原来把
        // C:/Users/…/scrcpy.exe 60+ 字符直接铺在行里，把整行挤变形。
        m_scrcpyLabel->setText(QStringLiteral("已就绪"));
        m_scrcpyLabel->setToolTip(m_scrcpyPath);
        setTone(m_scrcpyLabel, QStringLiteral("ok"));
        m_downloadButton->hide();
        return;
    }
    m_isDownloading = true;
    logView()->appendSection(QStringLiteral("下载 scrcpy"));
    QThreadPool::globalInstance()->start(new LambdaTask([this] { ensureScrcpyCore(); }));
}

void ScreenMirrorPage::ensureScrcpyCore()
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    // ── 停更离线版（2.0.6）：不再从服务器下载 scrcpy，改为本地查找 ──────────
    //  服务器关停后 /api/v2/packages/scrcpy 已不可用。查找顺序：
    //    ① 程序同目录（发布包可随包自带 scrcpy/ 子目录）
    //    ② 缓存目录（老用户之前下载过的仍在）
    //  两处都没有就在界面提示用户自行放置，不静默失败。
    const QString scrcpyDir =
        kc::AppStatePaths::ResolveCache(QStringLiteral("scrcpy"), QStringLiteral("scrcpy"),
                                        QStringLiteral("scrcpy.exe"));

    QString found = findScrcpyExe(scrcpyDir);
    if (found.isEmpty()) {
        // 程序同目录（含 scrcpy/ 子目录）
        const QString appDir = QCoreApplication::applicationDirPath();
        found = findScrcpyExe(appDir);
        if (found.isEmpty()) {
            found = findScrcpyExe(QDir(appDir).filePath(QStringLiteral("scrcpy")));
        }
    }

    if (found.isEmpty()) {
        logError(QStringLiteral("[投屏] 未找到 scrcpy.exe。离线版不再自动下载，"
                                "请把 scrcpy 解压到程序目录（或程序目录下的 scrcpy 文件夹）后重试"));
    }

    const QString result = found;
    const bool ok = !result.isEmpty();

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this, result, ok] {
        m_isDownloading = false;
        if (ok) {
            m_scrcpyPath = result;
            m_scrcpyLabel->setText(QStringLiteral("已就绪"));
            m_scrcpyLabel->setToolTip(result);
            setTone(m_scrcpyLabel, QStringLiteral("ok"));
            m_downloadButton->hide();
            logSuccess(QStringLiteral("[投屏] scrcpy 已就绪: %1").arg(result));
            success(QStringLiteral("scrcpy 已就绪"));
        } else {
            m_scrcpyLabel->setText(QStringLiteral("未找到"));
            m_scrcpyLabel->setToolTip(QStringLiteral(
                "未检测到 scrcpy.exe。请把 scrcpy 解压到程序目录后点「重新检测」。"));
            setTone(m_scrcpyLabel, QStringLiteral("failed"));
            m_downloadButton->setText(QStringLiteral("重新检测"));
            m_downloadButton->show();
            error(QStringLiteral("未找到 scrcpy.exe"), 6000);
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  启动 / 停止
// ─────────────────────────────────────────────────────────────────────────────

void ScreenMirrorPage::startScrcpy(const QString& mode, const QString& recordPath, bool fullscreen)
{
    if (m_busy.loadAcquire() != 0) return;
    if (m_scrcpyPath.isEmpty() || !QFile::exists(m_scrcpyPath)) {
        error(QStringLiteral("scrcpy 未找到，请先点击「下载 scrcpy」"));
        return;
    }

    m_lastMode = mode;
    logView()->appendSection(recordPath.isEmpty()
                                 ? (mode == QStringLiteral("wifi") ? QStringLiteral("无线投屏")
                                                                   : QStringLiteral("USB 投屏"))
                                 : QStringLiteral("录制屏幕"));
    setBusy(true);

    // 设置快照（UI 线程读好，worker 只用值）。
    MirrorSettings s;
    s.mode = mode;
    s.resolution = m_resolution->currentText();
    s.bitrate = m_bitrate->currentText();
    s.fps = m_fps->currentText();
    s.audio = m_audio->isChecked();
    s.crop = m_crop->isChecked();
    s.stayAwake = m_stayAwake->isChecked();
    s.otg = m_otg->isChecked();
    s.ip = m_deviceIp->text();

    QThreadPool::globalInstance()->start(
        new LambdaTask([this, s, recordPath, fullscreen] { startScrcpyCore(s, recordPath, fullscreen); }));
}

void ScreenMirrorPage::startScrcpyCore(const MirrorSettings& s, const QString& recordPath,
                                       bool fullscreen)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    // 已存在投屏会话：重启以应用新参数（C# 同款，等 400ms 让旧进程退出）。
    if (m_mirroring.loadAcquire() != 0) {
        logInfo(QStringLiteral("[投屏] 已存在投屏会话，正在重启以应用新参数..."));
        killProcessTree(m_pid);
        QThread::msleep(400);
    }

    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logWarning(QStringLiteral("[投屏] platform-tools 未就位，scrcpy 将退回 PATH 查找 adb"));
    }

    // 裁剪参数（需要 adb 读设备分辨率 / insets）。
    QString crop;
    if (s.crop) crop = buildCropArgument(tools);

    // C#：OTG 控制仅 USB 有效，无线模式下忽略并明说。
    if (s.mode == QStringLiteral("wifi") && s.otg)
        logWarning(QStringLiteral("[投屏] OTG 控制仅支持 USB 连接，已忽略该选项"));

    const QStringList args = buildScrcpyArgs(s, crop, recordPath, fullscreen);
    logInfo(QStringLiteral("[投屏] 启动 scrcpy %1").arg(args.join(QLatin1Char(' '))));

    // 🔴 scrcpy 认 ADB 环境变量（服务端 scrcpy.zip 已剔掉重复的 adb.exe）。
    //    startDetached 无法给子进程单独传环境，所以临时改本进程环境（启动完就还原），
    //    子进程继承快照值，且不污染子进程之外的任何东西。
    const QString adbPath = tools.adbPath();
    const QByteArray oldAdb = qgetenv("ADB");
    if (!adbPath.isEmpty() && QFile::exists(adbPath)) {
        qputenv("ADB", adbPath.toUtf8());
    } else {
        logWarning(QStringLiteral("[投屏] 未找到 adb.exe（%1），scrcpy 将退回 PATH 查找 adb").arg(adbPath));
    }

    qint64 pid = 0;
    const bool ok = QProcess::startDetached(m_scrcpyPath, args,
                                            QFileInfo(m_scrcpyPath).absolutePath(), &pid);

    // 还原本进程环境（子进程已拿到继承值）。
    if (oldAdb.isEmpty()) qunsetenv("ADB");
    else qputenv("ADB", oldAdb);

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));

    runOnUi([this, ok, pid, recordPath] {
        if (ok && pid > 0) {
            m_mirroring.storeRelease(1);
            m_pid = pid;
            m_stateLabel->setText(QStringLiteral("运行中 · PID %1").arg(pid));
            setTone(m_stateLabel, QStringLiteral("ok"));
            logSuccess(QStringLiteral("[投屏] scrcpy 已启动"));
            success(recordPath.isEmpty()
                        ? QStringLiteral("投屏已启动")
                        : QStringLiteral("正在录制到 %1").arg(QFileInfo(recordPath).fileName()));
        } else {
            m_mirroring.storeRelease(0);
            m_pid = 0;
            m_stateLabel->setText(QStringLiteral("启动失败"));
            setTone(m_stateLabel, QStringLiteral("failed"));
            logError(QStringLiteral("[投屏] 启动失败"));
            error(QStringLiteral("启动失败: scrcpy 进程未能创建"));
        }
        setBusy(false);
    });
}

void ScreenMirrorPage::stopScrcpy()
{
    if (m_mirroring.loadAcquire() == 0 || m_pid <= 0) return;
    const qint64 pid = m_pid;
    m_mirroring.storeRelease(0);
    m_pid = 0;
    QThreadPool::globalInstance()->start(new LambdaTask([this, pid] { stopScrcpyCore(pid); }));
}

void ScreenMirrorPage::stopScrcpyCore(qint64 pid)
{
    killProcessTree(pid);
    runOnUi([this] {
        m_stateLabel->setText(QStringLiteral("已停止"));
        setTone(m_stateLabel, QString());
        logInfo(QStringLiteral("[投屏] 已停止"));
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  无线连接 / 快捷操作
// ─────────────────────────────────────────────────────────────────────────────

void ScreenMirrorPage::connectWifi()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString ip = m_deviceIp->text().trimmed();
    if (ip.isEmpty() || ip == QStringLiteral("192.168.")) {
        error(QStringLiteral("请输入设备IP地址"));
        return;
    }
    logView()->appendSection(QStringLiteral("连接设备 %1:5555").arg(ip));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, ip] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        if (!tools.ensureToolsExist()) {
            error(QStringLiteral("adb 不可用，请检查网络后重试"));
        } else {
            const auto r = tools.executeAdb(QStringLiteral("connect %1:5555").arg(ip), 30000);
            const QString text = (!r.output.trimmed().isEmpty() ? r.output : r.error).trimmed();
            if (r.success && text.contains(QStringLiteral("connected"), Qt::CaseInsensitive)) {
                logSuccess(QStringLiteral("[投屏] 已连接 %1:5555").arg(ip));
                success(QStringLiteral("已连接 %1").arg(ip));
            } else {
                logWarning(QStringLiteral("[投屏] 连接失败: %1").arg(text));
                error(QStringLiteral("连接失败: %1").arg(text), 6000);
            }
        }
        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
        runOnUi([this] { setBusy(false); });
    }));
}

void ScreenMirrorPage::screenshot()
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("截图"));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        if (!tools.ensureToolsExist()) {
            error(QStringLiteral("adb 不可用，请检查网络后重试"));
        } else {
            const QString savePath = QDir(
                QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                .filePath(QStringLiteral("screenshot_%1.png")
                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));

            const auto cap = tools.executeAdb(
                QStringLiteral("shell screencap -p /sdcard/kun_screenshot.png"), 30000);
            if (!cap.success) {
                logError(QStringLiteral("[投屏] 截图失败: %1").arg(cap.error.trimmed()));
                error(QStringLiteral("截图失败"), 6000);
            } else {
                const auto pull = tools.executeAdb(
                    QStringLiteral("pull /sdcard/kun_screenshot.png \"%1\"").arg(savePath), 60000);
                tools.executeAdb(QStringLiteral("shell rm -f /sdcard/kun_screenshot.png"), 15000);
                if (pull.success && QFile::exists(savePath)) {
                    logSuccess(QStringLiteral("[投屏] 截图已保存: %1").arg(savePath));
                    success(QStringLiteral("截图已保存到桌面"));
                } else {
                    logError(QStringLiteral("[投屏] 截图导出失败: %1").arg(pull.error.trimmed()));
                    error(QStringLiteral("截图导出失败"), 6000);
                }
            }
        }
        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
        runOnUi([this] { setBusy(false); });
    }));
}

void ScreenMirrorPage::runKeyevent(int keycode, const QString& desc)
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(desc);
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, keycode, desc] {
        PlatformToolsManager tools;
        if (!tools.ensureToolsExist()) {
            error(QStringLiteral("adb 不可用，请检查网络后重试"));
        } else {
            const auto r = tools.executeAdb(
                QStringLiteral("shell input keyevent %1").arg(keycode), 15000);
            if (r.success) {
                logSuccess(QStringLiteral("[投屏] %1 已发送").arg(desc));
            } else {
                logWarning(QStringLiteral("[投屏] 按键发送失败: %1").arg(r.error.trimmed()));
                error(QStringLiteral("adb 命令执行失败"));
            }
        }
        runOnUi([this] { setBusy(false); });
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
//  裁剪参数（线程池；C# BuildCropArgumentAsync 的正则原样移植）
// ─────────────────────────────────────────────────────────────────────────────

QString ScreenMirrorPage::buildCropArgument(PlatformToolsManager& tools)
{
    const auto size = tools.executeAdb(QStringLiteral("shell wm size"), 15000);
    if (!size.success) {
        logWarning(QStringLiteral("[投屏] 读取设备分辨率失败，已忽略裁剪: %1").arg(size.output.trimmed()));
        return {};
    }

    // C# Regex.Matches(..., @"(\d{3,5})x(\d{3,5})") 取最后一组。
    static const QRegularExpression sizeRe(QStringLiteral("(\\d{3,5})x(\\d{3,5})"));
    QRegularExpressionMatchIterator it = sizeRe.globalMatch(size.output);
    QRegularExpressionMatch sizeMatch;
    while (it.hasNext()) sizeMatch = it.next();
    if (!sizeMatch.hasMatch()) {
        logWarning(QStringLiteral("[投屏] 未能解析设备分辨率，已忽略裁剪"));
        return {};
    }
    bool okW = false, okH = false;
    const int width = sizeMatch.captured(1).toInt(&okW);
    const int height = sizeMatch.captured(2).toInt(&okH);
    if (!okW || !okH || width <= 0 || height <= 0) {
        logWarning(QStringLiteral("[投屏] 设备分辨率数值异常，已忽略裁剪"));
        return {};
    }

    int top = 0;
    int bottom = 0;
    const auto inset = tools.executeAdb(QStringLiteral("shell dumpsys window"), 30000);
    if (inset.success) {
        static const QRegularExpression insetRe1(
            QStringLiteral("stable[Ii]?nsets?=?\\[?Rect\\(\\(?(\\d+),\\s*(\\d+)\\s*-\\s*(\\d+),\\s*(\\d+)\\)"));
        static const QRegularExpression insetRe2(
            QStringLiteral("mStableInsets=Rect\\((\\d+),\\s*(\\d+)\\s*-\\s*(\\d+),\\s*(\\d+)\\)"));
        QRegularExpressionMatch m = insetRe1.match(inset.output);
        if (!m.hasMatch()) m = insetRe2.match(inset.output);
        if (m.hasMatch()) {
            top = m.captured(2).toInt();
            bottom = m.captured(4).toInt();
        }
    }

    if (top <= 0 && bottom <= 0) {
        logWarning(QStringLiteral("[投屏] 未能识别状态栏 / 导航栏高度，已忽略裁剪"));
        return {};
    }

    const int cropHeight = height - top - bottom;
    if (cropHeight <= 0 || cropHeight > height) {
        logWarning(QStringLiteral("[投屏] 计算出的裁剪区域无效，已忽略裁剪"));
        return {};
    }

    const QString crop = QStringLiteral("%1:%2:0:%3").arg(width).arg(cropHeight).arg(top);
    logInfo(QStringLiteral("[投屏] 裁剪区域: %1（状态栏 %2px，导航栏 %3px）").arg(crop).arg(top).arg(bottom));
    return crop;
}

void ScreenMirrorPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
}

} // namespace kc::ui
