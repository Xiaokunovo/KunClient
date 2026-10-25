#include "ui/pages/PayloadPage.h"

#include "kc/auth/MembershipService.h"
#include "storage/PayloadParser.h"
#include "ui/ShellStatus.h"

#include "ui/theme/ThemeManager.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"
#include "ui/widgets/ToolProgressBar.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>
#include <functional>

namespace kc::ui {
namespace {

constexpr quint32 kMagic = 0x00444150u;   // C# PayloadPage.MAGIC（u32 LE）
constexpr int kColCheck = 0;
constexpr int kColIndex = 1;
constexpr int kColName = 2;
constexpr int kColSize = 3;
constexpr int kColOffset = 4;
constexpr qint64 kExtractBufferSize = 81920;

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

} // namespace

PayloadPage::PayloadPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("Payload解析"));
    buildUi();
}

void PayloadPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 2026-10-13：裸行平铺 → 按功能收进卡片（对齐 FastbootFlashPage 的 addCard 套路）。
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

    // ── 卡 1：固件 ──────────────────────────────────────────────────────
    QVBoxLayout* fileCard = addCard(QStringLiteral("固件"));
    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(8);

    m_payloadEdit = new QLineEdit(this);
    m_payloadEdit->setPlaceholderText(QStringLiteral("payload.bin 路径"));
    m_browseButton = new QPushButton(QStringLiteral("浏览…"), this);
    m_browseButton->setCursor(Qt::PointingHandCursor);
    connect(m_browseButton, &QPushButton::clicked, this, [this] { browsePayload(); });

    m_saveDirEdit = new QLineEdit(this);
    m_saveDirEdit->setPlaceholderText(QStringLiteral("提取目录（留空则提取时选择）"));
    m_pickDirButton = new QPushButton(QStringLiteral("目录…"), this);
    m_pickDirButton->setCursor(Qt::PointingHandCursor);
    connect(m_pickDirButton, &QPushButton::clicked, this, [this] { pickSaveDir(); });

    m_parseButton = new QPushButton(QStringLiteral("解析固件"), this);
    m_parseButton->setObjectName(QStringLiteral("primaryButton"));
    m_parseButton->setCursor(Qt::PointingHandCursor);
    connect(m_parseButton, &QPushButton::clicked, this, [this] { parsePayload(); });

    fileRow->addWidget(m_payloadEdit, 3);
    fileRow->addWidget(m_browseButton);
    fileRow->addWidget(m_saveDirEdit, 2);
    fileRow->addWidget(m_pickDirButton);
    fileRow->addWidget(m_parseButton);
    fileCard->addLayout(fileRow);

    m_infoLabel = new QLabel(this);
    m_infoLabel->setObjectName(QStringLiteral("statusValue"));
    fileCard->addWidget(m_infoLabel);

    // ── 卡 2：分区操作（工具行 + 进度）──────────────────────────────────
    QVBoxLayout* toolCard = addCard(QStringLiteral("分区操作"));
    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(8);

    m_selectAllButton = new QPushButton(QStringLiteral("全选"), this);
    m_deselectAllButton = new QPushButton(QStringLiteral("取消全选"), this);
    m_extractSelectedButton = new QPushButton(QStringLiteral("提取选中"), this);
    m_extractSelectedButton->setObjectName(QStringLiteral("primaryButton"));
    m_extractAllButton = new QPushButton(QStringLiteral("提取全部"), this);
    for (QPushButton* b : { m_selectAllButton, m_deselectAllButton,
                            m_extractSelectedButton, m_extractAllButton }) {
        b->setCursor(Qt::PointingHandCursor);
        toolRow->addWidget(b);
    }
    connect(m_selectAllButton, &QPushButton::clicked, this, [this] {
        m_updatingTable = true;
        for (auto& p : m_partitions) p.selected = true;
        for (int r = 0; r < m_table->rowCount(); ++r)
            m_table->item(r, kColCheck)->setCheckState(Qt::Checked);
        m_updatingTable = false;
    });
    connect(m_deselectAllButton, &QPushButton::clicked, this, [this] {
        m_updatingTable = true;
        for (auto& p : m_partitions) p.selected = false;
        for (int r = 0; r < m_table->rowCount(); ++r)
            m_table->item(r, kColCheck)->setCheckState(Qt::Unchecked);
        m_updatingTable = false;
    });
    connect(m_extractSelectedButton, &QPushButton::clicked, this, [this] { extractSelected(); });
    connect(m_extractAllButton, &QPushButton::clicked, this, [this] { extractAll(); });

    toolRow->addStretch();

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索分区"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedWidth(180);

    // 搜索过滤防抖：手停 220ms 才真正过滤（与 C# 同款）。
    m_filterDebounce = new QTimer(this);
    m_filterDebounce->setSingleShot(true);
    m_filterDebounce->setInterval(220);
    connect(m_filterDebounce, &QTimer::timeout, this,
            [this] { applyFilter(m_searchEdit->text().trimmed()); });
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this] { m_filterDebounce->start(); });
    toolRow->addWidget(m_searchEdit);
    toolCard->addLayout(toolRow);

    // ── 分区表 ──────────────────────────────────────────────────────────
    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("选"),
                                         QStringLiteral("#"),
                                         QStringLiteral("分区名"),
                                         QStringLiteral("大小"),
                                         QStringLiteral("偏移地址") });
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(kColCheck, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColIndex, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColName, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColSize, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColOffset, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_updatingTable || !item || item->column() != kColCheck) return;
        if (item->row() < m_partitions.size())
            m_partitions[item->row()].selected = item->checkState() == Qt::Checked;
    });
    layout->addWidget(m_table, 3);

    // ── 进度 ────────────────────────────────────────────────────────────
    m_progress = new ToolProgressBar(this);
    m_progressLabel = new QLabel(this);
    m_progressLabel->setObjectName(QStringLiteral("deviceHintLabel"));
    toolCard->addWidget(m_progress);
    toolCard->addWidget(m_progressLabel);

    // ── 日志 ────────────────────────────────────────────────────────────
    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);
}

