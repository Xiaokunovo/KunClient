#include "ui/pages/FastbootFlashPage.h"

#include "kc/auth/MembershipService.h"
#include "core/PartitionProtection.h"
#include "kc/tools/FastbootSizeFallback.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/ShellStatus.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include "ui/dialogs/ThemedMessageBox.h"
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QTableWidget>
#include <QThread>
#include <QThreadPool>
#include <QUuid>
#include <QVBoxLayout>

#include <functional>

namespace kc::ui {
namespace {

constexpr int kColCheck = 0;
constexpr int kColName = 1;
constexpr int kColType = 2;
constexpr int kColSize = 3;
constexpr int kColFile = 4;

/// 把一段函数体丢进全局线程池（等价 C# 的 Task.Run）。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

QString formatSize(qint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
}

/// 把 fastboot 的 `(bootloader) key: value` 输出摊平成字典（键小写、空格换连字符）。
QHash<QString, QString> parseGetvarLines(const QString& output)
{
    QHash<QString, QString> map;
    if (output.isEmpty()) return map;

    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("(bootloader)"), Qt::CaseInsensitive))
            line = line.mid(12).trimmed();

        const int idx = line.indexOf(QLatin1Char(':'));
        if (idx <= 0) continue;

        QString key = line.left(idx).trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('-'));
        if (key.isEmpty()) continue;
        map.insert(key, line.mid(idx + 1).trimmed());
    }
    return map;
}

bool tryVar(const QHash<QString, QString>& vars, const QString& key, QString* value)
{
    const QString v = vars.value(key).trimmed();
    if (v.isEmpty()) return false;
    *value = v;
    return true;
}

/// yes/true → yesText，no/false → noText，其它原样返回。
QString yesNo(const QString& value, const QString& yesText, const QString& noText)
{
    if (value.isEmpty() || value == QStringLiteral("-")) return QStringLiteral("-");
    if (value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0 ||
        value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        return yesText;
    if (value.compare(QStringLiteral("no"), Qt::CaseInsensitive) == 0 ||
        value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0)
        return noText;
    return value;
}

/// 解析 getvar 输出里的 `partition-size:<name>: 0x...`。
QHash<QString, qint64> parsePartitionSizes(const QString& output)
{
    QHash<QString, qint64> map;
    if (output.isEmpty()) return map;

    static const QRegularExpression re(
        QStringLiteral("partition-size:\\s*([A-Za-z0-9_\\-\\.]+)\\s*:\\s*(0x[0-9A-Fa-f]+|\\d+)"));
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const auto m = re.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        const QString text = m.captured(2);
        bool ok = false;
        const qint64 bytes = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                                 ? text.mid(2).toLongLong(&ok, 16)
                                 : text.toLongLong(&ok);
        if (ok) map.insert(m.captured(1), bytes);
    }
    return map;
}

} // namespace

FastbootFlashPage::FastbootFlashPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("通用Fastboot"));
    buildUi();
}

void FastbootFlashPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 2026-10-12：本页此前是「设备行 → 固件目录行 → 动作行 → 分区表 →
    // 快速刷写行 → 命令行长龙」的平铺流，且设备行把模式、5 个读数、两个
    // 按钮全挤在一行里（宽窗口才放得下）。现在按功能收进卡片：设备 /
    // 固件目录 / 分区操作 / 快速刷写 / 常用命令。
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

    // ── 卡 1：设备（模式 + 读数 + 入口按钮）────────────────────────────
    QVBoxLayout* deviceCard = addCard(QStringLiteral("设备"));
    auto* deviceTopRow = new QHBoxLayout;
    deviceTopRow->setSpacing(10);

    m_modeLabel = new QLabel(QStringLiteral("未检测"), this);
    m_modeLabel->setObjectName(QStringLiteral("deviceModeLabel"));
    QFont modeFont = m_modeLabel->font();
    modeFont.setBold(true);
    m_modeLabel->setFont(modeFont);
    deviceTopRow->addWidget(m_modeLabel);

    deviceTopRow->addStretch();
    // 「可视刷写 ›」快捷入口：C# 的 FASTBOOT 模式切换改为跳转式（对齐 FDL 入口先例）。
    auto* visualButton = new QPushButton(QStringLiteral("可视刷写 ›"), this);
    visualButton->setObjectName(QStringLiteral("primaryButton"));
    visualButton->setCursor(Qt::PointingHandCursor);
    visualButton->setToolTip(QStringLiteral("打开可视刷写（分区表 / GPT / rawprogram）"));
    connect(visualButton, &QPushButton::clicked, this,
            [this] { emit navigationRequested(QStringLiteral("VisualFlash")); });
    deviceTopRow->addWidget(visualButton);

    m_refreshButton = new QPushButton(QStringLiteral("检测设备"), this);
    m_refreshButton->setObjectName(QStringLiteral("primaryButton"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(m_refreshButton);
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refreshDevice(); });
    deviceTopRow->addWidget(m_refreshButton);
    deviceCard->addLayout(deviceTopRow);

    auto makeReading = [](const QString& label, QLabel** value) {
        auto* box = new QWidget;
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto* key = new QLabel(label, box);
        key->setObjectName(QStringLiteral("statusLabel"));
        *value = new QLabel(QStringLiteral("-"), box);
        (*value)->setObjectName(QStringLiteral("statusValue"));
        row->addWidget(key);
        row->addWidget(*value);
        return box;
    };
    // 5 个读数从「和设备模式抢同一行」挪到独立一行，窗口再窄也不会被挤没。
    auto* readingRow = new QHBoxLayout;
    readingRow->setSpacing(18);
    readingRow->addWidget(makeReading(QStringLiteral("序列号:"), &m_serialValue));
    readingRow->addWidget(makeReading(QStringLiteral("安全启动:"), &m_secureValue));
    readingRow->addWidget(makeReading(QStringLiteral("设备代号:"), &m_productValue));
    readingRow->addWidget(makeReading(QStringLiteral("解锁状态:"), &m_unlockedValue));
    readingRow->addWidget(makeReading(QStringLiteral("槽位:"), &m_slotValue));
    readingRow->addStretch();
    deviceCard->addLayout(readingRow);

    // ── 卡 2：固件目录（选目录后逐行按 <分区>.img/.bin/.mbn 匹配填充）────
    QVBoxLayout* romCard = addCard(QStringLiteral("固件目录"));
    auto* romRow = new QHBoxLayout;
    romRow->setSpacing(8);
    auto* romLabel = new QLabel(QStringLiteral("目录:"), this);
    romLabel->setObjectName(QStringLiteral("statusLabel"));
    romRow->addWidget(romLabel);

    m_romDirEdit = new QLineEdit(this);
    m_romDirEdit->setPlaceholderText(QStringLiteral("选择线刷包 / 固件目录，自动匹配 boot.img 等分区镜像"));
    m_romDirEdit->setReadOnly(true);
    romRow->addWidget(m_romDirEdit, 1);

    auto* romPickButton = new QPushButton(QStringLiteral("目录…"), this);
    romPickButton->setObjectName(QStringLiteral("primaryButton"));
    romPickButton->setCursor(Qt::PointingHandCursor);
    connect(romPickButton, &QPushButton::clicked, this, [this] { pickRomDir(); });
    romRow->addWidget(romPickButton);
    romCard->addLayout(romRow);

    // ── 卡 3：分区操作（写入/擦除/读表 + 搜索）──────────────────────────
    QVBoxLayout* actionCard = addCard(QStringLiteral("分区操作"));
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(8);

    auto* flashButton = new QPushButton(QStringLiteral("写入选中"), this);
    flashButton->setObjectName(QStringLiteral("primaryButton"));
    auto* eraseButton = new QPushButton(QStringLiteral("擦除选中"), this);
    auto* readTableButton = new QPushButton(QStringLiteral("读取分区表"), this);
    for (QPushButton* b : { flashButton, eraseButton, readTableButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        actionRow->addWidget(b);
    }
    connect(flashButton, &QPushButton::clicked, this, [this] { flashSelected(); });
    connect(eraseButton, &QPushButton::clicked, this, [this] { eraseSelected(); });
    connect(readTableButton, &QPushButton::clicked, this, [this] { readPartitionTable(); });

    actionRow->addStretch();

    // ADB 模式（对齐 YukiClient 自定义刷入的 UseADB）：勾选后「写入选中 / 快速刷写」
    // 改走 adb push + dd，适用于已 root 或处于 Fastbootd 的设备。
    m_adbMode = new QCheckBox(QStringLiteral("ADB 模式"), this);
    m_adbMode->setToolTip(QStringLiteral("push 到 /data/local/tmp → dd 到 /dev/block/by-name/<分区>\n"
                                         "需要 root 或 Fastbootd；失败会自动回落 su -c"));
    actionRow->addWidget(m_adbMode);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("搜索分区"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setFixedWidth(180);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        applyFilter(text.trimmed());
    });
    actionRow->addWidget(m_filterEdit);
    actionCard->addLayout(actionRow);

    // ── 分区管理表 ──────────────────────────────────────────────────────
    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("选"),
                                         QStringLiteral("分区"),
                                         QStringLiteral("类型"),
                                         QStringLiteral("大小"),
                                         QStringLiteral("镜像文件") });
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(kColCheck, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColType, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColSize, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColFile, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(28);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        onItemChanged(item);
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) { onCellDoubleClicked(row, column); });
    layout->addWidget(m_table, 3);

    // ── 卡 4：快速刷写（6 个固定分区一键写入 + 自定义分区）──────────────
    QVBoxLayout* quickCard = addCard(QStringLiteral("快速刷写"));
    auto* quickRow = new QHBoxLayout;
    quickRow->setSpacing(8);

    auto flashFromTable = [this](const QString& key) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).key == key) {
                flashSinglePartition(m_rows.at(i).name, m_rows.at(i).filePath);
                return;
            }
        }
    };
    for (const QString& key : { QStringLiteral("boot"), QStringLiteral("init_boot"),
                                QStringLiteral("vendor_boot"), QStringLiteral("dtbo"),
                                QStringLiteral("recovery"), QStringLiteral("system") }) {
        auto* b = new QPushButton(key, this);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(QStringLiteral("写入 %1（镜像取分区表对应行）").arg(key));
        m_busyButtons.append(b);
        connect(b, &QPushButton::clicked, this, [flashFromTable, key] { flashFromTable(key); });
        quickRow->addWidget(b);
    }

    // 自定义分区：分区名 / 镜像都取表格 custom 行（与 C# _customImagePath 同一口径）。
    auto* customButton = new QPushButton(QStringLiteral("写入自定义"), this);
    customButton->setCursor(Qt::PointingHandCursor);
    m_busyButtons.append(customButton);
    connect(customButton, &QPushButton::clicked, this, [this, flashFromTable] {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).key != QStringLiteral("custom")) continue;
            const QString name = m_rows.at(i).name.trimmed();
            if (name.isEmpty() || name == QStringLiteral("自定义分区")) {
                error(QStringLiteral("自定义分区还没填分区名，请先在表格里填上"));
                return;
            }
            flashSinglePartition(name, m_rows.at(i).filePath);
            return;
        }
    });
    quickRow->addWidget(customButton);

    // 高级操作（对齐 YukiClient 自定义刷入）：放进「快速刷写」这一行 —— 该行右侧
    // 本来就有富余空间，塞进「常用命令」会把那一行 13 个按钮挤到文字被截断。
    auto* vbmetaButton = new QPushButton(QStringLiteral("禁用vbmeta"), this);
    vbmetaButton->setToolTip(QStringLiteral("对 vbmeta / vbmeta_system / vbmeta_vendor 逐个刷入，\n"
                                            "参数三选一（镜像取「固件目录」里的同名文件）"));
    auto* cowButton = new QPushButton(QStringLiteral("删除COW"), this);
    cowButton->setToolTip(QStringLiteral("先查询 is-logical:yes 且名字带 -cow 的逻辑分区，确认后逐个删除"));
    for (QPushButton* b : { vbmetaButton, cowButton }) {
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        quickRow->addWidget(b);
    }
    connect(vbmetaButton, &QPushButton::clicked, this, [this] { disableVbmeta(); });
    connect(cowButton, &QPushButton::clicked, this, [this] { scanCowPartitions(); });

    quickRow->addStretch();
    quickCard->addLayout(quickRow);

    // ── 卡 5：常用命令（引导锁 / 擦除 / 重启 / getvar）──────────────────
    QVBoxLayout* cmdCard = addCard(QStringLiteral("常用命令"));
    auto* cmdRow = new QHBoxLayout;
    cmdRow->setSpacing(8);

    struct Cmd { QString label; QString args; QString desc; QString confirm; QString quotaKey; bool refreshAfter = false; };
    const QVector<Cmd> cmds = {
        { QStringLiteral("解锁BL"),      QStringLiteral("oem unlock"),       QStringLiteral("解锁Bootloader"), {}, {} },
        { QStringLiteral("回锁BL"),      QStringLiteral("oem lock"),         QStringLiteral("锁定Bootloader"), {}, {} },
        { QStringLiteral("解锁状态"),    QStringLiteral("oem device-info"),  QStringLiteral("查询解锁状态"), {}, {} },
        // 会员额度口径对齐 C#：格式化/擦除类吃 fastboot.erase，重启/getvar 不扣
        { QStringLiteral("格式化数据"),  QStringLiteral("-w"),               QStringLiteral("格式化数据"),
          QStringLiteral("格式化数据会清除所有用户数据，确定？"), QStringLiteral("fastboot.erase") },
        { QStringLiteral("擦除Cache"),   QStringLiteral("erase cache"),      QStringLiteral("擦除 Cache"), {}, QStringLiteral("fastboot.erase") },
        { QStringLiteral("擦Meta"), QStringLiteral("erase metadata"),   QStringLiteral("擦除 Metadata"), {}, QStringLiteral("fastboot.erase") },
        { QStringLiteral("重启BL"), QStringLiteral("reboot bootloader"), QStringLiteral("重启到Bootloader"), {}, {} },
        { QStringLiteral("重启系统"),    QStringLiteral("reboot"),           QStringLiteral("重启系统"), {}, {} },
        { QStringLiteral("重启EDL"),     QStringLiteral("reboot-edl"),       QStringLiteral("重启到EDL(9008)"), {}, {} },
        { QStringLiteral("Getvar"),  QStringLiteral("getvar all"),       QStringLiteral("Getvar全部信息"), {}, {} },
        // A/B 槽切换（移植自 TMFTool 的「Slots Set / 切换A槽 / 切换A分区」）。
        // 设备只读 current-slot、从不 set-active —— 之前想换槽只能自己去敲命令行。
        // 切换后重读设备读数（refreshAfter），否则顶部还挂着旧槽位。
        { QStringLiteral("切换A槽"),     QStringLiteral("--set-active=a"),   QStringLiteral("切换活动槽 → A"),
          QStringLiteral("将把活动槽切到 A（下次启动生效，当前系统不动）。\n\n"
                         "仅 A/B 分区设备支持；非 A/B 机型会返回 unknown command，"
                         "不会改动任何分区。是否继续？"), {}, true },
        { QStringLiteral("切换B槽"),     QStringLiteral("--set-active=b"),   QStringLiteral("切换活动槽 → B"),
          QStringLiteral("将把活动槽切到 B（下次启动生效，当前系统不动）。\n\n"
                         "仅 A/B 分区设备支持；非 A/B 机型会返回 unknown command，"
                         "不会改动任何分区。是否继续？"), {}, true },
        // 「另一个槽」：不用先知道当前是 a 还是 b（对齐 YukiClient 的 set_active other）。
        { QStringLiteral("另一槽"),   QStringLiteral("set_active other"), QStringLiteral("切换活动槽 → 另一个"),
          QStringLiteral("将把活动槽切到**另一个**（下次启动生效，当前系统不动）。\n\n"
                         "仅 A/B 分区设备支持。是否继续？"), {}, true },
    };
    for (const Cmd& c : cmds) {
        auto* b = new QPushButton(c.label, this);
        b->setCursor(Qt::PointingHandCursor);
        m_busyButtons.append(b);
        connect(b, &QPushButton::clicked, this, [this, c] {
            // 有确认文案的先确认（UI 线程），额度扣在 worker 里（阻塞 HTTP 不许上 GUI 线程）。
            if (!c.confirm.isEmpty()
                && ThemedMessageBox::warning(this, QStringLiteral("确认"), c.confirm,
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No) != QMessageBox::Yes)
                return;
            runFastbootCommand(c.args, c.desc, 120000, c.quotaKey, c.refreshAfter);
        });
        cmdRow->addWidget(b);
    }

    cmdRow->addStretch();
    cmdCard->addLayout(cmdRow);
    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);

    buildPartitionRows();
}

