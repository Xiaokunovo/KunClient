#include "ui/MainWindow.h"

#include "kc/devices/StorageBrandRunner.h"

#include "app/EdlWorker.h"
#include "core/EdlMaintenance.h"
#include "core/PartitionProtection.h"
#include "kc/config/AppStatePaths.h"
#include "kc/config/ClientConfig.h"
#include "oplus/OplusVipSession.h"
#include "storage/DynamicSuper.h"
#include "storage/RawProgramParser.h"
#include "storage/SparseImage.h"

#include <QAbstractItemView>
#include <QListView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QShowEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include "ui/dialogs/AnimatedDialog.h"
#include "ui/dialogs/EraseConfirmDialog.h"
#include "ui/dialogs/ThemedMessageBox.h"
#include "ui/pages/XiaomiEdlPage.h"
#include "ui/ShellStatus.h"
#include "ui/widgets/LogView.h"
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScreen>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizeGrip>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QStyle>
#include <QWindow>

#include <functional>
#include <memory>

#include <cstdio>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <limits>
#include <utility>

// 🔴 本文件整体在 namespace swt::edl 里（swt-edl 移植段），而统一弹窗在 kc::ui。
//    用 using 声明把它拉进本文件作用域，调用点就不必满屏写 kc::ui:: 前缀。
using kc::ui::ThemedMessageBox;

namespace swt::edl {
namespace {

/// 后台任务包装（本文件用：云端引导库索引 / 下载）。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

/// 「芯片 / 引导程序」下拉的行高压缩（2026-10-18）。
///
/// 云端引导库 249 款全平铺在这个下拉里，而 Windows 11 风格给条目留的行高约
/// 42px —— 一屏只放得下 17 行，滚到「努比亚」要滑十几屏。
/// 🔴 QSS 压不下去：`QComboBox#chipsetCombo QAbstractItemView::item { min-height }`
///    只是**下限**，样式自算的 42px 比它大，规则等于没写（实测仍 42px/行）。
///    所以改用 delegate 直接给 sizeHint 定死高度 —— 这是唯一能真正压下来的口子。
/// 宽度仍取基类结果：combo 弹层宽度是按「最宽条目」算的，写 0 会让弹层缩成
/// 一条缝（长文件名全被截断）。
class CompactRowDelegate final : public QStyledItemDelegate {
public:
    explicit CompactRowDelegate(int rowHeight, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_rowHeight(rowHeight)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(m_rowHeight);
        return size;
    }

private:
    int m_rowHeight = 24;
};

class WindowTitleBar final : public QFrame {
public:
    explicit WindowTitleBar(QMainWindow* window)
        : QFrame(window), m_window(window)
    {
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_window && m_window->windowHandle()) {
            m_window->windowHandle()->startSystemMove();
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_window) {
            m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized();
            event->accept();
            return;
        }
        QFrame::mouseDoubleClickEvent(event);
    }

private:
    QMainWindow* m_window = nullptr;
};

QWidget* makeProgressCell(QWidget* parent, QProgressBar** bar, QLabel** label,
                          const QString& initialText)
{
    auto* cell = new QWidget(parent);
    cell->setObjectName(QStringLiteral("progressCell"));
    auto* stack = new QGridLayout(cell);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);
    *bar = new QProgressBar(cell);
    (*bar)->setRange(0, 1000);
    (*bar)->setValue(0);
    (*bar)->setTextVisible(false);
    *label = new QLabel(initialText, cell);
    (*label)->setObjectName(QStringLiteral("progressOverlay"));
    (*label)->setAlignment(Qt::AlignCenter);
    (*label)->setAttribute(Qt::WA_TransparentForMouseEvents);
    stack->addWidget(*bar, 0, 0);
    stack->addWidget(*label, 0, 0);
    return cell;
}

enum class RawprogramXmlKind {
    Other,
    Standard,
    Nonstandard
};

struct RawprogramXmlName {
    RawprogramXmlKind kind = RawprogramXmlKind::Other;
    QString family;
    int index = -1;
};