void PayloadPage::browsePayload()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 payload.bin"), QString(),
        QStringLiteral("Payload文件 (*.bin);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    m_payloadEdit->setText(path);
}

void PayloadPage::pickSaveDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择提取目录"));
    if (dir.isEmpty()) return;
    m_saveDirEdit->setText(dir);
    logInfo(QStringLiteral("[Payload] 提取目录: %1").arg(dir));
}

// ─────────────────────────────────────────────────────────────────────────────
//  解析（与 C# ParsePayloadFile / ParseAsRawPayload 逐字对照）
// ─────────────────────────────────────────────────────────────────────────────

void PayloadPage::parsePayload()
{
    const QString file = m_payloadEdit->text().trimmed();
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        error(QStringLiteral("请选择有效的payload.bin文件"));
        return;
    }

    logView()->appendSection(QStringLiteral("解析固件"));
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    parsePayloadFile(file);
    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
}

void PayloadPage::parsePayloadFile(const QString& file)
{
    m_payloadPath = file;
    m_partitions.clear();

    // 真 OTA 的 payload.bin 是 CrAU + protobuf manifest；先认这个，再回落到历史
    // PAD 私有容器（0x00444150）解析 —— 后者只在前 1MB 扫分区名，拿不到 offset。
    if (swt::edl::PayloadParser::isCrAU(file)) {
        parseStandardPayload(file);
        return;
    }
    m_standardPayload = false;

    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        logError(QStringLiteral("[Payload] 解析失败: %1").arg(f.errorString()));
        parseAsRawPayload(file);
        return;
    }

    bool ok = false;
    quint32 magic = 0;
    qint64 headerSize = 0;
    qint64 entryCount = 0;
    QByteArray nameBytes;
    qint64 size = 0;
    qint64 offset = 0;

    // payload.bin format: 4-byte magic, 8-byte header_size, 8-byte entry_count（全小端，与 C# BinaryReader 一致）。
    ok = (f.read(reinterpret_cast<char*>(&magic), sizeof(magic)) == sizeof(magic));
    if (ok && magic != kMagic) {
        // Try big-endian（字节反转再比一次）。
        magic = ((magic & 0xFFu) << 24) | ((magic & 0xFF00u) << 8) |
                ((magic >> 8) & 0xFF00u) | ((magic >> 24) & 0xFFu);
    }

    if (magic != kMagic) {
        logWarning(QStringLiteral("[Payload] 未识别的payload格式，尝试解析..."));
        parseAsRawPayload(file);
        return;
    }

    auto readI64 = [&f, &ok](qint64* out) {
        ok = ok && f.read(reinterpret_cast<char*>(out), sizeof(qint64)) == sizeof(qint64);
    };
    readI64(&headerSize);
    readI64(&entryCount);
    if (!ok) {
        logError(QStringLiteral("[Payload] 解析失败: 头部不完整"));
        parseAsRawPayload(file);
        return;
    }

    logInfo(QStringLiteral("[Payload] Magic: 0x%1, HeaderSize: %2, 分区数: %3")
                .arg(magic, 8, 16, QLatin1Char('0'))
                .arg(headerSize)
                .arg(entryCount));

    for (qint64 i = 0; i < entryCount && f.pos() < headerSize; ++i) {
        quint32 nameLen = 0;
        if (f.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen)) != sizeof(nameLen) ||
            nameLen > 1024u * 1024u) {
            logError(QStringLiteral("[Payload] 解析失败: 表项损坏"));
            m_partitions.clear();
            parseAsRawPayload(file);
            return;
        }
        nameBytes.resize(static_cast<int>(nameLen));
        if (f.read(nameBytes.data(), nameLen) != static_cast<qint64>(nameLen)) break;

        if (f.read(reinterpret_cast<char*>(&size), sizeof(size)) != sizeof(size)) break;
        if (f.read(reinterpret_cast<char*>(&offset), sizeof(offset)) != sizeof(offset)) break;

        PartitionInfo part;
        part.index = m_partitions.size() + 1;
        part.name = QString::fromLatin1(nameBytes);
        while (part.name.endsWith(QLatin1Char('\0'))) part.name.chop(1);   // TrimEnd('\0')
        part.sizeText = formatSize(size);
        part.offset = offset;
        part.dataLength = size;
        m_partitions.append(part);

        // Align to 4 bytes（与 C# 相同：按当前文件位置补读到 4 字节边界）。
        const qint64 pad = (4 - (f.pos() % 4)) % 4;
        if (pad > 0) f.seek(f.pos() + pad);
    }

    refreshTable();
    m_infoLabel->setText(QStringLiteral("文件: %1    分区数: %2    总大小: %3")
                             .arg(QFileInfo(file).fileName())
                             .arg(m_partitions.size())
                             .arg(formatSize(QFileInfo(file).size())));
    logSuccess(QStringLiteral("[Payload] 解析完成: %1 个分区").arg(m_partitions.size()));
}

