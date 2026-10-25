#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  LogView — 日志视图（替代 PageLog.cs，987 行）
//
//  移植自 tests/src/KunClient.App/Services/PageLog.cs（该类实际定义在 Notify.cs 里）。
//  一个 QWidget 包裹 QTextEdit + QTextDocument，提供：
//   · 级别着色（时间戳压暗、级别徽章着级别色，WARN/ERROR 连正文一起着色）；
//   · 批量渲染（80ms flush 一批，避免逐行 ScrollToEnd 把文档重排上千遍）；
//   · 历史重放（结构化存行，切筛选/换主题时整篇重放）；
//   · 级别筛选（常规 = 除 TRACE 外全部 / 详细 = 全部 / 单级别精确匹配）；
//   · VT 式操作行（BeginOperation + CompleteOperation 在同一行尾补 OK/Error）；
//   · 版本信息块（VersionHeader / VersionRow / VersionDone）；
//   · 主题切换重绘（段落里的画刷是渲染时定格的，必须整篇重画）。
//
//  线程安全：append* 可从任意线程调用；行进入 QMutex 保护的待渲染队列，
//  由 UI 线程的 80ms QTimer 取走并画到 QTextDocument。
// ─────────────────────────────────────────────────────────────────────────────

#include <QAtomicInteger>
#include <QColor>
#include <QDateTime>
#include <QMutex>
#include <QVector>
#include <QWidget>

#include <functional>

class QTextEdit;
class QTextCursor;
class QTimer;

namespace kc::ui {

/// 日志级别（对应 C# 的级别名）。
enum class LogLevel {
    Trace,    // 详细（引擎内部细节，默认档位隐藏）
    Info,
    Ok,
    Warning,
    Error,
    Section,  // 区段标题（两侧带横线的居中蓝字）
    Footer,   // 收尾统计（右对齐蓝字）
};

class LogView : public QWidget {
    Q_OBJECT

public:
    explicit LogView(QWidget* parent = nullptr);
    ~LogView() override;

    /// 一批日志行渲染完成后触发（参数 = 这批的行数），供页面刷新行数徽章。
    /// 一批只回调一次（原实现一行一次：N 行 = N 次文本测量 + 布局失效）。
    using AppendedFn = std::function<void(int)>;

    // ── 追加接口（线程安全）──────────────────────────────────────────────
    void appendLine(LogLevel level, const QString& text);
    void appendInfo(const QString& text) { appendLine(LogLevel::Info, text); }
    void appendSuccess(const QString& text) { appendLine(LogLevel::Ok, text); }
    void appendWarning(const QString& text) { appendLine(LogLevel::Warning, text); }
    void appendError(const QString& text) { appendLine(LogLevel::Error, text); }
    void appendDetail(const QString& text) { appendLine(LogLevel::Trace, text); }
    void appendSection(const QString& title) { appendLine(LogLevel::Section, title); }
    void appendFooter(const QString& text) { appendLine(LogLevel::Footer, text); }

    /// VT 式操作行：起一行等待状态后缀，随后用 appendOperation(label, ok) 收尾。
    void beginOperation(const QString& body);
    /// 给最近一行未完结的操作行追加 OK / Error 后缀。
    void appendOperation(const QString& body, bool ok);

    /// 版本信息块（rows = 名称/值对）。空名称行用作块标题。
    using VersionRow = QPair<QString, QString>;
    void appendVersionBlock(const QVector<VersionRow>& rows, const QString& summary = QString());

    /// 清空（历史 + 界面）。清空按钮必须走这里，否则历史残留会「复活」旧日志。
    void clear();

    // ── 筛选 / 显示控制 ──────────────────────────────────────────────────
    /// 按级别过滤。nullopt = 全部；否则只留该级别。
    void setFilter(LogLevel level);
    void clearFilter(); // 「详细」= 全部显示

    /// 「常规」档位：显示除 TRACE 详细行之外的一切。
    void setNormalFilter();

    /// 详细行是否可见（便捷开关 = 常规档 vs 详细档）。
    void setDebugVisible(bool visible);

    /// 全局「详细」档：控制中心的「日志详细」磁贴拨一下，**所有活着的日志视图**
    /// 一起切档（EDL 页、安卓工具页、应用管理页各有自己的 LogView）。
    /// 值同时落到 QSettings 的 log/debugVisible，新建的 LogView 构造时按它初始化。
    static void setGlobalDebugVisible(bool visible);
    static bool globalDebugVisible();