void FastbootFlashPage::buildPartitionRows()
{
    // 与 C# BuildPartitionRows 一致的 7 行（固定顺序 + 自定义分区）。
    const QVector<QPair<QString, QString>> defs = {
        { QStringLiteral("boot"), QStringLiteral("boot") },
        { QStringLiteral("init_boot"), QStringLiteral("init_boot") },
        { QStringLiteral("vendor_boot"), QStringLiteral("vendor_boot") },
        { QStringLiteral("dtbo"), QStringLiteral("dtbo") },
        { QStringLiteral("recovery"), QStringLiteral("recovery") },
        { QStringLiteral("system"), QStringLiteral("system") },
        { QStringLiteral("custom"), QStringLiteral("自定义分区") },
    };

    m_updatingTable = true;
    m_rows.clear();
    m_table->setRowCount(defs.size());
    for (int i = 0; i < defs.size(); ++i) {
        Row row;
        row.key = defs.at(i).first;
        row.name = defs.at(i).second;
        m_rows.append(row);

        auto* checkItem = new QTableWidgetItem;
        checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        checkItem->setCheckState(Qt::Unchecked);
        m_table->setItem(i, kColCheck, checkItem);

        auto* nameItem = new QTableWidgetItem(row.name);
        if (row.key == QStringLiteral("custom")) {
            // 自定义分区名可编辑（对应 C# 的 TxtCustomPartition）。
            nameItem->setFlags(nameItem->flags() | Qt::ItemIsEditable);
        }
        m_table->setItem(i, kColName, nameItem);

        auto* typeItem = new QTableWidgetItem(QStringLiteral("raw"));
        typeItem->setForeground(ThemeManager::instance().color(ColorToken::TextMuted));
        m_table->setItem(i, kColType, typeItem);

        m_table->setItem(i, kColSize, new QTableWidgetItem(QStringLiteral("-")));
        m_table->setItem(i, kColFile, new QTableWidgetItem(QStringLiteral("双击选择文件")));
    }
    m_updatingTable = false;
}

void FastbootFlashPage::syncRowToTable(int index)
{
    if (index < 0 || index >= m_rows.size()) return;
    const Row& row = m_rows.at(index);
    m_updatingTable = true;
    m_table->item(index, kColName)->setText(row.name);
    m_table->item(index, kColFile)->setText(
        row.filePath.isEmpty() ? QStringLiteral("双击选择文件") : row.filePath);
    m_table->item(index, kColFile)->setToolTip(row.filePath);
    m_updatingTable = false;
}