void PayloadPage::parseStandardPayload(const QString& file)
{
    m_standardPayload = true;

    swt::edl::PayloadInfo info;
    QString errorText;
    if (!swt::edl::PayloadParser::inspect(file, &info, &errorText)) {
        m_standardPayload = false;
        m_partitions.clear();
        refreshTable();
        logError(QStringLiteral("[Payload] 解析失败: %1").arg(errorText));
        error(QStringLiteral("无法解析payload.bin格式"));
        m_infoLabel->setText(QStringLiteral("无法解析payload.bin格式"));
        return;
    }

    for (const auto& partition : info.partitions) {
        PartitionInfo part;
        part.index = m_partitions.size() + 1;
        part.name = partition.name;
        part.sizeText = formatSize(static_cast<qint64>(partition.size));
        m_partitions.append(part);
    }

    refreshTable();
    m_infoLabel->setText(QStringLiteral("文件: %1    分区数: %2    总大小: %3")
                             .arg(QFileInfo(file).fileName())
                             .arg(m_partitions.size())
                             .arg(formatSize(QFileInfo(file).size())));
    logInfo(QStringLiteral("[Payload] 标准 OTA（CrAU）：manifest %1 字节，数据区起点 0x%2")
                .arg(info.manifestSize)
                .arg(info.dataOffset, 0, 16));
    logSuccess(QStringLiteral("[Payload] 解析完成: %1 个分区").arg(m_partitions.size()));
    for (const auto& partition : info.partitions) {
        logInfo(QStringLiteral("  · %1  %2  (%3 个操作)")
                    .arg(partition.name, formatSize(static_cast<qint64>(partition.size)))
                    .arg(partition.operationCount));
    }
}

