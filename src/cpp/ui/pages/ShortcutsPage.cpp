#include "ui/pages/ShortcutsPage.h"

#include "kc/security/Digest.h"
#include "kc/tools/FirmwareChecksum.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSettings>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
//  快捷指令页的实现
//
//  三段结构，与页面布局一一对应：
//    · 左列：指令列表（选中即加载到右侧编辑器）；
//    · 右列：编辑器（名称 / 标签 / 步骤表格）；
//    · 底部：执行日志。
//
//  🔴 运行一定在**后台线程**（QThreadPool + LambdaTask）：Wait 步骤会 msleep，
//     VerifyChecksum 要读几 GB 的镜像算摘要 —— 放 UI 线程就是整窗卡死，
//     连「停止」都点不动。日志一律经 runOnUi 回 UI 线程再碰控件。
//
//  🔴 页面不持有任何设备句柄：需要设备的动作只发 requestDeviceAction 转交外壳。
//     这是本页与 EDL 页的边界 —— 快捷指令是「编排」，不是「设备驱动」。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::ui {
namespace {

/// QThreadPool 跑一个 std::function 的最小载体（与 PackageImageListDialog 同款）。
/// 不引 QtConcurrent：这里只需要「起一个线程执行一段 lambda」。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

// QSettings 键：整表存成 formatShortcutList() 的一段多行文本。
// 用户可以直接在设置目录里打开改（这就是不用 JSON 的理由，见 ShortcutModel.h）。
const char* kSettingsKey = "shortcuts/text";

/// 等待步骤的硬上限（秒）。Wait 是给人「插在两步之间等设备重启」用的，
/// 不是定时器 —— 十分钟的等待只可能是参数写错了。
constexpr int kMaxWaitSeconds = 60;

/// 左列宽度：够放下「标准备份流程 · 4 步」这种最长条目，又不至于挤压编辑器。
constexpr int kListWidth = 248;

/// 左列一条的两行文字：第一行名称、第二行「N 步 · 标签」。
/// 抽成自由函数是因为 reloadList / refreshListItem 两处都要用，且**必须逐字一致**
/// —— 两处各写一份，改文案时漏一处就是「重进页面文字变样」。
QString listItemText(const kc::tools::Shortcut& shortcut)
{
    const QString title = shortcut.name.trimmed().isEmpty() ? QStringLiteral("未命名")
                                                            : shortcut.name;
    const QString tags = shortcut.tags.isEmpty() ? QStringLiteral("无标签")
                                                 : shortcut.tags.join(QLatin1Char('/'));
    // 用一次性 arg 多参而不是链式 .arg()：指令名是用户输入，里面若有 "%2"
    // 链式调用会把它当成占位符吃掉（QString::arg 的经典坑）。
    return QStringLiteral("%1\n%2 步 · %3")
        .arg(title)
        .arg(static_cast<int>(shortcut.steps.size()))
        .arg(tags);
}

/// 悬停提示：名称 + 完整执行计划（一行一步）。左列只显示步数，细节在这里看。
QString listItemToolTip(const kc::tools::Shortcut& shortcut)
{
    const QString title = shortcut.name.trimmed().isEmpty() ? QStringLiteral("未命名")
                                                            : shortcut.name;
    return QStringLiteral("%1\n%2")
        .arg(title, kc::tools::describePlan(shortcut).join(QLatin1Char('\n')));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  构造 / 持久化
// ─────────────────────────────────────────────────────────────────────────────

ShortcutsPage::ShortcutsPage(QWidget* parent) : BasePage(parent)
{
    setObjectName(QStringLiteral("shortcutsPage"));
    setTitle(QStringLiteral("快捷指令"));
    buildUi();

    // 构造时先读一次：有配置就显示，没有就留空态（内置指令在首次 showEvent 才灌，
    // 因为「首次使用」的判据是「用户真的打开了这一页」，不是「进程启动了」）。
    QSettings settings;
    m_shortcuts = kc::tools::parseShortcutList(
        settings.value(QString::fromLatin1(kSettingsKey)).toString());
    reloadList();
    if (!m_shortcuts.isEmpty()) {
        m_syncing = true;
        m_list->setCurrentRow(0);
        m_syncing = false;
        loadEditor(0);
    } else {
        loadEditor(-1);
    }
}

QVector<kc::tools::Shortcut> ShortcutsPage::builtinShortcuts()
{
    // 内置指令刻意留了 BackupAll / VerifyChecksum 的空参数：这两步的真实路径要
    // 用户自己填（备份到哪、校验哪个文件），填之前运行会在日志里说明并跳过。
    // 不填一个假的 D:/backup 当默认值 —— 用户会以为真的备份到那儿了。
    kc::tools::Shortcut backup;
    backup.name = QStringLiteral("标准备份流程");
    backup.tags = QStringList{ QStringLiteral("常用") };

    kc::tools::ShortcutStep connect;
    connect.action = kc::tools::ShortcutAction::Connect;

    kc::tools::ShortcutStep readGpt;
    readGpt.action = kc::tools::ShortcutAction::ReadGpt;

    kc::tools::ShortcutStep backupAll;
    backupAll.action = kc::tools::ShortcutAction::BackupAll;

    kc::tools::ShortcutStep verify;
    verify.action = kc::tools::ShortcutAction::VerifyChecksum;

    backup.steps = { connect, readGpt, backupAll, verify };
    return { backup };
}

void ShortcutsPage::showEvent(QShowEvent* event)
{
    BasePage::showEvent(event);

    // 首次显示才灌内置指令。判据是「这个键**从来不存在**」，不是「值为空」——
    // 用户可能把指令全删了（键还在、值是空串），那种情况下再灌一遍会像删不掉。
    QSettings settings;
    const QString key = QString::fromLatin1(kSettingsKey);
    if (!settings.contains(key) && m_shortcuts.isEmpty()) {
        m_shortcuts = builtinShortcuts();
        saveToSettings();
        reloadList();
        if (!m_shortcuts.isEmpty()) {
            m_syncing = true;
            m_list->setCurrentRow(0);
            m_syncing = false;
            loadEditor(0);
        }
        appendLog(QStringLiteral("已写入内置指令「标准备份流程」（4 步）；回读全部分区 / 校验文件摘要的参数需要自己填。"));    }
}

void ShortcutsPage::saveToSettings()
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(kSettingsKey),
                      kc::tools::formatShortcutList(m_shortcuts));
}