void FastbootFlashPage::pickImageFor(int tableRow)
{
    if (tableRow < 0 || tableRow >= m_rows.size()) return;

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"), QString(),
        QStringLiteral("镜像文件 (*.img *.bin *.mbn *.zip);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    Row& row = m_rows[tableRow];
    row.filePath = path;
    syncRowToTable(tableRow);
    logInfo(QStringLiteral("[Fastboot] 已选择 %1 镜像: %2").arg(row.name, path));
}

void FastbootFlashPage::pickRomDir()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择固件目录"), m_romDirEdit->text());
    if (dir.isEmpty()) return;
    m_romDirEdit->setText(dir);

    // 对齐 C# BtnPickRomDir_Click：逐行按 <分区名>.img / .bin / .mbn 顺序取第一个存在的，
    // custom 行的镜像名由用户给，不猜。
    logView()->appendSection(QStringLiteral("固件目录匹配"));
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    int matched = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row& row = m_rows.at(i);
        if (row.key == QStringLiteral("custom")) continue;

        QString found;
        for (const QString& ext : { QStringLiteral(".img"), QStringLiteral(".bin"), QStringLiteral(".mbn") }) {
            const QString path = dir + QLatin1Char('/') + row.key + ext;
            if (QFileInfo::exists(path)) { found = path; break; }
        }
        if (found.isEmpty()) {
            logDetail(QStringLiteral("目录里没有 %1.img / .bin，跳过").arg(row.key));
            continue;
        }
        m_rows[i].filePath = found;
        syncRowToTable(i);
        ++matched;
        logInfo(QStringLiteral("[Fastboot] 已匹配 %1 镜像: %2").arg(row.key, QFileInfo(found).fileName()));
    }
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(
        QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    if (matched > 0) {
        success(QStringLiteral("已从目录匹配 %1 个分区镜像").arg(matched));
    } else {
        logWarning(QStringLiteral("[Fastboot] 目录里没有匹配到任何分区镜像（boot.img 等）"));
    }
}

void FastbootFlashPage::flashSinglePartition(const QString& partition, const QString& image)
{
    // 对齐 C# FlashPartitionAsync：校验镜像 → fastboot flash <分区> <镜像>，日志三明治收口。
    if (m_busy.loadAcquire() != 0) return;
    if (image.isEmpty()) {
        error(QStringLiteral("请选择 %1 镜像文件（双击分区表对应行，或用固件目录自动匹配）").arg(partition));
        return;
    }
    runFastbootCommand(QStringLiteral("flash %1 \"%2\"").arg(partition, image),
                       QStringLiteral("写入 %1").arg(partition), 600000,
                       QStringLiteral("fastboot.flash"));
}

void FastbootFlashPage::onItemChanged(QTableWidgetItem* item)
{
    if (m_updatingTable || !item || m_busy.loadAcquire() != 0) return;
    const int r = item->row();

    if (item->column() == kColCheck) {
        // 与 C# ChkPartition_Click：勾选但没文件 → 顺手弹选文件；取消则回退勾选。
        if (item->checkState() == Qt::Checked && m_rows.at(r).filePath.isEmpty()) {
            pickImageFor(r);
            if (m_rows.at(r).filePath.isEmpty()) {
                m_updatingTable = true;
                item->setCheckState(Qt::Unchecked);
                m_updatingTable = false;
            }
        }
        return;
    }

    if (item->column() == kColName) {
        // 只有 custom 行的名称可编辑（其它行 NoEditTriggers 拦不住程序路径，这里再兜一道）。
        if (m_rows.at(r).key != QStringLiteral("custom")) {
            syncRowToTable(r);
            return;
        }
        const QString name = item->text().trimmed();
        m_rows[r].name = name.isEmpty() ? QStringLiteral("自定义分区") : name;
        m_updatingTable = true;
        item->setText(m_rows.at(r).name);
        m_updatingTable = false;
    }
}

void FastbootFlashPage::onCellDoubleClicked(int row, int column)
{
    Q_UNUSED(column);
    pickImageFor(row);
}

void FastbootFlashPage::applyFilter(const QString& text)
{
    for (int r = 0; r < m_rows.size(); ++r) {
        const bool hit = text.isEmpty() || m_rows.at(r).name.contains(text, Qt::CaseInsensitive);
        m_table->setRowHidden(r, !hit);
    }
}

void FastbootFlashPage::onActivated()
{
    refreshDevice();
}

// ─────────────────────────────────────────────────────────────────────────────
//  忙碌闸门 + 线程池调度
// ─────────────────────────────────────────────────────────────────────────────

void FastbootFlashPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : m_busyButtons) {
        if (b) b->setEnabled(!busy);
    }
    m_table->setEnabled(!busy);
}

void FastbootFlashPage::refreshDevice()
{
    if (m_busy.loadAcquire() != 0) return;
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this] { refreshDeviceCore(); }));
}

void FastbootFlashPage::refreshDeviceCore()
{
    PlatformToolsManager tools;
    const auto r = tools.executeFastboot(QStringLiteral("devices"), 15000);
    const bool connected = r.success && r.output.contains(QStringLiteral("fastboot"),
                                                          Qt::CaseInsensitive);
    runOnUi([this, connected, output = r.output.trimmed()] {
        if (connected) {
            m_modeLabel->setText(QStringLiteral("Fastboot 已连接"));
            m_modeLabel->setPalette(QPalette(ThemeManager::instance().color(ColorToken::LogWarning)));
            logInfo(QStringLiteral("[Fastboot] 检测到 fastboot 设备"));
            logDetail(output);
        } else {
            m_modeLabel->setText(QStringLiteral("未检测到设备"));
            m_modeLabel->setPalette(QPalette(ThemeManager::instance().color(ColorToken::TextMuted)));
            logWarning(QStringLiteral("[Fastboot] 未检测到 fastboot 设备（设备需处于 Bootloader/Fastboot 模式）"));
        }
        setBusy(false);
    });
}

void FastbootFlashPage::readPartitionTable()
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("读分区表"));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this] { readPartitionTableCore(); }));
}

void FastbootFlashPage::readPartitionTableCore()
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    logView()->beginOperation(QStringLiteral("读取分区表 (fastboot getvar all)"));
    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logView()->appendOperation(QStringLiteral("读取分区表 (fastboot getvar all)"), false);
        logWarning(QStringLiteral("[Fastboot] fastboot 工具不可用，仅刷新本地显示"));
    } else {
        const auto r = tools.executeFastboot(QStringLiteral("getvar all"), 120000);
        logView()->appendOperation(QStringLiteral("读取分区表 (fastboot getvar all)"), r.success);
        // ⚠️ fastboot 把 getvar 的内容写 stderr，只读 stdout 会一行都拿不到 → 两路合起来。
        //    命令成功时 stderr 里那一片是分区数据，不能当错误刷屏。
        const QString getvarText = r.output + QLatin1Char('\n') + r.error;
        if (!r.success && !r.error.trimmed().isEmpty())
            logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
        if (r.success) {
            applyPartitionSizes(getvarText);
            applyDeviceReadings(getvarText);
        } else {
            logWarning(QStringLiteral("[Fastboot] 没读到分区表，仅刷新本地显示"));
        }
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this] { setBusy(false); });
}