RawprogramXmlName parseRawprogramXmlName(const QString& fileName)
{
    static const QRegularExpression standard(
        QStringLiteral("^(rawprogram)([0-9]+)\\.xml$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nonstandard(
        QStringLiteral("^(rawprogram_.+?)([0-9]+)\\.xml$"),
        QRegularExpression::CaseInsensitiveOption);
    auto match = standard.match(fileName);
    if (match.hasMatch()) {
        return {RawprogramXmlKind::Standard, match.captured(1).toLower(),
                match.captured(2).toInt()};
    }
    match = nonstandard.match(fileName);
    if (match.hasMatch()) {
        return {RawprogramXmlKind::Nonstandard, match.captured(1).toLower(),
                match.captured(2).toInt()};
    }
    return {};
}

QString rawprogramXmlFilters()
{
    // 「全部」放在第一条：QFileDialog::getOpenFileNames 默认用第一个过滤器，
    // 只有它能一次看到 rawprogram0.xml 与 rawprogram_xxx0.xml，用户才能混选两套方案。
    return QStringLiteral(
        "全部 rawprogram XML (rawprogram*.xml);;"
        "标准 XML (rawprogram?.xml rawprogram??.xml rawprogram???.xml);;"
        "非标准 XML (rawprogram_*.xml);;"
        "所有文件 (*)");
}

/// 分类用的宽松判定：`parseRawprogramXmlName` 要求名字末尾带索引
/// （`rawprogram_unsparse0.xml`），但真实包里也有 `rawprogram_unsparse.xml`
/// 这种缺索引的写法 —— 它若被判成 Other，混选检测与类别显示会整块失效。
/// 注意：**只用于分类**，兄弟文件展开仍走 `parseRawprogramXmlName`，
/// 否则会把同族的两个变体互相拖进来。
RawprogramXmlKind xmlKindForCategory(const QString& fileName)
{
    const RawprogramXmlKind strict = parseRawprogramXmlName(fileName).kind;
    if (strict != RawprogramXmlKind::Other) return strict;
    static const QRegularExpression loose(QStringLiteral("^rawprogram_.+\\.xml$"),
                                         QRegularExpression::CaseInsensitiveOption);
    return loose.match(fileName).hasMatch() ? RawprogramXmlKind::Nonstandard
                                            : RawprogramXmlKind::Other;
}

/// 同时出现标准与非标准两套 XML 时为 true。
bool isMixedXmlSelection(const QStringList& paths)
{
    bool standard = false;
    bool nonstandard = false;
    for (const QString& path : paths) {
        const RawprogramXmlKind kind = xmlKindForCategory(QFileInfo(path).fileName());
        standard = standard || kind == RawprogramXmlKind::Standard;
        nonstandard = nonstandard || kind == RawprogramXmlKind::Nonstandard;
    }
    return standard && nonstandard;
}

QString xmlCategoryName(const QStringList& paths)
{
    bool standard = false;
    bool nonstandard = false;
    for (const QString& path : paths) {
        const RawprogramXmlKind kind = xmlKindForCategory(QFileInfo(path).fileName());
        standard = standard || kind == RawprogramXmlKind::Standard;
        nonstandard = nonstandard || kind == RawprogramXmlKind::Nonstandard;
    }
    if (standard && nonstandard) return QStringLiteral("标准 + 非标准 XML");
    if (nonstandard) return QStringLiteral("非标准 XML");
    if (standard) return QStringLiteral("标准 XML");
    return QStringLiteral("XML");
}

/// 日志里给「族」一个人话名字：标准 rawprogram / 非标准 rawprogram_unsparse。
QString xmlFamilyKey(const QString& fileName)
{
    const RawprogramXmlKind kind = xmlKindForCategory(fileName);
    if (kind == RawprogramXmlKind::Other) return QStringLiteral("其他 XML");
    QString family = parseRawprogramXmlName(fileName).family;
    if (family.isEmpty()) {
        // 缺索引的非标准名（rawprogram_ufs.xml）：去掉 .xml 与末尾数字当族名。
        family = fileName;
        if (family.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) family.chop(4);
        while (!family.isEmpty() && family.back().isDigit()) family.chop(1);
    }
    return QStringLiteral("%1 %2")
        .arg(kind == RawprogramXmlKind::Standard ? QStringLiteral("标准")
                                                : QStringLiteral("非标准"),
             family);
}

/// 混选（标准 + 非标准 XML）时的重复目标检查。
///
/// 两套 XML 常指向同一批分区：同一个 LUN/起始扇区被写两次时，后写的覆盖先写的；
/// 若两份 XML 引用的是不同镜像，最终落盘的是列表靠后的那一份 —— 这类结果用户
/// 无法从「刷写成功」的提示里看出来，必须先摆到台面上。
/// 返回空 = 没有需要确认的重复；非空 = 逐条可展示的说明（最多 20 条）。
QStringList xmlMixConflictLines(const RawProgramPlan& plan)
{
    if (!isMixedXmlSelection(plan.xmlPaths)) return {};

    const auto describe = [](const RawProgramEntry& entry) {
        return QStringLiteral("%1（LUN %2，%3）")
            .arg(QFileInfo(entry.xmlPath).fileName())
            .arg(entry.lun)
            .arg(entry.label.isEmpty() ? entry.filename : entry.label);
    };

    QStringList lines;
    QHash<QString, const RawProgramEntry*> byTarget;
    QHash<QString, const RawProgramEntry*> byLabel;
    for (const RawProgramEntry& entry : plan.entries) {
        if (entry.startSectorResolved) {
            const QString key = QStringLiteral("%1/%2").arg(entry.lun).arg(entry.startSector);
            auto it = byTarget.constFind(key);
            if (it == byTarget.constEnd()) {
                byTarget.insert(key, &entry);
            } else if (it.value()->xmlPath.compare(entry.xmlPath, Qt::CaseInsensitive) != 0) {
                const RawProgramEntry& first = *it.value();
                const bool identical = first.sectorCount == entry.sectorCount &&
                    first.sourcePath.compare(entry.sourcePath, Qt::CaseInsensitive) == 0;
                lines.push_back(identical
                    ? QStringLiteral("LUN %1 起始扇区 %2 重复（同一镜像写两遍）：%3 ↔ %4")
                          .arg(entry.lun).arg(entry.startSector)
                          .arg(describe(first), describe(entry))
                    : QStringLiteral("LUN %1 起始扇区 %2 目标重叠，后写的会覆盖先写的：%3 ↔ %4")
                          .arg(entry.lun).arg(entry.startSector)
                          .arg(describe(first), describe(entry)));
            }
        }
        if (!entry.label.isEmpty()) {
            const QString key = entry.label.toLower();
            auto it = byLabel.constFind(key);
            if (it == byLabel.constEnd()) {
                byLabel.insert(key, &entry);
            } else if (it.value()->xmlPath.compare(entry.xmlPath, Qt::CaseInsensitive) != 0 &&
                       (it.value()->lun != entry.lun ||
                        it.value()->startSector != entry.startSector)) {
                // 同目标同 label 已在上面按「重复目标」报过，这里只补「同名不同位」。
                lines.push_back(QStringLiteral("分区 %1 在两份 XML 中重复定义：%2 ↔ %3")
                                    .arg(entry.label, describe(*it.value()), describe(entry)));
            }
        }
    }

    if (lines.size() > 20) {
        const int hidden = lines.size() - 20;
        lines = lines.mid(0, 20);
        lines.push_back(QStringLiteral("……另有 %1 项重复未列出（可在「预检刷写计划」里看全量明细）")
                            .arg(hidden));
    }
    return lines;
}

/// 混选说明块：直接拼进「确认 XML 刷机」的汇总文本，避免连续弹两个确认框。
QString xmlMixSummaryBlock(const RawProgramPlan& plan)
{
    if (!isMixedXmlSelection(plan.xmlPaths)) return {};
    const QStringList conflicts = xmlMixConflictLines(plan);
    if (conflicts.isEmpty()) {
        return QStringLiteral("\n\n已混选标准 + 非标准 XML（%1 个文件），未发现重复目标。")
            .arg(plan.xmlPaths.size());
    }
    return QStringLiteral(
        "\n\n【混选重复目标】标准 + 非标准 XML 同时指向同一 LUN/起始扇区 %1 处，"
        "后写的会覆盖先写的；两份 XML 引用不同镜像时，最终落盘的是列表靠后的那一份：\n%2")
        .arg(conflicts.size())
        .arg(conflicts.join(QLatin1Char('\n')));
}

QStringList expandRawprogramXmlSelection(const QStringList& selectedPaths)
{
    QStringList paths;
    for (const QString& selected : selectedPaths) {
        const QFileInfo selectedInfo(selected);
        const QString absolutePath = selectedInfo.absoluteFilePath();
        if (!paths.contains(absolutePath, Qt::CaseInsensitive)) paths.push_back(absolutePath);
        const RawprogramXmlName selectedName = parseRawprogramXmlName(selectedInfo.fileName());
        if (selectedName.kind == RawprogramXmlKind::Other) continue;
        const QFileInfoList siblings = selectedInfo.dir().entryInfoList(
            {QStringLiteral("rawprogram*.xml")}, QDir::Files, QDir::Name);
        for (const QFileInfo& siblingInfo : siblings) {
            const RawprogramXmlName siblingName = parseRawprogramXmlName(siblingInfo.fileName());
            if (siblingName.kind != selectedName.kind ||
                siblingName.family.compare(selectedName.family, Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QString sibling = siblingInfo.absoluteFilePath();
            if (!paths.contains(sibling, Qt::CaseInsensitive)) paths.push_back(sibling);
        }
    }
    std::sort(paths.begin(), paths.end(), [](const QString& left, const QString& right) {
        const RawprogramXmlName leftName = parseRawprogramXmlName(QFileInfo(left).fileName());
        const RawprogramXmlName rightName = parseRawprogramXmlName(QFileInfo(right).fileName());
        const int familyOrder = leftName.family.compare(rightName.family, Qt::CaseInsensitive);
        if (familyOrder != 0) return familyOrder < 0;
        if (leftName.index >= 0 && rightName.index >= 0 &&
            leftName.index != rightName.index) {
            return leftName.index < rightName.index;
        }
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    return paths;
}

// ─────────────────────────────────────────────────────────────────────────────
//  刷写日志 → LogView 级别映射
//
//  EdlWorker 吐的是纯文本行（没有级别字段），级别只能从语料推断。判据与原
//  formatLogHtml 的配色规则一一对应，只是把「上哪种颜色」改成「归哪一级」：
//    · 错误 / 失败          → Error（整行着色 + ERROR 徽章）
//    · 完成 / 成功 / 就绪    → Ok（OK 徽章）
//    · 其余（含「正在…」）    → Info（正文色、无徽章 —— 进度行挂 WARN 徽章
//                              反而像告警；与 XiaomiFastbootPage 的 onLine
//                              分类口径保持一致）
//    · Firehose 高频协议流水 → Trace（压暗，仅「详细」档可见）
// ─────────────────────────────────────────────────────────────────────────────

/// 是否属于「常规」档要隐藏的高频协议流水（原 shouldShowLog 的否定集）。
bool isEdlProtocolTrace(const QString& message)
{
    if (message.startsWith(QStringLiteral("Firehose: INFO:"))) return true;
    if (message.startsWith(QStringLiteral("发送 Firehose 命令："))) return true;
    if (message.startsWith(QStringLiteral("VIP READ："))) return true;
    if (message.startsWith(QStringLiteral("VIP PROGRAM："))) return true;
    if (message.startsWith(QStringLiteral("VIP ERASE："))) return true;
    if (message.startsWith(QStringLiteral("[Sahara][DEBUG]"))) return true;
    if (message.startsWith(QStringLiteral("Firehose: ERROR: operation 0 on")) &&
        message.contains(QStringLiteral("is forbidden"), Qt::CaseInsensitive)) {
        return true;
    }
    if (message.contains(QStringLiteral("not get permission,line="), Qt::CaseInsensitive)) {
        return true;
    }
    if (message.contains(QStringLiteral("begin boot log"), Qt::CaseInsensitive) ||
        message.contains(QStringLiteral("end boot log"), Qt::CaseInsensitive) ||
        (message.startsWith(QStringLiteral("Firehose: ERROR: B -")) &&
         message.contains(QStringLiteral("usb: init start"), Qt::CaseInsensitive))) {
        return true;
    }
    // 部分 OPPO programmer 在后续 verify/configure 成功时也会吐这些自相矛盾的
    // 内部诊断 —— 留在「详细」档给开发者，常规档不吓用户。
    if (message.contains(QStringLiteral("DevprgRSAVerify verify signature"))) return true;
    if (message.contains(QStringLiteral("SpmiBus_ReadLong"))) return true;
    if (message.contains(QStringLiteral("devprg_pm_charger_set"))) return true;

    // ── 引擎自己的握手细节（2026-10-18「日志有点杂」）──────────────────────
    // 下面这些**不是**协议原文，是 EdlWorker 在握手阶段打的进度说明。它们对
    // 排障有用（"卡在 Sahara 还是 Configure"一眼能看出来），但常规档里一次连接
    // 就刷出十几行，把真正的关键行（设备信息 / 分区表 / 写入结果）淹掉了。
    // 统一降到详细档：常规档一次连接只剩 3~4 行，详细档仍是一字不少。
    if (message.startsWith(QStringLiteral("串口配置："))) return true;
    if (message.startsWith(QStringLiteral("串口重连参数："))) return true;
    if (message.startsWith(QStringLiteral("串口已按 Sahara 参数重新打开："))) return true;
    if (message.startsWith(QStringLiteral("串口已重开："))) return true;
    if (message.startsWith(QStringLiteral("检测设备当前通信状态"))) return true;
    if (message.startsWith(QStringLiteral("状态检测："))) return true;
    if (message.startsWith(QStringLiteral("Firehose 探测未响应"))) return true;
    if (message.startsWith(QStringLiteral("Firehose 存储尚未就绪"))) return true;
    if (message.startsWith(QStringLiteral("Firehose 存储已配置"))) return true;
    if (message.startsWith(QStringLiteral("存储信息查询跳过："))) return true;
    if (message.startsWith(QStringLiteral("Sahara 交接"))) return true;
    if (message.startsWith(QStringLiteral("Sahara 自动切换候选 Programmer"))) return true;
    if (message.contains(QStringLiteral("容量已由 getstorageinfo 解析"))) return true;
    // 「解析 boot 版本信息…」这类逐分区进度：一行一个分区，10 个分区就是 10 行噪声。
    if (message.startsWith(QStringLiteral("解析 ")) &&
        message.endsWith(QStringLiteral("版本信息…"))) return true;
    if (message.startsWith(QStringLiteral("解析 super 动态分区元数据"))) return true;
    if (message.startsWith(QStringLiteral("读取 LUN ")) &&
        message.endsWith(QStringLiteral("主 GPT"))) return true;
    if (message.startsWith(QStringLiteral("Patch："))) return true;
    // super 的子镜像 / 分段细节：外层已经有一条操作行（刷写 super），
    // 内层再逐段报一遍只是把 12 行的日志撑成 30 行。
    if (message.startsWith(QStringLiteral("super ")) &&
        message.contains(QStringLiteral(" ← "))) return true;
    if (message.startsWith(QStringLiteral("写入 super metadata："))) return true;
    return false;
}

kc::ui::LogLevel edlLogLevel(const QString& message)
{
    if (isEdlProtocolTrace(message)) return kc::ui::LogLevel::Trace;
    if (message.contains(QStringLiteral("错误")) ||
        message.contains(QStringLiteral("失败")) ||
        message.startsWith(QStringLiteral("Firehose: ERROR"))) {
        return kc::ui::LogLevel::Error;
    }
    // 区段标题：一次刷机的起点，居中带横线，翻日志时一眼能定位「这一批从哪开始」。
    if (message.startsWith(QStringLiteral("开始刷写"))) return kc::ui::LogLevel::Section;
    // 收尾统计（右对齐）：`▪ XML 刷机完成：12 个 program 条目`。
    if (message.startsWith(QStringLiteral("XML 刷机完成")) ||
        message.startsWith(QStringLiteral("批量写入完成")) ||
        message.startsWith(QStringLiteral("批量擦除完成")) ||
        message.startsWith(QStringLiteral("回读任务完成"))) {
        return kc::ui::LogLevel::Footer;
    }
    // 「跳过 / 未确认 / 不完整」是**需要看见但不算失败**的情况：保护分区被跳过、
    // 启动分区设置没被确认、签名表只下到一半。原来是 Info 灰字，混在正常行里
    // 根本注意不到 —— 统一升成警告色。
    if (message.contains(QStringLiteral("跳过")) ||
        message.contains(QStringLiteral("未确认")) ||
        message.contains(QStringLiteral("不完整")) ||
        message.contains(QStringLiteral("【注意】"))) {
        return kc::ui::LogLevel::Warning;
    }
    if (message.contains(QStringLiteral("完成")) ||
        message.contains(QStringLiteral("成功")) ||
        message.contains(QStringLiteral("就绪"))) {
        return kc::ui::LogLevel::Ok;
    }
    return kc::ui::LogLevel::Info;
}

} // namespace

MainWindow::MainWindow(QWidget* parent, bool embedded)
    : QMainWindow(parent)
    , m_embedded(embedded)
{
    setWindowTitle(QStringLiteral("KunClient · 高通 9008 刷机工具"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1152, 748);
    // 🔴 内嵌到 EdlFlashPage 时**不能**要最小尺寸（2026-10-17 用户截图）：
    //    外壳内容区在 1300×730 这种窗口下只剩 ~1280×640，而 1024×700 的最小高度
    //    会把内嵌窗顶出页面下沿（底部状态栏被裁一半），宽度方向也让右侧卡片溢出、
    //    文字被切在窗口边上。独立窗口（embedded=false）才需要这个下限，
    //    免得用户把窗口拖到放不下控件。
    if (!embedded) setMinimumSize(1024, 700);

    auto* central = new QFrame(this);
    central->setObjectName(QStringLiteral("root"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    m_headerWidget = buildHeader();
    root->addWidget(m_headerWidget);

    auto* content = new QWidget(central);
    content->setObjectName(QStringLiteral("contentArea"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(10, 8, 10, 0);
    contentLayout->setSpacing(0);
    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setChildrenCollapsible(false);
    const qint64 tCtor = QDateTime::currentMSecsSinceEpoch();
    splitter->addWidget(buildLeftCard());
    const qint64 tLeft = QDateTime::currentMSecsSinceEpoch();
    splitter->addWidget(buildRightCard());
    const qint64 tRight = QDateTime::currentMSecsSinceEpoch();
    splitter->setHandleWidth(10);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({554, 565});
    contentLayout->addWidget(splitter);
    root->addWidget(content, 1);

    auto* footerFrame = new QFrame(central);
    footerFrame->setObjectName(QStringLiteral("footer"));
    footerFrame->setFixedHeight(33);
    m_footerFrame = footerFrame;
    auto* footer = new QHBoxLayout(footerFrame);
    footer->setContentsMargins(14, 0, 13, 0);
    auto* hint = new QLabel(QStringLiteral("遇到长时间无响应时，请耐心等待，避免重复操作。"), footerFrame);
    hint->setObjectName(QStringLiteral("footerWarning"));
    m_protocolStatus = new QLabel(QStringLiteral("协议通信状态：正常"), footerFrame);
    m_protocolStatus->setObjectName(QStringLiteral("protocolStatus"));
    m_bottomStatus = new QLabel(QStringLiteral("●  等待连接"), footerFrame);
    m_bottomStatus->setObjectName(QStringLiteral("footerStatus"));
    footer->addWidget(hint);
    footer->addStretch();
    footer->addWidget(m_protocolStatus);
    footer->addSpacing(40);
    footer->addWidget(m_bottomStatus);
    root->addWidget(footerFrame);

    setCentralWidget(central);

    m_autoScanTimer = new QTimer(this);
    m_autoScanTimer->setInterval(1500);
    connect(m_autoScanTimer, &QTimer::timeout, this, &MainWindow::refreshPorts);
    connect(m_autoScanCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("connection/autoScan"), checked);
        if (checked) {
            refreshPorts();
            m_autoScanTimer->start();
        } else {
            m_autoScanTimer->stop();
        }
    });

    m_progressAnimationTimer = new QTimer(this);
    m_progressAnimationTimer->setInterval(16);
    m_progressAnimationTimer->setTimerType(Qt::PreciseTimer);
    connect(m_progressAnimationTimer, &QTimer::timeout, this, [this] {
        auto advance = [](QProgressBar* bar, int target) {
            const int current = bar->value();
            const int delta = target - current;
            if (delta == 0) return true;
            if (delta < 0) {
                bar->setValue(target);
                return true;
            }
            const int step = qMax(1, (delta + 5) / 6);
            bar->setValue(qMin(target, current + step));
            return bar->value() == target;
        };
        const bool taskDone = advance(m_progressBar, m_taskProgressTarget);
        const bool overallDone = advance(m_totalProgressBar, m_overallProgressTarget);
        m_progressText->setText(QStringLiteral("当前任务  %1%")
                                    .arg(m_progressBar->value() / 10.0, 0, 'f', 1));
        m_totalProgressText->setText(QStringLiteral("总体进度  %1%")
                                         .arg(m_totalProgressBar->value() / 10.0,
                                              0, 'f', 1));
        if (taskDone && overallDone) m_progressAnimationTimer->stop();
    });

    // 主题初始化分两种宿主：
    //  · 独立运行（SWT-EDL 原用法）：读 QSettings，默认深色，自己刷全局样式表；
    //  · 嵌入 KunClient MainShell：**ctor 什么都不做** —— 全局样式表与持久化
    //    都归外壳（ThemeManager）。嵌入窗若在这里按自己的默认值（深色）刷样式表，
    //    会在 registerPages() 期间把整个应用翻成深色、并把 darkTheme=true 写进
    //    QSettings，之后外壳读到的就是深色 —— 曾造成「浅色主题下 EDL 日志一片
    //    发白看不见」（日志色板与外壳主题脱节）。外壳构造完后会立刻调
    //    applyTheme() 同步（见 EdlFlashPage）。
    if (!m_embedded) {
        QSettings settings;
        m_autoScanCheck->setChecked(
            settings.value(QStringLiteral("connection/autoScan"), true).toBool());
        m_autoRebootCheck->setChecked(
            settings.value(QStringLiteral("operation/autoReboot"), false).toBool());
        m_protectPartitionsCheck->setChecked(
            settings.value(QStringLiteral("operation/protectPartitions"), true).toBool());
        m_readInfoCheck->setChecked(
            settings.value(QStringLiteral("operation/readVersionInfo"), true).toBool());
        applyTheme(settings.value(QStringLiteral("appearance/darkTheme"), false).toBool());
    }

    m_worker = new EdlWorker;
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &MainWindow::connectRequested, m_worker, &EdlWorker::connectDevice);
    connect(this, &MainWindow::disconnectRequested, m_worker, &EdlWorker::disconnectDevice);
    connect(this, &MainWindow::readGptRequested, m_worker, &EdlWorker::readPartitionTable);
    connect(this, &MainWindow::readVersionInfoRequested,
            m_worker, &EdlWorker::readSystemVersionInfo);
    connect(this, &MainWindow::backupPartitionsRequested, m_worker, &EdlWorker::backupPartitions);
    connect(this, &MainWindow::flashPartitionsRequested, m_worker, &EdlWorker::flashPartitions);
    connect(this, &MainWindow::flashXmlRequested, m_worker, &EdlWorker::flashXml);
    connect(this, &MainWindow::erasePartitionsRequested, m_worker, &EdlWorker::erasePartitions);
    connect(this, &MainWindow::rebootRequested, m_worker, &EdlWorker::rebootDevice);
    connect(this, &MainWindow::rebootWithBcbRequested, m_worker, &EdlWorker::rebootWithBcb);
    connect(this, &MainWindow::factoryResetRequested, m_worker, &EdlWorker::factoryReset);
    connect(this, &MainWindow::enableOemUnlockRequested, m_worker, &EdlWorker::enableOemUnlock);
    connect(this, &MainWindow::backupBasebandRequested, m_worker,
            &EdlWorker::backupBasebandPartitions);
    connect(this, &MainWindow::readStorageInfoRequested, m_worker,
            &EdlWorker::readStorageInfo);
    connect(this, &MainWindow::eraseLunRequested, m_worker, &EdlWorker::eraseLun);
    connect(this, &MainWindow::writeGptRequested, m_worker, &EdlWorker::writeGptFile);
    connect(this, &MainWindow::readGptToFileRequested, m_worker, &EdlWorker::readGptFile);
    connect(this, &MainWindow::stopRequested, m_worker, &EdlWorker::requestStop,
            Qt::DirectConnection);

    connect(m_worker, &EdlWorker::logMessage, this, &MainWindow::appendLog,
            Qt::QueuedConnection);
    // TMF 风格操作行：开始一行 → 完成后补 OK/Error + 耗时（LogView 内部按配对计时）。
    connect(m_worker, &EdlWorker::logOperationBegin, this,
            [this](const QString& body) {
                if (m_logView) m_logView->beginOperation(body);
            },
            Qt::QueuedConnection);
    connect(m_worker, &EdlWorker::logOperationEnd, this,
            [this](const QString& body, bool ok) {
                if (m_logView) m_logView->appendOperation(body, ok);
            },
            Qt::QueuedConnection);
    connect(m_worker, &EdlWorker::stateChanged, this, &MainWindow::updateState);
    connect(m_worker, &EdlWorker::progressChanged, this, &MainWindow::updateProgress);
    connect(m_worker, &EdlWorker::overallProgressChanged,
            this, &MainWindow::updateOverallProgress);
    connect(m_worker, &EdlWorker::deviceInfoChanged, this, &MainWindow::updateDeviceInfo);
    connect(m_worker, &EdlWorker::partitionsReady, this, &MainWindow::showPartitions);
    connect(m_worker, &EdlWorker::operationFinished, this, &MainWindow::operationFinished);
    m_workerThread.start();

    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        appendLog(QStringLiteral("正在停止当前任务…"));
        emit stopRequested();
    });

    // 顺序有意：先报「引擎就绪」再枚举串口。反过来的话日志第一行永远是
    // 「扫描完成…」，看起来像是先扫了串口才发现引擎在（2026-10-18 实测观感）。
    // 文案压到一行以内：日志栏宽约 500px，原来那句「KunClient EDL 引擎初始化完成 ·
    // 请选择 Qualcomm 9008 串口与 Firehose Programmer」会折成两行，开屏就显乱。
    appendLog(QStringLiteral("EDL 引擎就绪 · 请选择 9008 串口与 Firehose 引导程序"));
    refreshPorts();
    setBusy(false);

    // 云端引导库的索引预取**不在构造函数里**发，放在 showEvent —— 见那里的注释。

    // 构造耗时探针（KC_TIMING=1）：内嵌 EDL 窗是冷启动最贵的一页（实测 ~1.2s），
    // 分段打出来才知道是「建卡片」还是「枚举串口」在吃时间。
    if (!qEnvironmentVariableIsEmpty("KC_TIMING")) {
        const qint64 tEnd = QDateTime::currentMSecsSinceEpoch();
        std::fprintf(stderr,
                     "[edl] left=%lld ms  right=%lld ms  worker+rest=%lld ms  total=%lld ms\n",
                     static_cast<long long>(tLeft - tCtor),
                     static_cast<long long>(tRight - tLeft),
                     static_cast<long long>(tEnd - tRight),
                     static_cast<long long>(tEnd - tCtor));
        std::fflush(stderr);
    }
}
MainWindow::~MainWindow()
{
    emit stopRequested();
    if (m_workerThread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, "disconnectDevice", Qt::BlockingQueuedConnection);
        m_workerThread.quit();
        m_workerThread.wait(5000);
    }
}

void MainWindow::setEmbeddedMode(bool embedded)
{
    // 嵌入 MainShell 时隐藏自带标题栏：外壳（kc::ui::MainShell）已有全局标题栏，
    // 双标题栏是视觉冗余。独立运行（SWT-EDL 原用法）不受影响。
    if (m_headerWidget) m_headerWidget->setVisible(!embedded);
    // 嵌入时主题按钮一并禁用：它的 applyTheme 只切本窗色板，与外壳脱节会再造一次
    // 「外壳浅色 / 日志深色」的错位。主题切换走外壳的「切换主题」。
    if (m_themeButton) m_themeButton->setEnabled(!embedded);
    // 嵌入时页脚三格（协议 / 连接状态 / 提示文案）整条收起 —— 2026-10-16 用户要求
    // 把这三格并进外壳状态栏（协议 + 状态各占一段，提示文案改跑马灯）。读数改由
    // updateState() 经 ShellStatus::setEdlLink() 上报；独立运行时页脚照旧。
    if (m_footerFrame) m_footerFrame->setVisible(!embedded);
    // 初始读数：本窗页脚一收起，外壳那两段得先有个值，别停在 "-"。
    if (embedded)
        kc::ui::ShellStatus::setEdlLink(QStringLiteral("正常"),
                                        QStringLiteral("●  等待连接"),
                                        QStringLiteral("idle"));
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);

    // 云端引导库：窗口**第一次真正显示**时异步拉一次索引，拉到就追加进
    // 「芯片 / 引导程序」下拉（2026-10-18 用户要的最终形态：条目直接罗列在
    // 下拉里，选中即下载回填）。ensureCloudLoaderList 自身幂等，重复 show 不会重拉。
    //
    // 🔴 放在 showEvent 而不是构造函数：构造函数里挂 singleShot(0) 会让**任何**
    //    建了本窗但从不显示的地方也去发一次 HTTP（单测就是这么用的 ——
    //    EdlMaintenanceUiTest 直接 `swt::edl::MainWindow window;` 不 show）。
    //    实测那样会把 maintenance-ui-tests 从 90s 拖到 11 分钟都跑不完
    //    （网络请求挂在全局线程池里，测试退出时 QThreadPool 析构要等它超时）。
    QTimer::singleShot(0, this, &MainWindow::ensureCloudLoaderList);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_programmerEdit && event->type() == QEvent::MouseButtonDblClick) {
        chooseProgrammer();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    const auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_DEVICECHANGE &&
        !m_connected && m_autoScanCheck && m_autoScanCheck->isChecked()) {
        QTimer::singleShot(120, this, &MainWindow::refreshPorts);
    }
    if (nativeMessage && nativeMessage->message == WM_NCHITTEST && !isMaximized()) {
        constexpr int border = 6;
        const QPoint cursor = mapFromGlobal(QCursor::pos());
        const bool left = cursor.x() >= 0 && cursor.x() < border;
        const bool right = cursor.x() <= width() && cursor.x() > width() - border;
        const bool top = cursor.y() >= 0 && cursor.y() < border;
        const bool bottom = cursor.y() <= height() && cursor.y() > height() - border;
        if (top && left) *result = HTTOPLEFT;
        else if (top && right) *result = HTTOPRIGHT;
        else if (bottom && left) *result = HTBOTTOMLEFT;
        else if (bottom && right) *result = HTBOTTOMRIGHT;
        else if (left) *result = HTLEFT;
        else if (right) *result = HTRIGHT;
        else if (top) *result = HTTOP;
        else if (bottom) *result = HTBOTTOM;
        else return QMainWindow::nativeEvent(eventType, message, result);
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

QWidget* MainWindow::buildHeader() {
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("topArea"));
    auto* column = new QVBoxLayout(header);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    auto* titleBar = new WindowTitleBar(this);
    titleBar->setObjectName(QStringLiteral("titleBar"));
    titleBar->setFixedHeight(40);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(15, 0, 6, 0);
    titleLayout->setSpacing(8);

    auto* badge = new QLabel(QStringLiteral("Q"), titleBar);
    badge->setObjectName(QStringLiteral("appBadge"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(18, 18);
    auto* title = new QLabel(QStringLiteral("KunClient"), titleBar);
    title->setObjectName(QStringLiteral("appTitle"));
    auto* minimize = new QPushButton(QStringLiteral("—"), titleBar);
    minimize->setObjectName(QStringLiteral("windowButton"));
    minimize->setFixedSize(42, 32);
    auto* close = new QPushButton(QStringLiteral("×"), titleBar);
    close->setObjectName(QStringLiteral("closeButton"));
    close->setFixedSize(42, 32);
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(close, &QPushButton::clicked, this, &QWidget::close);

    titleLayout->addWidget(badge);
    titleLayout->addWidget(title);
    titleLayout->addStretch();
    titleLayout->addWidget(minimize);
    titleLayout->addWidget(close);
    column->addWidget(titleBar);

    auto* navigation = new QFrame(header);
    navigation->setObjectName(QStringLiteral("navigation"));
    navigation->setFixedHeight(43);
    auto* navigationLayout = new QHBoxLayout(navigation);
    navigationLayout->setContentsMargins(25, 0, 15, 0);
    navigationLayout->setSpacing(26);
    auto* quick = new QPushButton(QStringLiteral("快速操作"), navigation);
    quick->setObjectName(QStringLiteral("navButton"));
    quick->setProperty("active", true);
    quick->setFixedHeight(42);
    auto* advanced = new QPushButton(QStringLiteral("高级功能"), navigation);
    advanced->setObjectName(QStringLiteral("navButton"));
    advanced->setFixedHeight(42);
    advanced->setEnabled(false);
    advanced->setToolTip(QStringLiteral("当前版本聚焦 EDL 功能"));
    auto* auxiliary = new QPushButton(QStringLiteral("辅助功能"), navigation);
    auxiliary->setObjectName(QStringLiteral("navButton"));
    auxiliary->setFixedHeight(42);
    auxiliary->setEnabled(false);
    auxiliary->setToolTip(QStringLiteral("当前版本聚焦 EDL 功能"));
    m_themeButton = new QPushButton(navigation);
    m_themeButton->setObjectName(QStringLiteral("themeButton"));
    m_themeButton->setCursor(Qt::PointingHandCursor);
    connect(m_themeButton, &QPushButton::clicked, this, [this] {
        applyTheme(!m_darkTheme);
    });
    navigationLayout->addWidget(quick);
    navigationLayout->addWidget(advanced);
    navigationLayout->addWidget(auxiliary);
    navigationLayout->addStretch();
    navigationLayout->addWidget(m_themeButton);
    column->addWidget(navigation);
    return header;
}

QWidget* MainWindow::buildLeftCard()
{
    const auto mark = [](const char* tag) {
        if (qEnvironmentVariableIsEmpty("KC_TIMING")) return;
        static qint64 last = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (last) std::fprintf(stderr, "[edl-left] %5lld ms  %s\n",
                               static_cast<long long>(now - last), tag);
        last = now;
    };
    mark("buildLeftCard 开始");
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(8);

    // 顶部条：左侧「刷机界面」标题（原为页签，本卡只剩一个视图，页签切换失去
    // 意义，保留标题给视觉锚点），右侧是**高级功能下拉框** + LUN 选择。
    //
    // 2026-10-19 用户口径：「把 edl 界面高级功能改成下拉式的选择框，点完这种
    // 独立弹窗的按钮后面就有页面挡住不显示」。原来是「高级功能 ›」按钮 → 弹一个
    // 620 宽的无边框模态窗，窗里再摆七八个按钮。两层弹窗叠着，里面那些按钮
    // （恢复出厂 / 备份基带 / GPT 读写 …）点开确认框时，确认框是挂在**内嵌 EDL 窗**
    // 上的顶层窗，而上面还压着那个 application-modal 的高级功能窗 —— 于是确认框
    // 出在后面，被页面和弹窗一起挡住，用户看到的就是「点了没反应」。
    // 现在把入口改成下拉：选中即执行，确认框直接挂在外壳窗口上，中间少一层模态窗。
    auto* tabs = new QFrame(card);
    tabs->setObjectName(QStringLiteral("cardTabs"));
    tabs->setFixedHeight(40);
    auto* tabLayout = new QHBoxLayout(tabs);
    tabLayout->setContentsMargins(17, 0, 12, 0);
    tabLayout->setSpacing(8);
    auto* flashTab = new QPushButton(QStringLiteral("刷机界面"), tabs);
    flashTab->setObjectName(QStringLiteral("cardTab"));
    flashTab->setProperty("active", true);
    flashTab->setFixedHeight(40);
    // 静态标题：不接受点击（唯一视图，无需切换），仅承担「当前在刷机界面」的语义。
    flashTab->setCursor(Qt::ArrowCursor);
    flashTab->setFocusPolicy(Qt::NoFocus);
    m_flashTabButton = flashTab;

    // LUN 选择：擦除磁盘 / GPT 读写都要指定 LUN（UFS 通常 6 个，eMMC 只有 LUN 0）。
    // 原来藏在高级功能弹窗里；弹窗没了就跟着入口一起搬到这一行。
    auto* lunKey = new QLabel(QStringLiteral("LUN"), tabs);
    lunKey->setObjectName(QStringLiteral("statusLabel"));
    m_lunCombo = new QComboBox(tabs);
    m_lunCombo->setObjectName(QStringLiteral("lunCombo"));
    for (int index = 0; index < 6; ++index) {
        m_lunCombo->addItem(QStringLiteral("LUN %1").arg(index), index);
    }
    m_lunCombo->setFixedWidth(84);
    m_lunCombo->setToolTip(QStringLiteral("擦除磁盘 LUN / GPT 读写的目标；"
                                          "UFS 通常有 6 个 LUN，eMMC 只有 LUN 0"));
    m_advancedCombo = new QComboBox(tabs);
    m_advancedCombo->setObjectName(QStringLiteral("advancedCombo"));
    m_advancedCombo->setToolTip(QStringLiteral("重启目标之外的维护工具：恢复出厂 / OEM 解锁 / "
                                               "基带备份 / 存储信息 / LUN 擦除 / GPT 读写"));
    m_advancedCombo->setMaxVisibleItems(12);
    // 🔴 收起时只占「高级功能 ▾」这几个字：条目里「读取存储信息 / 写入 GPT 文件」
    //    比占位文字长，默认 sizeHint 会按最长条目把这条下拉撑到 ~170px，右侧留
    //    一大块空白，看着像个没填完的输入框。AdjustToMinimumContentsLengthWithIcon
    //    只影响**收起态**宽度，弹出层照旧按条目内容自己撑开。
    m_advancedCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_advancedCombo->setMinimumContentsLength(8);
    // 第 0 项是「按钮样子」的占位：选完任何动作都复位回它（选动作是动作，
    // 不是持久选择 —— 与「芯片 / 引导程序」下拉同一口径）。
    // 🔴 项目里 QComboBox::drop-down 写了 `border:0; width:26px` 却没给 ::down-arrow
    //    指定 image，所以**原生箭头不画**（全局所有下拉都这样）。这里用文字里的 ▾
    //    补一个可辨识的下拉暗示，别改成 setEditable/自绘 —— 那会连带改到弹出行为。
    m_advancedCombo->addItem(QStringLiteral("高级功能 ▾"));
    {
        // 分组标题不可选（只做视觉分界）；条目用 itemData 挂动作 id，
        // 空 data = 非动作项（占位 / 分组标题）。
        const auto addHeader = [this](const QString& text) {
            m_advancedCombo->addItem(text);
        };
        const auto addAction = [this](const QString& text, const QString& id,
                                      const QString& tip) {
            m_advancedCombo->addItem(text, id);
            m_advancedCombo->setItemData(m_advancedCombo->count() - 1, tip, Qt::ToolTipRole);
        };
        addHeader(QStringLiteral("── 数据维护 ──"));
        addAction(QStringLiteral("恢复出厂"), QStringLiteral("factoryReset"),
                  QStringLiteral("只擦除 userdata 与 metadata，等价于系统设置里的恢复出厂"));
        addAction(QStringLiteral("强开 OEM 解锁"), QStringLiteral("oemUnlock"),
                  QStringLiteral("修改 devinfo 分区解锁开关；2024+ 机型可能因 AR fuse 失效"));
        addAction(QStringLiteral("备份基带分区"), QStringLiteral("basebandBackup"),
                  QStringLiteral("一键回读 modem / modemst1 / modemst2 / fsg / fsc / bluetooth"));
        addHeader(QStringLiteral("── 存储与分区 ──"));
        addAction(QStringLiteral("读取存储信息"), QStringLiteral("storageInfo"),
                  QStringLiteral("逐 LUN 查询 getstorageinfo：存储类型、产品型号与容量"));
        addAction(QStringLiteral("擦除磁盘 LUN"), QStringLiteral("eraseLun"),
                  QStringLiteral("整盘擦除左侧所选 LUN 的全部扇区（含分区表），数据不可恢复"));
        addAction(QStringLiteral("读取 GPT 文件"), QStringLiteral("readGpt"),
                  QStringLiteral("从左侧所选 LUN 的起始扇区读回 GPT，存为 .bin 文件"));
        addAction(QStringLiteral("写入 GPT 文件"), QStringLiteral("writeGpt"),
                  QStringLiteral("把选定的 gpt_mainN.bin 写到左侧所选 LUN 的起始扇区"));
        addHeader(QStringLiteral("── 检测 ──"));
        addAction(QStringLiteral("字库牌子（UFS / eMMC）"), QStringLiteral("storageBrand"),
                  QStringLiteral("用当前选中的引导程序读一次存储芯片：厂商 / 类型 / 芯片序列号。"
                                 "只发 NOP，不写任何分区"));

        // 占位项与分组标题设为不可选（点它们不该触发任何动作）。
        if (auto* model = qobject_cast<QStandardItemModel*>(m_advancedCombo->model())) {
            for (int i = 0; i < m_advancedCombo->count(); ++i) {
                if (!m_advancedCombo->itemData(i).toString().isEmpty()) continue;
                if (i == 0) continue;   // 第 0 项是当前值显示位，必须能“选中”才显示得出来
                if (QStandardItem* item = model->item(i)) {
                    item->setFlags(item->flags() & ~Qt::ItemIsSelectable
                                   & ~Qt::ItemIsEnabled);
                }
            }
        }
    }
    // 行高压到 28px：QSS 的 min-height 只是下限，压不动 Win11 样式自算的 42px
    // （同「芯片 / 引导程序」下拉那个坑），只能靠 delegate 定死。
    if (QAbstractItemView* view = m_advancedCombo->view()) {
        view->setItemDelegate(new CompactRowDelegate(28, view));
        // 🔴 弹出层宽度**跟着收起态宽度走**：上面把收起态压到「高级功能 ▾」的宽度
        //    之后，弹层也只有 ~100px 宽，分组标题「── 数据维护 ──」会被省略成
        //    「—— 数...护 ——」。这里按条目内容给足宽度，并关掉省略号兜底。
        view->setMinimumWidth(196);
        view->setTextElideMode(Qt::ElideNone);
        // 占位项（当前值「高级功能 ▾」）不在弹层里重复出现 —— 它只承担收起态显示。
        // 🔴 setRowHidden 在 QListView 上，QAbstractItemView 没有这个方法。
        if (auto* list = qobject_cast<QListView*>(view)) list->setRowHidden(0, true);
    }

    // 🔴 用 activated（只响应用户操作）而不是 currentIndexChanged：后者会被
    //    下面那句 setCurrentIndex(0) 复位再次触发，形成死循环。
    connect(m_advancedCombo, QOverload<int>::of(&QComboBox::activated), this,
            [this](int index) {
                const QString action = m_advancedCombo->itemData(index).toString();
                m_advancedCombo->setCurrentIndex(0);
                if (action.isEmpty()) return;
                if (action == QLatin1String("factoryReset")) {
                    confirmFactoryReset();
                } else if (action == QLatin1String("oemUnlock")) {
                    confirmEnableOemUnlock();
                } else if (action == QLatin1String("basebandBackup")) {
                    chooseBasebandBackup();
                } else if (action == QLatin1String("storageInfo")) {
                    requestStorageInfo();
                } else if (action == QLatin1String("eraseLun")) {
                    confirmEraseLun();
                } else if (action == QLatin1String("readGpt")) {
                    readGptToFile();
                } else if (action == QLatin1String("writeGpt")) {
                    writeGptFromFile();
                } else if (action == QLatin1String("storageBrand")) {
                    probeStorageBrand();
                }
            });

    tabLayout->addWidget(flashTab);
    tabLayout->addStretch();
    tabLayout->addWidget(lunKey);
    tabLayout->addWidget(m_lunCombo);
    tabLayout->addWidget(m_advancedCombo);
    layout->addWidget(tabs);
    mark("页签条就绪");

    auto* body = new QWidget(card);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(10, 3, 10, 0);
    bodyLayout->setSpacing(8);

    auto* connectionRow = new QHBoxLayout;
    connectionRow->addWidget(new QLabel(QStringLiteral("串口"), card));
    m_portCombo = new QComboBox(card);
    m_portCombo->setMinimumWidth(120);
    connectionRow->addWidget(m_portCombo, 1);
    m_autoScanCheck = new QCheckBox(QStringLiteral("自动扫描"), card);
    connectionRow->addWidget(m_autoScanCheck);
    connectionRow->addSpacing(12);
    connectionRow->addWidget(new QLabel(QStringLiteral("存储"), card));
    m_storageCombo = new QComboBox(card);
    m_storageCombo->addItem(QStringLiteral("UFS"), static_cast<int>(StorageType::Ufs));
    m_storageCombo->addItem(QStringLiteral("eMMC"), static_cast<int>(StorageType::Emmc));
    m_storageCombo->setFixedWidth(82);
    connectionRow->addWidget(m_storageCombo);
    bodyLayout->addLayout(connectionRow);
    mark("串口行就绪");

    auto* tableHost = new QWidget(card);
    auto* tableStack = new QGridLayout(tableHost);
    tableStack->setContentsMargins(0, 0, 0, 0);
    m_partitionTable = new QTableWidget(0, 5, tableHost);
    mark("QTableWidget 构造完成");
    m_partitionTable->setObjectName(QStringLiteral("partitionTable"));
    m_partitionTable->setHorizontalHeaderLabels({QString(), QStringLiteral("分区名"),
                                                   QStringLiteral("文件名"), QStringLiteral("大小"),
                                                   QStringLiteral("状态")});
    m_partitionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_partitionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_partitionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_partitionTable->setAlternatingRowColors(true);
    m_partitionTable->verticalHeader()->setVisible(false);
    m_partitionTable->horizontalHeader()->setStretchLastSection(true);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_partitionTable->setColumnWidth(0, 34);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_partitionTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_partitionTable->setMinimumHeight(260);
    mark("表头配置完成");
    tableStack->addWidget(m_partitionTable, 0, 0);
    mark("表加入布局");
    m_emptyState = new QLabel(QStringLiteral("选择 rawprogram XML 后，这里会显示分区镜像\n\n连接设备后可双击分区选择独立镜像"), tableHost);
    m_emptyState->setObjectName(QStringLiteral("emptyState"));
    m_emptyState->setAlignment(Qt::AlignCenter);
    m_emptyState->setAttribute(Qt::WA_TransparentForMouseEvents);
    tableStack->addWidget(m_emptyState, 0, 0, Qt::AlignCenter);
    bodyLayout->addWidget(tableHost, 1);
    mark("分区表就绪");

    m_selectAllCheck = new QCheckBox(m_partitionTable->horizontalHeader());
    m_selectAllCheck->setObjectName(QStringLiteral("headerCheck"));
    m_selectAllCheck->setFixedSize(18, 18);
    m_selectAllCheck->move(9, 9);
    m_selectAllCheck->raise();
    m_checkedCountLabel = new QLabel(card);
    m_checkedCountLabel->hide();
    connect(m_selectAllCheck, &QCheckBox::toggled, this, [this](bool checked) {
        const QSignalBlocker blocker(m_partitionTable);
        for (int row = 0; row < m_partitionTable->rowCount(); ++row) {
            if (auto* item = m_partitionTable->item(row, 0)) {
                item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
            refreshPartitionRowStatus(row);
        }
        updateCheckedCount();
    });
    connect(m_partitionTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem* item) {
                if (item && item->column() == 0) {
                    refreshPartitionRowStatus(item->row());
                    updateCheckedCount();
                }
            });
    connect(m_partitionTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { choosePartitionImage(row); });

    auto* directoryRow = new QHBoxLayout;
    m_firmwareDirectoryEdit = new QLineEdit(card);
    m_firmwareDirectoryEdit->setReadOnly(true);
    m_firmwareDirectoryEdit->setPlaceholderText(QStringLiteral("选择 rawprogram XML..."));
    m_firmwareBrowseButton = new QPushButton(QStringLiteral("选择资料"), card);
    m_firmwareBrowseButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_firmwareBrowseButton, &QPushButton::clicked, this,
            &MainWindow::chooseFirmwareDirectory);
    directoryRow->addWidget(m_firmwareDirectoryEdit, 1);
    directoryRow->addWidget(m_firmwareBrowseButton);
    // 「自动选择」：只让用户选一个包目录，XML 由程序自己找齐（2026-10-17）。
    // 与「选择资料」并存 —— 手动多选仍是混选/单文件调试的逃生门。
    m_autoXmlButton = new QPushButton(QStringLiteral("自动选择"), card);
    m_autoXmlButton->setObjectName(QStringLiteral("secondaryButton"));
    m_autoXmlButton->setToolTip(QStringLiteral(
        "选一个刷机包目录：自动找出目录里（含 IMAGES 子目录）的全部 rawprogram XML，"
        "标准与非标准一并载入，按族分组列出"));
    connect(m_autoXmlButton, &QPushButton::clicked, this,
            &MainWindow::autoSelectFirmwareXml);
    directoryRow->addWidget(m_autoXmlButton);
    bodyLayout->addLayout(directoryRow);
    m_edlPreflightButton = new QPushButton(QStringLiteral("预检刷写计划"), card);
    m_edlPreflightButton->setObjectName(QStringLiteral("edlPreflightButton"));
    m_edlPreflightButton->setToolTip(QStringLiteral(
        "仅离线检查源包，不连接、不刷写；动态起点和 LUN 容量仍需设备确认"));
    connect(m_edlPreflightButton, &QPushButton::clicked, this, &MainWindow::preflightXmlPlan);
    bodyLayout->addWidget(m_edlPreflightButton);

    auto* programmerRow = new QHBoxLayout;
    m_programmerEdit = new QLineEdit(card);
    m_programmerEdit->setPlaceholderText(QStringLiteral("双击选择 firehose 文件..."));
    m_programmerEdit->installEventFilter(this);
    auto* modeLabel = new QLabel(QStringLiteral("模式"), card);
    modeLabel->setObjectName(QStringLiteral("strongLabel"));
    m_modeCombo = new QComboBox(card);
    m_modeCombo->addItem(QStringLiteral("EDL"));
    m_modeCombo->setFixedWidth(82);
    programmerRow->addWidget(m_programmerEdit, 1);
    programmerRow->addWidget(modeLabel);
    programmerRow->addWidget(m_modeCombo);
    bodyLayout->addLayout(programmerRow);

    auto* chipsetRow = new QHBoxLayout;
    m_chipsetCombo = new QComboBox(card);
    m_chipsetCombo->setObjectName(QStringLiteral("chipsetCombo"));
    m_chipsetCombo->addItem(QStringLiteral("官方高通骁龙芯片（Qualcomm EDL）"));
    // 停更离线版（2.0.6）：云端引导库已随服务器关停移除 —— 下拉里只剩第 0 项
    // （官方高通芯片），引导程序一律走下方「引导程序」输入框 + 浏览按钮选本地文件。
    m_connectButton = new QPushButton(QStringLiteral("查找分区"), card);
    m_connectButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::toggleConnection);
    chipsetRow->addWidget(m_chipsetCombo, 1);
    chipsetRow->addWidget(m_connectButton);
    bodyLayout->addLayout(chipsetRow);

    m_vipDirectoryEdit = new QLineEdit(card);
    m_vipDirectoryEdit->hide();
    m_vipBrowseButton = new QPushButton(card);
    m_vipBrowseButton->hide();
    connect(m_vipBrowseButton, &QPushButton::clicked, this,
            &MainWindow::chooseVipPackage);

    auto* optionsRow = new QHBoxLayout;
    optionsRow->setSpacing(10);
    m_autoRebootCheck = new QCheckBox(QStringLiteral("自动重启"), card);
    m_autoRebootCheck->setToolTip(QStringLiteral(
        "读取/写入/刷机任务完成后自动重启到系统；\n擦除任务完成后自动重启回 9008，便于继续下发引导"));
    connect(m_autoRebootCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("operation/autoReboot"), checked);
    });
    m_vipCheck = new QCheckBox(QStringLiteral("OPPO VIP"), card);
    m_vipCheck->setToolTip(QStringLiteral("执行 Digest → Verify → Sign → SHA-256 → Configure 序列"));
    m_generateXmlCheck = new QCheckBox(QStringLiteral("生成 XML"), card);
    m_generateXmlCheck->setChecked(
        QSettings().value(QStringLiteral("backup/generateXml"), true).toBool());
    m_generateXmlCheck->setToolTip(
        QStringLiteral("回读所选分区后，按 LUN 生成可再次刷写的 rawprogram XML"));
    connect(m_generateXmlCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("backup/generateXml"), checked);
    });
    m_protectPartitionsCheck = new QCheckBox(QStringLiteral("保护分区"), card);
    m_protectPartitionsCheck->setToolTip(
        QStringLiteral("保护校准、基带指纹及 OPLUS NV 分区；写入和擦除时默认跳过"));
    connect(m_protectPartitionsCheck, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("operation/protectPartitions"), checked);
        for (int row = 0; row < m_partitions.size(); ++row) refreshPartitionRowStatus(row);
    });
    m_readInfoCheck = new QCheckBox(QStringLiteral("读取信息"), card);
    m_readInfoCheck->setToolTip(QStringLiteral("读取 GPT 后自动解析系统版本、Android 版本与安全补丁"));
    connect(m_readInfoCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("operation/readVersionInfo"), checked);
    });
    // 保留用户数据（移植自 TMFTool 的「保数据 / 保留用户数据」）：XML 刷写时
    // 跳过 userdata / metadata。默认**关闭** —— 刷机默认行为是「整包覆盖」，
    // 静默保数据会让用户以为刷干净了，实际旧数据还在。
    m_keepUserDataCheck = new QCheckBox(QStringLiteral("保留用户数据"), card);
    m_keepUserDataCheck->setChecked(
        QSettings().value(QStringLiteral("operation/keepUserData"), false).toBool());
    m_keepUserDataCheck->setToolTip(QStringLiteral(
        "XML 刷写时跳过 userdata / metadata（不清数据）；\n"
        "首次开机可能因系统版本跨度过大而异常，跨版本刷机不建议开"));
    connect(m_keepUserDataCheck, &QCheckBox::toggled, this, [](bool checked) {
        QSettings().setValue(QStringLiteral("operation/keepUserData"), checked);
    });
    optionsRow->addWidget(m_autoRebootCheck);
    optionsRow->addWidget(m_vipCheck);
    optionsRow->addWidget(m_generateXmlCheck);
    optionsRow->addWidget(m_protectPartitionsCheck);
    optionsRow->addWidget(m_keepUserDataCheck);
    optionsRow->addWidget(m_readInfoCheck);
    optionsRow->addStretch();
    bodyLayout->addLayout(optionsRow);
    mark("选项行就绪");

    connect(m_vipCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked || m_busy || m_connected) return;
        if (m_vipDirectoryEdit->text().isEmpty()) {
            const QFileInfo programmer(m_programmerEdit->text());
            const QString candidate = programmer.isFile() ? programmer.absolutePath()
                : (!m_selectedXmlPaths.isEmpty()
                       ? QFileInfo(m_selectedXmlPaths.front()).absolutePath()
                       : QFileInfo(m_firmwareDirectoryEdit->text()).absolutePath());
            if (!candidate.isEmpty()) m_vipDirectoryEdit->setText(candidate);
        }
        if (!m_vipDirectoryEdit->text().isEmpty() && loadVipPackage(false)) return;
        QTimer::singleShot(0, this, &MainWindow::chooseVipPackage);
    });
    connect(m_storageCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_vipCheck->isChecked() && !m_vipDirectoryEdit->text().isEmpty()) {
            loadVipPackage(false);
        }
    });

    auto* actionRow = new QHBoxLayout;
    m_readGptButton = new QPushButton(QStringLiteral("读取分区表"), card);
    m_readVersionButton = new QPushButton(QStringLiteral("读取版本"), card);
    m_backupButton = new QPushButton(QStringLiteral("读取分区"), card);
    m_eraseButton = new QPushButton(QStringLiteral("擦除分区"), card);
    m_flashButton = new QPushButton(QStringLiteral("写入分区"), card);
    m_xmlFlashButton = m_flashButton;
    m_flashButton->setObjectName(QStringLiteral("primaryButton"));
    connect(m_readGptButton, &QPushButton::clicked, this, [this] {
        if (m_connected) {
            setBusy(true);
            emit readGptRequested();
        } else {
            toggleConnection();
        }
    });
    connect(m_readVersionButton, &QPushButton::clicked, this, [this] {
        if (!m_connected) {
            m_readVersionAfterGpt = true;
            toggleConnection();
        } else if (m_partitions.isEmpty()) {
            m_readVersionAfterGpt = true;
            setBusy(true);
            emit readGptRequested();
        } else {
            setBusy(true);
            emit readVersionInfoRequested();
        }
    });
    connect(m_backupButton, &QPushButton::clicked, this, &MainWindow::chooseBackup);
    connect(m_flashButton, &QPushButton::clicked, this, [this] {
        if (checkedPartitions().isEmpty()) chooseXmlAndFlash();
        else chooseImageAndFlash();
    });
    connect(m_eraseButton, &QPushButton::clicked, this, &MainWindow::confirmErase);
    actionRow->addWidget(m_readGptButton);
    actionRow->addWidget(m_readVersionButton);
    actionRow->addWidget(m_backupButton);
    actionRow->addWidget(m_eraseButton);
    actionRow->addWidget(m_flashButton);
    bodyLayout->addLayout(actionRow);
    bodyLayout->addSpacing(28);
    // 2026-10-16：原 QStackedWidget（0=刷机界面 / 1=高级功能）撤掉，本卡只剩
    // 刷机界面一个视图；高级功能 2026-10-19 起是页签条右侧的下拉框（见上）。
    layout->addWidget(body, 1);
    mark("buildLeftCard 结束");
    return card;
}