// ─────────────────────────────────────────────────────────────────────────────
//  界面
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPage::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    root->addWidget(m_notify);

    // ── 标题 + 一行说明 ─────────────────────────────────────────────────
    // 页面顶栏已经显示页名，这里仍要标题：控制台图标网格进来时顶栏只有「‹ 控制台」，
    // 一块干净的标题让这一页有「自己的头」。
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("快捷指令"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    titleRow->addWidget(title);
    auto* subtitle = new QLabel(
        QStringLiteral("把常做的几步串成一键：连设备 → 读 GPT → 备份"), this);
    subtitle->setObjectName(QStringLiteral("settingsHint"));
    titleRow->addWidget(subtitle);
    titleRow->addStretch();
    root->addLayout(titleRow);

    // ── 主体：左列表 / 右编辑器 ─────────────────────────────────────────
    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // ① 左卡：指令列表 + 三个按钮
    auto* listCard = new QFrame(this);
    listCard->setObjectName(QStringLiteral("card"));
    listCard->setFrameShape(QFrame::StyledPanel);
    listCard->setFixedWidth(kListWidth);
    auto* listLayout = new QVBoxLayout(listCard);
    listLayout->setContentsMargins(14, 12, 14, 12);
    listLayout->setSpacing(8);

    auto* listHeader = new QLabel(QStringLiteral("我的指令"), listCard);
    listHeader->setObjectName(QStringLiteral("groupHeader"));
    listLayout->addWidget(listHeader);

    m_list = new QListWidget(listCard);
    m_list->setObjectName(QStringLiteral("shortcutList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listLayout->addWidget(m_list, 1);

    auto* listButtons = new QHBoxLayout;
    listButtons->setSpacing(6);
    auto* addButton = new QPushButton(QStringLiteral("新建"), listCard);
    addButton->setCursor(Qt::PointingHandCursor);
    auto* delButton = new QPushButton(QStringLiteral("删除"), listCard);
    delButton->setCursor(Qt::PointingHandCursor);
    m_runButton = new QPushButton(QStringLiteral("运行"), listCard);
    m_runButton->setObjectName(QStringLiteral("primaryButton"));
    m_runButton->setCursor(Qt::PointingHandCursor);
    m_runButton->setToolTip(QStringLiteral("按顺序执行这条指令（后台线程，日志在下方）"));
    listButtons->addWidget(addButton);
    listButtons->addWidget(delButton);
    listButtons->addWidget(m_runButton, 1);
    listLayout->addLayout(listButtons);
    body->addWidget(listCard);

    // ② 右卡：编辑器
    auto* editorCard = new QFrame(this);
    editorCard->setObjectName(QStringLiteral("card"));
    editorCard->setFrameShape(QFrame::StyledPanel);
    auto* editorOuter = new QVBoxLayout(editorCard);
    editorOuter->setContentsMargins(14, 12, 14, 12);
    editorOuter->setSpacing(8);

    auto* editorHeader = new QLabel(QStringLiteral("指令内容"), editorCard);
    editorHeader->setObjectName(QStringLiteral("groupHeader"));
    editorOuter->addWidget(editorHeader);

    m_editorPane = new QWidget(editorCard);
    auto* editorLayout = new QVBoxLayout(m_editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(8);

    // 名称 / 标签两行输入。标签用逗号分隔（与存储格式一致，用户手改也对得上）。
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(8);
    auto* nameKey = new QLabel(QStringLiteral("名称"), m_editorPane);
    nameKey->setObjectName(QStringLiteral("statusLabel"));
    nameKey->setFixedWidth(40);
    m_nameEdit = new QLineEdit(m_editorPane);
    m_nameEdit->setObjectName(QStringLiteral("shortcutName"));
    m_nameEdit->setPlaceholderText(QStringLiteral("例如：标准备份流程"));
    nameRow->addWidget(nameKey);
    nameRow->addWidget(m_nameEdit, 1);

    auto* tagsKey = new QLabel(QStringLiteral("标签"), m_editorPane);
    tagsKey->setObjectName(QStringLiteral("statusLabel"));
    tagsKey->setFixedWidth(40);
    m_tagsEdit = new QLineEdit(m_editorPane);
    m_tagsEdit->setObjectName(QStringLiteral("shortcutTags"));
    m_tagsEdit->setPlaceholderText(QStringLiteral("逗号分隔，如：常用,备份"));
    nameRow->addWidget(tagsKey);
    nameRow->addWidget(m_tagsEdit, 1);
    editorLayout->addLayout(nameRow);

    // 步骤表 + 四个小按钮。按钮放在标题行右侧：表格会随窗口长高，
    // 按钮跟着表格跑的话窗口一拉长就要跨半个屏幕去点「上移」。
    auto* stepHeaderRow = new QHBoxLayout;
    stepHeaderRow->setSpacing(6);
    auto* stepHeader = new QLabel(QStringLiteral("步骤"), m_editorPane);
    stepHeader->setObjectName(QStringLiteral("groupHeader"));
    stepHeaderRow->addWidget(stepHeader);
    stepHeaderRow->addStretch();

    auto* addStepButton = new QPushButton(QStringLiteral("添加步骤"), m_editorPane);
    addStepButton->setObjectName(QStringLiteral("fdlMiniButton"));   // 小号按钮皮肤（复用）
    addStepButton->setCursor(Qt::PointingHandCursor);
    auto* delStepButton = new QPushButton(QStringLiteral("删除步骤"), m_editorPane);
    delStepButton->setObjectName(QStringLiteral("fdlMiniButton"));
    delStepButton->setCursor(Qt::PointingHandCursor);
    auto* upStepButton = new QPushButton(QStringLiteral("上移"), m_editorPane);
    upStepButton->setObjectName(QStringLiteral("fdlMiniButton"));
    upStepButton->setCursor(Qt::PointingHandCursor);
    auto* downStepButton = new QPushButton(QStringLiteral("下移"), m_editorPane);
    downStepButton->setObjectName(QStringLiteral("fdlMiniButton"));
    downStepButton->setCursor(Qt::PointingHandCursor);
    stepHeaderRow->addWidget(addStepButton);
    stepHeaderRow->addWidget(delStepButton);
    stepHeaderRow->addWidget(upStepButton);
    stepHeaderRow->addWidget(downStepButton);
    editorLayout->addLayout(stepHeaderRow);

    m_steps = new QTableWidget(0, 3, m_editorPane);
    m_steps->setObjectName(QStringLiteral("shortcutSteps"));
    m_steps->setHorizontalHeaderLabels(
        { QStringLiteral("动作"), QStringLiteral("参数"), QStringLiteral("说明") });
    m_steps->verticalHeader()->setVisible(false);
    m_steps->horizontalHeader()->setStretchLastSection(false);
    m_steps->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_steps->setColumnWidth(0, 168);
    m_steps->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_steps->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_steps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_steps->setSelectionMode(QAbstractItemView::SingleSelection);
    m_steps->setEditTriggers(QAbstractItemView::DoubleClicked
                             | QAbstractItemView::EditKeyPressed
                             | QAbstractItemView::AnyKeyPressed);
    m_steps->verticalHeader()->setDefaultSectionSize(34);   // 放得下 cellWidget 的下拉框
    m_steps->setWordWrap(false);
    editorLayout->addWidget(m_steps, 1);

    editorOuter->addWidget(m_editorPane, 1);
    body->addWidget(editorCard, 1);
    root->addLayout(body, 5);

    // ── 底部：执行日志 ──────────────────────────────────────────────────
    auto* logCard = new QFrame(this);
    logCard->setObjectName(QStringLiteral("card"));
    logCard->setFrameShape(QFrame::StyledPanel);
    auto* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(14, 10, 14, 10);
    logLayout->setSpacing(6);
    auto* logHeader = new QLabel(QStringLiteral("执行日志"), logCard);
    logHeader->setObjectName(QStringLiteral("groupHeader"));
    logLayout->addWidget(logHeader);
    m_log = new LogView(logCard);
    m_log->setObjectName(QStringLiteral("shortcutLog"));
    registerLogView(m_log);
    logLayout->addWidget(m_log, 1);
    root->addWidget(logCard, 2);

    // ── 接线 ────────────────────────────────────────────────────────────
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_syncing) return;
        saveCurrentToModel();   // 先把上一份编辑内容收进模型（m_current 仍是旧值）
        loadEditor(row);
    });

    connect(addButton, &QPushButton::clicked, this, [this] {
        saveCurrentToModel();
        kc::tools::Shortcut fresh;
        fresh.name = QStringLiteral("新指令");
        kc::tools::ShortcutStep first;
        first.action = kc::tools::ShortcutAction::Connect;
        fresh.steps.push_back(first);
        m_shortcuts.push_back(fresh);
        saveToSettings();
        reloadList();
        m_syncing = true;
        m_list->setCurrentRow(m_shortcuts.size() - 1);
        m_syncing = false;
        loadEditor(m_shortcuts.size() - 1);
        if (m_nameEdit) {
            m_nameEdit->setFocus();
            m_nameEdit->selectAll();
        }
    });

    connect(delButton, &QPushButton::clicked, this, [this] {
        if (m_current < 0 || m_current >= m_shortcuts.size()) return;
        const QString name = m_shortcuts.at(m_current).name;
        const int removed = m_current;
        m_shortcuts.removeAt(removed);
        m_current = -1;
        saveToSettings();
        reloadList();
        // 删完优先选中同一位置（原位置被后面那条顶上来了），删的是最后一条就退一条。
        const int next = m_shortcuts.isEmpty()
                             ? -1
                             : qBound(0, removed, static_cast<int>(m_shortcuts.size()) - 1);
        m_syncing = true;
        m_list->setCurrentRow(next);
        m_syncing = false;
        loadEditor(next);
        appendLog(QStringLiteral("已删除指令「%1」").arg(name));
    });

    connect(m_runButton, &QPushButton::clicked, this, [this] { runCurrent(); });

    connect(m_nameEdit, &QLineEdit::textEdited, this, [this](const QString&) {
        if (m_syncing) return;
        saveCurrentToModel();
        saveToSettings();
    });
    connect(m_tagsEdit, &QLineEdit::textEdited, this, [this](const QString&) {
        if (m_syncing) return;
        saveCurrentToModel();
        saveToSettings();
    });

    connect(addStepButton, &QPushButton::clicked, this, [this] {
        if (m_current < 0) return;
        kc::tools::ShortcutStep step;
        step.action = kc::tools::ShortcutAction::Connect;
        addStepRow(step);
        saveCurrentToModel();
        saveToSettings();
        const int last = m_steps->rowCount() - 1;
        m_steps->setCurrentCell(last, 0);
    });
    connect(delStepButton, &QPushButton::clicked, this, [this] {
        if (m_current < 0) return;
        const int row = m_steps->currentRow();
        if (row < 0) return;
        m_steps->removeRow(row);
        saveCurrentToModel();
        saveToSettings();
    });
    connect(upStepButton, &QPushButton::clicked, this, [this] { moveStepRow(-1); });
    connect(downStepButton, &QPushButton::clicked, this, [this] { moveStepRow(1); });

    // 表格里的编辑（参数列改字 / 动作列换下拉）同样要落回模型。
    connect(m_steps, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
        if (m_syncing) return;
        saveCurrentToModel();
        saveToSettings();
    });

    updateEditorEnabled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  列表 / 编辑器同步
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPage::reloadList()
{
    if (!m_list) return;

    // 重建期间屏蔽 currentRowChanged：clear() 会把当前行打到 -1，
    // 不挡的话每次刷新都会触发一次「保存 + 加载空编辑器」。
    const QSignalBlocker blocker(m_list);
    m_list->clear();

    if (m_shortcuts.isEmpty()) {
        // 空态：一条不可选的灰字提示。用它而不是另摆一个 QLabel ——
        // 列表里多一条「假条目」比在列表外面挂提示更省布局，位置也对。
        auto* item = new QListWidgetItem(QStringLiteral("还没有快捷指令\n点「新建」开始"), m_list);
        item->setFlags(Qt::NoItemFlags);   // 不可选、不可点
        item->setTextAlignment(Qt::AlignCenter);
        item->setForeground(ThemeManager::instance().color(ColorToken::TextFaint));
        return;
    }

    for (const kc::tools::Shortcut& shortcut : m_shortcuts) {
        auto* item = new QListWidgetItem(listItemText(shortcut), m_list);
        item->setToolTip(listItemToolTip(shortcut));
    }
}