void FastbootFlashPage::applyPartitionSizes(const QString& output)
{
    const QHash<QString, qint64> sizes = parsePartitionSizes(output);
    if (sizes.isEmpty()) {
        logWarning(QStringLiteral("[Fastboot] getvar 输出里没有 partition-size 行"));
        return;
    }

    runOnUi([this, sizes] {
        m_updatingTable = true;
        int filled = 0;
        for (int i = 0; i < m_rows.size(); ++i) {
            const qint64 bytes = sizes.value(m_rows.at(i).name, -1);
            if (bytes >= 0) {
                m_table->item(i, kColSize)->setText(formatSize(bytes));
                ++filled;
            }
        }
        m_updatingTable = false;
        logInfo(QStringLiteral("[Fastboot] 读到 %1 个分区大小").arg(filled));
    });
}

void FastbootFlashPage::applyDeviceReadings(const QString& output)
{
    const QHash<QString, QString> vars = parseGetvarLines(output);
    if (vars.isEmpty()) return;

    QString serial, secure, product, unlocked, slot;
    const bool touched =
        tryVar(vars, QStringLiteral("serialno"), &serial) |
        tryVar(vars, QStringLiteral("secure"), &secure) |
        tryVar(vars, QStringLiteral("product"), &product);

    // 解锁状态：getvar all 给 unlocked=yes/no，oem device-info 给 device-unlocked。
    QString unlockedVar;
    if (!tryVar(vars, QStringLiteral("unlocked"), &unlockedVar))
        tryVar(vars, QStringLiteral("device-unlocked"), &unlockedVar);
    if (!unlockedVar.isEmpty()) unlocked = yesNo(unlockedVar, QStringLiteral("已解锁"), QStringLiteral("未解锁"));

    if (!tryVar(vars, QStringLiteral("current-slot"), &slot)) {
        QString slotCount;
        if (tryVar(vars, QStringLiteral("slot-count"), &slotCount))
            slot = QStringLiteral("%1 槽位").arg(slotCount);
    }

    // 设备模式：is-userspace=yes → fastbootd，否则 Bootloader。
    QString mode;
    QString userspace;
    if (tryVar(vars, QStringLiteral("is-userspace"), &userspace) &&
        userspace.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0)
        mode = QStringLiteral("fastbootd");
    else
        mode = QStringLiteral("Bootloader");

    runOnUi([this, serial, secure, product, unlocked, slot, mode, touched] {
        if (!serial.isEmpty()) m_serialValue->setText(serial);
        if (!secure.isEmpty()) m_secureValue->setText(yesNo(secure, QStringLiteral("开启"), QStringLiteral("关闭")));
        if (!product.isEmpty()) m_productValue->setText(product);
        if (!unlocked.isEmpty()) m_unlockedValue->setText(unlocked);
        if (!slot.isEmpty()) m_slotValue->setText(slot);
        if (touched || !unlocked.isEmpty() || !slot.isEmpty()) {
            m_modeLabel->setText(mode);
            m_modeLabel->setPalette(QPalette(ThemeManager::instance().color(ColorToken::LogWarning)));
        }
        const QString device = !serial.isEmpty() ? serial : m_productValue->text();
        if (!device.isEmpty() && device != QStringLiteral("-"))
            setShellStatus(device, mode, QStringLiteral("就绪"));
    });
}

void FastbootFlashPage::flashSelected()
{
    if (m_busy.loadAcquire() != 0) return;

    QVector<int> targets;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_table->item(i, kColCheck)->checkState() == Qt::Checked) targets.append(i);
    }
    if (targets.isEmpty()) {
        error(QStringLiteral("请先勾选要写入的分区"));
        return;
    }
    QStringList missing;
    for (int i : targets) {
        if (m_rows.at(i).filePath.isEmpty()) missing.append(m_rows.at(i).name);
    }
    if (!missing.isEmpty()) {
        error(QStringLiteral("这些分区还没选镜像文件（双击对应行选择）: %1").arg(missing.join(QStringLiteral("、"))));
        return;
    }
    for (int i : targets) {
        if (m_rows.at(i).name.trimmed().isEmpty()) {
            error(QStringLiteral("自定义分区还没填分区名，请先在表格里填上"));
            return;
        }
    }

    // 保护分区二次确认（与 EDL / 可视刷写同一张名单）。
    QStringList protectedNames;
    for (int i : targets) {
        if (swt::edl::isProtectedPartitionName(m_rows.at(i).name)) {
            protectedNames << m_rows.at(i).name;
        }
    }
    if (!protectedNames.isEmpty()) {
        if (ThemedMessageBox::warning(
                this, QStringLiteral("保护分区"),
                QStringLiteral("以下分区在保护名单里，写错可能导致设备无法开机：\n%1\n\n"
                               "确定继续写入？")
                    .arg(protectedNames.join(QStringLiteral("、"))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
    }

    logView()->appendSection(QStringLiteral("写入分区"));
    const bool adbMode = m_adbMode && m_adbMode->isChecked();
    if (adbMode) {
        logInfo(QStringLiteral("[Fastboot] ADB 模式：push 到 /data/local/tmp 后 dd 写入"
                               "（需要 root 或 Fastbootd）"));
    }
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, targets, adbMode] { flashSelectedCore(targets, adbMode); }));
}

