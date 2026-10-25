#include "ui/widgets/LogView.h"

#include "kc/diag/SessionLog.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QFont>
#include <QMetaObject>
#include <QMutexLocker>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

/// 所有活着的 LogView。控制中心的「日志详细」磁贴靠它一次性切全部实例 ——
/// 日志视图分散在 EDL / 安卓工具 / 应用管理等多个页面，逐个找控件既脆弱又漏。
QSet<LogView*>& liveViews() {
    static QSet<LogView*> s_views;
    return s_views;
}

const char* kKeyDebugVisible = "log/debugVisible";

/// 把级别映射到主题令牌色。LOG 用的级别色与 DesignTokens 的 Theme.Log* 同值。
QColor levelColor(const ThemeManager& tm, LogLevel level) {
    switch (level) {
        case LogLevel::Ok:      return tm.color(ColorToken::LogSuccess);
        case LogLevel::Warning: return tm.color(ColorToken::LogWarning);
        case LogLevel::Error:   return tm.color(ColorToken::LogError);
        case LogLevel::Trace:   return tm.color(ColorToken::TextFaint);
        case LogLevel::Section:
        case LogLevel::Footer:  return tm.color(ColorToken::LogInfo);
        case LogLevel::Info:
        default:                return tm.color(ColorToken::LogInfo);
    }
}

QString levelBadge(LogLevel level) {
    switch (level) {
        case LogLevel::Ok:      return QStringLiteral("OK");
        case LogLevel::Warning: return QStringLiteral("WARN");
        case LogLevel::Error:   return QStringLiteral("ERROR");
        case LogLevel::Trace:   return QStringLiteral("TRACE");
        case LogLevel::Info:
        default:                return QStringLiteral("INFO");
    }
}

const char* kEdlPrefix = "[EDL] ";

QString stripEdlPrefix(const QString& body) {
    return body.startsWith(QLatin1String(kEdlPrefix))
               ? body.mid(static_cast<int>(qstrlen(kEdlPrefix)))
               : body;
}

/// 行间隔列的宽度（字符数）。`+12.4s` / `+3m12s` 正好 6 格，
/// 等宽字体下「有间隔」与「无间隔」两种行的左边界才能对齐。
constexpr int kGapWidth = 6;
/// 间隔小于这个值就不显示：满屏 `+0.1s` 只是噪音，用户要看的是「哪一步卡住了」。
constexpr qint64 kGapThresholdMs = 800;

/// 真正决定「这一行长什么样」的级别。
/// 操作行（beginOperation→appendOperation）的 level 恒为 Info —— 级别留给筛选和
/// 落盘用，界面上的符号/颜色另按 OK/Error 走，这样筛选口径与视觉呈现互不干扰。
LogLevel displayLevel(LogLevel level, bool hasStatus, bool statusOk) {
    if (hasStatus) return statusOk ? LogLevel::Ok : LogLevel::Error;
    return level;
}

/// 级别符号：一列 1 个字符，充当整篇日志的「脊椎」。
QString levelGlyph(LogLevel level) {
    switch (level) {
        case LogLevel::Ok:      return QStringLiteral("✓");
        case LogLevel::Warning: return QStringLiteral("!");
        case LogLevel::Error:   return QStringLiteral("✕");
        case LogLevel::Section: return QStringLiteral("▍");
        case LogLevel::Footer:  return QStringLiteral("▪");
        case LogLevel::Trace:   return QStringLiteral("·");
        case LogLevel::Info:
        default:                return QStringLiteral("·");
    }
}

/// 毫秒 → 人类可读耗时：`0.92s` / `12.4s` / `3m12s`。
QString formatDuration(qint64 ms) {
    if (ms < 0) return QString();
    if (ms < 10000) return QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 2);
    if (ms < 60000) return QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    const qint64 totalSec = ms / 1000;
    return QStringLiteral("%1m%2s")
        .arg(totalSec / 60)
        .arg(totalSec % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

LogView::LogView(QWidget* parent) : QWidget(parent) {
    buildUi();
    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(FlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this]() { flush(); });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool) { recolor(); });

    // 构造时按上次的「详细」档位初始化：控制中心里拨过之后，新开的页面要跟着走。
    m_debugVisible = QSettings().value(QLatin1String(kKeyDebugVisible), false).toBool();
    liveViews().insert(this);
}