// ─────────────────────────────────────────────────────────────────────────────
//  FDL 流程窗（2026-10-16）
//
//  用户要求：「还要 fdl 也放在重启列表里，不需要独立的页面，点击自动执行命令」。
//  于是撤掉外壳里的 XiaomiEdl 独立页签，把流程本体（kc::ui::XiaomiEdlPage）
//  装进一个弹窗，从右下角「重启设备 ▾」菜单点开即**自动开跑**完整流程：
//    getvar token → 卡密求签 → fastboot stage → oem edl。
//  弹窗同样懒创建常驻；重复打开时若上一轮还在跑，runFlow() 自带的忙碌闸门会挡住。
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow::openFdlFlow()
{
    if (!m_fdlDialog) {
        auto* dialog = new kc::ui::AnimatedDialog(this);
        dialog->setObjectName(QStringLiteral("fdlFlowDialog"));
        dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        dialog->setAttribute(Qt::WA_TranslucentBackground, true);
        dialog->setModal(true);

        auto* card = new QFrame(dialog);
        card->setObjectName(QStringLiteral("msgRoot"));
        card->setMinimumWidth(700);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(0);

        auto* titleBar = new QFrame(card);
        titleBar->setObjectName(QStringLiteral("msgTitleBar"));
        auto* titleLayout = new QHBoxLayout(titleBar);
        titleLayout->setContentsMargins(18, 10, 12, 10);
        titleLayout->setSpacing(8);
        auto* titleLabel = new QLabel(QStringLiteral("FDL · 进入 EDL（小米）"), titleBar);
        titleLabel->setObjectName(QStringLiteral("msgTitle"));
        titleLayout->addWidget(titleLabel, 1);
        auto* closeButton = new QPushButton(QStringLiteral("✕"), titleBar);
        closeButton->setObjectName(QStringLiteral("msgClose"));
        closeButton->setToolTip(QStringLiteral("关闭（不中断正在进行的流程，可回来查看）"));
        closeButton->setCursor(Qt::ArrowCursor);
        titleLayout->addWidget(closeButton);
        connect(closeButton, &QPushButton::clicked, dialog, &QDialog::reject);

        auto* body = new QFrame(card);
        body->setObjectName(QStringLiteral("msgBody"));
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(0);
        m_fdlPage = new kc::ui::XiaomiEdlPage(body);
        bodyLayout->addWidget(m_fdlPage, 1);

        cardLayout->addWidget(titleBar);
        cardLayout->addWidget(body, 1);

        auto* outer = new QVBoxLayout(dialog);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(card);
        dialog->resize(720, 580);
        m_fdlDialog = dialog;
    }

    if (QWidget* host = window()) {
        if (QScreen* screen = host->screen()) {
            const QRect avail = screen->availableGeometry();
            const QPoint center = host->mapToGlobal(host->rect().center());
            const QSize size = m_fdlDialog->size();
            QPoint target(center.x() - size.width() / 2, center.y() - size.height() / 2);
            target.setX(qBound(avail.left(), target.x(), avail.right() - size.width()));
            target.setY(qBound(avail.top(), target.y(), avail.bottom() - size.height()));
            m_fdlDialog->move(target);
        }
    }

    // 「点击自动执行命令」：弹窗起来后立刻开跑完整流程。
    // 🔴 这里**不要**先调 onActivated() —— 它的 refreshDevice() 会先占住忙碌闸门，
    //    紧接着的 startFlow() 就撞上「上一次操作还没结束」（2026-10-16 探针实测）。
    //    设备读数刷新本来就包含在流程第一步（fastboot devices）里。
    QTimer::singleShot(0, m_fdlPage, [this] { m_fdlPage->startFlow(); });
    m_fdlDialog->show();
    m_fdlDialog->raise();
    m_fdlDialog->activateWindow();
}

