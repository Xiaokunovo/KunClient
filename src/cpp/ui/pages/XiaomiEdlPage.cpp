#include "ui/pages/XiaomiEdlPage.h"

#include "kc/auth/SignCodec.h"
#include "kc/config/AppSettings.h"
#include "kc/devices/DeviceWatcher.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/dialogs/FdlAuthDialog.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QByteArray>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
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

/// 工作目录（blob.raw 落这里，便于排查）。临时签名文件放 %TEMP%\KunClient（退出自动清）。
QString workDir() { return QDir::temp().filePath(QStringLiteral("KunClient/edl")); }
QString tempSignPath() { return QDir::temp().filePath(QStringLiteral("KunClient/sign.bin")); }

constexpr int kEdlCmdTimeoutMs = 15000;   // oem edl 会让设备当场断 USB，超时是常态
constexpr int kEdlWaitMs = 30000;         // 9008 重枚举 + 驱动加载一般 5~20 秒

/// fastboot 的 getvar 回包形如 "(bootloader) token: xxxx"，取冒号后面的值。
QString grepVar(const QString& output, const QString& key)
{
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        const int idx = line.indexOf(key + QStringLiteral(":"));
        if (idx < 0) continue;
        return line.mid(idx + key.size() + 1).trimmed();
    }
    return QString();
}

/// 清掉十六进制里的空格 / 冒号 / 连字符 / 0x 前缀（口径统一在 kc::auth::SignCodec）。
QString cleanHex(QString text) { return kc::auth::cleanHex(text); }

/// sig/blob 解析：十六进制（可带空格/冒号/0x 前缀）或 Base64 → 字节；都不认识返回空。
QByteArray tryParseBytes(const QString& text) { return kc::auth::tryParseBytes(text); }

/// 从读 blob 的回包里抠出 blob 本体（token → blob → 最长一段 hex → 原文，逐级退让）。
QString extractBlob(const QString& output)
{
    QString v = grepVar(output, QStringLiteral("token"));
    if (v.isEmpty()) v = grepVar(output, QStringLiteral("blob"));
    if (!v.isEmpty()) return v;

    QString best;
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('<'))) continue;
        const int colon = line.lastIndexOf(QLatin1Char(':'));
        const QString hex = cleanHex(colon >= 0 ? line.mid(colon + 1) : line);
        if (hex.size() >= 32 && hex.size() > best.size()) best = hex;
    }
    return best.size() >= 32 ? best : output.trimmed();
}

/// 取第一条有意义的回包行（跳过空行与 $ 开头）。
QString firstMeaningfulLine(const QString& output)
{
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1Char('$'))) return line;
    }
    return QStringLiteral("（无输出）");
}

} // namespace

XiaomiEdlPage::XiaomiEdlPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("FDL"));

    const auto& s = AppSettings::current();
    m_readCmd = s.xiaomiEdlReadCommand.trimmed().isEmpty()
                    ? QStringLiteral("getvar token") : s.xiaomiEdlReadCommand.trimmed();
    m_writeCmd = s.xiaomiEdlWriteCommand.trimmed().isEmpty()
                     ? QStringLiteral("stage") : s.xiaomiEdlWriteCommand.trimmed();
    m_edlCmd = s.xiaomiEdlEdlCommand.trimmed().isEmpty()
                   ? QStringLiteral("oem edl") : s.xiaomiEdlEdlCommand.trimmed();

    buildUi();
    logInfo(QStringLiteral("流程：%1 → （求 sig）→ %2 → %3；stage 的文件 = 仅 sig（不含 blob）")
                .arg(m_readCmd, m_writeCmd, m_edlCmd));
}