LogView::~LogView() { liveViews().remove(this); }

void LogView::setGlobalDebugVisible(bool visible) {
    QSettings().setValue(QLatin1String(kKeyDebugVisible), visible);
    const auto views = liveViews();
    for (LogView* view : views) view->setDebugVisible(visible);
}

bool LogView::globalDebugVisible() {
    return QSettings().value(QLatin1String(kKeyDebugVisible), false).toBool();
}

void LogView::buildUi() {
    setObjectName(QStringLiteral("logView"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_edit = new QTextEdit(this);
    m_edit->setObjectName(QStringLiteral("logTextEdit"));
    m_edit->setReadOnly(true);
    m_edit->setUndoRedoEnabled(false);
    m_edit->setAcceptRichText(false);
    m_edit->setFrameShape(QFrame::NoFrame);
    m_edit->setLineWrapMode(QTextEdit::WidgetWidth);

    QFont mono;
    mono.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                      QStringLiteral("monospace")});
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(9.0);
    m_edit->setFont(mono);

    layout->addWidget(m_edit);

    m_edit->document()->setMaximumBlockCount(MaxLines + m_trimSlack);
}

// ── 追加接口 ──────────────────────────────────────────────────────────────

void LogView::appendLine(LogLevel level, const QString& text) {
    Line line;
    line.time = QDateTime::currentDateTime();
    line.level = level;
    line.body = stripEdlPrefix(text);
    switch (level) {
        case LogLevel::Ok:
            line.operationStyle = false; // OK 保留徽章
            break;
        case LogLevel::Warning:
        case LogLevel::Error:
            line.colorizeBody = true; // 异常行整行醒目
            break;
        case LogLevel::Trace:
            line.operationStyle = true;
            line.dim = true;
            break;
        case LogLevel::Section:
        case LogLevel::Footer:
            line.operationStyle = true;
            break;
        case LogLevel::Info:
        default:
            // 常态行不带级别徽章（满屏 INFO 是噪音）。
            line.operationStyle = true;
            break;
    }
    enqueue(line);
}

void LogView::beginOperation(const QString& body) {
    // 记起始时刻：appendOperation 收尾时用它算这一步实际跑了多久。
    m_opStartedMs.storeRelease(QDateTime::currentMSecsSinceEpoch());

    Line line;
    line.time = QDateTime::currentDateTime();
    line.level = LogLevel::Info;
    line.body = stripEdlPrefix(body);
    line.operationStyle = true;
    line.awaitingStatus = true;
    enqueue(line);
}

void LogView::appendOperation(const QString& body, bool ok) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 startedMs = m_opStartedMs.fetchAndStoreRelease(0);
    // 只有真的配过 beginOperation 才算耗时；单独调 appendOperation 时 startedMs=0。
    const qint64 durationMs =
        (startedMs > 0 && nowMs > startedMs) ? (nowMs - startedMs) : -1;
    const QString status = ok ? QStringLiteral("OK") : QStringLiteral("Error");

    // 一次操作在日志里只占**一行**（2026-10-18「日志有点杂」）：
    // 收尾不再新增一行，而是把 begin 行尾的 `....` 就地换成 `OK (0.8s)`。
    // 12 个分区的刷写因此从 24 行降到 12 行。
    {
        QMutexLocker lock(&m_mutex);
        for (int i = m_history.size() - 1; i >= 0; --i) {
            Line& l = m_history[i];
            if (!l.operationStyle || !l.awaitingStatus) break;   // 没有配对，走退化分支
            l.awaitingStatus = false;
            l.hasStatus = true;
            l.statusText = status;
            l.statusOk = ok;
            l.durationMs = durationMs;

            // 还没画到文档上：直接把它改成完成行，flush 时就是一行收尾态。
            for (int p = m_pending.size() - 1; p >= 0; --p) {
                Line& queued = m_pending[p];
                if (!queued.operationStyle || !queued.awaitingStatus) continue;
                queued.awaitingStatus = false;
                queued.hasStatus = true;
                queued.statusText = status;
                queued.statusOk = ok;
                queued.durationMs = durationMs;
                scheduleFlush();
                return;
            }

            m_pendingOpStatus.append(PendingOpStatus{ body, ok, durationMs });
            scheduleFlush();
            return;
        }
    }

    // 没有配对的 beginOperation（单独调用）：退化成一行独立的完成行。
    Line line;
    line.time = QDateTime::currentDateTime();
    line.level = LogLevel::Info;
    line.body = stripEdlPrefix(body);
    line.operationStyle = true;
    line.hasStatus = true;
    line.statusText = status;
    line.statusOk = ok;
    line.durationMs = durationMs;
    enqueue(line);
}