QWidget* MainWindow::makeInfoCell(const QString& title, QLabel** valueLabel)
{
    auto* cell = new QWidget;
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* key = new QLabel(title, cell);
    key->setObjectName(QStringLiteral("infoKey"));
    auto* value = new QLabel(QStringLiteral("—"), cell);
    value->setObjectName(QStringLiteral("infoValue"));
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(key);
    layout->addWidget(value);
    *valueLabel = value;
    return cell;
}

QWidget* MainWindow::buildRightCard()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(0);

    auto* logHeaderWidget = new QFrame(card);
    logHeaderWidget->setObjectName(QStringLiteral("sectionHeader"));
    logHeaderWidget->setFixedHeight(40);
    auto* logHeader = new QHBoxLayout(logHeaderWidget);
    logHeader->setContentsMargins(14, 0, 11, 0);
    auto* logTitle = new QLabel(QStringLiteral("操作日志"), logHeaderWidget);
    logTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_debugLogButton = new QPushButton(QStringLiteral("Debug"), logHeaderWidget);
    m_debugLogButton->setObjectName(QStringLiteral("debugButton"));
    m_debugLogButton->setCheckable(true);
    m_debugLogButton->setToolTip(QStringLiteral("开启后显示 Firehose 原始帧、命令及身份重试等完整日志"));
    connect(m_debugLogButton, &QPushButton::toggled, this, [this](bool checked) {
        m_debugLogButton->setText(checked ? QStringLiteral("Debug：完整") : QStringLiteral("Debug"));
        // 「详细」档 = 显示 Trace 行（Firehose 原始帧 / 命令 / 身份重试等）。
        // LogView 切档会整篇重放历史，等价于原先的 rebuildLogView()。
        if (m_logView) m_logView->setDebugVisible(checked);
    });
    auto* clear = new QPushButton(QStringLiteral("清空"), logHeaderWidget);
    clear->setObjectName(QStringLiteral("secondaryButton"));
    connect(clear, &QPushButton::clicked, this, [this] {
        // LogView 自带结构化历史，清它一处即可（原实现要同时清 m_logHistory，
        // 否则 Debug 档重放会「复活」已清掉的日志）。
        if (m_logView) m_logView->clear();
    });
    logHeader->addWidget(logTitle);
    logHeader->addStretch();
    logHeader->addWidget(m_debugLogButton);
    logHeader->addWidget(clear);
    layout->addWidget(logHeaderWidget);

    // 日志视图换成外壳统一的 kc::ui::LogView（= C# PageLog 的 C++ 移植）：
    // 级别着色 / 区段横线标题 / 「常规·详细」档位筛选 / 批量渲染 / 主题切换重绘，
    // 与 Payload、Fastboot、OnePlus 等页的日志同一套观感。原先这里是本窗私有
    // 的 QTextEdit + 自写 formatLogHtml 上色，和别的页面各说各话（2026-10-13
    // 用户要求「所有刷写的日志请使用 C# 版本的」）。
    // 8px 内衬 = 原 QSS `QTextEdit#logView { margin: 8px 8px 0 8px; }` 的那圈
    // 留白；内层 QTextEdit#logTextEdit 的边框/圆角由主题表给出，不必在此重画。
    m_logView = new kc::ui::LogView(card);
    m_logView->setObjectName(QStringLiteral("logView"));
    m_logView->setContentsMargins(8, 8, 8, 0);
    m_logView->setMinimumHeight(255);
    m_logView->setMaximumHeight(280);
    // 默认「常规」档：Firehose 高频协议流水（Trace）不进界面，但整条流水仍落
    // SessionLog，开发者拿「上传日志」时不会缺料。
    // 2026-10-17：档位改跟控制中心的「日志详细」磁贴走 —— 用户在外壳里拨过一次，
    // 进 EDL 页就该是那个档，而不是又被这里按回「常规」。
    const bool detailed = kc::ui::LogView::globalDebugVisible();
    m_logView->setDebugVisible(detailed);
    m_debugLogButton->setChecked(detailed);
    m_debugLogButton->setText(detailed ? QStringLiteral("Debug：完整") : QStringLiteral("Debug"));
    layout->addWidget(m_logView, 1);

    auto* infoHeader = new QFrame(card);
    infoHeader->setObjectName(QStringLiteral("sectionHeader"));
    infoHeader->setFixedHeight(43);
    auto* infoHeaderLayout = new QHBoxLayout(infoHeader);
    infoHeaderLayout->setContentsMargins(14, 0, 0, 0);
    auto* infoTitle = new QLabel(QStringLiteral("设备信息"), infoHeader);
    infoTitle->setObjectName(QStringLiteral("sectionTitle"));
    infoHeaderLayout->addWidget(infoTitle);
    infoHeaderLayout->addStretch();
    layout->addWidget(infoHeader);

    auto* infoBody = new QWidget(card);
    infoBody->setMinimumHeight(168);
    auto* infoGrid = new QGridLayout(infoBody);
    infoGrid->setContentsMargins(14, 9, 14, 9);
    infoGrid->setHorizontalSpacing(38);
    infoGrid->setVerticalSpacing(8);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("品牌"), &m_brandValue), 0, 0);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("芯片"), &m_modelValue), 0, 1);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("CPUID"), &m_cpuidValue), 0, 2);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("用户区大小"), &m_storageValue), 1, 0);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("当前模式"), &m_modeValue), 1, 1);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("存储型号"), &m_systemValue), 1, 2);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("设备名称"), &m_productNameValue), 2, 0);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("Android"), &m_androidValue), 2, 1);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("系统版本"), &m_versionValue), 2, 2);
    infoGrid->addWidget(makeInfoCell(QStringLiteral("安全补丁"), &m_securityPatchValue), 3, 0);
    infoGrid->setColumnStretch(0, 1);
    infoGrid->setColumnStretch(1, 1);
    infoGrid->setColumnStretch(2, 1);
    layout->addWidget(infoBody);

    m_portValue = new QLabel(card);
    m_portValue->hide();
    m_programmerValue = new QLabel(card);
    m_programmerValue->hide();
    m_systemValue->setText(QStringLiteral("—"));

    m_stateBanner = new QLabel(QStringLiteral("连接设备后即可开始操作"), card);
    m_stateBanner->setObjectName(QStringLiteral("stateBanner"));
    m_stateBanner->setFixedHeight(36);
    m_stateBanner->setContentsMargins(12, 0, 12, 0);
    layout->addWidget(m_stateBanner);

    auto* progressArea = new QWidget(card);
    auto* progressLayout = new QGridLayout(progressArea);
    progressLayout->setContentsMargins(0, 9, 12, 0);
    progressLayout->setHorizontalSpacing(9);
    progressLayout->setVerticalSpacing(8);
    auto* taskProgress = makeProgressCell(progressArea, &m_progressBar, &m_progressText,
                                           QStringLiteral("当前任务  0.0%"));
    auto* totalProgress = makeProgressCell(progressArea, &m_totalProgressBar,
                                            &m_totalProgressText,
                                            QStringLiteral("总体进度  0.0%"));
    m_progressBar->setObjectName(QStringLiteral("taskProgressBar"));
    m_totalProgressBar->setObjectName(QStringLiteral("overallProgressBar"));
    m_speedText = new QLabel(QStringLiteral("速率  0.00 MB/s"), card);
    m_speedText->setObjectName(QStringLiteral("speedText"));
    m_speedText->setMinimumWidth(116);
    m_speedText->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_stopButton = new QPushButton(QStringLiteral("■  停止"), card);
    m_stopButton->setObjectName(QStringLiteral("dangerButton"));

    m_rebootButton = new QPushButton(QStringLiteral("重启设备  ▾"), card);
    m_rebootButton->setObjectName(QStringLiteral("secondaryButton"));
    m_rebootMenu = new QMenu(m_rebootButton);
    m_rebootMenu->setObjectName(QStringLiteral("rebootMenu"));
    // 分组（2026-10-16）：常规重启 / BCB 目标（原在「高级功能」页）/ FDL 通道。
    QAction* rebootSystem = m_rebootMenu->addAction(QStringLiteral("重启到系统"));
    QAction* rebootEdl = m_rebootMenu->addAction(QStringLiteral("重启到 9008"));
    m_rebootMenu->addSeparator();
    QAction* rebootRecovery = m_rebootMenu->addAction(QStringLiteral("重启到 Recovery"));
    QAction* rebootFastbootD = m_rebootMenu->addAction(QStringLiteral("重启到 FastbootD"));
    // FDL（原「小米EDL」独立页 / 后并入本菜单）：2026-10-17 用户要求**移出菜单**，
    // 做成 EDL 页顶部快捷条上的独立按钮（见 EdlFlashPage 的 fdlButton →
    // MainWindow::startFdlFlow）。它是一条完整入口流程，不是「重启」的一种。
    connect(rebootSystem, &QAction::triggered, this, [this] { confirmReboot(false); });
    connect(rebootEdl, &QAction::triggered, this, [this] { confirmReboot(true); });
    connect(rebootRecovery, &QAction::triggered, this,
            &MainWindow::confirmRebootToRecovery);
    connect(rebootFastbootD, &QAction::triggered, this,
            &MainWindow::confirmRebootToFastbootD);
    connect(m_rebootButton, &QPushButton::clicked, this, [this] {
        if (!m_rebootMenu) return;
        const QSize menuSize = m_rebootMenu->sizeHint();
        const QPoint below = m_rebootButton->mapToGlobal(
            QPoint(0, m_rebootButton->height() + 4));
        const QPoint above = m_rebootButton->mapToGlobal(
            QPoint(0, -menuSize.height() - 4));
        const QRect available = m_rebootButton->screen()->availableGeometry();
        m_rebootMenu->popup(available.contains(
                                QRect(below, menuSize)) ? below : above);
    });
    progressLayout->addWidget(taskProgress, 0, 0, 1, 2);
    progressLayout->addWidget(m_speedText, 0, 2, 1, 2);
    progressLayout->addWidget(totalProgress, 1, 0, 1, 2);
    progressLayout->addWidget(m_rebootButton, 1, 2);
    progressLayout->addWidget(m_stopButton, 1, 3);
    progressLayout->setColumnStretch(0, 1);
    progressLayout->setColumnStretch(1, 1);
    layout->addWidget(progressArea);
    return card;
}