void PayloadPage::extractStandardCore(const QVector<PartitionInfo>& partitions,
                                      const QString& outputDir)
{
    const qint64 started = QDateTime::currentMSecsSinceEpoch();

    QStringList names;
    for (const PartitionInfo& part : partitions) names << part.name;
    logInfo(QStringLiteral("[Payload] 标准 OTA：按 manifest 重建 %1 个分区...").arg(names.size()));

    QStringList extracted;
    QString errorText;
    const bool ok = swt::edl::PayloadParser::extract(
        m_payloadPath, outputDir, names, &extracted, &errorText,
        [this](int percent, const QString& message) {
            runOnUi([this, percent, message] {
                m_progress->setValue(percent);
                if (!message.isEmpty()) m_progressLabel->setText(message);
            });
        });

    for (const QString& name : extracted) {
        logSuccess(QStringLiteral("[Payload] 已提取: %1").arg(QDir(outputDir).filePath(name)));
    }
    if (!ok) logError(QStringLiteral("[Payload] 提取失败: %1").arg(errorText));

    runOnUi([this, ok, extracted, outputDir] {
        if (ok) {
            m_progress->setValue(100);
            m_progressLabel->setText(QStringLiteral("提取完成: %1 个分区 → %2")
                                         .arg(extracted.size())
                                         .arg(outputDir));
            success(QStringLiteral("提取完成: %1 个分区").arg(extracted.size()));
        } else {
            m_progressLabel->setText(QStringLiteral("提取失败"));
            error(QStringLiteral("提取失败，详见日志"));
        }
    });

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    ShellStatus::endTask();
    runOnUi([this] { setBusy(false); });
}

void PayloadPage::parseAsRawPayload(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return;

    // Try to find ASCII partition names in the binary（前 1MB）。
    const QByteArray buffer = f.read(qMin<qint64>(f.size(), 1024 * 1024));
    const QVector<const char*> knownParts = {
        "boot", "system", "vendor", "product", "system_ext",
        "dtbo", "recovery", "init_boot", "modem",
    };

    for (const char* part : knownParts) {
        const QByteArray needle(part);
        const int at = buffer.indexOf(needle);
        if (at < 0) continue;

        PartitionInfo p;
        p.index = m_partitions.size() + 1;
        p.name = QString::fromLatin1(needle);
        p.sizeText = QStringLiteral("未知");
        p.offset = at;
        p.dataLength = 0;
        m_partitions.append(p);
    }

    if (!m_partitions.isEmpty()) {
        refreshTable();
        m_infoLabel->setText(QStringLiteral("文件: %1    检测到 %2 个分区名（格式不标准）")
                                 .arg(QFileInfo(file).fileName())
                                 .arg(m_partitions.size()));
        logWarning(QStringLiteral("[Payload] 使用名称检测模式，找到 %1 个分区").arg(m_partitions.size()));
    } else {
        refreshTable();   // _partitions 已清空 → 表格也要跟着空
        logError(QStringLiteral("[Payload] 无法解析payload.bin格式"));
        m_infoLabel->setText(QStringLiteral("无法解析payload.bin格式"));
    }
}