bool LogView::patchOpenOperation(const PendingOpStatus& op) {
    if (m_openOpBlockPos < 0) return false;
    QTextDocument* doc = m_edit->document();
    QTextBlock block = doc->findBlock(m_openOpBlockPos);
    if (!block.isValid() || !block.text().endsWith(QStringLiteral("...."))) {
        m_openOpBlockPos = -1;
        return false;
    }

    const ThemeManager& tm = ThemeManager::instance();
    const QColor dim = tm.color(ColorToken::Text).darker(165);
    const QColor accent = levelColor(tm, op.ok ? LogLevel::Ok : LogLevel::Error);

    QTextCursor cur(block);
    cur.movePosition(QTextCursor::EndOfBlock);
    cur.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, 4);   // 选中 `....`
    cur.removeSelectedText();

    QTextCharFormat plain;
    plain.setForeground(dim);
    cur.insertText(QStringLiteral("  "), plain);

    QTextCharFormat strong;
    strong.setForeground(accent);
    strong.setFontWeight(QFont::DemiBold);
    cur.insertText(op.ok ? QStringLiteral("OK") : QStringLiteral("Error"), strong);

    if (op.durationMs >= 0) {
        QTextCharFormat weak;
        weak.setForeground(dim);
        cur.insertText(QStringLiteral("   (%1)").arg(formatDuration(op.durationMs)), weak);
    }

    m_openOpBlockPos = -1;
    return true;
}

void LogView::appendVersionBlock(const QVector<VersionRow>& rows, const QString& summary) {
    // 块头（强调色加粗标题）。
    {
        Line head;
        head.time = QDateTime::currentDateTime();
        head.level = LogLevel::Ok;
        head.body = QStringLiteral("设备版本信息");
        head.operationStyle = true;
        head.colorizeBody = true;
        enqueue(head);
    }
    if (!summary.trimmed().isEmpty()) {
        Line s;
        s.time = QDateTime::currentDateTime();
        s.level = LogLevel::Info;
        s.body = summary;
        s.operationStyle = true;
        enqueue(s);
    }
    for (const VersionRow& row : rows) {
        Line line;
        line.time = QDateTime::currentDateTime();
        line.level = LogLevel::Info;
        line.body = row.first;                                     // 名称加粗
        line.nameValue = true;
        line.colorizeBody = true;
        line.operationStyle = true;
        if (!row.second.isEmpty()) {
            line.bodySuffix = QStringLiteral("：") + row.second;    // 值正文色
        }
        enqueue(line);
    }
}

void LogView::clear() {
    {
        QMutexLocker lock(&m_mutex);
        m_history.clear();
        m_pending.clear();
        m_pendingOpStatus.clear();
    }
    m_edit->clear();
    m_lastRenderedMs = 0;
    m_opStartedMs.storeRelease(0);
    m_openOpBlockPos = -1;
}

// ── 筛选 ──────────────────────────────────────────────────────────────────

void LogView::setFilter(LogLevel level) {
    m_filter = static_cast<int>(level);
    m_debugVisible = (level == LogLevel::Trace);
    {
        QMutexLocker lock(&m_mutex);
        m_pending.clear(); // 旧档位下排进去的行丢掉，否则重建后会漏出来
    }
    rebuild();
}

void LogView::clearFilter() {
    m_filter = -1;
    m_debugVisible = true;
    {
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
    }
    rebuild();
}