void ShortcutsPage::refreshListItem(int index)
{
    if (!m_list || index < 0 || index >= m_shortcuts.size()) return;
    QListWidgetItem* item = m_list->item(index);
    if (!item) return;
    const kc::tools::Shortcut& shortcut = m_shortcuts.at(index);
    item->setText(listItemText(shortcut));
    item->setToolTip(listItemToolTip(shortcut));
}

void ShortcutsPage::loadEditor(int index)
{
    m_current = (index >= 0 && index < m_shortcuts.size()) ? index : -1;

    // 灌数据期间挡住所有回写信号：setText / setCellWidget / setCurrentIndex 都会
    // 触发编辑类信号，不挡的话每灌一格就回写一次模型（还会互相覆盖）。
    m_syncing = true;

    if (m_current < 0) {
        if (m_nameEdit) m_nameEdit->clear();
        if (m_tagsEdit) m_tagsEdit->clear();
        clearStepRows();
        m_syncing = false;
        updateEditorEnabled();
        return;
    }

    const kc::tools::Shortcut& shortcut = m_shortcuts.at(m_current);
    if (m_nameEdit) m_nameEdit->setText(shortcut.name);
    if (m_tagsEdit) m_tagsEdit->setText(shortcut.tags.join(QLatin1Char(',')));

    clearStepRows();
    for (const kc::tools::ShortcutStep& step : shortcut.steps) addStepRow(step);

    m_syncing = false;
    updateEditorEnabled();
}