void FastbootFlashPage::flashSelectedCore(const QVector<int>& targets, bool adbMode)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    PlatformToolsManager tools;
    const bool haveTools = tools.ensureToolsExist();

    // 状态栏双进度条：批量写入 = 总进度按分区数推进；fastboot 无分区内部进度 → -1 收起第二条。
    ShellStatus::beginTask(QStringLiteral("写入选中"));
    for (int idx = 0; idx < targets.size(); ++idx) {
        const int i = targets.at(idx);
        const Row row = m_rows.at(i);
        ShellStatus::report(QStringLiteral("写入 %1").arg(row.name), std::nullopt, std::nullopt,
                            std::nullopt, targets.size() > 0
                                              ? std::optional<double>(idx * 100.0 / targets.size())
                                              : std::nullopt,
                            -1.0);
        const QString label =
            QStringLiteral("写入 %1: %2").arg(row.name, QFileInfo(row.filePath).fileName());
        logView()->beginOperation(label);
        if (!haveTools) {
            logView()->appendOperation(label, false);
            logError(QStringLiteral("[Fastboot] fastboot 工具不可用，请检查网络后重试"));
            continue;
        }
        logInfo(QStringLiteral("[Fastboot] 刷写 %1: %2").arg(
            row.name, adbMode ? QStringLiteral("adb push + dd")
                              : QStringLiteral("fastboot flash %1 \"%2\"").arg(row.name, row.filePath)));
        // 会员额度：与 C# FlashPartition 同口径 —— 一个分区一份额度，额度用完立刻停手。
        if (!kc::MembershipService::guardAndConsume(QStringLiteral("Fastboot 刷写"),
                                                    QStringLiteral("fastboot.flash"))) {
            logError(QStringLiteral("[Fastboot] 会员额度校验未通过，剩余分区已取消"));
            logView()->appendOperation(label, false);
            break;
        }

        bool ok = false;
        if (adbMode) {
            QString adbError;
            ok = flashViaAdb(row.name, row.filePath, &adbError);
            if (!ok && !adbError.trimmed().isEmpty()) {
                logWarning(QStringLiteral("[Fastboot错误] %1").arg(adbError.trimmed()));
            }
        } else {
            const auto r = FastbootSizeFallback::execute(
                QStringLiteral("flash %1 \"%2\"").arg(row.name, row.filePath), 600000, QString(),
                [this](const QString& line) { logInfo(line); });
            if (r.success && !r.output.trimmed().isEmpty()) logInfo(r.output.trimmed());
            if (!r.error.trimmed().isEmpty()) {
                logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
            }
            ok = r.success;
        }

        logView()->appendOperation(label, ok);
        if (ok) {
            logSuccess(QStringLiteral("[Fastboot] %1 刷写成功").arg(row.name));
        } else {
            logError(QStringLiteral("[Fastboot] %1 刷写失败，请确认设备处于 Fastboot 模式且镜像与机型匹配").arg(row.name));
        }
    }

    // 任务收尾：进度停 100%，任务名/分区名保留（对齐 C# ShellStatus.EndTask）。
    ShellStatus::endTask();
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this] {
        setBusy(false);
        success(QStringLiteral("写入流程结束"));
    });
}

void FastbootFlashPage::eraseSelected()
{
    if (m_busy.loadAcquire() != 0) return;

    QVector<int> targets;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_table->item(i, kColCheck)->checkState() == Qt::Checked) targets.append(i);
    }
    if (targets.isEmpty()) {
        error(QStringLiteral("请先勾选要擦除的分区"));
        return;
    }
    for (int i : targets) {
        if (m_rows.at(i).name.trimmed().isEmpty()) {
            error(QStringLiteral("自定义分区还没填分区名，请先在表格里填上"));
            return;
        }
    }

    QStringList names;
    for (int i : targets) names.append(m_rows.at(i).name);
    if (ThemedMessageBox::warning(this, QStringLiteral("确认"),
                             QStringLiteral("确定擦除以下分区？\n%1").arg(names.join(QStringLiteral("、"))),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    logView()->appendSection(QStringLiteral("擦除分区"));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, targets] { eraseSelectedCore(targets); }));
}

void FastbootFlashPage::eraseSelectedCore(const QVector<int>& targets)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    PlatformToolsManager tools;
    const bool haveTools = tools.ensureToolsExist();

    // 状态栏双进度条：批量擦除按分区数推进。
    ShellStatus::beginTask(QStringLiteral("擦除选中"));
    for (int idx = 0; idx < targets.size(); ++idx) {
        const int i = targets.at(idx);
        const Row row = m_rows.at(i);
        ShellStatus::report(QStringLiteral("擦除 %1").arg(row.name), std::nullopt, std::nullopt,
                            std::nullopt, targets.size() > 0
                                              ? std::optional<double>(idx * 100.0 / targets.size())
                                              : std::nullopt,
                            -1.0);
        const QString label = QStringLiteral("擦除 %1").arg(row.name);
        logView()->beginOperation(label);
        if (!haveTools) {
            logView()->appendOperation(label, false);
            logError(QStringLiteral("[Fastboot] fastboot 工具不可用，请检查网络后重试"));
            continue;
        }
        logInfo(QStringLiteral("[Fastboot] 擦除 %1: fastboot erase %2").arg(row.name, row.name));
        // 会员额度：与「擦除 Cache / 擦除 Metadata」同一口径 —— 一个分区一份额度，用完立刻停手。
        if (!kc::MembershipService::guardAndConsume(QStringLiteral("Fastboot 擦除 / 格式化"),
                                                    QStringLiteral("fastboot.erase"))) {
            logError(QStringLiteral("[Fastboot] 会员额度校验未通过，剩余分区已取消"));
            logView()->appendOperation(label, false);
            break;
        }
        const auto r = tools.executeFastboot(QStringLiteral("erase %1").arg(row.name), 120000);
        if (!r.error.trimmed().isEmpty()) logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
        logView()->appendOperation(label, r.success);
    }

    ShellStatus::endTask();
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this] { setBusy(false); });
}

void FastbootFlashPage::runFastbootCommand(const QString& args, const QString& desc,
                                           int timeoutMs, const QString& quotaKey,
                                           bool refreshAfter)
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(desc);
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, args, desc, timeoutMs, quotaKey, refreshAfter] {
            const qint64 started = QDateTime::currentMSecsSinceEpoch();
            bool ok = false;
            ShellStatus::beginTask(desc);
            // 会员额度（对齐 C#：格式化/擦除类扣 fastboot.erase）；扣在 worker 里。
            if (!quotaKey.isEmpty()
                && !kc::MembershipService::guardAndConsume(
                       QStringLiteral("Fastboot 擦除 / 格式化"), quotaKey)) {
                error(QStringLiteral("会员额度校验未通过，已取消"));
            } else {
                ok = runFastbootCore(args, desc, timeoutMs);
            }
            ShellStatus::endTask();
            const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
            logView()->appendFooter(QStringLiteral("%1, 耗时: %2秒")
                                        .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                                        .arg(seconds, 0, 'f', 2));
            runOnUi([this, refreshAfter] {
                setBusy(false);
                // 切换活动槽之后设备读数（current-slot）已经变了，立刻重读一次，
                // 否则顶部还挂着旧的 A/B，用户会以为没生效。
                if (refreshAfter) refreshDevice();
            });
        }));
}