    /// 自动滚到底。用户往上翻看历史时关掉，新日志就不会再把视口拽走。
    void setAutoScroll(bool enabled) { m_autoScroll.storeRelease(enabled ? 1 : 0); }
    bool autoScroll() const { return m_autoScroll.loadAcquire() != 0; }

    /// 导出纯文本（复制全部用）。
    QString toPlainText();

    /// 主题切换：把所有日志整篇重画（画刷是渲染时定格的，不重画会留在旧主题）。
    void recolor();

    /// 底层 QTextEdit（供外部挂右键菜单等）。不转移所有权。
    QTextEdit* textEdit() const { return m_edit; }

    void setAppendedCallback(AppendedFn fn) { m_onAppended = std::move(fn); }

    /// 日志保留的最大行数：超出后从最旧的丢。
    static constexpr int MaxLines = 2000;
    /// 批量渲染间隔。
    static constexpr int FlushIntervalMs = 80;

private:
    /// 一行日志的结构化记录（只记字段，不记渲染结果 —— 筛选取时整篇重放）。
    struct Line {
        QDateTime time;
        LogLevel level = LogLevel::Info;
        QString body;
        QString bodySuffix;   // 版本行的值
        bool dim = false;                 // 整行压暗（详细行）
        bool operationStyle = false;      // 无级别徽章
        bool nameValue = false;           // 版本行（名称加粗）
        bool colorizeBody = false;        // 正文用级别色
        bool awaitingStatus = false;      // 等 OK/Error 后缀
        bool hasStatus = false;
        QString statusText;
        bool statusOk = false;
        /// 本行操作的耗时（毫秒）；-1 = 不是一次「beginOperation→appendOperation」配对。
        /// 只有配对的收尾行才有值 —— 这是日志里最被需要的那个数字。
        qint64 durationMs = -1;
        // 已渲染的段落位置（补后缀时直接定位；重建后失效则退回线性扫）。
        int documentPosition = -1;
    };

    void buildUi();
    void enqueue(const Line& line);            // 判筛选 + 入队（线程安全）
    void scheduleFlush();
    void flush();                              // UI 线程定时器回调
    bool matches(const Line& line) const;      // 是否通过当前筛选
    void renderLine(const Line& line);         // 画一行到文档
    void rebuild();                            // 整篇重放
    void trimDocument();
    void attachStatus(int pos, const Line& line);

    /// appendOperation 跨线程送来的收尾请求，由 flush 在 UI 线程消化。
    struct PendingOpStatus {
        QString body;
        bool ok = false;
        qint64 durationMs = -1;
    };
    /// 把「等待中」那行的 `....` 就地换成 `  OK (0.8s)`（UI 线程）。
    /// 成功返回 true；该行还没画到文档上（或被裁掉 / 被筛掉）返回 false，
    /// 调用方退化为「新增一行完成行」。
    bool patchOpenOperation(const PendingOpStatus& op);

    QTextEdit* m_edit = nullptr;
    QTimer* m_flushTimer = nullptr;

    // 待渲染队列 + 历史（都由 m_mutex 保护）。
    mutable QMutex m_mutex;
    QVector<Line> m_pending;
    QVector<Line> m_history;
    /// 待补后缀的收尾请求（跨线程写入，flush 在 UI 线程消化）。
    QVector<PendingOpStatus> m_pendingOpStatus;

    // 筛选：-1 = 全部；否则精确匹配某个 level 的 int 值。TRACE 隐藏由 m_debugVisible 控制。
    int m_filter = -1;
    bool m_debugVisible = true;

    QAtomicInt m_autoScroll{1};
    int m_trimSlack = 64;
    AppendedFn m_onAppended;

    /// 上一条**已渲染**行的时间戳，用来算行间隔（+2.1s）。
    /// 重放 / 清空时归零 —— 间隔是「屏幕上看得到的行距」，跟着渲染走而不是跟着历史走。
    qint64 m_lastRenderedMs = 0;
    /// beginOperation 的起始毫秒数，供 appendOperation 算耗时（跨线程，走原子）。
    QAtomicInteger<qint64> m_opStartedMs{0};
    /// 「等待中」那行在文档里的块起点（UI 线程）。收尾时用它定位、把 `....`
    /// 原地换成 `OK` —— 这样一次操作在日志里只占**一行**。
    /// -1 = 当前没有已渲染的等待行（还没画 / 已收尾 / 刚重建过）。
    int m_openOpBlockPos = -1;
};

} // namespace kc::ui