/// 串口指纹：是不是 Qualcomm 9008（EDL）端口。
///
/// 之前只认 description/manufacturer 里的几个关键字，实测不够：
///  · 标准驱动写 "Qualcomm HS-USB QDLoader 9008"（能中）；
///  · 小米/一加/OPPO 的定制驱动写 "QUSB_BULK_CID:xxxx_SN:..."（中不了）；
///  · 最可靠的是 **VID 0x05C6（Qualcomm）+ PID 0x9008**，这个组合不会变。
bool MainWindow::isEdlPort(const QSerialPortInfo& port)
{
    if (port.hasVendorIdentifier() && port.hasProductIdentifier()) {
        if (port.vendorIdentifier() == 0x05c6 && port.productIdentifier() == 0x9008) return true;
    }
    const QString fingerprint = port.description() + QLatin1Char(' ') + port.manufacturer() +
                                QLatin1Char(' ') + port.portName();
    static const char* const kHints[] = {
        "9008", "QDLoader", "Qualcomm HS-USB", "QUSB_BULK", "Sahara", "Firehose",
    };
    for (const char* hint : kHints) {
        if (fingerprint.contains(QLatin1String(hint), Qt::CaseInsensitive)) return true;
    }
    return false;
}

void MainWindow::refreshPorts()
{
    if (m_connected) return;
    const auto ports = QSerialPortInfo::availablePorts();
    QStringList snapshotParts;
    for (const QSerialPortInfo& port : ports) {
        snapshotParts.push_back(QStringLiteral("%1|%2|%3|%4")
            .arg(port.portName(), port.description())
            .arg(port.hasVendorIdentifier() ? port.vendorIdentifier() : 0, 4, 16, QLatin1Char('0'))
            .arg(port.hasProductIdentifier() ? port.productIdentifier() : 0, 4, 16, QLatin1Char('0')));
    }
    const QString snapshot = snapshotParts.join(QLatin1Char(';'));
    if (snapshot == m_lastPortSnapshot && m_portCombo->count() > 0) return;
    m_lastPortSnapshot = snapshot;

    const QString previous = m_portCombo->currentData().toString();
    m_portCombo->clear();

    // 9008 设备排最前并打 ★：插上手机后下拉框第一项就是它，直接回车即可。
    // 其余串口（蓝牙虚拟口、调试口）原样排在后面 —— 用户可能就是要用它。
    // 控制中心的「自动识别 9008」关掉时整段跳过：识别、打星、自动连接一起停。
    const bool autoDetect =
        QSettings().value(QStringLiteral("connection/autoDetect9008"), true).toBool();
    QStringList edlPorts;
    QStringList otherPorts;
    QString edlLabel;
    for (const QSerialPortInfo& port : ports) {
        if (autoDetect && isEdlPort(port)) {
            edlPorts.push_back(port.portName());
            if (edlLabel.isEmpty()) {
                edlLabel = QStringLiteral("%1  ·  %2")
                               .arg(port.portName(), port.description().isEmpty()
                                                         ? QStringLiteral("9008 设备")
                                                         : port.description());
            }
        } else {
            otherPorts.push_back(port.portName());
        }
    }

    auto appendPortItem = [this](const QString& name, bool starred) {
        const QSerialPortInfo info(name);
        const QString desc = info.description().isEmpty() ? QStringLiteral("串口设备")
                                                          : info.description();
        m_portCombo->addItem(starred ? QStringLiteral("★ %1  ·  %2").arg(name, desc)
                                     : QStringLiteral("%1  ·  %2").arg(name, desc),
                             name);
    };
    for (const QString& name : std::as_const(edlPorts)) appendPortItem(name, true);
    for (const QString& name : std::as_const(otherPorts)) appendPortItem(name, false);

    // 之前选中的端口若还在，尊重用户的选择（手动改过就别抢回去）。
    int preferred = edlPorts.isEmpty() ? -1 : 0;
    for (int i = 0; i < m_portCombo->count(); ++i) {
        if (m_portCombo->itemData(i).toString() == previous) {
            preferred = i;
            break;
        }
    }
    if (preferred >= 0) m_portCombo->setCurrentIndex(preferred);
    if (ports.isEmpty()) m_portCombo->addItem(QStringLiteral("未发现串口"), QString());
    if (!m_busy && !m_connected && m_readGptButton) {
        m_readGptButton->setEnabled(!m_portCombo->currentData().toString().isEmpty());
    }

    if (!edlPorts.isEmpty()) {
        appendLog(QStringLiteral("已自动识别 Qualcomm 9008 端口：%1").arg(edlLabel));
    } else if (!ports.isEmpty() && !autoDetect) {
        appendLog(QStringLiteral("扫描完成，发现 %1 个串口（自动识别已关闭）").arg(ports.size()));
    } else if (!ports.isEmpty()) {
        appendLog(QStringLiteral("扫描完成，发现 %1 个串口（未识别到 9008 设备）").arg(ports.size()));
    } else {
        appendLog(QStringLiteral("扫描完成，未发现串口"));
    }

    // ── 自动连接 ────────────────────────────────────────────────────────
    // 只在「确实识别到 9008 + 手上已有 programmer + 用户没手动断开过」时做：
    // 缺 programmer 的自动连接只会弹一个错误框，比不连更烦。
    const QString currentPort = m_portCombo->currentData().toString();
    const bool programmerReady = (m_vipCheck && m_vipCheck->isChecked())
                                 || !m_programmerEdit->text().trimmed().isEmpty();
    if (!edlPorts.isEmpty() && !m_connected && !m_busy && !m_autoConnectSuppressed &&
        programmerReady && !currentPort.isEmpty() && currentPort != m_autoConnectedPort &&
        edlPorts.contains(currentPort)) {
        m_autoConnectedPort = currentPort;
        appendLog(QStringLiteral("检测到 9008 设备，正在自动连接…"));
        // 延后一拍：让下拉框先完成布局，日志也能排在「已自动识别」之后。
        QTimer::singleShot(180, this, [this, currentPort] {
            if (m_connected || m_busy) return;
            if (m_portCombo->currentData().toString() != currentPort) return;
            toggleConnection();
        });
    }
}

void MainWindow::ensureCloudLoaderList()
{
    // 2026-10-21：**云端引导库已整体下线**（服务端 /api/v2/loaders/firehose/index 恒回空）。
    // 原来这里会异步拉一次索引，空结果就往日志里写一行
    // 「云端引导库获取失败：引导库还是空的」—— 功能都删了，这条错误只会让人以为网络坏了。
    // 现在直接标记「已加载」并返回：下拉里只剩第 0 项（官方高通芯片），
    // 引导程序一律走本地选择（下方「引导程序」输入框 + 浏览按钮）。
    m_cloudLoaderLoaded = true;
}


// fillCloudLoaderItems / downloadCloudLoader / showCloudLoaderList
// 已随云端引导库移除（停更离线版 2.0.6，服务器关停）。引导程序改走本地文件选择。


void MainWindow::chooseProgrammer()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 Firehose Programmer"), {},
        QStringLiteral("Firehose Programmer (*.elf *.melf *.mbn *.bin);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    setProgrammerPath(path);
}

void MainWindow::setProgrammerPath(const QString& path)
{
    if (path.isEmpty()) return;
    m_programmerEdit->setText(path);

    OplusVipPackage package;
    QString error;
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    const QString directory = QFileInfo(path).absolutePath();
    // A file chosen from Desktop or a firmware root may have hundreds of
    // thousands of descendants. Only inspect its sibling files here; explicit
    // package-directory selection remains the recursive discovery entry point.
    if (OplusVipSession::discoverPackage(directory, storage, &package, &error,
                                         false, path)) {
        m_vipDirectoryEdit->setText(directory);
        m_vipPackage = package;
        m_programmerEdit->setText(package.programmerPath);
        m_vipDirectoryEdit->setToolTip(
            QStringLiteral("Programmer: %1\nDigest: %2\nSign: %3\n候选签名数: %4")
                .arg(QFileInfo(package.programmerPath).fileName(),
                     QFileInfo(package.digestPath).fileName(),
                     QFileInfo(package.signaturePath).fileName())
                .arg(package.programmerCandidates.size()));
        {
            // Avoid the toggled handler recursively scanning the same directory
            // while chooseProgrammer() is still on the GUI thread.
            const QSignalBlocker blocker(m_vipCheck);
            m_vipCheck->setChecked(true);
        }
        appendLog(QStringLiteral("同目录已识别为 OPPO VIP 引导包：%1 / %2 / %3（%4 个签名候选）")
                      .arg(QFileInfo(package.programmerPath).fileName(),
                           QFileInfo(package.digestPath).fileName(),
                           QFileInfo(package.signaturePath).fileName())
                      .arg(package.programmerCandidates.size()));
    } else {
        // The newly selected file is a standard standalone programmer. Drop a
        // previously cached VIP package so Connect uses exactly this file.
        m_vipPackage = {};
        m_vipDirectoryEdit->clear();
        m_vipDirectoryEdit->setToolTip({});
        const QSignalBlocker blocker(m_vipCheck);
        m_vipCheck->setChecked(false);
    }
}

void MainWindow::chooseFirmwareDirectory()
{
    QString initialPath;
    if (!m_selectedXmlPaths.isEmpty()) {
        initialPath = QFileInfo(m_selectedXmlPaths.front()).absolutePath();
    } else if (!m_firmwareDirectoryEdit->text().isEmpty()) {
        const QFileInfo current(m_firmwareDirectoryEdit->text());
        initialPath = current.isDir() ? current.absoluteFilePath() : current.absolutePath();
    }
    const QStringList selectedPaths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择刷机 XML（标准 / 非标准可混选）"), initialPath, rawprogramXmlFilters());
    if (selectedPaths.isEmpty()) return;

    loadFirmwareXmlPaths(selectedPaths);
}

void MainWindow::autoSelectFirmwareXml()
{
    QString initialDirectory;
    if (!m_selectedXmlPaths.isEmpty()) {
        initialDirectory = QFileInfo(m_selectedXmlPaths.front()).absolutePath();
    } else if (!m_firmwareDirectoryEdit->text().isEmpty()) {
        const QFileInfo current(m_firmwareDirectoryEdit->text());
        initialDirectory = current.isDir() ? current.absoluteFilePath() : current.absolutePath();
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择刷机包目录（自动发现 rawprogram XML）"), initialDirectory);
    if (directory.isEmpty()) return;

    QString usedDirectory;
    QString error;
    const QStringList discovered = RawProgramParser::discover(directory, &usedDirectory, &error);
    if (discovered.isEmpty()) {
        appendLog(QStringLiteral("自动选择失败：%1").arg(error));
        ThemedMessageBox::warning(this, QStringLiteral("自动选择 XML"), error);
        return;
    }

    // 用户界面只显示「[标准 + 非标准 XML] rawprogram0.xml +N」，到底吃进了哪几套
    // 方案、各几个文件，必须落进日志 —— 这是后面解释「为什么某个分区被写了两遍」的依据。
    QHash<QString, QStringList> byFamily;
    QStringList familyOrder;
    for (const QString& path : discovered) {
        const QString fileName = QFileInfo(path).fileName();
        const QString key = xmlFamilyKey(fileName);
        if (!byFamily.contains(key)) familyOrder.push_back(key);
        byFamily[key].push_back(fileName);
    }
    appendLog(QStringLiteral("自动选择：%1 → 找到 %2 个 rawprogram XML")
                  .arg(QDir::toNativeSeparators(usedDirectory))
                  .arg(discovered.size()));
    for (const QString& key : std::as_const(familyOrder)) {
        const QStringList files = byFamily.value(key);
        appendLog(QStringLiteral("  · %1：%2 个文件（%3）")
                      .arg(key)
                      .arg(files.size())
                      .arg(files.join(QStringLiteral("、"))));
    }
    if (familyOrder.size() > 1) {
        appendLog(QStringLiteral("多套方案同时载入：同 LUN/起始扇区的条目会被写两次，"
                                 "开始刷写前会列出重复目标。"));
    }
    loadFirmwareXmlPaths(discovered);
}

void MainWindow::loadFirmwareXmlPaths(const QStringList& selectedPaths)
{
    const QStringList xmlPaths = expandRawprogramXmlSelection(selectedPaths);
    RawProgramPlan plan;
    QString error;
    if (!RawProgramParser::parse(xmlPaths, &plan, &error)) {
        ThemedMessageBox::warning(this, QStringLiteral("XML 资料"), error);
        appendLog(QStringLiteral("XML 资料解析失败：%1").arg(error));
        return;
    }
    // 标准 / 非标准可混选。载入阶段只记录重复目标（不弹框：`--xml` 探针也走这里，
    // 弹模态框会把无头探针挂死），真正的确认放在「开始刷写」入口。
    const QStringList mixConflicts = xmlMixConflictLines(plan);
    if (isMixedXmlSelection(plan.xmlPaths)) {
        appendLog(QStringLiteral("XML 方案：%1（%2 个文件）")
                      .arg(xmlCategoryName(plan.xmlPaths))
                      .arg(plan.xmlPaths.size()));
        for (const QString& line : mixConflicts) {
            appendLog(QStringLiteral("【注意】混选重复目标：%1").arg(line));
        }
    }

    m_selectedXmlPaths = plan.xmlPaths;
    const QFileInfo firstXml(m_selectedXmlPaths.front());
    const QString category = xmlCategoryName(m_selectedXmlPaths);
    const QString display = m_selectedXmlPaths.size() == 1
        ? QDir::toNativeSeparators(firstXml.absoluteFilePath())
        : QStringLiteral("[%1] %2  +%3").arg(category, firstXml.fileName())
              .arg(m_selectedXmlPaths.size() - 1);
    m_firmwareDirectoryEdit->setText(display);
    m_firmwareDirectoryEdit->setToolTip(
        m_selectedXmlPaths.join(QLatin1Char('\n')));

    if (!m_connected) {
        // ⚠ 表体被换成 rawprogram XML 条目后，行号语义就和 m_partitions 脱钩了。
        // checkedPartitions() 是「表行号 → m_partitions.at(row)」的映射，不同步
        // 清空的话，勾选 XML 行再点擦除/写入会打到上一台设备的分区列表上。
        m_partitions.clear();
        m_partitionImagePaths.clear();
        {
            const QSignalBlocker selectAllBlocker(m_selectAllCheck);
            m_selectAllCheck->setChecked(false);
        }
        const QSignalBlocker blocker(m_partitionTable);
        m_partitionTable->setRowCount(plan.entries.size());
        for (int row = 0; row < plan.entries.size(); ++row) {
            const RawProgramEntry& entry = plan.entries.at(row);
            const QFileInfo image(entry.sourcePath);
            const QStringList values{
                QString(),
                entry.label.isEmpty() ? QStringLiteral("—") : entry.label,
                image.fileName().isEmpty() ? entry.filename : image.fileName(),
                image.isFile() ? formatBytes(static_cast<quint64>(image.size()))
                               : QStringLiteral("—"),
                entry.dynamicSuper ? QStringLiteral("META 免合并")
                                   : (image.isFile() ? QStringLiteral("就绪")
                                                     : QStringLiteral("镜像缺失，跳过"))
            };
            for (int column = 0; column < values.size(); ++column) {
                auto* item = new QTableWidgetItem(values.at(column));
                item->setFlags(item->flags() & ~Qt::ItemIsEditable &
                               ~Qt::ItemIsUserCheckable);
                // 混选时同一 label 可能来自两份 XML，行尾必须能看出条目归属。
                item->setToolTip(QStringLiteral("%1\nXML: %2")
                                     .arg(entry.sourcePath,
                                          QFileInfo(entry.xmlPath).fileName()));
                m_partitionTable->setItem(row, column, item);
            }
        }
        m_emptyState->hide();
    }
    appendLog(QStringLiteral("%1 资料已载入：%2 个 XML，%3 个可刷写条目")
                  .arg(category).arg(plan.xmlPaths.size()).arg(plan.entries.size()));
    for (const QString& warning : plan.warnings) appendLog(warning);

    OplusVipPackage package;
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    QString packageDirectory = firstXml.absolutePath();
    if (firstXml.dir().dirName().compare(QStringLiteral("IMAGES"), Qt::CaseInsensitive) == 0) {
        packageDirectory = QDir(packageDirectory).absoluteFilePath(QStringLiteral(".."));
    }
    if (!OplusVipSession::discoverPackage(packageDirectory, storage, &package, &error) &&
        packageDirectory.compare(firstXml.absolutePath(), Qt::CaseInsensitive) != 0) {
        OplusVipSession::discoverPackage(firstXml.absolutePath(), storage, &package, &error);
    }
    if (package.enabled) {
        m_vipDirectoryEdit->setText(package.packageDirectory);
        m_vipPackage = package;
        m_programmerEdit->setText(package.programmerPath);
        m_vipCheck->setChecked(true);
    }
}

void MainWindow::chooseVipPackage()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择 OPPO VIP 引导包目录"), m_vipDirectoryEdit->text());
    if (directory.isEmpty()) return;
    m_vipDirectoryEdit->setText(directory);
    loadVipPackage(true);
}