void XiaomiEdlPage::buildUi()
{
    // 2026-10-17：本页是「裸行布局」（没有 #card），控件直接浮在壁纸上 ——
    // 铺一张与卡片同材质的整页玻璃板，用户口径「这页没实现液态玻璃」即指此处。
    setGlassSheet(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // ── 设备读数行 + 动作按钮 ────────────────────────────────────────────
    auto* deviceRow = new QHBoxLayout;
    deviceRow->setSpacing(10);

    auto makeReading = [](const QString& label, QLabel** value) {
        auto* box = new QWidget;
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto* key = new QLabel(label, box);
        key->setObjectName(QStringLiteral("statusLabel"));
        *value = new QLabel(QStringLiteral("未检测"), box);
        (*value)->setObjectName(QStringLiteral("statusValue"));
        row->addWidget(key);
        row->addWidget(*value);
        return box;
    };
    deviceRow->addWidget(makeReading(QStringLiteral("Fastboot/9008:"), &m_deviceValue));
    deviceRow->addWidget(makeReading(QStringLiteral("机型 / BL:"), &m_productValue));
    deviceRow->addStretch();

    m_runButton = new QPushButton(QStringLiteral("一键进入 EDL"), this);
    m_runButton->setObjectName(QStringLiteral("primaryButton"));
    m_refreshButton = new QPushButton(QStringLiteral("刷新设备"), this);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setEnabled(false);
    for (QPushButton* b : { m_runButton, m_refreshButton, m_stopButton }) {
        b->setCursor(Qt::PointingHandCursor);
        deviceRow->addWidget(b);
    }
    m_busyButtons = { m_runButton, m_refreshButton };
    connect(m_runButton, &QPushButton::clicked, this, [this] { runFlow(); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refreshDevice(); });
    connect(m_stopButton, &QPushButton::clicked, this, [this] { stopFlow(); });
    layout->addLayout(deviceRow);

    // 卡密行 / 页内 sig 行已删除（2026-10-17）：签名来源统一收到「FDL Auth」弹窗里，
    // 原 token 与 sig 并排展示，用户核对完再写入。

    // ── 当前步骤 + 结果框 ────────────────────────────────────────────────
    m_stepLabel = new QLabel(QStringLiteral("就绪"), this);
    m_stepLabel->setObjectName(QStringLiteral("deviceModeLabel"));
    QFont stepFont = m_stepLabel->font();
    stepFont.setBold(true);
    m_stepLabel->setFont(stepFont);
    layout->addWidget(m_stepLabel);

    m_resultBox = new QWidget(this);
    auto* resultLayout = new QHBoxLayout(m_resultBox);
    resultLayout->setContentsMargins(12, 10, 12, 10);
    resultLayout->setSpacing(10);
    m_resultIcon = new QLabel(m_resultBox);
    QFont iconFont = m_resultIcon->font();
    iconFont.setPointSize(24);
    iconFont.setBold(true);
    m_resultIcon->setFont(iconFont);
    auto* textColumn = new QVBoxLayout;
    textColumn->setSpacing(4);
    m_resultTitle = new QLabel(m_resultBox);
    m_resultTitle->setWordWrap(true);
    QFont titleFont = m_resultTitle->font();
    titleFont.setBold(true);
    m_resultTitle->setFont(titleFont);
    m_resultSub = new QLabel(m_resultBox);
    m_resultSub->setObjectName(QStringLiteral("statusLabel"));
    m_resultSub->setWordWrap(true);
    textColumn->addWidget(m_resultTitle);
    textColumn->addWidget(m_resultSub);
    resultLayout->addWidget(m_resultIcon);
    resultLayout->addLayout(textColumn, 1);
    m_resultBox->hide();
    layout->addWidget(m_resultBox);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);
}

void XiaomiEdlPage::onActivated()
{
    refreshDevice();
}

void XiaomiEdlPage::startFlow()
{
    runFlow();
}

// ─────────────────────────────────────────────────────────────────────────────
//  忙碌闸门
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiEdlPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
    m_stopButton->setEnabled(busy);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UI 动作
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiEdlPage::refreshDevice()
{
    if (m_busy.loadAcquire() != 0) {
        logWarning(QStringLiteral("一键流程进行中，暂不能刷新设备"));
        return;
    }
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this] { refreshDeviceCore(); }));
}