void ShortcutsPage::updateEditorEnabled()
{
    const bool has = m_current >= 0;
    if (m_editorPane) m_editorPane->setEnabled(has);
    if (m_runButton) m_runButton->setEnabled(has && !m_running);
}

kc::tools::Shortcut ShortcutsPage::currentEditorShortcut() const
{
    kc::tools::Shortcut shortcut;
    if (!m_nameEdit || !m_tagsEdit || !m_steps) return shortcut;

    shortcut.name = m_nameEdit->text().trimmed();
    for (const QString& rawTag : m_tagsEdit->text().split(QLatin1Char(','))) {
        const QString tag = rawTag.trimmed();
        if (!tag.isEmpty()) shortcut.tags.push_back(tag);
    }
    shortcut.steps = currentEditorSteps();
    return shortcut;
}

QVector<kc::tools::ShortcutStep> ShortcutsPage::currentEditorSteps() const
{
    QVector<kc::tools::ShortcutStep> steps;
    if (!m_steps) return steps;

    steps.reserve(m_steps->rowCount());
    for (int row = 0; row < m_steps->rowCount(); ++row) {
        kc::tools::ShortcutStep step;

        // 动作列是 cellWidget（QComboBox），不是 QTableWidgetItem —— 直接取 item
        // 永远拿不到，会静默变成「连接设备」。
        auto* combo = qobject_cast<QComboBox*>(m_steps->cellWidget(row, 0));
        if (combo) {
            const QVariant data = combo->currentData();
            step.action = static_cast<kc::tools::ShortcutAction>(data.toInt());
        }
        if (QTableWidgetItem* item = m_steps->item(row, 1)) step.parameter = item->text().trimmed();
        steps.push_back(step);
    }
    return steps;
}

