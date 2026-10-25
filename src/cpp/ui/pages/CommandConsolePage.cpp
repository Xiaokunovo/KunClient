#include "ui/pages/CommandConsolePage.h"

#include "kc/tools/PlatformToolsManager.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/NotificationBar.h"

#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QThreadPool>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

/// 控制台保留的最大块数，防止 adb logcat 之类长时间输出把内存堆爆（同 C# MaxConsoleLines）。
constexpr int kMaxConsoleLines = 3000;

} // namespace

CommandConsolePage::CommandConsolePage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("命令控制台"));
    buildUi();
}

CommandConsolePage::~CommandConsolePage()
{
    if (m_proc) {
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
    }
}

void CommandConsolePage::showEvent(QShowEvent* event)
{
    BasePage::showEvent(event);
    // C#：Loaded 里静默 EnsureToolsAsync —— 只在第一次进入页面时跑。
    if (!m_checkedOnce) {
        m_checkedOnce = true;
        checkTools(true);
    }
}

void CommandConsolePage::buildUi()
{
    // 2026-10-17：整页铺玻璃板。命令窗口（#consoleView）本身也是玻璃，两层叠起来
    // 才有足够的底给等宽正文 —— 单层 30% 的白压在壁纸上，adb 输出根本读不出来。
    setGlassSheet(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // ── 分组标题 ────────────────────────────────────────────────────────────
    auto* header = new QLabel(QStringLiteral("命令窗口"), this);
    header->setObjectName(QStringLiteral("groupHeader"));
    layout->addWidget(header);

    // ── 输出区：等宽只读控制台（对应 C# TxtConsole LogRichTextBox）──────────
    m_console = new QPlainTextEdit(this);
    m_console->setObjectName(QStringLiteral("consoleView"));
    m_console->setReadOnly(true);
    m_console->setUndoRedoEnabled(false);
    m_console->setMaximumBlockCount(kMaxConsoleLines);
    m_console->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::TypeWriter);
    mono.setPointSize(9);
    m_console->setFont(mono);
    layout->addWidget(m_console, 1);

    // ── 命令行：> 提示符 + 模式下拉 + 输入框（回车执行、↑/↓ 翻历史）+ 执行 ──
    auto* inputRow = new QHBoxLayout;
    inputRow->setSpacing(6);

    auto* prompt = new QLabel(QStringLiteral(">"), this);
    QFont monoPrompt = mono;
    monoPrompt.setBold(true);
    prompt->setFont(monoPrompt);
    prompt->setForegroundRole(QPalette::WindowText);
    {
        QPalette pal = prompt->palette();
        pal.setColor(QPalette::WindowText, ThemeManager::instance().color(ColorToken::Accent));
        prompt->setPalette(pal);
    }

    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(QStringLiteral("adb shell"));
    m_modeCombo->addItem(QStringLiteral("fastboot"));
    m_modeCombo->addItem(QStringLiteral("原样执行 (cmd)"));
    m_modeCombo->setFixedWidth(140);

    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText(QStringLiteral("输入命令，回车执行；↑/↓ 翻历史"));
    connect(m_inputEdit, &QLineEdit::returnPressed, this, [this] { sendInput(); });

    m_runButton = new QPushButton(QStringLiteral("执行"), this);
    m_runButton->setObjectName(QStringLiteral("primaryButton"));
    m_runButton->setCursor(Qt::PointingHandCursor);
    m_runButton->setMinimumWidth(72);
    connect(m_runButton, &QPushButton::clicked, this, [this] { sendInput(); });

    inputRow->addWidget(prompt);
    inputRow->addWidget(m_modeCombo);
    inputRow->addWidget(m_inputEdit, 1);
    inputRow->addWidget(m_runButton);
    layout->addLayout(inputRow);

    // ── 按键翻历史（C# TxtCommand_KeyDown 的 ↑/↓）──────────────────────────
    m_inputEdit->installEventFilter(this);

    // ── 快捷操作行：停止 / 重启会话 / 清屏 / 系统CMD / 检测工具 ─────────────
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(6);

    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_restartButton = new QPushButton(QStringLiteral("重启会话"), this);
    auto* clearButton = new QPushButton(QStringLiteral("清屏"), this);
    auto* openCmdButton = new QPushButton(QStringLiteral("系统 CMD"), this);
    m_checkToolsButton = new QPushButton(QStringLiteral("检测工具"), this);
    for (QPushButton* b : { m_stopButton, m_restartButton, clearButton, openCmdButton,
                            m_checkToolsButton }) {
        b->setCursor(Qt::PointingHandCursor);
        actionRow->addWidget(b);
    }
    actionRow->addStretch();
    layout->addLayout(actionRow);

    connect(m_stopButton, &QPushButton::clicked, this,
            [this] { stopSession(QString()); });
    connect(m_restartButton, &QPushButton::clicked, this, [this] {
        stopSession(QStringLiteral("[会话已重启]"));
        startSession(true);
    });
    connect(clearButton, &QPushButton::clicked, this, [this] { m_console->clear(); });
    connect(openCmdButton, &QPushButton::clicked, this, [this] {
        // C# BtnOpenCmd_Click：真 cmd 窗口，PATH 已带 adb / fastboot。
        PlatformToolsManager tools;
        const QString toolsDir = tools.toolsDirectory();
        const QString cmdline =
            QStringLiteral("set \"PATH=%1;%PATH%\"").arg(toolsDir);
        if (QProcess::startDetached(QStringLiteral("cmd.exe"),
                                    { QStringLiteral("/k"), cmdline }, toolsDir)) {
            success(QStringLiteral("已打开系统命令提示符（可直接输入 adb / fastboot）"));
        } else {
            error(QStringLiteral("打开命令提示符失败"));
        }
    });
    connect(m_checkToolsButton, &QPushButton::clicked, this, [this] { checkTools(false); });
    connect(m_modeCombo, &QComboBox::currentIndexChanged, this, [this] { onModeChanged(); });

    // ── 提示行（C# TxtHint，文案全部由代码写入）────────────────────────────
    m_hintLabel = new QLabel(QStringLiteral("回车执行；↑ / ↓ 翻历史命令。"), this);
    m_hintLabel->setObjectName(QStringLiteral("consoleHint"));
    layout->addWidget(m_hintLabel);

    setSessionButtons();
}