void XiaomiEdlPage::refreshDeviceCore()
{
    PlatformToolsManager tools(
        [this](const QString& msg) { logInfo(QStringLiteral("· %1").arg(msg)); });

    bool ready = tools.checkToolsExist();
    if (!ready) ready = tools.ensureToolsExist();

    QString deviceText = QStringLiteral("未检测到 Fastboot 设备");
    QString productText = QStringLiteral("-");
    if (ready) {
        const auto r = tools.executeFastboot(QStringLiteral("devices"), 15000);
        const QString out = r.output + QStringLiteral("\n") + r.error;
        QString serial;
        for (const QString& raw : out.split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.endsWith(QStringLiteral("fastboot"))) {
                serial = line.section(QLatin1Char(' '), 0, 0);
                break;
            }
        }
        if (!serial.isEmpty()) {
            deviceText = serial + QStringLiteral("  (fastboot)");
            const auto product = tools.executeFastboot(QStringLiteral("getvar product"), 10000);
            const auto unlocked = tools.executeFastboot(QStringLiteral("getvar unlocked"), 10000);
            const QString name = grepVar(product.output + QStringLiteral("\n") + product.error,
                                         QStringLiteral("product"));
            const QString lockState = grepVar(unlocked.output + QStringLiteral("\n") + unlocked.error,
                                              QStringLiteral("unlocked"));
            const QString lockText = lockState.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0
                                         ? QStringLiteral("BL 已解锁")
                                         : lockState.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0
                                               ? QStringLiteral("BL 已锁定")
                                               : QStringLiteral("未知");
            productText = QStringLiteral("%1 · %2").arg(name.isEmpty() ? QStringLiteral("-") : name, lockText);
            logInfo(QStringLiteral("Fastboot 设备：%1 · %2 · %3").arg(serial, name, lockText));
        } else {
            logWarning(QStringLiteral("没有检测到 Fastboot 设备，请把手机进 Fastboot 模式后重试"));
        }
    } else {
        logError(QStringLiteral("fastboot 获取失败，请检查网络后点「刷新设备」重试"));
    }

    runOnUi([this, deviceText, productText] {
        m_deviceValue->setText(deviceText);
        m_productValue->setText(productText);
        setBusy(false);
    });
}

void XiaomiEdlPage::stopFlow()
{
    if (m_busy.loadAcquire() == 0) return;
    logWarning(QStringLiteral("[FDL] 正在停止…"));
    setStep(QStringLiteral("正在停止…"));
    m_cancel.storeRelease(1);
}