bool FastbootFlashPage::runFastbootCore(const QString& args, const QString& desc, int timeoutMs)
{
    logInfo(QStringLiteral("[Fastboot] %1: fastboot %2").arg(desc, args));
    logView()->beginOperation(QStringLiteral("fastboot %1").arg(args));

    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logView()->appendOperation(QStringLiteral("fastboot %1").arg(args), false);
        error(QStringLiteral("fastboot 工具不可用，请检查网络后重试"));
        return false;
    }

    const auto r = FastbootSizeFallback::execute(
        args, timeoutMs, QString(), [this](const QString& line) { logInfo(line); });
    logView()->appendOperation(QStringLiteral("fastboot %1").arg(args), r.success);
    if (r.success && !r.output.trimmed().isEmpty()) {
        logInfo(r.output.trimmed());
        // getvar / oem device-info 的输出顺手刷设备读数。
        if (args.startsWith(QStringLiteral("getvar")) || args.contains(QStringLiteral("device-info")))
            applyDeviceReadings(r.output);
    }
    if (!r.error.trimmed().isEmpty()) logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));

    if (r.success) {
        success(QStringLiteral("%1 完成").arg(desc));
    } else {
        error(QStringLiteral("%1 失败，请确认设备已连接并处于 Fastboot 模式").arg(desc));
    }
    return r.success;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ADB 模式刷入（移植自 YukiClient GenericTab.UseADB 分支）
//    adb push "<img>" /data/local/tmp/ftb_flash_<guid>.img
//    → adb root（失败回落 su -c）
//    → shell dd if=<remote> of=/dev/block/by-name/<Part> bs=4M conv=fsync
//    → shell rm -f <remote>（无论成败都清）
//  适合：设备已 root / 在 Fastbootd（userspace fastboot 不认 `fastboot flash` 的分区）。
// ─────────────────────────────────────────────────────────────────────────────
bool FastbootFlashPage::flashViaAdb(const QString& partition, const QString& image, QString* error)
{
    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        if (error) *error = QStringLiteral("平台工具不可用（adb 未就绪）");
        return false;
    }

    const QString remote = QStringLiteral("/data/local/tmp/ftb_flash_%1.img")
                               .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    logInfo(QStringLiteral("[ADB] 推送 %1 → %2")
                .arg(QFileInfo(image).fileName(), remote));
    const auto push = tools.executeAdb(QStringLiteral("push \"%1\" \"%2\"").arg(image, remote),
                                       900000);
    if (!push.success) {
        if (error) {
            *error = QStringLiteral("adb push 失败: %1")
                         .arg(push.error.trimmed().isEmpty() ? push.output.trimmed()
                                                             : push.error.trimmed());
        }
        return false;
    }

    // adb root 会让 adbd 重启，给一点时间；失败也不中断，后面 su -c 兜底。
    tools.executeAdb(QStringLiteral("root"), 20000);
    QThread::msleep(1200);

    const QString ddCommand =
        QStringLiteral("shell dd if=%1 of=/dev/block/by-name/%2 bs=4M conv=fsync")
            .arg(remote, partition);
    auto result = tools.executeAdb(ddCommand, 900000);
    if (!result.success) {
        logWarning(QStringLiteral("[ADB] 直接 dd 失败，改用 su -c 重试"));
        const auto su = tools.executeAdb(
            QStringLiteral("shell su -c \"dd if=%1 of=/dev/block/by-name/%2 bs=4M conv=fsync\"")
                .arg(remote, partition),
            900000);
        if (su.success) result = su;
    }

    tools.executeAdb(QStringLiteral("shell rm -f %1").arg(remote), 30000);

    if (!result.success) {
        if (error) {
            *error = QStringLiteral("dd 写入失败: %1")
                         .arg(result.error.trimmed().isEmpty() ? result.output.trimmed()
                                                               : result.error.trimmed());
        }
        return false;
    }
    logSuccess(QStringLiteral("[ADB] %1 写入完成").arg(partition));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  禁用 vbmeta 校验（三选一 → 连刷 vbmeta / vbmeta_system / vbmeta_vendor）
// ─────────────────────────────────────────────────────────────────────────────
void FastbootFlashPage::disableVbmeta()
{
    if (m_busy.loadAcquire() != 0) return;

    const QStringList options = {
        QStringLiteral("--disable-verity --disable-verification（推荐）"),
        QStringLiteral("--disable-verity"),
        QStringLiteral("--disable-verification"),
        QStringLiteral("取消"),
    };
    const int choice = ThemedMessageBox::custom(
        this, QStringLiteral("禁用 vbmeta 校验"),
        QStringLiteral("将对 vbmeta / vbmeta_system / vbmeta_vendor 逐个执行：\n"
                       "    fastboot <所选参数> flash <分区> <镜像>\n\n"
                       "镜像取自「固件目录」（根目录或 images/ 下的同名文件），"
                       "只刷存在的镜像，缺哪个跳过哪个。"),
        options, 0);
    if (choice < 0 || choice > 2) return;

    const QString flags = choice == 0 ? QStringLiteral("--disable-verity --disable-verification")
                          : choice == 1 ? QStringLiteral("--disable-verity")
                                        : QStringLiteral("--disable-verification");

    const QString romDir = m_romDirEdit->text().trimmed();
    if (romDir.isEmpty() || !QDir(romDir).exists()) {
        error(QStringLiteral("请先在「固件目录」里选一个包含 vbmeta*.img 的目录"));
        return;
    }

    QStringList images;
    for (const QString& name : { QStringLiteral("vbmeta"), QStringLiteral("vbmeta_system"),
                                 QStringLiteral("vbmeta_vendor") }) {
        const QStringList candidates = {
            QDir(romDir).filePath(name + QStringLiteral(".img")),
            QDir(romDir).filePath(QStringLiteral("images/") + name + QStringLiteral(".img")),
        };
        for (const QString& candidate : candidates) {
            if (QFileInfo::exists(candidate)) {
                images << candidate;
                break;
            }
        }
    }
    if (images.isEmpty()) {
        error(QStringLiteral("固件目录里没有 vbmeta / vbmeta_system / vbmeta_vendor 镜像"));
        return;
    }

    logView()->appendSection(QStringLiteral("禁用 vbmeta 校验"));
    setBusy(true);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, flags, images] { disableVbmetaCore(flags, images); }));
}