bool CommandConsolePage::eventFilter(QObject* watched, QEvent* event)
{
    // ↑/↓ 翻历史（等价 C# TxtCommand_KeyDown；只拦输入框自己的按键）。
    if (watched == m_inputEdit && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up) {
            navigateHistory(-1);
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            navigateHistory(1);
            return true;
        }
    }
    return BasePage::eventFilter(watched, event);
}

QString CommandConsolePage::currentMode() const
{
    switch (m_modeCombo ? m_modeCombo->currentIndex() : 0) {
    case 0: return QStringLiteral("adb");
    case 1: return QStringLiteral("fastboot");
    default: return QStringLiteral("cmd");
    }
}

void CommandConsolePage::setHint(const QString& text)
{
    if (m_hintLabel) m_hintLabel->setText(text);
}

void CommandConsolePage::setSessionButtons()
{
    const bool running = m_proc && m_proc->state() != QProcess::NotRunning;
    if (m_stopButton) m_stopButton->setEnabled(running);
    if (m_inputEdit) m_inputEdit->setEnabled(true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  工具就绪（PlatformToolsManager 的阻塞方法只在线程池里调）
// ─────────────────────────────────────────────────────────────────────────────

void CommandConsolePage::checkTools(bool silent)
{
    if (m_checking.loadAcquire() != 0) return;
    m_checking.storeRelease(1);
    if (m_checkToolsButton) m_checkToolsButton->setEnabled(false);
    if (!silent) setHint(QStringLiteral("正在从服务器获取 ADB / Fastboot…"));

    QPointer<CommandConsolePage> guard(this);
    // PlatformToolsManager 构造很轻（解析目录 + mkdir），UI 线程取目录，worker 里跑阻塞方法。
    PlatformToolsManager tools;
    const QString toolsDir = tools.toolsDirectory();
    QThreadPool::globalInstance()->start([this, guard, silent, toolsDir] {
        PlatformToolsManager worker;
        bool ok = worker.checkToolsExist();
        if (!ok) ok = worker.ensureToolsExist();

        runOnUi([this, guard, ok, toolsDir, silent] {
            if (!guard) return;
            m_toolsReady = ok;
            if (ok) {
                setHint(QStringLiteral("已就绪 · 工具目录：%1").arg(toolsDir));
            } else {
                setHint(QStringLiteral("ADB / Fastboot 不可用：请检查网络后点「检测工具」重试"));
            }
            if (!silent) {
                ok ? success(QStringLiteral("ADB / Fastboot 已就绪"))
                   : error(QStringLiteral("ADB / Fastboot 获取失败，请检查网络后重试"));
            }
            m_checkToolsButton->setEnabled(true);
            m_checking.storeRelease(0);
        });
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  持久交互会话（QProcess 信号槽，全程 UI 线程事件循环）
// ─────────────────────────────────────────────────────────────────────────────

void CommandConsolePage::startSession(bool announce)
{
    const QString mode = currentMode();

    // 子进程 PATH 前置 platform-tools 目录（同 C#：只影响这一个子进程）。
    PlatformToolsManager tools;
    const QString toolsDir = tools.toolsDirectory();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PATH"),
               toolsDir + QDir::listSeparator() + env.value(QStringLiteral("PATH")));

    auto* proc = new QProcess(this);
    proc->setProcessEnvironment(env);
    proc->setWorkingDirectory(toolsDir);
    proc->setProcessChannelMode(QProcess::SeparateChannels);

    if (mode == QStringLiteral("adb")) {
        proc->setProgram(tools.adbPath());
        proc->setArguments({ QStringLiteral("shell") });   // adb shell 交互会话
    } else if (mode == QStringLiteral("fastboot")) {
        proc->setProgram(tools.fastbootPath());
        proc->setArguments({});                            // fastboot 交互（stdin 逐条喂）
    } else {
        // cmd.exe /Q /K：回显关掉、执行完第一条保持会话；chcp 65001 保证 UTF-8。
        proc->setProgram(QStringLiteral("cmd.exe"));
        proc->setArguments({ QStringLiteral("/Q"), QStringLiteral("/K"),
                             QStringLiteral("chcp 65001 >nul") });
    }

    connect(proc, &QProcess::readyReadStandardOutput, this, [this] {
        if (!m_proc) return;
        appendOut(QString::fromUtf8(m_proc->readAllStandardOutput()));
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this] {
        if (!m_proc) return;
        appendOut(QString::fromUtf8(m_proc->readAllStandardError()));
    });
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        const QString text = err == QProcess::FailedToStart
                                 ? QStringLiteral("启动命令执行环境失败（进程不存在或无权限）")
                                 : QStringLiteral("命令执行环境异常（错误码 %1）").arg(int(err));
        appendOut(QStringLiteral("✗ ") + text);
        error(text);
        if (err == QProcess::FailedToStart) {
            // FailedToStart 之后不会再有 finished —— 这里就地收尸。
            if (m_proc) m_proc->deleteLater();
            m_proc = nullptr;
            m_procMode.clear();
            setSessionButtons();
        }
    });
    connect(proc, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        appendOut(QStringLiteral("[会话结束，退出码 %1]").arg(exitCode));
        if (m_proc) m_proc->deleteLater();
        m_proc = nullptr;
        setSessionButtons();
    });

    m_proc = proc;
    m_procMode = mode;
    proc->start();

    if (announce) {
        const QString label = mode == QStringLiteral("adb")
                                  ? QStringLiteral("adb shell")
                                  : mode == QStringLiteral("fastboot")
                                        ? QStringLiteral("fastboot")
                                        : QStringLiteral("cmd");
        appendOut(QStringLiteral("── 新会话：%1（PATH 已含 %2）──").arg(label, toolsDir));
    }
    setSessionButtons();
}

void CommandConsolePage::stopSession(const QString& reason)
{
    if (!m_proc) return;
    if (m_proc->state() != QProcess::NotRunning) m_proc->kill();
    m_proc->deleteLater();
    m_proc = nullptr;
    m_procMode.clear();
    appendOut(reason.isEmpty() ? QStringLiteral("[已停止]") : reason);
    setSessionButtons();
}

void CommandConsolePage::onModeChanged()
{
    // 模式切换：杀掉旧会话并立即按新模式重启（任务约定：kill and respawn）。
    if (m_proc) {
        stopSession(QStringLiteral("[模式切换，旧会话已关闭]"));
        startSession(true);
    }
}

void CommandConsolePage::sendInput()
{
    const QString input = m_inputEdit->text().trimmed();
    if (input.isEmpty()) return;

    // 历史：连续重复的命令不重复记（同 C#）。
    if (m_history.isEmpty() || m_history.last() != input) m_history.append(input);
    m_historyIndex = m_history.size();

    if (!m_proc) {
        if (!m_toolsReady) checkTools(true);
        startSession(true);
        // QProcess::start 是异步的；进程随后 readyRead 时自然会出输出，
        // 但写 stdin 也要等它真正起来 —— 这里靠 buffered 写（Qt 会缓冲到进程可写）。
    }
    if (!m_proc) {
        error(QStringLiteral("无法启动命令执行环境"));
        return;
    }

    // 回显（交互进程自己的 prompt 不经我们，这里补一行可读的命令痕迹，同 C# "$ cmd"）。
    const QString prefix = m_procMode == QStringLiteral("adb")
                               ? QStringLiteral("adb")
                               : m_procMode == QStringLiteral("fastboot")
                                     ? QStringLiteral("fastboot")
                                     : QString();
    appendOut(QStringLiteral("$ ") + (prefix.isEmpty() ? input : prefix + QLatin1Char(' ') + input));
    m_inputEdit->clear();

    m_proc->write((input + QStringLiteral("\r\n")).toUtf8());
}

void CommandConsolePage::navigateHistory(int delta)
{
    if (m_history.isEmpty()) return;
    // 与 C# NavigateHistory 一致：historyIndex 停在 size 处 = 空输入（新命令）。
    m_historyIndex = qBound(0, m_historyIndex + delta, m_history.size());
    m_inputEdit->setText(m_historyIndex < m_history.size() ? m_history.at(m_historyIndex)
                                                           : QString());
    m_inputEdit->end(false);
}

void CommandConsolePage::appendOut(const QString& text)
{
    if (!m_console || text.isEmpty()) return;
    QString line = text;
    while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
        line.chop(1);
    if (!line.isEmpty()) m_console->appendPlainText(line);
}

} // namespace kc::ui