void XiaomiEdlPage::runFlow()
{
    if (m_busy.loadAcquire() != 0) {
        error(QStringLiteral("上一次操作还没结束"));
        return;
    }

    setResultVisible(false);
    m_cancel.storeRelease(0);
    logView()->appendSection(QStringLiteral("一键进入 EDL"));
    logInfo(QStringLiteral("===== 开始：一键进入 EDL ====="));
    setBusy(true);
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QThreadPool::globalInstance()->start(new LambdaTask([this, started] {
        const bool ok = runFlowCore();
        const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
        logView()->appendFooter(
            QStringLiteral("%1, 耗时: %2秒")
                .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"),
                     QString::number(seconds, 'f', 2)));
        logInfo(QStringLiteral("===== 流程结束 ====="));
        runOnUi([this] { setBusy(false); });
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
//  FDL Auth 弹窗（worker 线程 → UI 线程）
// ─────────────────────────────────────────────────────────────────────────────

bool XiaomiEdlPage::requestSig(const QString& blob, QString* sigOut)
{
    struct Answer {
        bool accepted = false;
        QString sig;
    } answer;

    // 🔴 开窗只能在 UI 线程：本函数跑在 QThreadPool 的 worker 上，用 BlockingQueued 把
    //    「构造 + exec()」整体搬到 UI 线程，worker 阻塞等窗口关闭。UI 线程此时在 exec()
    //    的嵌套事件循环里照常转（动画、重绘都在走），所以不会死锁。
    //    answer 是 worker 栈上的对象，Blocking 语义保证调用返回前它一直有效。
    // 🔴 不要改成「发信号 + 回调」：流程是线性的（读 blob → 等 sig → stage），
    //    回调化要把 ⑤⑥⑦ 整段搬进 lambda，比现在难读得多。
    QMetaObject::invokeMethod(
        this,
        [this, &blob, &answer] {
            setStep(QStringLiteral("等待填写 sig…"));
            FdlAuthDialog dlg(blob, this);
            dlg.setModal(true);
            if (dlg.exec() == QDialog::Accepted) {
                answer.sig = dlg.sigText();
                // 弹窗内已校验过，这里再确认一次（防止后续改动把校验拿掉）。
                answer.accepted = !kc::auth::tryParseBytes(answer.sig).isEmpty();
            }
        },
        Qt::BlockingQueuedConnection);

    if (!answer.accepted) return false;
    if (sigOut) *sigOut = answer.sig;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  一键流程（worker 线程）
// ─────────────────────────────────────────────────────────────────────────────

bool XiaomiEdlPage::runFlowCore()
{
    const auto cancelled = [this] { return m_cancel.loadAcquire() != 0; };

    PlatformToolsManager tools(
        [this](const QString& msg) { logInfo(QStringLiteral("· %1").arg(msg)); });
    if (!tools.checkToolsExist() && !tools.ensureToolsExist()) {
        logError(QStringLiteral("fastboot 获取失败，请检查网络"));
        error(QStringLiteral("fastboot 获取失败，请检查网络"));
        setStep(QStringLiteral("失败：fastboot 工具不可用"));
        return false;
    }

    const auto runFastboot = [&](const QString& args, int timeoutMs) {
        logInfo(QStringLiteral("$ fastboot %1").arg(args));
        return tools.executeFastboot(args, timeoutMs);
    };

    // ① Fastboot 端口
    setStep(QStringLiteral("检查 Fastboot 端口…"));
    const auto dev = runFastboot(QStringLiteral("devices"), 15000);
    const QString devOut = dev.output + QStringLiteral("\n") + dev.error;
    QString serial;
    for (const QString& raw : devOut.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.endsWith(QStringLiteral("fastboot"))) {
            serial = line.section(QLatin1Char(' '), 0, 0);
            break;
        }
    }
    if (serial.isEmpty()) {
        logError(QStringLiteral("未检测到 Fastboot 设备：请确认手机已进入 Fastboot 模式、驱动正常"));
        logInfo(QStringLiteral("`fastboot devices` 原始回包：") + devOut.trimmed());
        error(QStringLiteral("没有检测到 Fastboot 设备"));
        setStep(QStringLiteral("失败：没有 Fastboot 设备"));
        showResult(false, QStringLiteral("失败：没有检测到 Fastboot 设备"),
                   QStringLiteral("请让手机进入 Fastboot 模式并连上电脑。"));
        return false;
    }
    runOnUi([this, serial] {
        m_deviceValue->setText(serial + QStringLiteral("  (fastboot)"));
    });
    logSuccess(QStringLiteral("Fastboot 端口正常：") + serial);

    // ② BL 锁状态 —— yes 就直接进 EDL，签名流程全省
    setStep(QStringLiteral("读取 BL 解锁状态…"));
    const auto un = runFastboot(QStringLiteral("getvar unlocked"), 15000);
    const QString lockState =
        grepVar(un.output + QStringLiteral("\n") + un.error, QStringLiteral("unlocked"));
    logInfo(QStringLiteral("unlocked = ") +
            (lockState.isEmpty() ? QStringLiteral("（没有回包）") : lockState));

    if (lockState.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0) {
        logSuccess(QStringLiteral("BL 已解锁 —— 不需要签名，直接进 EDL"));
        runOnUi([this, serial] {
            m_productValue->setText(QStringLiteral("- · BL 已解锁"));
        });
        // 直接复用下面的 SwitchToEdl 段（BL 已解锁时不需要 stage，跳过签名）。
    } else {
        if (lockState.isEmpty())
            logWarning(QStringLiteral("没读到 unlocked 状态，按「未解锁」继续走读 blob + 求 sig"));

        // ③ 读 blob
        setStep(QStringLiteral("读取设备 blob…"));
        const auto rr = runFastboot(m_readCmd, 60000);
        const QString rawOut = rr.output + QStringLiteral("\n") + rr.error;
        QDir().mkpath(workDir());
        QFile blobRaw(QFileInfo(QDir(workDir()), QStringLiteral("blob.raw")).absoluteFilePath());
        if (blobRaw.open(QIODevice::WriteOnly | QIODevice::Truncate))
            blobRaw.write(rawOut.toUtf8());

        const QString blob = extractBlob(rawOut);
        if (blob.isEmpty()) {
            logError(QStringLiteral("没有读到 blob（`%1` 没有回包）：设备可能不在 Fastboot 模式，"
                                    "或该机型不支持这条命令")
                         .arg(m_readCmd));
            error(QStringLiteral("没有读到 blob"));
            setStep(QStringLiteral("失败：没有读到 blob"));
            showResult(false, QStringLiteral("失败：没有读到 blob"),
                       QStringLiteral("设备没有回 blob。确认手机在 Fastboot 模式；"
                                      "若机型不认这条命令，到设置里改「读取命令」。"));
            return false;
        }
        const QByteArray blobBytes = tryParseBytes(blob);
        logSuccess(QStringLiteral("blob 已读回：%1 字符%2")
                       .arg(blob.size())
                       .arg(!blobBytes.isEmpty()
                                ? QStringLiteral("（%1 字节）").arg(blobBytes.size())
                                : QStringLiteral("（不是 hex，打包时按原文处理）")));

        // ④ 求 sig：弹「FDL Auth」窗 —— 原 token 只读 + 复制，sig 粘贴框，点「写入」才继续
        setStep(QStringLiteral("等待填写 sig…"));
        logInfo(QStringLiteral("打开「FDL Auth」窗口，等待粘贴 sig…"));
        QString sig;
        if (!requestSig(blob, &sig)) {
            logWarning(QStringLiteral("已取消「FDL Auth」—— 没有 sig 就没法进 EDL"));
            error(QStringLiteral("已取消：没有填写 sig"));
            setStep(QStringLiteral("已取消：没有填写 sig"));
            showResult(false, QStringLiteral("已取消：没有填写 sig"),
                       QStringLiteral("把原 token 复制到别处签名后，重新点「一键进入 EDL」，"
                                      "在弹窗里粘贴 sig 再写入。"));
            return false;
        }
        logSuccess(QStringLiteral("已收下 sig：%1 字符").arg(sig.trimmed().size()));

        const QByteArray sigBytes = tryParseBytes(sig);
        if (sigBytes.isEmpty()) {
            setStep(QStringLiteral("失败：sig 格式不认识"));
            showResult(false, QStringLiteral("失败：sig 格式不认识"),
                       QStringLiteral("sig 支持十六进制（可带空格 / 冒号 / 0x 前缀）与 Base64 两种写法。"));
            return false;
        }

        // ⑤ 落临时 bin：🔴 文件里只有 sig，不拼 blob（blob+sig 会被设备打回验签失败）。
        setStep(QStringLiteral("打包签名文件…"));
        const QString tmp = tempSignPath();
        QDir().mkpath(QFileInfo(tmp).absolutePath());
        {
            QFile f(tmp);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(sigBytes) != sigBytes.size()) {
                logError(QStringLiteral("写临时签名文件失败：%1").arg(f.errorString()));
                setStep(QStringLiteral("失败：写签名文件失败"));
                return false;
            }
        }
        logSuccess(QStringLiteral("已打包：%1（%2 字节 = 仅 sig，不含 blob、无附加头）")
                       .arg(QFileInfo(tmp).fileName())
                       .arg(sigBytes.size()));

        // ⑥ stage → ⑦ 无论成败立刻删临时文件
        setStep(QStringLiteral("上传签名到设备暂存区…"));
        const auto wr = runFastboot(QStringLiteral("%1 \"%2\"").arg(m_writeCmd, tmp), 120000);
        QFile::remove(tmp);
        logInfo(QStringLiteral("临时签名文件已删除"));
        if (!wr.success) {
            logError(QStringLiteral("上传失败：%1").arg(firstMeaningfulLine(wr.output + wr.error)));
            setStep(QStringLiteral("失败：签名上传失败"));
            showResult(false, QStringLiteral("失败：签名上传失败"),
                       QStringLiteral("设备没有接受这份签名文件 —— 确认 sig 与这台设备的 blob 是配套的。"));
            return false;
        }
        logSuccess(QStringLiteral("签名已送进设备暂存区"));
    }

    // ⑧ oem edl → ⑨ 以 9008 端口出现为唯一判据（退出码不可靠：设备当场断 USB）
    setStep(QStringLiteral("下发 ") + m_edlCmd + QStringLiteral(" …"));
    logInfo(QStringLiteral("切换到高通 9008…"));
    const auto er = runFastboot(m_edlCmd, kEdlCmdTimeoutMs);
    logInfo(QStringLiteral("`%1` 已下发（成功=%2）—— 后面以 9008 端口是否出现为准")
                .arg(m_edlCmd)
                .arg(er.success ? QStringLiteral("是") : QStringLiteral("否")));
    const QString first = firstMeaningfulLine(er.output + er.error);
    if (first != QStringLiteral("（无输出）")) logInfo(QStringLiteral("设备回包：") + first);

    setStep(QStringLiteral("等待设备进入 9008…"));
    logInfo(QStringLiteral("等待 9008 端口出现（最多 %1 秒）…").arg(kEdlWaitMs / 1000));
    kc::DeviceWatcher watcher;
    kc::DeviceInfo device;
    QString driverless;
    if (watcher.waitForDevice(kc::DeviceType::Edl9008, kEdlWaitMs, &device, cancelled)) {
        runOnUi([this, port = device.portName, desc = device.description] {
            m_deviceValue->setText(port + QStringLiteral("  (9008)"));
        });
        logSuccess(QStringLiteral("检测到 9008：%1 · %2").arg(device.portName, device.description));
        setStep(QStringLiteral("完成：设备已进入 EDL"));
        success(QStringLiteral("已进入 EDL（9008）"));
        showResult(true, QStringLiteral("成功：设备已进入 EDL"),
                   QStringLiteral("已检测到 9008 端口 %1 · %2").arg(device.portName, device.description));
        setShellStatus(device.portName, QStringLiteral("EDL 模式 (9008)"));
        return true;
    }

    if (cancelled()) {
        logWarning(QStringLiteral("[FDL] 已手动停止"));
        setStep(QStringLiteral("已停止"));
        return false;
    }

    // 兜底：驱动没装时 9008 是没有 COM 口的未知设备，waitForDevice 会漏掉它 ——
    // 再扫一遍设备列表里 PID_9008 的节点。
    const auto all = watcher.allDevices();
    for (const auto& d : all) {
        if (d.deviceId.contains(QStringLiteral("PID_9008"), Qt::CaseInsensitive) ||
            d.description.contains(QStringLiteral("9008"), Qt::CaseInsensitive)) {
            driverless = d.description.isEmpty() ? d.deviceId : d.description;
            break;
        }
    }
    if (!driverless.isEmpty()) {
        runOnUi([this, driverless] { m_deviceValue->setText(driverless + QStringLiteral("  (9008)")); });
        logSuccess(QStringLiteral("检测到 9008 设备：%1（没有 COM 端口，多半是驱动没装）").arg(driverless));
        setStep(QStringLiteral("完成：设备已进入 EDL（驱动缺失）"));
        success(QStringLiteral("已进入 EDL（9008）"));
        showResult(true, QStringLiteral("成功：设备已进入 EDL"),
                   QStringLiteral("检测到 9008 设备「%1」，但没拿到 COM 端口 —— "
                                  "多半是 9008 驱动没装，装上驱动再刷写。")
                       .arg(driverless));
        return true;
    }

    logError(QStringLiteral("等了 %1 秒，没有出现 9008 端口").arg(kEdlWaitMs / 1000));
    setStep(QStringLiteral("失败：没有检测到 9008"));
    error(QStringLiteral("失败：没有检测到 9008 端口"));
    showResult(false, QStringLiteral("失败：没有检测到 9008 端口"),
               QStringLiteral("设备没进 EDL。常见原因：sig 与这台设备的 blob 不配套、"
                              "该机型封锁了 oem edl、或 9008 驱动没装。"));
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  步骤 / 结果
// ─────────────────────────────────────────────────────────────────────────────

void XiaomiEdlPage::setStep(const QString& text)
{
    runOnUi([this, text] {
        if (m_stepLabel) m_stepLabel->setText(text);
    });
}

void XiaomiEdlPage::setResultVisible(bool visible)
{
    runOnUi([this, visible] {
        if (m_resultBox) m_resultBox->setVisible(visible);
    });
}

void XiaomiEdlPage::showResult(bool ok, const QString& title, const QString& sub)
{
    // 颜色取主题令牌（对象名 + 全局 QSS，不做 setStyleSheet 局部样式）。
    const QColor accent = ThemeManager::instance().color(ok ? ColorToken::Ok : ColorToken::Danger);
    runOnUi([this, ok, title, sub, accent] {
        if (!m_resultBox) return;
        m_resultIcon->setText(ok ? QStringLiteral("\u2714") : QStringLiteral("\u2718"));
        QPalette pal = m_resultIcon->palette();
        pal.setColor(QPalette::WindowText, accent);
        m_resultIcon->setPalette(pal);
        m_resultTitle->setText(title);
        QPalette titlePal = m_resultTitle->palette();
        titlePal.setColor(QPalette::WindowText, accent);
        m_resultTitle->setPalette(titlePal);
        m_resultSub->setText(sub);
        m_resultBox->show();
    });
}

} // namespace kc::ui