void ShortcutsPage::saveCurrentToModel()
{
    if (m_current < 0 || m_current >= m_shortcuts.size()) return;
    if (!m_nameEdit || !m_tagsEdit || !m_steps) return;
    m_shortcuts[m_current] = currentEditorShortcut();
    refreshListItem(m_current);
}

void ShortcutsPage::clearStepRows()
{
    if (!m_steps) return;
    // 显式摘下动作列的下拉框再清表：Qt 的 clearContents / setRowCount 对
    // cellWidget 的回收行为在不同版本里不一致（有的只清 item），这里先
    // setCellWidget(r, 0, nullptr) 让表格自己 delete 掉，不留悬空指针。
    for (int row = 0; row < m_steps->rowCount(); ++row) m_steps->setCellWidget(row, 0, nullptr);
    m_steps->clearContents();
    m_steps->setRowCount(0);
}

void ShortcutsPage::addStepRow(const kc::tools::ShortcutStep& step)
{
    if (!m_steps) return;

    const int row = m_steps->rowCount();
    m_steps->insertRow(row);

    auto* combo = new QComboBox(m_steps);
    combo->setObjectName(QStringLiteral("shortcutActionCombo"));
    for (const kc::tools::ShortcutAction action : kc::tools::allShortcutActions()) {
        combo->addItem(kc::tools::shortcutActionName(action), static_cast<int>(action));
    }
    const int index = combo->findData(static_cast<int>(step.action));
    combo->setCurrentIndex(index >= 0 ? index : 0);
    m_steps->setCellWidget(row, 0, combo);

    auto* paramItem = new QTableWidgetItem(step.parameter);
    paramItem->setToolTip(QStringLiteral("这一步的参数；不需要参数的步骤留空"));
    m_steps->setItem(row, 1, paramItem);

    auto* hintItem = new QTableWidgetItem();
    // 说明列只读：它由动作推导，编辑它没有意义。
    hintItem->setFlags(hintItem->flags() & ~Qt::ItemIsEditable);
    hintItem->setForeground(ThemeManager::instance().color(ColorToken::TextMuted));
    m_steps->setItem(row, 2, hintItem);

    // 换动作时同步说明列（也顺带把「需不需要参数」提示出来）。
    // 行号在回调里**现查**：删除 / 上移 / 下移会重建整表，构造时捕获的 row 早就过期了。
    connect(combo, &QComboBox::currentIndexChanged, this, [this, combo] {
        int row = -1;
        for (int r = 0; r < m_steps->rowCount(); ++r) {
            if (m_steps->cellWidget(r, 0) == combo) {
                row = r;
                break;
            }
        }
        if (row < 0) return;
        updateStepHint(row);
        if (m_syncing) return;
        saveCurrentToModel();
        saveToSettings();
    });
    updateStepHint(row);
}