void LogView::setNormalFilter() {
    m_filter = -1;
    m_debugVisible = false; // 常规 = 除 TRACE 之外的一切
    {
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
    }
    rebuild();
}

void LogView::setDebugVisible(bool visible) {
    m_debugVisible = visible;
    {
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
    }
    rebuild();
}

bool LogView::matches(const Line& line) const {
    // TRACE 详细行：由 debugVisible 控制。
    if (line.level == LogLevel::Trace && !m_debugVisible) return false;
    if (m_filter < 0) return true;
    return static_cast<int>(line.level) == m_filter;
}

// ── 入队 / 批量渲染 ────────────────────────────────────────────────────────

void LogView::enqueue(const Line& line) {
    // 先汇到 UI 线程再入队：内部结构与 QTimer 都不是线程安全的。
    // 线程池线程直接入队会与 UI 线程的 flush 竞争。转 UI 线程后所有内部结构
    // 都由 m_mutex 保护，且 flush 只在 UI 线程跑。
    if (QThread::currentThread() != this->thread()) {
        Line copy = line;
        QMetaObject::invokeMethod(
            this, [this, copy]() { enqueue(copy); }, Qt::QueuedConnection);
        return;
    }

    // 落盘（「上传日志给开发者」的数据源，见 kc/diag/SessionLog）。
    // 🔴 必须在筛选判定**之前**写：文件里保留完整流水，不被界面上的
    //    「常规 / 详细」档位裁掉 —— 开发者排查时要的正是被裁掉的 TRACE。
    QString fileText = line.body;
    if (!line.bodySuffix.isEmpty()) fileText += line.bodySuffix;
    if (line.hasStatus && !line.statusText.isEmpty())
        fileText += QStringLiteral(" → ") + line.statusText;
    kc::diag::SessionLog::instance().append(line.time, levelBadge(line.level), fileText);

    {
        QMutexLocker lock(&m_mutex);
        // 历史**全量**留存：常规档期间被裁掉的 TRACE 也要能进历史，否则用户中途
        // 开「详细」档时重放不出这段时间的 Firehose 原始帧（与 SessionLog 落盘
        // 不裁的口径一致 —— 落盘注释里说的「开发者要的正是被裁掉的 TRACE」）。
        m_history.append(line);
        if (m_history.size() > MaxLines + m_trimSlack) {
            m_history.remove(0, m_history.size() - MaxLines);
        }
        if (!matches(line)) return;
        m_pending.append(line);
    }
    scheduleFlush();
}

void LogView::scheduleFlush() {
    if (!m_flushTimer->isActive()) {
        m_flushTimer->start();
    }
}

void LogView::flush() {
    QVector<Line> batch;
    QVector<PendingOpStatus> ops;
    {
        QMutexLocker lock(&m_mutex);
        if (m_pending.isEmpty() && m_pendingOpStatus.isEmpty()) {
            m_flushTimer->stop();
            return;
        }
        batch.swap(m_pending);
        ops.swap(m_pendingOpStatus);
    }

    for (const Line& line : batch) {
        renderLine(line);
    }

    // 收尾请求必须在**本批渲染之后**处理：begin 行可能就在这一批里，
    // 先补后缀会找不到块（m_openOpBlockPos 还是 -1）。
    int rendered = batch.size();
    for (const PendingOpStatus& op : ops) {
        if (patchOpenOperation(op)) continue;
        // 那行没画到文档上（被筛掉 / 已被裁掉 / 从没渲染过）→ 补一条独立完成行，
        // 否则用户看到的是「有开始没结果」。
        Line line;
        line.time = QDateTime::currentDateTime();
        line.level = LogLevel::Info;
        line.body = stripEdlPrefix(op.body);
        line.operationStyle = true;
        line.hasStatus = true;
        line.statusText = op.ok ? QStringLiteral("OK") : QStringLiteral("Error");
        line.statusOk = op.ok;
        line.durationMs = op.durationMs;
        renderLine(line);
        ++rendered;
    }

    trimDocument();

    // 滚底整批只做一次（原实现一行一次）。
    if (autoScroll()) {
        auto* bar = m_edit->verticalScrollBar();
        bar->setValue(bar->maximum());
    }

    if (m_onAppended) m_onAppended(rendered);
}