void FastbootFlashPage::disableVbmetaCore(const QString& flags, const QStringList& images)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    ShellStatus::beginTask(QStringLiteral("禁用 vbmeta"));

    PlatformToolsManager tools;
    const bool haveTools = tools.ensureToolsExist();
    bool allOk = haveTools;
    if (!haveTools) logError(QStringLiteral("[vbmeta] 平台工具不可用"));

    for (int i = 0; haveTools && i < images.size(); ++i) {
        const QString path = images.at(i);
        const QString partition = QFileInfo(path).completeBaseName();
        ShellStatus::report(QStringLiteral("vbmeta %1").arg(partition), std::nullopt,
                            std::nullopt, std::nullopt,
                            static_cast<double>(i) / images.size() * 100.0, -1.0);
        logInfo(QStringLiteral("[vbmeta] fastboot %1 flash %2 \"%3\"").arg(flags, partition, path));

        const auto r = FastbootSizeFallback::execute(
            QStringLiteral("%1 flash %2 \"%3\"").arg(flags, partition, path), 300000, QString(),
            [this](const QString& line) { logInfo(line); });
        if (!r.error.trimmed().isEmpty()) {
            logWarning(QStringLiteral("[Fastboot错误] %1").arg(r.error.trimmed()));
        }
        if (r.success) {
            logSuccess(QStringLiteral("[vbmeta] %1 已写入").arg(partition));
        } else {
            allOk = false;
            logError(QStringLiteral("[vbmeta] %1 写入失败").arg(partition));
        }
    }

    ShellStatus::endTask();
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this, allOk] {
        setBusy(false);
        if (allOk) success(QStringLiteral("vbmeta 处理完成"));
        else error(QStringLiteral("vbmeta 处理失败，详见日志"));
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  删除 COW 逻辑分区
//    先 getvar all，挑出同时含 is-logical 与 yes 的行，按 [':',' '] 切分取名字，
//    名字含 "-cow" 的逐个 delete-logical-partition；boot / vendor_boot / init_boot 豁免。
//    删除是**先查询再确认**两步，避免用户不知道要删什么就点了。
// ─────────────────────────────────────────────────────────────────────────────
void FastbootFlashPage::scanCowPartitions()
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("查询 COW 逻辑分区"));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this] { scanCowCore(); }));
}

void FastbootFlashPage::scanCowCore()
{
    PlatformToolsManager tools;
    if (!tools.ensureToolsExist()) {
        logError(QStringLiteral("[COW] 平台工具不可用"));
        runOnUi([this] { setBusy(false); });
        return;
    }

    const auto r = tools.executeFastboot(QStringLiteral("getvar all"), 120000);
    const QString output = r.output + QLatin1Char('\n') + r.error;
    if (output.trimmed().isEmpty()) {
        logError(QStringLiteral("[COW] getvar all 没有返回内容，请确认设备在 Fastboot/Fastbootd"));
        runOnUi([this] { setBusy(false); });
        return;
    }

    QStringList logical;
    static const QRegularExpression separator(QStringLiteral("[:\\s]+"));
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (!line.contains(QStringLiteral("is-logical"), Qt::CaseInsensitive)) continue;
        if (!line.contains(QStringLiteral("yes"), Qt::CaseInsensitive)) continue;
        const QStringList fields = line.split(separator, Qt::SkipEmptyParts);
        for (int i = 0; i < fields.size(); ++i) {
            if (fields.at(i).compare(QStringLiteral("is-logical"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (i + 2 < fields.size()
                && fields.at(i + 2).compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0) {
                logical << fields.at(i + 1).trimmed();
            }
            break;
        }
    }

    QStringList cows;
    for (const QString& name : logical) {
        if (!name.contains(QStringLiteral("-cow"), Qt::CaseInsensitive)) continue;
        const QString base = name.section(QStringLiteral("-cow"), 0, 0);
        if (base == QStringLiteral("boot") || base == QStringLiteral("vendor_boot")
            || base == QStringLiteral("init_boot")) {
            continue;
        }
        if (!cows.contains(name)) cows << name;
    }

    if (cows.isEmpty()) {
        logInfo(QStringLiteral("[COW] 没有发现可删除的 COW 逻辑分区（共 %1 个逻辑分区）")
                    .arg(logical.size()));
        runOnUi([this] {
            setBusy(false);
            success(QStringLiteral("没有 COW 分区"));
        });
        return;
    }

    logInfo(QStringLiteral("[COW] 发现 %1 个 COW 分区: %2")
                .arg(cows.size())
                .arg(cows.join(QStringLiteral("、"))));
    QPointer<FastbootFlashPage> guard(this);
    runOnUi([this, guard, cows] {
        if (!guard) return;
        setBusy(false);
        deleteCowPartitions(cows);
    });
}

void FastbootFlashPage::deleteCowPartitions(const QStringList& names)
{
    if (names.isEmpty()) return;
    if (ThemedMessageBox::warning(
            this, QStringLiteral("删除 COW 分区"),
            QStringLiteral("将逐个执行 fastboot delete-logical-partition：\n%1\n\n"
                           "COW 是 OTA 增量升级留下的快照分区，删除后不会影响当前系统，"
                           "但**未完成的 OTA 会失效**。确定继续？")
                .arg(names.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    logView()->appendSection(QStringLiteral("删除 COW 分区"));
    setBusy(true);
    QThreadPool::globalInstance()->start(new LambdaTask([this, names] { deleteCowCore(names); }));
}

void FastbootFlashPage::deleteCowCore(const QStringList& names)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    ShellStatus::beginTask(QStringLiteral("删除 COW 分区"));

    PlatformToolsManager tools;
    const bool haveTools = tools.ensureToolsExist();
    int okCount = 0;

    for (int i = 0; haveTools && i < names.size(); ++i) {
        const QString& name = names.at(i);
        ShellStatus::report(QStringLiteral("删除 %1").arg(name), std::nullopt, std::nullopt,
                            std::nullopt,
                            static_cast<double>(i) / names.size() * 100.0, -1.0);
        const auto r = tools.executeFastboot(
            QStringLiteral("delete-logical-partition %1").arg(name), 60000);
        if (r.success) {
            ++okCount;
            logSuccess(QStringLiteral("[COW] 已删除 %1").arg(name));
        } else {
            logError(QStringLiteral("[COW] 删除 %1 失败: %2")
                         .arg(name, r.error.trimmed().isEmpty() ? r.output.trimmed()
                                                                : r.error.trimmed()));
        }
    }

    ShellStatus::endTask();
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    runOnUi([this, okCount, total = names.size()] {
        setBusy(false);
        if (okCount == total) success(QStringLiteral("已删除 %1 个 COW 分区").arg(okCount));
        else error(QStringLiteral("删除完成 %1/%2，详见日志").arg(okCount).arg(total));
    });
}

} // namespace kc::ui