void ShortcutsPage::updateStepHint(int row)
{
    if (!m_steps || row < 0 || row >= m_steps->rowCount()) return;
    QTableWidgetItem* item = m_steps->item(row, 2);
    if (!item) return;

    auto* combo = qobject_cast<QComboBox*>(m_steps->cellWidget(row, 0));
    if (!combo) {
        item->setText(QString());
        return;
    }
    const auto action = static_cast<kc::tools::ShortcutAction>(combo->currentData().toInt());
    const QString hint = kc::tools::shortcutActionParameterHint(action);
    item->setText(hint.isEmpty() ? QStringLiteral("无需参数") : hint);
}

void ShortcutsPage::moveStepRow(int delta)
{
    if (m_current < 0 || !m_steps) return;
    const int row = m_steps->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= m_steps->rowCount()) return;

    // 先读出现有步骤（含正在编辑的参数），交换后整表重建 ——
    // 直接搬 QTableWidget 的行会连同 cellWidget 一起搬，Qt 那边对 cellWidget
    // 的父子关系处理很容易留下野指针，重建最稳。
    QVector<kc::tools::ShortcutStep> steps = currentEditorSteps();
    if (row >= steps.size() || target >= steps.size()) return;
    std::swap(steps[row], steps[target]);

    m_syncing = true;
    clearStepRows();
    for (const kc::tools::ShortcutStep& step : steps) addStepRow(step);
    m_syncing = false;

    m_steps->setCurrentCell(target, 0);
    saveCurrentToModel();
    saveToSettings();
}

// ─────────────────────────────────────────────────────────────────────────────
//  运行
// ─────────────────────────────────────────────────────────────────────────────