void PayloadPage::refreshTable()
{
    m_updatingTable = true;
    m_table->setRowCount(m_partitions.size());
    const auto& theme = ThemeManager::instance();
    for (int i = 0; i < m_partitions.size(); ++i) {
        const PartitionInfo& p = m_partitions.at(i);

        auto* checkItem = new QTableWidgetItem;
        checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        checkItem->setCheckState(p.selected ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(i, kColCheck, checkItem);

        auto* indexItem = new QTableWidgetItem(QString::number(p.index));
        indexItem->setForeground(theme.color(ColorToken::TextMuted));
        m_table->setItem(i, kColIndex, indexItem);

        m_table->setItem(i, kColName, new QTableWidgetItem(p.name));
        m_table->setItem(i, kColSize, new QTableWidgetItem(p.sizeText));
        // 备用解析模式没有偏移 → 显示 "-"（与 C# OffsetText 一致）。
        m_table->setItem(i, kColOffset,
                         new QTableWidgetItem(p.offset > 0
                                                  ? QStringLiteral("0x%1").arg(p.offset, 0, 16)
                                                  : QStringLiteral("-")));
    }
    m_updatingTable = false;
    applyFilter(m_searchEdit ? m_searchEdit->text().trimmed() : QString());
}

void PayloadPage::applyFilter(const QString& text)
{
    for (int r = 0; r < m_partitions.size(); ++r) {
        const bool hit = text.isEmpty() || m_partitions.at(r).name.contains(text, Qt::CaseInsensitive);
        m_table->setRowHidden(r, !hit);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  提取
// ─────────────────────────────────────────────────────────────────────────────

void PayloadPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    m_parseButton->setEnabled(!busy);
    m_browseButton->setEnabled(!busy);
    m_pickDirButton->setEnabled(!busy);
    m_extractSelectedButton->setEnabled(!busy);
    m_extractAllButton->setEnabled(!busy);
    m_selectAllButton->setEnabled(!busy);
    m_deselectAllButton->setEnabled(!busy);
}

void PayloadPage::extractSelected()
{
    QVector<PartitionInfo> selected;
    for (const PartitionInfo& p : m_partitions) {
        if (p.selected) selected.append(p);
    }
    if (selected.isEmpty()) {
        error(QStringLiteral("请先选择要提取的分区"));
        return;
    }
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("Payload 分区提取"),
                                              QStringLiteral("payload.extract")))
        return;
    QString dir = m_saveDirEdit->text().trimmed();
    if (dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择提取目录"));
        if (dir.isEmpty()) return;
        m_saveDirEdit->setText(dir);
    }
    if (!kc::MembershipService::consume(QStringLiteral("payload.extract"))) return;
    startExtract(selected, dir);
}

void PayloadPage::extractAll()
{
    if (m_partitions.isEmpty()) {
        error(QStringLiteral("请先解析payload.bin"));
        return;
    }
    // 「提取全部」与「提取选中」共用同一份额度（对齐 C#：漏掉这里 = 全部提取绕过额度）。
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("Payload 分区提取"),
                                              QStringLiteral("payload.extract")))
        return;
    QString dir = m_saveDirEdit->text().trimmed();
    if (dir.isEmpty()) {
        dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择提取目录"));
        if (dir.isEmpty()) return;
        m_saveDirEdit->setText(dir);
    }
    if (!kc::MembershipService::consume(QStringLiteral("payload.extract"))) return;
    startExtract(m_partitions, dir);
}