bool MainWindow::loadVipPackage(bool showDialog)
{
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    QString error;
    OplusVipPackage package;
    if (!OplusVipSession::discoverPackage(m_vipDirectoryEdit->text(), storage, &package, &error)) {
        m_vipPackage = {};
        if (showDialog) ThemedMessageBox::warning(this, QStringLiteral("VIP 引导包"), error);
        else appendLog(QStringLiteral("VIP 引导包识别：%1").arg(error));
        return false;
    }
    m_vipPackage = package;
    m_programmerEdit->setText(package.programmerPath);
    m_vipDirectoryEdit->setToolTip(QStringLiteral("Programmer: %1\nDigest: %2\nSign: %3\n候选签名数: %4")
        .arg(QFileInfo(package.programmerPath).fileName(), QFileInfo(package.digestPath).fileName(),
             QFileInfo(package.signaturePath).fileName())
        .arg(package.programmerCandidates.size()));
    appendLog(QStringLiteral("VIP 引导包已识别：%1 / %2 / %3（%4 个同载荷签名候选）")
        .arg(QFileInfo(package.programmerPath).fileName(), QFileInfo(package.digestPath).fileName(),
             QFileInfo(package.signaturePath).fileName())
        .arg(package.programmerCandidates.size()));
    return true;
}

void MainWindow::toggleConnection()
{
    if (m_connected) {
        m_readGptAfterConnect = false;
        // 用户主动断开：记住这个意愿，之后扫描到同一个 9008 端口也不再自动连。
        m_autoConnectSuppressed = true;
        setBusy(true);
        emit disconnectRequested();
        return;
    }
    const QString port = m_portCombo->currentData().toString();
    QString programmer = m_programmerEdit->text().trimmed();
    if (port.isEmpty()) {
        ThemedMessageBox::information(this, QStringLiteral("连接参数"),
                                 QStringLiteral("请选择有效的 Qualcomm 9008 串口。"));
        return;
    }
    // 手动连接成功路径：解除抑制，并把该端口记为「已自动连过」，避免重复弹。
    m_autoConnectSuppressed = false;
    m_autoConnectedPort = port;
    const auto storage = static_cast<StorageType>(m_storageCombo->currentData().toInt());
    OplusVipPackage vipPackage;
    if (m_vipCheck->isChecked()) {
        if (!loadVipPackage(true)) return;
        vipPackage = m_vipPackage;
        programmer = vipPackage.programmerPath;
    }
    m_readGptAfterConnect = true;
    setBusy(true);
    emit connectRequested(port, programmer, storage, vipPackage);
}

PartitionInfo MainWindow::selectedPartition(bool* ok) const
{
    const int row = m_partitionTable->currentRow();
    const bool valid = row >= 0 && row < m_partitions.size();
    if (ok) *ok = valid;
    return valid ? m_partitions.at(row) : PartitionInfo{};
}

QVector<PartitionInfo> MainWindow::checkedPartitions() const
{
    QVector<PartitionInfo> selected;
    for (int row = 0; row < m_partitionTable->rowCount() && row < m_partitions.size(); ++row) {
        const QTableWidgetItem* item = m_partitionTable->item(row, 0);
        if (item && item->checkState() == Qt::Checked) selected.push_back(m_partitions.at(row));
    }
    return selected;
}

void MainWindow::updateCheckedCount()
{
    const int count = checkedPartitions().size();
    if (m_checkedCountLabel) {
        m_checkedCountLabel->setText(QStringLiteral("已勾选 %1 个分区").arg(count));
    }
    if (m_selectAllCheck) {
        const QSignalBlocker blocker(m_selectAllCheck);
        m_selectAllCheck->setChecked(count > 0 && count == m_partitionTable->rowCount());
    }
}

void MainWindow::refreshPartitionRowStatus(int row)
{
    if (row < 0 || row >= m_partitions.size()) return;
    QTableWidgetItem* statusItem = m_partitionTable->item(row, 4);
    const QTableWidgetItem* checkItem = m_partitionTable->item(row, 0);
    if (!statusItem || !checkItem) return;
    const bool checked = checkItem->checkState() == Qt::Checked;
    const bool imageSelected = row < m_partitionImagePaths.size() &&
        QFileInfo(m_partitionImagePaths.at(row)).isFile();
    const QString& name = m_partitions.at(row).name;
    const bool guarded = isProtectedPartitionName(name) &&
        m_protectPartitionsCheck && m_protectPartitionsCheck->isChecked();

    if (imageSelected) {
        statusItem->setText(QStringLiteral("镜像已选择"));
        statusItem->setForeground(QColor(
            m_darkTheme ? QStringLiteral("#3ddc97") : QStringLiteral("#138451")));
    } else if (checked) {
        statusItem->setText(QStringLiteral("双击选择镜像"));
        statusItem->setForeground(QColor(
            m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09")));
    } else {
        statusItem->setText(guarded ? QStringLiteral("保护已开启") : QStringLiteral("就绪"));
        statusItem->setForeground(QColor(guarded
            ? (m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09"))
            : (m_darkTheme ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))));
    }
}

bool MainWindow::applyProtectionPolicy(QVector<PartitionInfo>* partitions,
                                       QStringList* imagePaths,
                                       const QString& operation,
                                       bool* allowProtected)
{
    if (!partitions || !allowProtected) return false;
    const bool enabled = m_protectPartitionsCheck &&
                         m_protectPartitionsCheck->isChecked();
    *allowProtected = !enabled;
    if (!enabled) return !partitions->isEmpty();

    QStringList protectedNames;
    for (const PartitionInfo& partition : std::as_const(*partitions)) {
        if (isProtectedPartitionName(partition.name) &&
            !protectedNames.contains(partition.name, Qt::CaseInsensitive)) {
            protectedNames.push_back(partition.name);
        }
    }
    if (protectedNames.isEmpty()) return !partitions->isEmpty();

    // 保护分区二次确认：统一走 ThemedMessageBox（有退场动画 + 双主题皮肤）。
    // 默认按钮是「跳过」—— 与旧 QMessageBox.setDefaultButton(skipButton) 一致，
    // 误触回车不会去动校准/基带指纹分区。
    const QString confirmText =
        QStringLiteral("%1任务包含以下校准、基带指纹或设备配置分区：\n\n%2\n\n"
                       "默认动作会跳过这些分区，其余分区继续执行。")
            .arg(operation, protectedNames.join(QStringLiteral("、")));
    const int picked = ThemedMessageBox::custom(
        this, QStringLiteral("保护分区确认"), confirmText,
        {QStringLiteral("继续操作保护分区"), QStringLiteral("跳过保护分区")},
        /*defaultIndex=*/1, ThemedMessageBox::Severity::Warning);
    if (picked == 1) {
        *allowProtected = true;
        appendLog(QStringLiteral("保护分区二次确认已通过：%1").arg(
            protectedNames.join(QStringLiteral("、"))));
        return true;
    }

    for (qsizetype index = partitions->size(); index > 0; --index) {
        if (!isProtectedPartitionName(partitions->at(index - 1).name)) continue;
        partitions->removeAt(index - 1);
        if (imagePaths && index - 1 < imagePaths->size()) imagePaths->removeAt(index - 1);
    }
    appendLog(QStringLiteral("保护分区已跳过：%1").arg(
        protectedNames.join(QStringLiteral("、"))));
    return !partitions->isEmpty();
}

void MainWindow::chooseBackup()
{
    const QVector<PartitionInfo> partitions = checkedPartitions();
    if (partitions.isEmpty()) {
        ThemedMessageBox::information(this, QStringLiteral("读取分区"),
                                 QStringLiteral("请先勾选需要读取的分区。"));
        return;
    }
    QSettings settings;
    const QString initialParent = settings.value(
        QStringLiteral("backup/parentDirectory"), QDir::homePath()).toString();
    const QString parentDirectory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择回读文件夹的保存位置"), initialParent,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (parentDirectory.isEmpty()) return;
    settings.setValue(QStringLiteral("backup/parentDirectory"), parentDirectory);

    bool created = false;
    const QString directory = createBackupDirectory(parentDirectory, &created);
    if (!created) {
        ThemedMessageBox::warning(this, QStringLiteral("回读保存目录"),
                             QStringLiteral("创建回读文件夹失败：%1")
                                 .arg(parentDirectory));
        return;
    }
    appendLog(QStringLiteral("回读保存目录：%1").arg(directory));
    setBusy(true);
    emit backupPartitionsRequested(partitions, directory,
                                   m_generateXmlCheck && m_generateXmlCheck->isChecked());
}

QString MainWindow::createBackupDirectory(const QString& parent, bool* ok)
{
    if (ok) *ok = false;
    QDir directory(parent);
    const QString baseName = QStringLiteral("KunClient-回读-%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")));
    QString folderName = baseName;
    for (int suffix = 1; directory.exists(folderName); ++suffix) {
        folderName = QStringLiteral("%1-%2").arg(baseName).arg(suffix, 2, 10, QLatin1Char('0'));
    }
    if (!directory.mkpath(folderName)) return {};
    if (ok) *ok = true;
    return QDir::toNativeSeparators(
        QFileInfo(directory.filePath(folderName)).absoluteFilePath());
}