void ShortcutsPage::runCurrent()
{
    if (m_running) {
        appendLog(QStringLiteral("已有指令正在执行，等它结束再试。"));
        return;
    }
    saveCurrentToModel();
    if (m_current < 0 || m_current >= m_shortcuts.size()) {
        error(QStringLiteral("先选中一条指令"));
        return;
    }

    const kc::tools::Shortcut shortcut = m_shortcuts.at(m_current);
    QString invalid;
    if (!kc::tools::validateShortcut(shortcut, &invalid)) {
        error(invalid);
        appendLog(QStringLiteral("无法运行：%1").arg(invalid));
        return;
    }

    // 先把执行计划写进日志：用户点完「运行」立刻能看到「它打算做什么」，
    // 而不是等失败回来才知道参数是空的。
    appendLog(QStringLiteral("开始执行「%1」（%2 步）").arg(shortcut.name).arg(shortcut.steps.size()));
    for (const QString& line : kc::tools::describePlan(shortcut)) appendLog(QStringLiteral("    %1").arg(line));

    m_running = true;
    if (m_runButton) m_runButton->setEnabled(false);

    // 🔴 后台线程执行：Wait 会 msleep、校验会读整块镜像，放 UI 线程整窗卡死。
    //    guard 防页面先被销毁（QPointer 在 UI 线程析构时自动置空）。
    QPointer<ShortcutsPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([guard, shortcut] {
        kc::tools::ShortcutRunner runner;

        runner.setStepHandler([guard](const kc::tools::ShortcutStep& step, QString* stepError) {
            if (!guard) return false;
            // executeStep 里只碰线程安全的入口（runOnUi / emit 信号 / 纯 IO），
            // 不会直接摸控件。
            return guard->executeStep(step, stepError);
        });

        runner.setProgressHandler([guard](int index, int total, const QString& description) {
            if (!guard) return;
            const QString line =
                QStringLiteral("[%1/%2] %3").arg(index + 1).arg(total).arg(description);
            guard->runOnUi([guard, line] {
                if (guard) guard->appendLog(line);
            });
        });

        const kc::tools::ShortcutRunResult result = runner.run(shortcut);
        if (!guard) return;
        guard->runOnUi([guard, shortcut, result] {
            if (guard) guard->finishRun(shortcut, result);
        });
    }));
}

bool ShortcutsPage::executeStep(const kc::tools::ShortcutStep& step, QString* error)
{
    if (error) error->clear();

    switch (step.action) {
        case kc::tools::ShortcutAction::Connect:
        case kc::tools::ShortcutAction::ReadGpt:
        case kc::tools::ShortcutAction::BackupAll:
        case kc::tools::ShortcutAction::FlashXml:
        case kc::tools::ShortcutAction::Reboot: {
            // 这些都要设备：页面拿不到 EDL 窗，转交外壳。**返回 true** ——
            // 「转交了」就是这一步能做的全部，卡在这里整条指令就没法往下走。
            const QString action = kc::tools::shortcutActionName(step.action);
            const QString parameter = step.parameter.trimmed();
            appendLog(QStringLiteral("需要设备：已请求外壳执行「%1」%2")
                          .arg(action, parameter.isEmpty()
                                            ? QString()
                                            : QStringLiteral("（参数：%1）").arg(parameter)));
            // 🔴 信号必须回 UI 线程再发：本函数跑在 worker 线程，直接 emit 时
            //    万一外壳用直连方式接线，槽就会在 worker 线程里摸控件。
            //    走 runOnUi 后，信号的发射线程永远是 UI 线程，接线方式怎么选都安全。
            runOnUi([this, action, parameter] { emit requestDeviceAction(action, parameter); });
            return true;
        }

        case kc::tools::ShortcutAction::VerifyChecksum: {
            const QString path = step.parameter.trimmed();
            if (path.isEmpty()) {
                // 内置指令里这一步就是空参数（让用户自己填）。空着不阻断，
                // 但要写清楚「跳过了」，不能让用户以为校验过。
                appendLog(QStringLiteral("校验文件摘要：没有填文件路径，已跳过（可在编辑器里补上）"));
                return true;
            }

            const QFileInfo info(path);
            if (!info.exists() || !info.isFile()) {
                if (error) *error = QStringLiteral("文件不存在：%1").arg(path);
                appendLog(QStringLiteral("校验文件摘要：文件不存在 → %1").arg(path));
                return false;
            }

            appendLog(QStringLiteral("校验文件摘要：正在计算 SHA-256…（%1）")
                          .arg(info.fileName()));
            QString hashError;
            const QString actual = kc::crypto::hashFile(path, kc::crypto::HashAlgo::Sha256,
                                                        &hashError);
            if (actual.isEmpty()) {
                if (error) *error = hashError.isEmpty() ? QStringLiteral("摘要计算失败") : hashError;
                appendLog(QStringLiteral("校验文件摘要失败：%1").arg(hashError));
                return false;
            }

            // 同名 `.sha256` 校验文件：有就比对，没有就把算出来的摘要报出来。
            // 没有校验文件不算失败 —— 用户可能只想核对一下镜像的摘要。
            const QString sidecar = kc::tools::checksumFileName(path, kc::crypto::HashAlgo::Sha256);
            QFile checkFile(sidecar);
            if (!checkFile.exists()) {
                appendLog(QStringLiteral("SHA-256 = %1（未找到 %2，只报告不比对）")
                              .arg(actual, QFileInfo(sidecar).fileName()));
                return true;
            }
            if (!checkFile.open(QIODevice::ReadOnly)) {
                if (error) *error = QStringLiteral("无法读取校验文件：%1").arg(sidecar);
                appendLog(QStringLiteral("无法读取校验文件：%1（%2）")
                              .arg(QFileInfo(sidecar).fileName(), checkFile.errorString()));
                return false;
            }
            const QVector<kc::tools::ChecksumRecord> records =
                kc::tools::parseChecksumText(checkFile.readAll());
            checkFile.close();

            if (records.isEmpty()) {
                if (error) *error = QStringLiteral("校验文件里没有可识别的记录");
                appendLog(QStringLiteral("校验文件「%1」里没有可识别的记录")
                              .arg(QFileInfo(sidecar).fileName()));
                return false;
            }

            const QString fileName = info.fileName();
            for (const kc::tools::ChecksumRecord& record : records) {
                const QString recordName = QFileInfo(record.name).fileName();
                if (recordName.compare(fileName, Qt::CaseInsensitive) != 0) continue;

                if (record.hash.compare(actual, Qt::CaseInsensitive) == 0) {
                    appendLog(QStringLiteral("校验通过：%1（SHA-256 一致）").arg(fileName));
                    return true;
                }
                if (error) {
                    *error = QStringLiteral("摘要不一致：期望 %1，实际 %2")
                                 .arg(record.hash.left(16) + QStringLiteral("…"), actual.left(16) + QStringLiteral("…"));
                }
                appendLog(QStringLiteral("校验失败：%1\n    期望 %2\n    实际 %3")
                              .arg(fileName, record.hash, actual));
                return false;
            }

            if (error) *error = QStringLiteral("校验文件里没有「%1」的记录").arg(fileName);
            appendLog(QStringLiteral("校验文件「%1」里没有「%2」的记录")
                          .arg(QFileInfo(sidecar).fileName(), fileName));
            return false;
        }

        case kc::tools::ShortcutAction::Wait: {
            bool ok = false;
            int seconds = step.parameter.trimmed().toInt(&ok);
            if (!ok || seconds <= 0) {
                appendLog(QStringLiteral("等待：秒数无效（%1），已跳过")
                              .arg(step.parameter.trimmed().isEmpty() ? QStringLiteral("空")
                                                                      : step.parameter));
                return true;
            }
            // 上限 60 秒：这条指令是「省点几下」，不是定时任务。
            seconds = qMin(seconds, kMaxWaitSeconds);
            appendLog(QStringLiteral("等待 %1 秒…").arg(seconds));
            QThread::msleep(static_cast<unsigned long>(seconds) * 1000UL);
            return true;
        }
    }

    if (error) *error = QStringLiteral("未知动作");
    return false;
}