void PayloadPage::startExtract(QVector<PartitionInfo> partitions, const QString& outputDir)
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("提取文件"));
    setBusy(true);
    m_progress->setValue(0);
    m_progressLabel->setText(QStringLiteral("准备提取 %1 个分区...").arg(partitions.size()));
    QPointer<PayloadPage> guard(this);
    QThreadPool::globalInstance()->start(
        new LambdaTask([this, guard, partitions, outputDir] {
            ShellStatus::beginTask(QStringLiteral("Payload 提取"));
            extractCore(partitions, outputDir);
        }));
}

void PayloadPage::extractCore(QVector<PartitionInfo> partitions, const QString& outputDir)
{
    if (m_standardPayload) {
        extractStandardCore(partitions, outputDir);
        return;
    }

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QDir().mkpath(outputDir);
    const QString payloadPath = m_payloadPath;

    logInfo(QStringLiteral("[Payload] 开始提取 %1 个分区...").arg(partitions.size()));

    QFile src(payloadPath);
    bool allOk = src.open(QIODevice::ReadOnly);
    if (!allOk) logError(QStringLiteral("[Payload] 提取错误: %1").arg(src.errorString()));

    for (int i = 0; allOk && i < partitions.size(); ++i) {
        const PartitionInfo& part = partitions.at(i);
        logInfo(QStringLiteral("[Payload] 提取: %1 (%2)").arg(part.name, part.sizeText));
        runOnUi([this, i, part, total = partitions.size()] {
            m_progressLabel->setText(
                QStringLiteral("提取中 (%1/%2): %3").arg(i + 1).arg(total).arg(part.name));
        });
        ShellStatus::report(std::nullopt, part.name, std::nullopt, std::nullopt,
                            (static_cast<double>(i) + 1.0) / partitions.size() * 100.0, -1.0);

        if (part.dataLength > 0 && part.offset > 0) {
            if (!src.seek(part.offset)) {
                logError(QStringLiteral("[Payload] 提取错误: %1").arg(src.errorString()));
                break;
            }
            const QString outputPath = QDir(outputDir).filePath(part.name + QStringLiteral(".img"));
            QFile dst(outputPath);
            if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                logError(QStringLiteral("[Payload] 提取错误: %1").arg(dst.errorString()));
                break;
            }
            qint64 remaining = part.dataLength;
            char buffer[kExtractBufferSize];
            while (remaining > 0) {
                const qint64 toRead = qMin<qint64>(static_cast<qint64>(sizeof(buffer)), remaining);
                const qint64 read = src.read(buffer, toRead);
                if (read <= 0) break;
                if (dst.write(buffer, read) != read) {
                    logError(QStringLiteral("[Payload] 提取错误: %1").arg(dst.errorString()));
                    allOk = false;
                    break;
                }
                remaining -= read;
            }
            dst.close();
            if (allOk) logSuccess(QStringLiteral("[Payload] 已提取: %1").arg(outputPath));
        } else {
            logWarning(QStringLiteral("[Payload] 跳过 %1（无偏移信息）").arg(part.name));
        }

        const double pct = static_cast<double>(i + 1) / partitions.size() * 100.0;
        runOnUi([this, pct] { m_progress->setValue(pct); });
    }
    src.close();

    if (allOk) {
        logSuccess(QStringLiteral("[Payload] 提取完成: %1").arg(outputDir));
        runOnUi([this, partitions, outputDir] {
            m_progress->setValue(100);
            m_progressLabel->setText(QStringLiteral("提取完成: %1 个分区 → %2")
                                         .arg(partitions.size()).arg(outputDir));
            success(QStringLiteral("提取完成: %1 个分区").arg(partitions.size()));
        });
    } else {
        runOnUi([this] {
            m_progressLabel->setText(QStringLiteral("提取失败"));
            error(QStringLiteral("提取失败，详见日志"));
        });
    }

    const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    logView()->appendFooter(QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
    ShellStatus::endTask();
    runOnUi([this] { setBusy(false); });
}

} // namespace kc::ui