void LogView::renderLine(const Line& line) {
    const ThemeManager& tm = ThemeManager::instance();
    QTextCursor cur(m_edit->document());
    cur.movePosition(QTextCursor::End);

    const QColor bodyColor0 = tm.color(ColorToken::Text);
    const QColor dim = bodyColor0.darker(165);
    const QColor rule = tm.color(ColorToken::BorderSubtle);
    const LogLevel shown = displayLevel(line.level, line.hasStatus, line.statusOk);
    const QColor accent = levelColor(tm, shown);

    auto insert = [&cur](const QString& text, const QColor& color, bool bold) {
        if (text.isEmpty()) return;
        QTextCharFormat fmt;
        fmt.setForeground(color);
        if (bold) fmt.setFontWeight(QFont::DemiBold);
        cur.insertText(text, fmt);
    };
    // 段落格式（行距 / 底色）作用在「光标所在块」。当前块已有内容时先换块，
    // 否则直接套用 —— 否则文档开头会多出一条空行。
    auto startBlock = [&cur](const QTextBlockFormat& bf) {
        if (cur.block().text().isEmpty() && cur.block().blockNumber() == 0) {
            cur.setBlockFormat(bf);
        } else {
            cur.insertBlock(bf);
        }
    };

    // ── 时间列 ───────────────────────────────────────────────────────────
    // 两段信息：① 绝对时刻 HH:mm:ss（跟外部日志/截图对时间用）；
    //             ② 距上一行的间隔 +12.4s（一眼看出「哪一步卡住了」）。
    // 间隔小于阈值留空，但**宽度恒定**，否则有间隔的行会整体右移、左边界参差。
    const qint64 lineMs = line.time.toMSecsSinceEpoch();
    QString gapText;
    if (m_lastRenderedMs > 0 && lineMs > m_lastRenderedMs) {
        const qint64 delta = lineMs - m_lastRenderedMs;
        if (delta >= kGapThresholdMs) {
            gapText = QStringLiteral("+") + formatDuration(delta);
        }
    }
    m_lastRenderedMs = lineMs;
    const QString gap = gapText.rightJustified(kGapWidth, QLatin1Char(' '));
    const QString stamp = line.time.toString(QStringLiteral("HH:mm:ss"));

    // ── 段头：整行淡色底 + 加粗强调色标题 + 上下留白 ─────────────────────
    if (line.level == LogLevel::Section) {
        QTextBlockFormat bf;
        bf.setTopMargin(14);
        bf.setBottomMargin(5);
        QColor band = accent;
        band.setAlpha(24);
        bf.setBackground(band);
        startBlock(bf);
        insert(stamp, dim, false);
        insert(QStringLiteral("  "), dim, false);
        insert(gap, dim, false);
        insert(QStringLiteral("  "), dim, false);
        insert(levelGlyph(line.level), accent, true);
        insert(QStringLiteral("  "), accent, true);
        insert(line.body, accent, true);
        return;
    }

    // ── 收尾统计：`▪ 完成，耗时 12.34s` ──────────────────────────────────
    if (line.level == LogLevel::Footer) {
        QTextBlockFormat bf;
        bf.setTopMargin(3);
        bf.setBottomMargin(2);
        startBlock(bf);
        insert(stamp, dim, false);
        insert(QStringLiteral("  "), dim, false);
        insert(gap, dim, false);
        insert(QStringLiteral("  "), dim, false);
        insert(levelGlyph(line.level), accent, true);
        insert(QStringLiteral("  "), accent, true);
        insert(line.body, accent, true);
        return;
    }

    // ── 普通行 ───────────────────────────────────────────────────────────
    QTextBlockFormat bf;
    if (line.level == LogLevel::Error) {
        // 失败行给一条淡红底：翻日志时不用逐字读，扫一眼就能定位。
        QColor band = accent;
        band.setAlpha(22);
        bf.setBackground(band);
        bf.setTopMargin(1);
        bf.setBottomMargin(1);
    } else if (line.level == LogLevel::Warning) {
        QColor band = accent;
        band.setAlpha(14);
        bf.setBackground(band);
    }
    startBlock(bf);

    insert(stamp, dim, false);
    insert(QStringLiteral("  "), dim, false);
    insert(gap, dim, false);
    insert(QStringLiteral("  "), dim, false);
    insert(levelGlyph(shown), shown == LogLevel::Info ? rule : accent, shown != LogLevel::Info);

    // 详细行缩进 4 格：Firehose 原始帧、寄存器读数这类「支撑证据」沉到下一层，
    // 主流程行始终贴着左边界，扫读时不会被细节打断。
    if (line.level == LogLevel::Trace) insert(QStringLiteral("    "), dim, false);

    const QColor textColor = line.dim ? dim : (line.colorizeBody ? accent : bodyColor0);
    insert(QStringLiteral(" ") + line.body, textColor, line.nameValue);

    if (!line.bodySuffix.isEmpty()) insert(line.bodySuffix, bodyColor0, false);

    // ── 操作行状态（TMF 风格 `Start Write boot.... → OK`）─────────────────
    // 🔴 这里此前是空的：statusText 只在 toPlainText() 导出时被拼进纯文本，
    //    界面上**根本不显示** —— Fastboot / 应用管理页那套
    //    beginOperation → appendOperation 一直是「两行一模一样的文本」，
    //    既没有 OK 也没有失败标记，看不出这一步到底成没成。
    if (line.awaitingStatus && !line.hasStatus) {
        insert(QStringLiteral("...."), dim, false);   // 等待中
        // 记下块起点：收尾时用它把 `....` 原地换成 `OK`，一次操作只占一行。
        m_openOpBlockPos = cur.block().position();
    }
    if (line.hasStatus && !line.statusText.isEmpty()) {
        insert(QStringLiteral("  "), dim, false);
        insert(line.statusText, accent, true);        // OK 绿 / Error 红
    }

    // 操作耗时：`(1.24s)` 压在收尾行末尾，与左边的间隔列互补 ——
    // 间隔列回答「这一步离上一步隔了多久」，这里回答「这一步本身跑了多久」。
    if (line.durationMs >= 0) {
        insert(QStringLiteral("   "), dim, false);
        insert(QStringLiteral("(%1)").arg(formatDuration(line.durationMs)), dim, false);
    }
}