void MainWindow::choosePartitionImage(int row)
{
    if (m_busy || !m_connected || row < 0 || row >= m_partitions.size()) return;
    const QTableWidgetItem* checkItem = m_partitionTable->item(row, 0);
    if (!checkItem || checkItem->checkState() != Qt::Checked) {
        ThemedMessageBox::information(this, QStringLiteral("选择镜像"),
                                 QStringLiteral("请先勾选该分区，再双击选择刷写镜像。"));
        return;
    }
    const PartitionInfo& partition = m_partitions.at(row);
    const QString currentPath = row < m_partitionImagePaths.size()
        ? m_partitionImagePaths.at(row) : QString();
    const QString initialPath = currentPath.isEmpty()
        ? QString() : QFileInfo(currentPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("为分区 %1 选择镜像").arg(partition.name), initialPath,
        QStringLiteral("磁盘镜像 (*.img *.bin *.raw);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    QFile image(path);
    if (!image.open(QIODevice::ReadOnly) || image.size() <= 0) {
        ThemedMessageBox::warning(this, QStringLiteral("选择镜像"),
                             QStringLiteral("镜像文件读取失败：%1").arg(image.errorString()));
        return;
    }
    quint64 payloadBytes = static_cast<quint64>(image.size());
    if (SparseImage::hasSparseMagic(image)) {
        SparseImageInfo sparseInfo;
        QString sparseError;
        if (!SparseImage::inspect(image, &sparseInfo, &sparseError)) {
            ThemedMessageBox::warning(this, QStringLiteral("选择镜像"), sparseError);
            return;
        }
        payloadBytes = sparseInfo.expandedSize();
    }
    const quint64 bytesPerSector =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs)
            ? 4096ULL : 512ULL;
    const quint64 capacity = partition.sectorCount() >
            std::numeric_limits<quint64>::max() / bytesPerSector
        ? std::numeric_limits<quint64>::max()
        : partition.sectorCount() * bytesPerSector;
    if (payloadBytes > capacity) {
        ThemedMessageBox::warning(
            this, QStringLiteral("镜像过大"),
            QStringLiteral("%1 展开后为 %2，超过分区 %3 的容量 %4。")
                .arg(QFileInfo(path).fileName(), formatBytes(payloadBytes),
                     partition.name, formatBytes(capacity)));
        return;
    }

    while (m_partitionImagePaths.size() < m_partitions.size()) {
        m_partitionImagePaths.push_back(QString());
    }
    m_partitionImagePaths[row] = QFileInfo(path).absoluteFilePath();
    if (auto* fileItem = m_partitionTable->item(row, 2)) {
        fileItem->setText(QFileInfo(path).fileName());
        fileItem->setToolTip(QFileInfo(path).absoluteFilePath());
    }
    if (auto* statusItem = m_partitionTable->item(row, 4)) {
        statusItem->setText(QStringLiteral("镜像已选择"));
        statusItem->setForeground(QColor(m_darkTheme ? QStringLiteral("#3ddc97")
                                                     : QStringLiteral("#138451")));
    }
    m_partitionTable->selectRow(row);
    appendLog(QStringLiteral("分区镜像已选择：%1 → %2（%3）")
                  .arg(QFileInfo(path).fileName(), partition.name,
                       formatBytes(payloadBytes)));
}

void MainWindow::chooseImageAndFlash()
{
    QVector<PartitionInfo> partitions;
    QStringList imagePaths;
    for (int row = 0; row < m_partitionTable->rowCount() && row < m_partitions.size(); ++row) {
        const QTableWidgetItem* check = m_partitionTable->item(row, 0);
        if (check && check->checkState() == Qt::Checked) {
            partitions.push_back(m_partitions.at(row));
            imagePaths.push_back(row < m_partitionImagePaths.size()
                ? m_partitionImagePaths.at(row) : QString());
        }
    }
    if (partitions.isEmpty()) {
        ThemedMessageBox::information(this, QStringLiteral("写入分区"),
                                 QStringLiteral("请先勾选需要写入的分区。"));
        return;
    }

    bool allowProtected = false;
    if (!applyProtectionPolicy(&partitions, &imagePaths,
                               QStringLiteral("写入"), &allowProtected)) {
        appendLog(QStringLiteral("写入任务中仅包含保护分区，本次任务已结束"));
        return;
    }

    QStringList missing;
    QStringList mapping;
    for (int index = 0; index < partitions.size(); ++index) {
        const PartitionInfo& partition = partitions.at(index);
        const QString selectedPath = imagePaths.at(index);
        const QFileInfo image(selectedPath);
        if (selectedPath.isEmpty() || !image.isFile() || image.size() <= 0) {
            missing.push_back(partition.name);
            for (int row = 0; row < m_partitions.size(); ++row) {
                if (m_partitions.at(row).lun == partition.lun &&
                    m_partitions.at(row).firstLba == partition.firstLba) {
                    if (auto* statusItem = m_partitionTable->item(row, 4)) {
                        statusItem->setText(QStringLiteral("双击选择镜像"));
                        statusItem->setForeground(QColor(m_darkTheme
                            ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09")));
                    }
                    break;
                }
            }
            continue;
        }
        mapping.push_back(QStringLiteral("%1  →  %2")
                              .arg(image.fileName(), partition.name));
    }

    if (!missing.isEmpty()) {
        ThemedMessageBox::warning(
            this, QStringLiteral("刷写镜像未选择"),
            QStringLiteral("以下已勾选分区还没有指定镜像：\n%1\n\n"
                           "请双击对应分区行选择镜像，然后再次点击写入分区。")
                .arg(missing.join(QStringLiteral("、"))));
        return;
    }

    const auto answer = ThemedMessageBox::warning(
        this, QStringLiteral("确认写入"),
        QStringLiteral("即将执行 %1 个分区写入：\n\n%2\n\n该操作会覆盖原有数据。")
            .arg(partitions.size()).arg(mapping.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    setBusy(true);
    emit flashPartitionsRequested(partitions, imagePaths, allowProtected);
}

void MainWindow::preflightXmlPlan()
{
    if (m_busy) return;
    QStringList selectedPaths = m_selectedXmlPaths;
    if (selectedPaths.isEmpty()) {
        selectedPaths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择 rawprogram XML（仅源包预检，可多选，标准 / 非标准可混选）"), {},
            rawprogramXmlFilters());
    }
    if (selectedPaths.isEmpty()) return;

    const QStringList paths = expandRawprogramXmlSelection(selectedPaths);

    RawProgramPlan plan;
    QString parseError;
    const bool parsed = RawProgramParser::parse(paths, &plan, &parseError);
    const quint32 expectedSectorSize =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs) ? 4096U : 512U;
    QStringList details;
    QString inspectError;
    const bool inspected = RawProgramParser::inspect(plan, expectedSectorSize, &details, &inspectError);
    // 混选（标准 + 非标准 XML）时把重复目标摆到报告最前面 —— 预检是离线操作，
    // 这里不弹确认框，但绝不能让它看起来「一切正常」。
    const QStringList mixConflicts = xmlMixConflictLines(plan);
    if (isMixedXmlSelection(plan.xmlPaths)) {
        QStringList mixBlock{QStringLiteral("=== 混选：标准 + 非标准 XML（%1 个文件）===")
                                 .arg(plan.xmlPaths.size())};
        if (mixConflicts.isEmpty()) {
            mixBlock.push_back(QStringLiteral("未发现重复目标 / 重复 label。"));
        } else {
            mixBlock.push_back(QStringLiteral(
                "【重要警告】检测到 %1 处重复目标：同一 LUN/起始扇区会被写两次，"
                "后写的覆盖先写的；两份 XML 引用不同镜像时最终落盘的是列表靠后的那一份。")
                                   .arg(mixConflicts.size()));
            for (const QString& line : mixConflicts) mixBlock.push_back(QStringLiteral("  · ") + line);
        }
        details = mixBlock + details;
    }
    if (!parsed) {
        details.prepend(QStringLiteral("【XML 解析失败】%1\n下面只展示已解析部分和已收集警告，不是完整计划。")
                            .arg(parseError));
        if (inspected) details.last() = QStringLiteral("XML 未完整解析，预检失败；未刷写。");
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("edlPreflightDialog"));
    dialog.setWindowTitle(QStringLiteral("预检刷写计划 — 仅源包预检"));
    dialog.resize(960, 720);
    auto* layout = new QVBoxLayout(&dialog);
    auto* notice = new QLabel(QStringLiteral(
        "仅源包预检，不是已刷写：未发送设备指令，不会自动重启。\n"
        "动态起点和 LUN 容量尚需连接设备确认；数字 range 合法不等于真实 LUN 范围合法。"), &dialog);
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    QFont noticeFont = notice->font();
    noticeFont.setBold(true);
    notice->setFont(noticeFont);
    layout->addWidget(notice);
    auto* result = new QLabel(parsed && inspected
        ? QStringLiteral("源包校验通过；请务必核对下方所有警告 / 跳过项（原计划警告 %1 条）。")
              .arg(plan.warnings.size())
        : QStringLiteral("源包预检失败；下方展示全部已解析条目、错误和警告。"), &dialog);
    result->setWordWrap(true);
    layout->addWidget(result);

    auto* protectionTable = new QTableWidget(static_cast<int>(plan.entries.size()), 5, &dialog);
    protectionTable->setHorizontalHeaderLabels({QStringLiteral("条目"), QStringLiteral("LUN"),
        QStringLiteral("label"), QStringLiteral("文件"), QStringLiteral("保护分区标记")});
    protectionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    protectionTable->verticalHeader()->setVisible(false);
    protectionTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    protectionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    protectionTable->setMaximumHeight(190);
    for (int row = 0; row < protectionTable->rowCount(); ++row) {
        const RawProgramEntry& entry = plan.entries.at(row);
        const QStringList cells{QString::number(row + 1), QString::number(entry.lun), entry.label,
            entry.filename, isProtectedPartitionMapping(entry.label, entry.filename)
                ? QStringLiteral("【保护分区】仅标记，不授权写入")
                : QStringLiteral("非保护名称（不代表设备范围安全）")};
        for (int column = 0; column < cells.size(); ++column) {
            protectionTable->setItem(row, column, new QTableWidgetItem(cells.at(column)));
        }
    }
    layout->addWidget(protectionTable);
    auto* report = new QPlainTextEdit(&dialog);
    report->setReadOnly(true);
    report->setLineWrapMode(QPlainTextEdit::NoWrap);
    report->setPlainText(details.join(QLatin1Char('\n')));
    layout->addWidget(report, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::chooseXmlAndFlash()
{
    QStringList selectedPaths = m_selectedXmlPaths;
    if (selectedPaths.isEmpty()) {
        selectedPaths = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择 rawprogram XML（可多选，标准 / 非标准可混选）"), {},
            rawprogramXmlFilters());
    }
    if (selectedPaths.isEmpty()) return;

    const QStringList paths = expandRawprogramXmlSelection(selectedPaths);

    RawProgramPlan plan;
    QString error;
    if (!RawProgramParser::parse(paths, &plan, &error)) {
        ThemedMessageBox::warning(this, QStringLiteral("XML 刷机计划"), error);
        return;
    }
    if (isMixedXmlSelection(plan.xmlPaths)) {
        appendLog(QStringLiteral("XML 方案：%1（%2 个文件）")
                      .arg(xmlCategoryName(plan.xmlPaths))
                      .arg(plan.xmlPaths.size()));
    }
    m_selectedXmlPaths = plan.xmlPaths;

    int dynamicStarts = 0;
    for (const RawProgramEntry& entry : plan.entries) {
        if (!entry.startSectorResolved) ++dynamicStarts;
    }
    QString summary = QStringLiteral(
        "已选择 %1 个 XML（%2），共 %3 个 program 条目。\n"
        "镜像数据约 %4。\n"
        "将按 XML 原始顺序直接写入各 LUN 和起始扇区。")
        .arg(plan.xmlPaths.size())
        .arg(xmlCategoryName(plan.xmlPaths))
        .arg(plan.entries.size())
        .arg(formatBytes(plan.totalSourceBytes));
    if (plan.hasSplitSuper()) {
        // 免合并细节：逐动态 super 条目重建计划，列出动态分区数 / 总字节 / 槽位，
        // 让用户在点「开始」前就能核对「包里到底要写几个分区、多大」。
        // totalSourceBytes 已在解析阶段含免合并字节，这里只做明细展开，不重复计入总量。
        QStringList dynLines;
        int dynPartitions = 0;
        quint64 dynBytes = 0;
        for (const RawProgramEntry& entry : plan.entries) {
            if (!entry.dynamicSuper) continue;
            QString definitionPath;
            QString dynError;
            DynamicSuperPlan dynPlan;
            const QString label = entry.label.isEmpty() ? entry.filename : entry.label;
            if (DynamicSuperPlanner::discover(entry.xmlPath, &definitionPath, &dynError) &&
                DynamicSuperPlanner::create(entry.xmlPath, definitionPath, &dynPlan, &dynError)) {
                dynPartitions += dynPlan.images.size();
                dynBytes += dynPlan.totalWriteBytes();
                dynLines.append(QStringLiteral("  %1（免合并）：%2 个动态分区，%3，%4 个槽位")
                                   .arg(label)
                                   .arg(dynPlan.images.size())
                                   .arg(formatBytes(dynPlan.totalWriteBytes()))
                                   .arg(dynPlan.metadataSlotCount));
                for (const DynamicSuperImage& image : std::as_const(dynPlan.images)) {
                    const QString imageName = QFileInfo(image.sourcePath).fileName();
                    dynLines.append(QStringLiteral("    · %1 → %2（%3）")
                                        .arg(image.partitionName, imageName,
                                             formatBytes(image.expandedSize)));
                }
            } else {
                dynLines.append(QStringLiteral("  %1（免合并）：计划解析失败 —— %2")
                                   .arg(label, dynError));
            }
        }
        summary += QStringLiteral(
                       "\n\n检测到 %1 个 super 条目，走 META 免合并（共 %2 个动态分区，%3）：")
                       .arg(plan.superEntryCount)
                       .arg(dynPartitions)
                       .arg(formatBytes(dynBytes));
        summary += QLatin1Char('\n') + dynLines.join(QLatin1Char('\n'));
    }
    if (dynamicStarts > 0) {
        summary += QStringLiteral("\n\n有 %1 个 NUM_DISK_SECTORS 条目，将按设备各 LUN 容量解析。")
                       .arg(dynamicStarts);
    }
    if (!plan.warnings.isEmpty()) {
        summary += QStringLiteral("\n\n必须核对的计划警告（缺失/跳过项不会被写入）：\n%1")
                       .arg(plan.warnings.join(QLatin1Char('\n')));
    }
    summary += xmlMixSummaryBlock(plan);
    summary += QStringLiteral("\n\n开始执行 XML 刷机？");

    const auto answer = ThemedMessageBox::warning(
        this, QStringLiteral("确认 XML 刷机"), summary,
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    bool allowProtected = !(m_protectPartitionsCheck &&
                            m_protectPartitionsCheck->isChecked());
    QStringList protectedNames;
    for (const RawProgramEntry& entry : std::as_const(plan.entries)) {
        if (!isProtectedPartitionMapping(entry.label, entry.filename)) continue;
        const QString name = normalizedPartitionName(
            !entry.label.isEmpty() ? entry.label : entry.filename);
        if (!protectedNames.contains(name, Qt::CaseInsensitive)) protectedNames.push_back(name);
    }
    if (!allowProtected && !protectedNames.isEmpty()) {
        // XML 保护分区二次确认：同 MainWindow 手动刷写那一套，默认「跳过」。
        const QString confirmText =
            QStringLiteral("XML 计划包含以下保护分区：\n\n%1\n\n"
                           "默认动作会跳过这些分区，并锁定对应物理范围。")
                .arg(protectedNames.join(QStringLiteral("、")));
        const int picked = ThemedMessageBox::custom(
            this, QStringLiteral("XML 保护分区确认"), confirmText,
            {QStringLiteral("继续刷写保护分区"), QStringLiteral("跳过保护分区")},
            /*defaultIndex=*/1, ThemedMessageBox::Severity::Warning);
        allowProtected = (picked == 1);
        appendLog(allowProtected
            ? QStringLiteral("XML 保护分区二次确认已通过：%1")
                  .arg(protectedNames.join(QStringLiteral("、")))
            : QStringLiteral("XML 将跳过保护分区：%1")
                  .arg(protectedNames.join(QStringLiteral("、"))));
    }

    setBusy(true);
    const bool keepUserData = (m_keepUserDataCheck && m_keepUserDataCheck->isChecked());
    if (keepUserData) {
        appendLog(QStringLiteral("已勾选「保留用户数据」：本次 XML 刷写将跳过 userdata / metadata。"));
    }
    emit flashXmlRequested(plan.xmlPaths, allowProtected, keepUserData);
}

void MainWindow::confirmErase()
{
    QVector<PartitionInfo> partitions = checkedPartitions();
    if (partitions.isEmpty()) {
        ThemedMessageBox::information(this, QStringLiteral("擦除分区"),
                                 QStringLiteral("请先勾选需要擦除的分区。"));
        return;
    }
    bool allowProtected = false;
    if (!applyProtectionPolicy(&partitions, nullptr,
                               QStringLiteral("擦除"), &allowProtected)) {
        appendLog(QStringLiteral("擦除任务中仅包含保护分区，本次任务已结束"));
        return;
    }
    quint64 totalBytes = 0;
    QStringList names;
    const quint64 bytesPerSector =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs)
            ? 4096ULL : 512ULL;
    for (const PartitionInfo& partition : partitions) {
        const quint64 bytes = partition.sectorCount() * bytesPerSector;
        totalBytes = bytes > std::numeric_limits<quint64>::max() - totalBytes
            ? std::numeric_limits<quint64>::max() : totalBytes + bytes;
        names.push_back(QStringLiteral("%1 (LUN %2)").arg(partition.name).arg(partition.lun));
    }
    const QString summary = QStringLiteral("即将擦除 %1 个分区，共 %2。")
                                .arg(partitions.size()).arg(formatBytes(totalBytes));
    kc::ui::EraseConfirmDialog dlg(this, summary, names);
    if (dlg.exec() != QDialog::Accepted || !dlg.confirmed()) return;
    setBusy(true);
    emit erasePartitionsRequested(partitions, allowProtected);
}

void MainWindow::confirmReboot(bool toEdl)
{
    if (!m_connected) return;
    const QString destination = toEdl ? QStringLiteral("9008 模式")
                                      : QStringLiteral("系统");
    if (ThemedMessageBox::question(
            this, QStringLiteral("重启到%1").arg(destination),
            QStringLiteral("确认通过 Firehose 将设备重启到%1？").arg(destination),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在发送重启到%1命令…").arg(destination));
    emit rebootRequested(toEdl);
}

void MainWindow::confirmRebootToRecovery()
{
    if (!m_connected || m_busy) return;
    if (ThemedMessageBox::question(
            this, QStringLiteral("重启到 Recovery"),
            QStringLiteral("将向 misc 分区写入 BCB（boot-recovery）并重启设备。\n"
                           "设备会进入 Recovery 模式，EDL 会话随之断开。是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在写入 BCB 并重启到 Recovery…"));
    emit rebootWithBcbRequested(QStringLiteral("boot-recovery"));
}

void MainWindow::confirmRebootToFastbootD()
{
    if (!m_connected || m_busy) return;
    if (ThemedMessageBox::question(
            this, QStringLiteral("重启到 FastbootD"),
            QStringLiteral("将向 misc 分区写入 BCB（boot-fastboot）并重启设备。\n"
                           "设备会进入 FastbootD 模式，EDL 会话随之断开。是否继续？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在写入 BCB 并重启到 FastbootD…"));
    emit rebootWithBcbRequested(QStringLiteral("boot-fastboot"));
}

void MainWindow::confirmFactoryReset()
{
    if (!m_connected || m_busy) return;
    // 与 worker 同口径预告将擦的分区（归一化名忽略 _a/_b 槽位）——
    // 恢复出厂是破坏性操作，弹窗里必须点名，不能只说「将擦除若干分区」。
    QStringList names;
    for (const PartitionInfo& partition : m_partitions) {
        const QString normalized = normalizedPartitionName(partition.name);
        if (normalized == QLatin1String("userdata") ||
            normalized == QLatin1String("metadata")) {
            names.push_back(QStringLiteral("%1（LUN %2）")
                                .arg(partition.name)
                                .arg(partition.lun));
        }
    }
    if (names.isEmpty()) {
        ThemedMessageBox::information(
            this, QStringLiteral("恢复出厂"),
            QStringLiteral("未找到 userdata / metadata 分区——请先读取 GPT 分区表。"));
        return;
    }
    if (ThemedMessageBox::question(
            this, QStringLiteral("恢复出厂"),
            QStringLiteral("将擦除以下分区：%1\n\n设备上的用户数据会全部丢失，"
                           "其余分区不受影响。是否继续？")
                .arg(names.join(QStringLiteral("、"))),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在执行恢复出厂（擦除 userdata / metadata）…"));
    emit factoryResetRequested();
}

void MainWindow::confirmEnableOemUnlock()
{
    if (!m_connected || m_busy) return;
    if (ThemedMessageBox::question(
            this, QStringLiteral("强开 OEM 解锁"),
            QStringLiteral("将修改 devinfo 分区的解锁开关并重启设备。\n\n"
                           "· 仅对带 devinfo 分区的机型有效；\n"
                           "· 2024 年及以后的部分机型因 AR fuse 已烧写，此操作可能无效；\n"
                           "· 解锁开关生效后，Bootloader 仍可能要求厂商账号验证。\n\n"
                           "确认继续？"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在读取 devinfo 分区并修改 OEM 解锁开关…"));
    emit enableOemUnlockRequested();
}

void MainWindow::chooseBasebandBackup()
{
    if (!m_connected || m_busy) return;
    QSettings settings;
    const QString initialParent = settings.value(
        QStringLiteral("backup/parentDirectory"), QDir::homePath()).toString();
    const QString parentDirectory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择基带备份的保存位置"), initialParent,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (parentDirectory.isEmpty()) return;
    settings.setValue(QStringLiteral("backup/parentDirectory"), parentDirectory);
    bool created = false;
    const QString directory = createBackupDirectory(parentDirectory, &created);
    if (!created) {
        ThemedMessageBox::warning(this, QStringLiteral("基带备份"),
                             QStringLiteral("创建备份文件夹失败：%1")
                                 .arg(parentDirectory));
        return;
    }
    appendLog(QStringLiteral("基带备份保存目录：%1").arg(directory));
    setBusy(true);
    emit backupBasebandRequested(directory);
}

void MainWindow::requestStorageInfo()
{
    if (!m_connected || m_busy) return;
    setBusy(true);
    appendLog(QStringLiteral("正在逐 LUN 读取存储信息…"));
    emit readStorageInfoRequested();
}

void MainWindow::confirmEraseLun()
{
    if (!m_connected || m_busy) return;
    const quint32 lun = m_lunCombo ? m_lunCombo->currentData().toUInt() : 0U;
    if (ThemedMessageBox::question(
            this, QStringLiteral("擦除磁盘 LUN"),
            QStringLiteral("将整盘擦除 LUN %1 的全部扇区（含分区表）。\n"
                           "该 LUN 上的所有数据都会丢失，且无法通过本工具撤销。是否继续？")
                .arg(lun),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在整盘擦除 LUN %1…").arg(lun));
    emit eraseLunRequested(lun);
}

void MainWindow::writeGptFromFile()
{
    if (!m_connected || m_busy) return;
    const quint32 lun = m_lunCombo ? m_lunCombo->currentData().toUInt() : 0U;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要写入 LUN %1 的 GPT 文件").arg(lun), QString(),
        QStringLiteral("GPT 镜像 (*.bin *.img);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    if (ThemedMessageBox::question(
            this, QStringLiteral("写入 GPT"),
            QStringLiteral("将把 %1 写入 LUN %2 的起始扇区。\n"
                           "该 LUN 的现有分区表会被覆盖，写完后请重新读取分区表。是否继续？")
                .arg(QFileInfo(path).fileName()).arg(lun),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes) return;
    setBusy(true);
    appendLog(QStringLiteral("正在写入 GPT 到 LUN %1…").arg(lun));
    emit writeGptRequested(path, lun);
}

void MainWindow::probeStorageBrand()
{
    // 「小米字库牌子」2026-10-21 从独立页搬进 EDL 页的「高级功能 ▾」：
    // 复用本页当前选中的串口与引导程序，日志直接进 EDL 日志，结果也写在日志里
    // （独立页那张结果卡没了，用户口径「结果显示在日志里」）。
    if (m_brandProbeRunning) {
        appendLog(QStringLiteral("字库牌子：上一次探测还没结束，请稍候"));
        return;
    }
    const QString port = m_portCombo ? m_portCombo->currentData().toString() : QString();
    const QString programmer = m_programmerEdit ? m_programmerEdit->text().trimmed() : QString();
    if (port.isEmpty()) {
        appendLog(QStringLiteral("字库牌子：请先选择 9008 串口（左侧「串口」下拉）"));
        return;
    }
    if (programmer.isEmpty() || !QFileInfo::exists(programmer)) {
        appendLog(QStringLiteral("字库牌子：请先在「引导程序」里选择 firehose 文件"
                                 "（prog_*firehose*.elf 或 xbl_*devprg*.melf）"));
        return;
    }

    m_brandProbeRunning = true;
    appendLog(QStringLiteral("字库牌子：开始探测（端口 %1 / 引导 %2）")
                  .arg(port, QFileInfo(programmer).fileName()));

    auto cancel = std::make_shared<std::atomic_bool>(false);
    QPointer<MainWindow> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([guard, port, programmer, cancel] {
        const auto out = kc::devices::runStorageBrandProbe(
            port, programmer, *cancel, [guard](const QString& line) {
                if (!guard) return;
                QMetaObject::invokeMethod(guard, [guard, line] {
                    if (guard) guard->appendLog(line);
                }, Qt::QueuedConnection);
            });
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, out] {
            if (!guard) return;
            guard->m_brandProbeRunning = false;
            guard->finishStorageBrandProbe(out);
        }, Qt::QueuedConnection);
    }));
}

void MainWindow::finishStorageBrandProbe(const kc::devices::StorageBrandProbeOutcome& out)
{
    if (!out.ok) {
        appendLog(QStringLiteral("字库牌子：探测失败 —— %1").arg(out.error));
        return;
    }
    const kc::devices::StorageBrandResult& r = out.result;
    QString brand = kc::devices::storageBrandTitle(r);
    if (brand.isEmpty()) brand = r.vendor;
    appendLog(QStringLiteral("字库牌子：%1 · %2%3")
                  .arg(brand.isEmpty() ? QStringLiteral("未知厂商") : brand,
                       r.storageType.isEmpty() ? QStringLiteral("存储") : r.storageType,
                       r.chipSerial.isEmpty() ? QString()
                                              : QStringLiteral(" · 芯片序列号 ") + r.chipSerial));
    if (!r.rawLine.isEmpty())
        appendLog(QStringLiteral("字库牌子：原始回包 %1").arg(r.rawLine));
}

void MainWindow::readGptToFile()
{
    if (!m_connected || m_busy) return;
    const quint32 lun = m_lunCombo ? m_lunCombo->currentData().toUInt() : 0U;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存 LUN %1 的 GPT").arg(lun), gptFileName(lun, false),
        QStringLiteral("GPT 镜像 (*.bin);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    setBusy(true);
    appendLog(QStringLiteral("正在读取 LUN %1 的 GPT…").arg(lun));
    emit readGptToFileRequested(path, lun);
}

void MainWindow::applyTheme(bool dark)
{
    m_darkTheme = dark;
    // 嵌入模式下全局样式表与持久化都归外壳（ThemeManager）：这里再刷一次
    // qApp->setStyleSheet 会把整个应用翻到另一个主题，写 QSettings 则会覆盖
    // 外壳的默认（浅色）。独立运行保持原行为。
    if (!m_embedded) {
        QFile style(dark ? QStringLiteral(":/styles/dark.qss")
                         : QStringLiteral(":/styles/light.qss"));
        if (style.open(QIODevice::ReadOnly)) {
            qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
        }
        QSettings settings;
        settings.setValue(QStringLiteral("appearance/darkTheme"), dark);
    }
    if (m_themeButton) {
        m_themeButton->setText(dark ? QStringLiteral("●  深色模式")
                                    : QStringLiteral("○  浅色模式"));
        m_themeButton->setToolTip(dark ? QStringLiteral("点击切换到浅色模式")
                                       : QStringLiteral("点击切换到深色模式"));
    }
    if (m_partitionTable && m_partitionTable->rowCount() == m_partitions.size()) {
        for (int row = 0; row < m_partitions.size(); ++row) {
            const bool guarded = isProtectedPartitionName(m_partitions.at(row).name) &&
                m_protectPartitionsCheck && m_protectPartitionsCheck->isChecked();
            if (QTableWidgetItem* status = m_partitionTable->item(row, 4)) {
                status->setForeground(QColor(guarded
                    ? (dark ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09"))
                    : (dark ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))));
            }
        }
    }
    // 日志画刷是渲染时定格的：换主题必须整篇重画，否则浅色底上留着深色主题
    // 画上去的浅字（2026-10-12 那次「整片日志发白看不见」）。LogView 自己也接了
    // ThemeManager::themeChanged，这里是嵌入模式下由外壳 applyTheme 转发时的兜底。
    if (m_logView) m_logView->recolor();
}

void MainWindow::appendLog(const QString& message)
{
    if (!m_logView) return;
    // 只做一件事：把引擎的纯文本行翻成级别喂给 LogView。时间戳、着色、档位
    // 筛选、批量渲染、历史上限、落盘全部由 LogView 负责（原先这些散在本类的
    // m_logHistory / formatLogHtml / shouldShowLog / rebuildLogView 四处）。
    m_logView->appendLine(edlLogLevel(message), message);
}

void MainWindow::updateState(SessionState state, const QString& detail)
{
    if (state == SessionState::Ready) m_connected = true;
    if (state == SessionState::Disconnected || state == SessionState::Failed) m_connected = false;
    m_connectButton->setText(m_connected ? QStringLiteral("断开连接") : QStringLiteral("查找分区"));
    m_stateBanner->setText(detail);
    m_modeValue->setText(stateText(state));
    m_bottomStatus->setText(QStringLiteral("●  %1").arg(stateText(state)));
    m_bottomStatus->setProperty("connected", m_connected);
    m_bottomStatus->setProperty("failed", state == SessionState::Failed);
    m_bottomStatus->style()->unpolish(m_bottomStatus);
    m_bottomStatus->style()->polish(m_bottomStatus);
    m_protocolStatus->setText(state == SessionState::Failed
        ? QStringLiteral("协议通信状态：异常") : QStringLiteral("协议通信状态：正常"));
    m_protocolStatus->setProperty("failed", state == SessionState::Failed);
    m_protocolStatus->style()->unpolish(m_protocolStatus);
    m_protocolStatus->style()->polish(m_protocolStatus);
    // 嵌入外壳时把这两格读数上报给外壳状态栏（本窗页脚已整条收起，见 setEmbeddedMode）。
    // 独立运行时页脚照旧显示，不上报。
    if (m_embedded) {
        const bool failed = (state == SessionState::Failed);
        kc::ui::ShellStatus::setEdlLink(
            failed ? QStringLiteral("异常") : QStringLiteral("正常"),
            QStringLiteral("●  %1").arg(stateText(state)),
            failed ? QStringLiteral("failed")
                   : (m_connected ? QStringLiteral("ok") : QStringLiteral("idle")));
    }
}

void MainWindow::updateProgress(quint64 completed, quint64 total)
{
    if (total == 0) {
        m_taskProgressTarget = 0;
        m_progressBar->setValue(0);
        m_progressText->setText(QStringLiteral("当前任务  0.0%"));
        m_progressText->setToolTip({});
        return;
    }

    const double ratio = qMin(1.0, static_cast<double>(completed) / static_cast<double>(total));
    m_taskProgressTarget = static_cast<int>(ratio * 1000.0);
    if (completed == 0) m_progressBar->setValue(0);
    m_progressText->setText(QStringLiteral("当前任务  %1%")
                                .arg(ratio * 100.0, 0, 'f', 1));
    m_progressText->setToolTip(QStringLiteral("%1 / %2")
                                   .arg(formatBytes(completed), formatBytes(total)));
    if (!m_progressAnimationTimer->isActive()) m_progressAnimationTimer->start();
}

void MainWindow::updateOverallProgress(quint64 completed, quint64 total)
{
    if (total == 0) {
        m_overallProgressTarget = 0;
        m_totalProgressBar->setValue(0);
        m_totalProgressText->setText(QStringLiteral("总体进度  0.0%"));
        m_totalProgressText->setToolTip({});
        m_speedTimer.invalidate();
        m_speedLastCompleted = 0;
        m_speedTotal = 0;
        m_speedLastUpdateMs = 0;
        m_smoothedBytesPerSecond = 0.0;
        m_speedText->setText(QStringLiteral("速率  0.00 MB/s"));
        return;
    }

    const bool transferChanged = !m_speedTimer.isValid() || total != m_speedTotal ||
                                 completed < m_speedLastCompleted;
    if (transferChanged) {
        m_speedTimer.start();
        m_speedLastCompleted = completed;
        m_speedTotal = total;
        m_speedLastUpdateMs = 0;
        m_smoothedBytesPerSecond = 0.0;
        m_speedText->setText(QStringLiteral("速率  0.00 MB/s"));
    } else if (total >= 64ULL * 1024ULL &&
               (m_speedTimer.elapsed() - m_speedLastUpdateMs >= 500 || completed == total)) {
        const qint64 elapsedMs = m_speedTimer.elapsed();
        const quint64 transferred = completed - m_speedLastCompleted;
        if (elapsedMs > 0 && transferred > 0) {
            const double average = static_cast<double>(transferred) * 1000.0 /
                                   static_cast<double>(elapsedMs);
            m_smoothedBytesPerSecond = m_smoothedBytesPerSecond <= 0.0
                ? average
                : m_smoothedBytesPerSecond * 0.80 + average * 0.20;
            m_speedText->setText(QStringLiteral("速率  %1 MB/s")
                                     .arg(m_smoothedBytesPerSecond / (1024.0 * 1024.0),
                                          0, 'f', 2));
        }
        m_speedLastUpdateMs = elapsedMs;
    }
    const double ratio = qMin(1.0, static_cast<double>(completed) /
                                    static_cast<double>(total));
    m_overallProgressTarget = static_cast<int>(ratio * 1000.0);
    if (completed == 0) m_totalProgressBar->setValue(0);
    m_totalProgressText->setText(QStringLiteral("总体进度  %1%")
                                     .arg(ratio * 100.0, 0, 'f', 1));
    m_totalProgressText->setToolTip(QStringLiteral("%1 / %2").arg(completed).arg(total));
    if (!m_progressAnimationTimer->isActive()) m_progressAnimationTimer->start();
}

void MainWindow::updateDeviceInfo(const DeviceInfo& info)
{
    m_portValue->setText(info.portName.isEmpty() ? QStringLiteral("—") : info.portName);
    const QString storageText = info.storageBytes > 0
        ? QStringLiteral("%1 GB").arg(
              static_cast<double>(info.storageBytes) /
                  (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
        : QStringLiteral("—");
    m_storageValue->setText(storageText);
    m_storageValue->setToolTip(QStringLiteral("存储类型：%1\n原始字节数：%2")
        .arg(info.storage.isEmpty() ? QStringLiteral("—") : info.storage)
        .arg(info.storageBytes));
    m_programmerValue->setText(info.programmer.isEmpty() ? QStringLiteral("—") :
        (info.vipMode.isEmpty() ? info.programmer : QStringLiteral("%1  ·  %2").arg(info.programmer, info.vipMode)));
    const QString detectedBrand = !info.brand.isEmpty() ? info.brand :
        (info.vipMode.contains(QStringLiteral("OPPO"), Qt::CaseInsensitive)
             ? QStringLiteral("OPLUS") : QStringLiteral("Qualcomm"));
    m_brandValue->setText(info.portName.isEmpty() ? QStringLiteral("—") : detectedBrand);
    QString chip = info.model;
    static const QRegularExpression chipName(
        QStringLiteral("((?:SM|MSM|SDM|APQ)[0-9]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch chipMatch = chipName.match(chip);
    if (chipMatch.hasMatch()) chip = chipMatch.captured(1).toUpper();
    else if (chip.isEmpty() && !info.msmId.isEmpty()) chip = info.msmId.toUpper();
    m_modelValue->setText(chip.isEmpty() ? QStringLiteral("—") : chip);
    const QString identity = !info.serial.isEmpty() ? info.serial : info.hwid;
    m_cpuidValue->setText(identity.isEmpty() ? QStringLiteral("—") : identity);
    m_systemValue->setText(info.storageProduct.isEmpty()
        ? QStringLiteral("—")
        : (info.storageVendor.isEmpty()
               ? info.storageProduct
               : QStringLiteral("%1 %2").arg(info.storageVendor, info.storageProduct)));
    m_productNameValue->setText(info.productName.isEmpty()
        ? QStringLiteral("—") : info.productName);
    m_androidValue->setText(info.androidVersion.isEmpty()
        ? QStringLiteral("—") : info.androidVersion);
    m_versionValue->setText(info.systemVersion.isEmpty()
        ? QStringLiteral("—") : info.systemVersion);
    m_versionValue->setToolTip(info.buildFingerprint);
    m_securityPatchValue->setText(info.securityPatch.isEmpty()
        ? QStringLiteral("—") : info.securityPatch);
    m_stateBanner->setToolTip(info.portName.isEmpty() ? QString() :
        QStringLiteral("串口：%1\nProgrammer：%2\n模式：%3\nSahara：%4\nMSM ID：%5\nSerial：%6\nPK Hash：%7\n存储型号：%8\n存储厂商：%9\n设备名称：%10\nAndroid：%11\n系统版本：%12\n安全补丁：%13")
            .arg(info.portName, info.programmer, info.vipMode,
                 info.saharaVersion, info.msmId, info.serial, info.pkHash,
                 info.storageProduct, info.storageVendor, info.productName,
                 info.androidVersion, info.systemVersion, info.securityPatch));
}

void MainWindow::showPartitions(const QVector<PartitionInfo>& partitions)
{
    const QSignalBlocker tableBlocker(m_partitionTable);
    const QSignalBlocker selectAllBlocker(m_selectAllCheck);
    m_partitions = partitions;
    m_partitionImagePaths.clear();
    m_partitionImagePaths.reserve(partitions.size());
    for (int row = 0; row < partitions.size(); ++row) {
        m_partitionImagePaths.push_back(QString());
    }
    m_partitionTable->setRowCount(partitions.size());
    m_selectAllCheck->setChecked(false);
    m_emptyState->setText(QStringLiteral("选择 rawprogram XML 后，这里会显示分区镜像\n\n连接设备后可双击分区选择独立镜像"));
    m_emptyState->setVisible(partitions.isEmpty());
    const quint64 bytesPerSector =
        m_storageCombo->currentData().toInt() == static_cast<int>(StorageType::Ufs) ? 4096ULL : 512ULL;
    for (int row = 0; row < partitions.size(); ++row) {
        const PartitionInfo& p = partitions.at(row);
        const bool guarded = isProtectedPartitionName(p.name) &&
            m_protectPartitionsCheck && m_protectPartitionsCheck->isChecked();
        const QStringList columns{QString(), p.name, QStringLiteral("—"),
                                  formatBytes(p.sectorCount() * bytesPerSector),
                                  guarded ? QStringLiteral("保护已开启")
                                          : QStringLiteral("就绪")};
        for (int column = 0; column < columns.size(); ++column) {
            auto* item = new QTableWidgetItem(columns.at(column));
            if (column == 0) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Unchecked);
            }
            if (column == 4) {
                item->setForeground(QColor(guarded
                    ? (m_darkTheme ? QStringLiteral("#f4c95d") : QStringLiteral("#a96c09"))
                    : (m_darkTheme ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))));
            }
            item->setToolTip(QStringLiteral("LUN %1，起始扇区 %2，扇区数 %3")
                                 .arg(p.lun).arg(p.firstLba).arg(p.sectorCount()));
            m_partitionTable->setItem(row, column, item);
        }
    }
    if (!partitions.isEmpty()) m_partitionTable->selectRow(0);
    updateCheckedCount();
}

void MainWindow::operationFinished(bool success, const QString& message)
{
    const bool startAutomaticGptRead = success && m_readGptAfterConnect && m_connected;
    const bool startVersionRead = success && m_connected &&
        message.startsWith(QStringLiteral("分区表读取完成")) &&
        (m_readVersionAfterGpt || (m_readInfoCheck && m_readInfoCheck->isChecked()));
    const bool completedDataReadOrWrite =
        message.startsWith(QStringLiteral("批量读取完成")) ||
        (message.startsWith(QStringLiteral("分区 ")) &&
         (message.contains(QStringLiteral("已备份")) ||
          message.endsWith(QStringLiteral("写入完成")))) ||
        message.startsWith(QStringLiteral("批量写入完成")) ||
        // 「批量擦除完成」以前不在列表里 ⇒ 勾了自动重启再点擦除根本不会重启。
        message.startsWith(QStringLiteral("批量擦除完成")) ||
        message.startsWith(QStringLiteral("XML 刷机完成"));
    const bool autoRebootAfterData = success && m_connected &&
        m_autoRebootCheck && m_autoRebootCheck->isChecked() &&
        completedDataReadOrWrite;
    // 擦除后通常紧接着还要刷写，所以自动重启的目标必须回 9008：设备一旦被
    // reset 到系统就不再是 EDL，用户「重新发引导」会卡在「怎么再进 9008」上。
    // 读/写/刷机完成则相反，目标是进系统验证。
    const bool eraseJustFinished = message.startsWith(QStringLiteral("批量擦除完成"));
    if (!success || !m_connected) {
        m_readGptAfterConnect = false;
        m_readVersionAfterGpt = false;
    }
    setBusy(false);
    if (!success && !message.contains(QStringLiteral("停止"))) {
        ThemedMessageBox::warning(this, QStringLiteral("EDL 操作"), message);
    }
    if (startAutomaticGptRead) {
        m_readGptAfterConnect = false;
        appendLog(QStringLiteral("连接完成，自动读取 GPT 分区表…"));
        setBusy(true);
        emit readGptRequested();
    } else if (startVersionRead) {
        m_readVersionAfterGpt = false;
        appendLog(QStringLiteral("分区表已就绪，自动读取系统版本信息…"));
        setBusy(true);
        emit readVersionInfoRequested();
    } else if (autoRebootAfterData) {
        appendLog(eraseJustFinished
            ? QStringLiteral("擦除任务完成，按设置自动重启回 9008（设备将重新枚举，"
                             "可直接点连接重新下发引导）…")
            : QStringLiteral("读写任务完成，按设置自动重启到系统…"));
        setBusy(true);
        emit rebootRequested(eraseJustFinished);
    }
}

void MainWindow::setBusy(bool busy)
{
    if (busy && !m_busy) {
        m_speedTimer.invalidate();
        m_speedLastCompleted = 0;
        m_speedTotal = 0;
        m_speedLastUpdateMs = 0;
        m_smoothedBytesPerSecond = 0.0;
        if (m_speedText) m_speedText->setText(QStringLiteral("速率  0.00 MB/s"));
    }
    m_busy = busy;
    m_portCombo->setEnabled(!busy && !m_connected);
    m_storageCombo->setEnabled(!busy && !m_connected);
    m_modeCombo->setEnabled(!busy && !m_connected);
    m_chipsetCombo->setEnabled(!busy && !m_connected);
    m_programmerEdit->setEnabled(!busy && !m_connected);
    m_firmwareDirectoryEdit->setEnabled(!busy);
    m_firmwareBrowseButton->setEnabled(!busy);
    if (m_autoXmlButton) m_autoXmlButton->setEnabled(!busy);
    m_edlPreflightButton->setEnabled(!busy);
    m_autoScanCheck->setEnabled(!busy && !m_connected);
    if (m_autoScanTimer) {
        if (!busy && !m_connected && m_autoScanCheck->isChecked()) m_autoScanTimer->start();
        else m_autoScanTimer->stop();
    }
    m_vipCheck->setEnabled(!busy && !m_connected);
    m_autoRebootCheck->setEnabled(!busy);
    m_generateXmlCheck->setEnabled(!busy);
    m_protectPartitionsCheck->setEnabled(!busy);
    m_readInfoCheck->setEnabled(!busy);
    m_vipDirectoryEdit->setEnabled(!busy && !m_connected && m_vipCheck->isChecked());
    m_vipBrowseButton->setEnabled(!busy && !m_connected && m_vipCheck->isChecked());
    m_connectButton->setEnabled(!busy);
    const bool ready = m_connected && !busy;
    const bool hasPort = m_portCombo && !m_portCombo->currentData().toString().isEmpty();
    m_readGptButton->setEnabled(!busy && (m_connected || hasPort));
    m_readVersionButton->setEnabled(!busy && (m_connected || hasPort));
    m_backupButton->setEnabled(ready && !m_partitions.isEmpty());
    m_flashButton->setEnabled(ready);
    m_eraseButton->setEnabled(ready && !m_partitions.isEmpty());
    m_rebootButton->setEnabled(ready);
    m_stopButton->setEnabled(busy);
    m_selectAllCheck->setEnabled(ready && !m_partitions.isEmpty());
    // 高级功能下拉（2026-10-19：弹窗 → 下拉）：依赖 GPT 的项要等分区表就绪；
    // 存储信息 / LUN / GPT 只依赖连接。逐项置灰而不是整框禁用 —— 未连接时也允许
    // 展开看一眼有哪些工具（灰条目不响应选中）。
    if (m_lunCombo) m_lunCombo->setEnabled(ready);
    if (m_advancedCombo) m_advancedCombo->setEnabled(!busy);
    updateAdvancedItems();
}

void MainWindow::updateAdvancedItems()
{
    if (!m_advancedCombo) return;
    auto* model = qobject_cast<QStandardItemModel*>(m_advancedCombo->model());
    if (!model) return;

    const bool ready = m_connected && !m_busy;
    const bool hasPartitions = !m_partitions.isEmpty();
    for (int i = 1; i < m_advancedCombo->count(); ++i) {
        QStandardItem* item = model->item(i);
        if (!item) continue;
        const QString action = item->data(Qt::UserRole).toString();
        if (action.isEmpty()) continue;   // 分组标题：保持不可选
        bool enabled = false;
        if (action == QLatin1String("factoryReset") ||
            action == QLatin1String("oemUnlock") ||
            action == QLatin1String("basebandBackup")) {
            enabled = ready && hasPartitions;
        } else {
            // storageInfo / eraseLun / readGpt / writeGpt
            enabled = ready;
        }
        // 🔴 用 ItemIsEnabled 而不是 setEnabled：QStandardItemModel 下只有
        //    ItemIsEnabled 会让条目变灰且点不动，setEnabled 是 QWidget 的接口。
        item->setEnabled(enabled);
    }
}

QString MainWindow::formatBytes(quint64 bytes)
{
    static const QStringList units{QStringLiteral("B"), QStringLiteral("KiB"),
                                   QStringLiteral("MiB"), QStringLiteral("GiB"),
                                   QStringLiteral("TiB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, unit == 0 ? 'f' : 'f', unit == 0 ? 0 : 2).arg(units.at(unit));
}

} // namespace swt::edl