void ShortcutsPage::finishRun(const kc::tools::Shortcut& shortcut,
                              const kc::tools::ShortcutRunResult& result)
{
    m_running = false;
    updateEditorEnabled();

    // 注意：logSuccess / logError 写的就是本页注册的那个 LogView（registerLogView），
    // 所以这里**不要**再 appendLog 同一条 —— 会一屏两行一样的字。
    if (result.ok) {
        const QString message =
            QStringLiteral("「%1」执行完成（%2 步）").arg(shortcut.name).arg(result.completedSteps);
        logSuccess(message);
        success(message);
        return;
    }

    const QString failed = result.failedStep.isEmpty() ? QStringLiteral("未知步骤")
                                                       : result.failedStep;
    const QString message = QStringLiteral("「%1」在第 %2 步失败：%3")
                                .arg(shortcut.name)
                                .arg(result.completedSteps + 1)
                                .arg(result.error);
    logWarning(QStringLiteral("失败于：%1").arg(failed));
    logError(message);
    error(message);
}

bool ShortcutsPage::runShortcutByName(const QString& name)
{
    const QString key = name.trimmed();
    if (key.isEmpty()) return false;
    if (m_running) {
        appendLog(QStringLiteral("「%1」没跑：已有指令正在执行").arg(key));
        return false;
    }

    for (int i = 0; i < m_shortcuts.size(); ++i) {
        if (m_shortcuts.at(i).name != key) continue;

        // 选中它，再走与点「运行」完全相同的路径（日志 / 后台线程 / 通知一致）。
        saveCurrentToModel();
        m_syncing = true;
        m_list->setCurrentRow(i);
        m_syncing = false;
        loadEditor(i);
        runCurrent();
        return true;
    }
    appendLog(QStringLiteral("找不到指令「%1」").arg(key));
    return false;
}

void ShortcutsPage::appendLog(const QString& text)
{
    if (text.isEmpty()) return;
    // runOnUi 自带线程判定：UI 线程直接执行，worker 线程排队。
    runOnUi([this, text] {
        if (m_log) m_log->appendInfo(text);
    });
}

} // namespace kc::ui