void LogView::trimDocument() {
    QTextDocument* doc = m_edit->document();
    if (doc->blockCount() <= MaxLines + m_trimSlack) return;
    // 攒一批再裁：攒到多出 TrimSlack 行才统一裁，把「逐行裁」摊成「每 64 行裁一次」。
    QTextCursor cur(doc);
    cur.movePosition(QTextCursor::Start);
    while (doc->blockCount() > MaxLines) {
        cur.select(QTextCursor::BlockUnderCursor);
        cur.removeSelectedText();
        if (!cur.movePosition(QTextCursor::NextBlock)) break;
        cur.deleteChar();
    }
}

void LogView::rebuild() {
    m_edit->clear();
    m_lastRenderedMs = 0; // 重放要重算行间隔，否则首行会跟上一屏的最后一行比
    // 文档被清空 → 之前记的块位置全部作废（重放时遇到未完结行会重新记）。
    m_openOpBlockPos = -1;
    QVector<Line> snapshot;
    {
        QMutexLocker lock(&m_mutex);
        snapshot = m_history;
    }
    for (const Line& line : snapshot) {
        if (matches(line)) renderLine(line);
    }
    if (autoScroll()) {
        auto* bar = m_edit->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

QString LogView::toPlainText() {
    return m_edit->toPlainText();
}

void LogView::recolor() {
    // 段落里的画刷是渲染时定格的 —— 主题切换只换令牌，画在文档上的旧颜色
    // 不会跟着变：浅色下的黑字留在深色主题里 = 整片日志看不见。
    {
        QMutexLocker lock(&m_mutex);
        m_pending.clear();
    }
    rebuild();
}

void LogView::attachStatus(int /*pos*/, const Line& /*line*/) {
    // 状态后缀在 renderLine/rebuild 时直接画入（与 C# 的 AttachStatusRun 对应）；
    // 本函数预留给"行已渲染后再补后缀"的高频路径，当前统一走 rebuild。
}

} // namespace kc::ui
