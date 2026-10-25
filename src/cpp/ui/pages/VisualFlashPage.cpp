#include "ui/pages/VisualFlashPage.h"

#include "kc/auth/MembershipService.h"
#include "ui/ShellStatus.h"
#include "kc/tools/AdbGptReader.h"
#include "kc/tools/FastbootSizeFallback.h"
#include "kc/tools/PlatformToolsManager.h"
#include "ui/widgets/AppIcons.h"
#include "ui/widgets/GlassMenu.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/NotificationBar.h"
#include "ui/widgets/ToolProgressBar.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include "ui/dialogs/ThemedMessageBox.h"
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QThreadPool>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

// C# GetPartitionType：按分区名归类显示。
QString partitionTypeOf(const QString& name)
{
    const QString lower = name.toLower();
    static const char* kGroups[][6] = {
        {"boot", "init_boot", "recovery", "dtbo", nullptr, "Boot"},
        {"system", "vendor", "product", "system_ext", "odm", "System"},
        {"modem", "radio", nullptr, nullptr, nullptr, "Modem"},
        {"userdata", "metadata", nullptr, nullptr, nullptr, "Data"},
        {"xbl", "abl", "tz", "hyp", "devcfg", "rpm"},
        {"gpt", "gpt_backup0", "gpt_backup1", nullptr, nullptr, "GPT"},
    };
    for (auto& group : kGroups) {
        for (int i = 0; i < 5; ++i) {
            if (group[i] && lower == QLatin1String(group[i]))
                return QString::fromUtf8(group[5]);
        }
    }
    return QStringLiteral("Other");
}

QString formatBytes(qint64 bytes)
{
    if (bytes <= 0) return QStringLiteral("-");
    const qint64 kb = 1024, mb = kb * 1024, gb = mb * 1024;
    if (bytes >= gb) return QStringLiteral("%1 GB").arg(bytes / double(gb), 0, 'f', 1);
    if (bytes >= mb) return QStringLiteral("%1 MB").arg(bytes / double(mb), 0, 'f', 1);
    if (bytes >= kb) return QStringLiteral("%1 KB").arg(bytes / double(kb), 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

VisualFlashPage::VisualFlashPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("可视刷写"));
    buildUi();
}

void VisualFlashPage::buildUi()
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

    // ── 分区表工具行 ─────────────────────────────────────────────────────
    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(8);
    m_readButton = new QPushButton(QStringLiteral("读取分区表"), this);
    m_readButton->setObjectName(QStringLiteral("primaryButton"));
    m_readButton->setCursor(Qt::PointingHandCursor);
    m_readButton->setToolTip(QStringLiteral("优先 dd 磁盘头解析真 GPT（分区名 / 大小 / 起始扇区 / 类型 GUID，需 root）；\n"
                                            "读不到时退回 fastboot getvar all（只有名字 + 大小）"));
    connect(m_readButton, &QPushButton::clicked, this, [this] { readPartTable(); });
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { readPartTable(); });
    toolRow->addWidget(m_readButton);
    toolRow->addWidget(m_refreshButton);
    toolRow->addStretch();
    layout->addLayout(toolRow);

    // ── 分区表（选择 / 分区名 / 大小 / 起始扇区 / 类型；右键菜单三操作）──
    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("选择"), QStringLiteral("分区名"),
                                        QStringLiteral("大小"), QStringLiteral("起始扇区"),
                                        QStringLiteral("类型")});
    // 只让「分区名」列拉伸：stretchLastSection 会把剩余宽度全塞给「类型」，分区名反而窄。
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    // 选择/大小/起始扇区/类型 用固定宽度：默认均分会让勾选列占掉小半屏。
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 56);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_table->setColumnWidth(2, 104);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->setColumnWidth(3, 104);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->setColumnWidth(4, 110);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int row = m_table->rowAt(pos.y());
        if (row < 0 || row >= m_partitions.size()) return;
        const QString name = m_partitions.at(row).name;

        // iOS 26 悬浮毛玻璃菜单（取代原生 QMenu：方角、不透明、不跟壁纸）。
        const bool dark = ThemeManager::instance().isDark();
        const QColor iconColor = dark ? QColor(255, 255, 255, 205) : QColor(20, 24, 30, 205);
        GlassMenu menu(this);
        menu.addItem(QIcon(appIconPixmap(AppIcon::Package, iconColor, 16)),
                     QStringLiteral("读取分区"), [this, name] { readPartition(name); });
        menu.addItem(QIcon(appIconPixmap(AppIcon::Folder, iconColor, 16)),
                     QStringLiteral("写入分区"), [this, name] { writePartition(name); });
        menu.addSeparator();
        menu.addItem(QIcon(appIconPixmap(AppIcon::Bolt, iconColor, 16)),
                     QStringLiteral("擦除分区"), [this, name] { erasePartition(name); });
        menu.execAt(QRect(m_table->viewport()->mapToGlobal(pos), QSize(0, 0)));
    });
    layout->addWidget(m_table, 3);

    // ── 刷写镜像行 ───────────────────────────────────────────────────────
    auto* flashRow = new QHBoxLayout;
    flashRow->setSpacing(8);
    m_imageEdit = new QLineEdit(this);
    m_imageEdit->setPlaceholderText(QStringLiteral("镜像文件（.img / .bin / .mbn）"));
    auto* browseButton = new QPushButton(QStringLiteral("浏览…"), this);
    browseButton->setCursor(Qt::PointingHandCursor);
    connect(browseButton, &QPushButton::clicked, this, [this] { browseImage(); });
    m_targetEdit = new QLineEdit(this);
    m_targetEdit->setPlaceholderText(QStringLiteral("目标分区名"));
    m_targetEdit->setFixedWidth(160);
    m_writeButton = new QPushButton(QStringLiteral("写入"), this);
    m_writeButton->setObjectName(QStringLiteral("primaryButton"));
    m_writeButton->setCursor(Qt::PointingHandCursor);
    connect(m_writeButton, &QPushButton::clicked, this, [this] { writeSelected(); });
    flashRow->addWidget(m_imageEdit, 3);
    flashRow->addWidget(browseButton);
    flashRow->addWidget(m_targetEdit);
    flashRow->addWidget(m_writeButton);
    layout->addLayout(flashRow);

    // ── GPT 操作行 ───────────────────────────────────────────────────────
    auto* gptRow = new QHBoxLayout;
    gptRow->setSpacing(8);
    m_backupGptButton = new QPushButton(QStringLiteral("备份 GPT"), this);
    m_restoreGptButton = new QPushButton(QStringLiteral("恢复 GPT"), this);
    m_rawprogramButton = new QPushButton(QStringLiteral("生成 rawprogram"), this);
    m_backupGptButton->setToolTip(QStringLiteral("把磁盘头原始扇区（真 GPT，24576 字节）导出到文件，可原样写回（需 root）"));
    m_restoreGptButton->setToolTip(QStringLiteral("把 GPT 备份写回磁盘头（先校验 EFI PART 签名，需 root）"));
    m_rawprogramButton->setToolTip(QStringLiteral("按真 GPT 生成 rawprogram.xml（起始扇区 / 扇区数 / LUN 取自分区表）"));
    for (QPushButton* b : { m_backupGptButton, m_restoreGptButton, m_rawprogramButton }) {
        b->setCursor(Qt::PointingHandCursor);
        gptRow->addWidget(b);
    }
    connect(m_backupGptButton, &QPushButton::clicked, this, [this] { backupGpt(); });
    connect(m_restoreGptButton, &QPushButton::clicked, this, [this] { restoreGpt(); });
    connect(m_rawprogramButton, &QPushButton::clicked, this, [this] { generateRawprogram(); });
    gptRow->addStretch();
    layout->addLayout(gptRow);

    // ── 进度 + 日志 ──────────────────────────────────────────────────────
    m_progress = new ToolProgressBar(this);
    m_progressLabel = new QLabel(this);
    m_progressLabel->setObjectName(QStringLiteral("deviceHintLabel"));
    layout->addWidget(m_progress);
    layout->addWidget(m_progressLabel);

    auto* log = new LogView(this);
    registerLogView(log);
    layout->addWidget(log, 2);
}

void VisualFlashPage::onActivated()
{
    setShellStatus(QStringLiteral("-"), QStringLiteral("Fastboot"), QStringLiteral("可视刷写"));
}

void VisualFlashPage::setBusy(bool busy)
{
    m_busy.storeRelease(busy ? 1 : 0);
    for (QPushButton* b : { m_readButton, m_refreshButton, m_writeButton,
                            m_backupGptButton, m_restoreGptButton, m_rawprogramButton })
        b->setEnabled(!busy);
}

// ── 读取分区表 ───────────────────────────────────────────────────────────────
//  ① 首选：adb 读磁盘头 → GptParser，拿到**真 GPT**（起始 LBA / 类型 GUID 齐全）
//  ② adb 不可用（设备在 fastboot / 未 root）→ fastboot getvar all 的
//     partition-size（引导器上报的名字 + 大小，不是 GPT 结构）
//  ③ 再退 adb by-name 名字列表（只有名字）
void VisualFlashPage::readPartTable()
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("读取分区表"));
    setBusy(true);
    m_progress->setValue(0);
    m_progressLabel->setText(QStringLiteral("读取分区表…"));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        tools.ensureToolsExist();

        QVector<PartitionInfo> partitions;
        QString source;
        QString gptError;
        const auto logFn = [guard](const QString& line) {
            if (guard) guard->logInfo(line);
        };

        // ① 真 GPT：dd 磁盘头（需 root）→ GptParser
        const AdbGptReader::Result gpt = AdbGptReader::read(tools, logFn);
        if (gpt.ok) {
            source = QStringLiteral("GPT 原始分区表 %1（%2 字节/扇区）")
                         .arg(gpt.disk)
                         .arg(gpt.sectorSize);
            for (const swt::edl::PartitionInfo& p : gpt.partitions) {
                PartitionInfo info;
                info.name = p.name.isEmpty() ? QStringLiteral("(未命名@%1)").arg(p.firstLba) : p.name;
                info.size = formatBytes(static_cast<qint64>(p.sectorCount()) * gpt.sectorSize);
                info.startLba = QString::number(p.firstLba);
                info.type = partitionTypeOf(info.name);
                info.guid = p.typeGuid;
                partitions.append(info);
            }
        } else {
            gptError = gpt.error;
            if (guard) {
                guard->logWarning(QStringLiteral("[可视刷写] 读不到 GPT 原始分区表: %1").arg(gpt.error));
            }
        }

        // ② fastboot getvar all：只有名字 + 大小（引导器口径）
        if (partitions.isEmpty()) {
            const auto fr = tools.executeFastboot(QStringLiteral("getvar all"), 30000);
            // ⚠️ fastboot 把 getvar 的内容写 stderr，只读 stdout 会一行都拿不到。
            const QString getvarText = fr.output + QLatin1Char('\n') + fr.error;
            if (fr.success && !getvarText.trimmed().isEmpty()) {
                // 真实分区行：`partition-size:boot: 0x4000000`
                static const QRegularExpression re(
                    QStringLiteral("partition-size:\\s*([A-Za-z0-9_\\-\\.]+)\\s*:\\s*(0x[0-9A-Fa-f]+|\\d+)"),
                    QRegularExpression::CaseInsensitiveOption);
                const QStringList lines = getvarText.split(QLatin1Char('\n'));
                for (const QString& raw : lines) {
                    const auto m = re.match(raw.trimmed());
                    if (!m.hasMatch()) continue;
                    PartitionInfo info;
                    info.name = m.captured(1);
                    const QString sizeText = m.captured(2);
                    qint64 bytes = 0;
                    if (sizeText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
                        bytes = sizeText.mid(2).toLongLong(nullptr, 16);
                    else
                        bytes = sizeText.toLongLong();
                    info.size = formatBytes(bytes);
                    info.type = partitionTypeOf(info.name);
                    partitions.append(info);
                }
                if (!partitions.isEmpty()) {
                    source = QStringLiteral("fastboot getvar all（仅名字 + 大小，非 GPT 结构）");
                }
            }
        }

        // ③ adb by-name 名字列表
        if (partitions.isEmpty()) {
            const auto ar = tools.executeAdb(QStringLiteral("shell ls /dev/block/by-name/"), 20000);
            if (ar.success && !ar.output.trimmed().isEmpty()) {
                const QStringList names = ar.output.split(QRegularExpression(QStringLiteral("[\\r\\n\\s]+")));
                for (const QString& name : names) {
                    const QString trimmed = name.trimmed();
                    if (trimmed.isEmpty() || trimmed.size() <= 1) continue;
                    PartitionInfo info;
                    info.name = trimmed;
                    info.size = QStringLiteral("-");
                    info.type = partitionTypeOf(trimmed);
                    partitions.append(info);
                }
                if (!partitions.isEmpty()) {
                    source = QStringLiteral("adb by-name 名字列表（无大小）");
                }
            }
        }

        runOnUi([this, guard, partitions, started, source, gptError] {
            if (!guard) return;
            applyPartitions(partitions);
            const double seconds = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
            if (!partitions.isEmpty()) {
                logSuccess(QStringLiteral("[可视刷写] 找到 %1 个分区（来源：%2）")
                               .arg(partitions.size())
                               .arg(source));
                m_progress->setValue(100);
                m_progressLabel->setText(QStringLiteral("找到 %1 个分区").arg(partitions.size()));
                setShellStatus(QStringLiteral("-"), QStringLiteral("Fastboot"),
                               gptError.isEmpty() ? QStringLiteral("GPT 已读取")
                                                  : QStringLiteral("分区表已读取"));
            } else {
                logWarning(QStringLiteral("[可视刷写] 未找到分区信息"));
                m_progressLabel->setText(QStringLiteral("未找到分区信息"));
            }
            logView()->appendFooter(
                QStringLiteral("完成, 耗时: %1秒").arg(seconds, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

void VisualFlashPage::applyPartitions(const QVector<PartitionInfo>& partitions)
{
    m_partitions = partitions;
    m_table->setRowCount(partitions.size());
    for (int i = 0; i < partitions.size(); ++i) {
        const PartitionInfo& info = partitions.at(i);
        auto* check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        check->setCheckState(Qt::Unchecked);
        m_table->setItem(i, 0, check);
        m_table->setItem(i, 1, new QTableWidgetItem(info.name));
        m_table->setItem(i, 2, new QTableWidgetItem(info.size));
        m_table->setItem(i, 3,
                         new QTableWidgetItem(info.startLba.isEmpty() ? QStringLiteral("-")
                                                                      : info.startLba));
        auto* typeItem = new QTableWidgetItem(info.type);
        if (!info.guid.isEmpty()) typeItem->setToolTip(QStringLiteral("类型 GUID: %1").arg(info.guid));
        m_table->setItem(i, 4, typeItem);
    }
}

// ── 备份 GPT：磁盘头原始扇区（真 GPT 二进制，可原样写回）────────────────────
//  旧实现把 getvar 的文本写进 .bin（不是 GPT），dd 目标还是 /dev/block/by-name/gpt
//  —— 绝大多数机型没有这个节点，GPT 在磁盘头（/dev/block/sda、mmcblk0…）。
//  现在：探测磁盘 → dd 24576 字节 → pull 到**用户选的路径**。没有 root 就不写假 .bin。
void VisualFlashPage::backupGpt()
{
    if (m_busy.loadAcquire() != 0) return;
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("分区备份 / 导出镜像"),
                                              QStringLiteral("edl.backup")))
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存GPT备份"), QStringLiteral("gpt_backup.bin"),
        QStringLiteral("GPT文件|*.bin;*.gpt"));
    if (path.isEmpty()) return;
    if (!kc::MembershipService::consume(QStringLiteral("edl.backup"))) return;

    logView()->appendSection(QStringLiteral("备份 GPT"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("备份 GPT…"));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, path] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        tools.ensureToolsExist();
        const auto logFn = [guard](const QString& line) {
            if (guard) guard->logInfo(line);
        };

        QString error;
        const QString disk = AdbGptReader::detectDisk(tools, logFn, &error);
        QByteArray head;
        const bool rawOk = !disk.isEmpty()
                           && AdbGptReader::readHead(tools, disk, &head, &error, logFn);
        const quint32 sectorSize = rawOk ? AdbGptReader::sniffSectorSize(head) : 0;

        bool fileOk = false;
        QString textPath;
        if (rawOk && sectorSize != 0) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                fileOk = file.write(head) == head.size();
                file.close();
            } else {
                error = QStringLiteral("文件不可写: %1").arg(file.errorString());
            }
        } else {
            if (error.isEmpty()) error = QStringLiteral("磁盘头不是 GPT（缺少 EFI PART 签名）");
            // 没有 root 就**不写假 .bin**：只把设备信息另存 .txt，明确告知原始数据没拿到。
            textPath = path + QStringLiteral(".txt");
            const auto fr = tools.executeFastboot(QStringLiteral("getvar all"), 30000);
            QFile info(textPath);
            if (info.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QString content =
                    QStringLiteral("# KunClient GPT 备份失败（未取到原始数据）\n"
                                   "# 原因: %1\n# 日期: %2\n")
                        .arg(error)
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                const QString getvarText = fr.output + QLatin1Char('\n') + fr.error;
                if (!getvarText.trimmed().isEmpty()) {
                    content += QStringLiteral("\n# fastboot getvar all:\n")
                               + getvarText.trimmed() + QLatin1Char('\n');
                }
                info.write(content.toUtf8());
                info.close();
            } else {
                textPath.clear();
            }
        }

        runOnUi([this, guard, fileOk, error, headSize = head.size(), disk, sectorSize, textPath,
                 started, path] {
            if (!guard) return;
            if (fileOk) {
                logSuccess(QStringLiteral("[可视刷写] GPT 已备份: %1（%2 字节，%3，%4 字节/扇区）")
                               .arg(path)
                               .arg(headSize)
                               .arg(disk)
                               .arg(sectorSize));
                m_progress->setValue(100);
                m_progressLabel->setText(QStringLiteral("GPT 备份完成"));
                setShellStatus(QStringLiteral("-"), QStringLiteral("Fastboot"),
                               QStringLiteral("GPT 已备份"));
            } else {
                logError(QStringLiteral("[可视刷写] GPT 备份失败: %1").arg(error));
                if (!textPath.isEmpty()) {
                    logWarning(QStringLiteral("[可视刷写] 设备信息已另存: %1（不是 GPT 原始数据）")
                                   .arg(textPath));
                }
                m_progressLabel->setText(QStringLiteral("GPT 备份失败"));
            }
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(fileOk ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

// ── 恢复 GPT：校验文件 → push → dd 写回**磁盘头**（危险操作带确认）──────────
//  旧实现 dd 到 /dev/block/by-name/gpt（多数机型不存在）。现在先嗅 EFI PART
//  确认是真 GPT，再按文件大小算扇区数写回探测到的整盘。
void VisualFlashPage::restoreGpt()
{
    if (m_busy.loadAcquire() != 0) return;
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("分区表（GPT）写入 / 修复"),
                                              QStringLiteral("edl.gpt")))
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择GPT备份"), QString(),
        QStringLiteral("GPT文件|*.bin;*.gpt"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error(QStringLiteral("打不开 GPT 文件: %1").arg(file.errorString()));
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    const quint32 sectorSize = AdbGptReader::sniffSectorSize(data);
    if (sectorSize == 0) {
        error(QStringLiteral("不是有效的 GPT 文件：LBA1 缺少 EFI PART 签名"));
        return;
    }
    if (data.size() < static_cast<qsizetype>(sectorSize) * 2
        || data.size() % static_cast<qsizetype>(sectorSize) != 0) {
        error(QStringLiteral("GPT 文件大小非法：%1 字节（需为 %2 的整数倍且至少 2 扇区）")
                  .arg(data.size())
                  .arg(sectorSize));
        return;
    }

    if (ThemedMessageBox::warning(
            this, QStringLiteral("确认"),
            QStringLiteral("即将把 %1（%2 字节，%3 字节/扇区）写回设备磁盘头。\n\n"
                           "分区表写错会导致设备无法启动，确定继续？")
                .arg(QFileInfo(path).fileName())
                .arg(data.size())
                .arg(sectorSize),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    if (!kc::MembershipService::consume(QStringLiteral("edl.gpt"))) return;

    logView()->appendSection(QStringLiteral("恢复 GPT"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("恢复 GPT…"));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, path, data, sectorSize] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        tools.ensureToolsExist();
        const auto logFn = [guard](const QString& line) {
            if (guard) guard->logInfo(line);
        };

        QString err;
        const bool ok = AdbGptReader::writeHead(tools, QString(), data, sectorSize, &err, logFn);

        runOnUi([this, guard, ok, err, started] {
            if (!guard) return;
            if (ok) {
                logSuccess(QStringLiteral("[可视刷写] GPT 恢复完成，建议重启设备"));
                success(QStringLiteral("GPT恢复完成，建议重启设备"));
                m_progress->setValue(100);
                m_progressLabel->setText(QStringLiteral("GPT恢复完成"));
            } else {
                logError(QStringLiteral("[可视刷写] GPT恢复失败: %1").arg(err));
                m_progressLabel->setText(QStringLiteral("GPT恢复失败"));
            }
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

// ── 生成 rawprogram.xml：优先按真 GPT（起始扇区 / 扇区数 / LUN 全准）────────
//  没有 root 读不到 GPT 时退回旧的 getvar `partition:` 骨架（字段全 0，只当模板）。
void VisualFlashPage::generateRawprogram()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存rawprogram.xml"), QStringLiteral("rawprogram0.xml"),
        QStringLiteral("XML文件|*.xml"));
    if (path.isEmpty()) return;

    logView()->appendSection(QStringLiteral("生成 rawprogram.xml"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("生成 rawprogram.xml…"));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, path] {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        tools.ensureToolsExist();
        const auto logFn = [guard](const QString& line) {
            if (guard) guard->logInfo(line);
        };

        QString xml;
        int entries = 0;
        QString source;

        const AdbGptReader::Result gpt = AdbGptReader::read(tools, logFn);
        if (gpt.ok) {
            source = QStringLiteral("真 GPT（%1，%2 字节/扇区）").arg(gpt.disk).arg(gpt.sectorSize);
            xml = QStringLiteral("<?xml version=\"1.0\" ?>\n<data>\n"
                                 "  <!-- Auto-generated by KunClient from real GPT -->\n");
            for (const swt::edl::PartitionInfo& p : gpt.partitions) {
                if (p.name.isEmpty()) continue;
                const quint64 startByte = p.firstLba * gpt.sectorSize;
                const quint64 bytes = p.sectorCount() * gpt.sectorSize;
                xml += QStringLiteral(
                           "  <program SECTOR_SIZE_IN_BYTES=\"%1\" file=\"%2\" label=\"%3\" "
                           "num_partition_sectors=\"%4\" physical_partition_number=\"%5\" "
                           "size_in_bytes=\"%6\" sparse=\"false\" start_byte_hex=\"0x%7\" "
                           "start_sector=\"%8\" />\n")
                           .arg(gpt.sectorSize)
                           .arg(p.name + QStringLiteral(".img"), p.name)
                           .arg(p.sectorCount())
                           .arg(p.lun)
                           .arg(bytes)
                           .arg(QString::number(startByte, 16))
                           .arg(p.firstLba);
                ++entries;
            }
        } else {
            const auto fr = tools.executeFastboot(QStringLiteral("getvar all"), 30000);
            source = QStringLiteral("fastboot getvar all（字段全 0，仅作模板）");
            xml = QStringLiteral("<?xml version=\"1.0\" ?>\n<data>\n"
                                 "  <!-- Auto-generated by KunClient -->\n");
            const QString getvarText = fr.output + QLatin1Char('\n') + fr.error;
            if (fr.success && !getvarText.trimmed().isEmpty()) {
                const QStringList lines = getvarText.split(QLatin1Char('\n'));
                for (const QString& line : lines) {
                    const QString trimmed = line.trimmed();
                    if (!trimmed.startsWith(QStringLiteral("partition:"))) continue;
                    const QString name = trimmed.mid(QStringLiteral("partition:").length()).trimmed();
                    if (name.isEmpty()) continue;
                    xml += QStringLiteral("  <program SECTOR_SIZE_IN_BYTES=\"512\" file=\"%1.img\" "
                                          "label=\"%1\" num_partition_sectors=\"0\" "
                                          "physical_partition_number=\"0\" size_in_bytes=\"0\" "
                                          "sparse=\"false\" start_byte_hex=\"0x0\" start_sector=\"0\" />\n")
                               .arg(name);
                    ++entries;
                }
            }
        }
        xml += QStringLiteral("</data>\n");

        QFile file(path);
        const bool ok = file.open(QIODevice::WriteOnly | QIODevice::Text)
                        && file.write(xml.toUtf8()) >= 0;
        file.close();

        runOnUi([this, guard, ok, path, started, entries, source] {
            if (!guard) return;
            if (ok) {
                logSuccess(QStringLiteral("[可视刷写] rawprogram.xml 已生成: %1（%2 条，来源：%3）")
                               .arg(path)
                               .arg(entries)
                               .arg(source));
                m_progress->setValue(100);
                m_progressLabel->setText(QStringLiteral("rawprogram.xml 已生成"));
            } else {
                logError(QStringLiteral("[可视刷写] 生成rawprogram失败: 文件不可写"));
                m_progressLabel->setText(QStringLiteral("生成失败"));
            }
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

void VisualFlashPage::browseImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"), QString(),
        QStringLiteral("镜像文件|*.img;*.bin;*.mbn"));
    if (!path.isEmpty()) m_imageEdit->setText(path);
}

// ── 写入选中（镜像 + 目标分区）───────────────────────────────────────────────
void VisualFlashPage::writeSelected()
{
    if (m_busy.loadAcquire() != 0) return;
    const QString image = m_imageEdit->text().trimmed();
    const QString partition = m_targetEdit->text().trimmed();
    if (image.isEmpty() || partition.isEmpty()) {
        error(QStringLiteral("请选择镜像和目标分区"));
        return;
    }
    if (!QFileInfo::exists(image)) {
        error(QStringLiteral("镜像文件不存在: %1").arg(image));
        return;
    }

    // 额度扣在 worker 里（阻塞 HTTP 不许上 GUI 线程）。
    logView()->appendSection(QStringLiteral("写入分区"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("正在刷写…（fastboot 不提供字节级进度）"));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, image, partition] {
        ShellStatus::beginTask(QStringLiteral("写入分区"));
        ShellStatus::report(std::nullopt, partition, std::nullopt, std::nullopt, std::nullopt, -1.0);
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        bool ok = false;
        QString err;
        if (!kc::MembershipService::guardAndConsume(QStringLiteral("写入指定分区"),
                                                    QStringLiteral("edl.write"))) {
            err = QStringLiteral("会员额度校验未通过，已取消");
        } else {
            PlatformToolsManager tools;
            if (!tools.ensureToolsExist()) {
                err = QStringLiteral("平台工具不可用，已终止");
            } else {
                const auto r = FastbootSizeFallback::execute(
                    QStringLiteral("flash %1 \"%2\"").arg(partition, image), 600000, QString(),
                    [this](const QString& line) { logInfo(line); });
                ok = r.success;
                err = r.error;
            }
        }

        runOnUi([this, guard, ok, err, partition, started] {
            if (!guard) return;
            if (ok) {
                logSuccess(QStringLiteral("[可视刷写] 写入完成: %1").arg(partition));
                m_progress->setValue(100);
                m_progressLabel->setText(QStringLiteral("进度: 100%"));
            } else {
                logError(QStringLiteral("[可视刷写] 写入失败: %1").arg(err));
                m_progressLabel->setText(QStringLiteral("写入失败"));
            }
            ShellStatus::endTask();
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

// ── 表格右键三操作 ───────────────────────────────────────────────────────────
//  读取分区：旧实现用 `fastboot get_staged`（那是取回 download 暂存缓冲，不是读分区）。
//  现在：adb 在线 → root + dd + pull；fastboot 在线 → 试 fastbootd 的 `fetch`。
void VisualFlashPage::readPartition(const QString& name)
{
    if (m_busy.loadAcquire() != 0) return;
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("分区备份 / 导出镜像"),
                                              QStringLiteral("edl.backup")))
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存分区镜像"), name + QStringLiteral(".img"),
        QStringLiteral("镜像文件|*.img"));
    if (path.isEmpty()) return;
    if (!kc::MembershipService::consume(QStringLiteral("edl.backup"))) return;

    logView()->appendSection(QStringLiteral("读取分区"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("读取分区: %1").arg(name));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, name, path] {
        ShellStatus::beginTask(QStringLiteral("读取分区"));
        ShellStatus::report(std::nullopt, name, std::nullopt, std::nullopt, std::nullopt, -1.0);
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        tools.ensureToolsExist();
        const auto logFn = [guard](const QString& line) {
            if (guard) guard->logInfo(line);
        };

        bool ok = false;
        QString err;
        if (tools.checkAdbDevice()) {
            ok = AdbGptReader::pullPartition(tools, name, path, &err, logFn);
        } else if (tools.checkFastbootDevice()) {
            // fastboot 协议没有通用的「分区 → 主机」命令；只有 fastbootd 的 fetch 可能支持。
            const auto r = tools.executeFastboot(
                QStringLiteral("fetch %1 \"%2\"").arg(name, path), 600000);
            ok = r.success && QFileInfo(path).size() > 0;
            err = ok ? QString()
                     : (r.error.trimmed().isEmpty() ? QStringLiteral("设备不支持 fastboot fetch")
                                                    : r.error.trimmed());
        } else {
            err = QStringLiteral("没有可用的设备连接（adb / fastboot 都不可用）");
        }

        runOnUi([this, guard, ok, err, name, path, started] {
            if (!guard) return;
            if (ok) {
                logSuccess(QStringLiteral("[可视刷写] 读取完成: %1 → %2").arg(name, path));
                m_progress->setValue(100);
            } else {
                logError(QStringLiteral("[可视刷写] 读取失败: %1").arg(err));
            }
            m_progressLabel->setText(ok ? QStringLiteral("读取完成") : QStringLiteral("读取失败"));
            ShellStatus::endTask();
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

void VisualFlashPage::writePartition(const QString& name)
{
    if (m_busy.loadAcquire() != 0) return;
    if (!kc::MembershipService::ensureAllowed(QStringLiteral("写入指定分区"),
                                              QStringLiteral("edl.write")))
        return;
    const QString image = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"), QString(),
        QStringLiteral("镜像文件|*.img;*.bin"));
    if (image.isEmpty()) return;
    if (!kc::MembershipService::consume(QStringLiteral("edl.write"))) return;

    logView()->appendSection(QStringLiteral("写入分区"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("写入分区: %1").arg(name));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, name, image] {
        ShellStatus::beginTask(QStringLiteral("写入分区"));
        ShellStatus::report(std::nullopt, name, std::nullopt, std::nullopt, std::nullopt, -1.0);
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        PlatformToolsManager tools;
        tools.ensureToolsExist();
        const auto r = FastbootSizeFallback::execute(
            QStringLiteral("flash %1 \"%2\"").arg(name, image), 600000, QString(),
            [this](const QString& line) { logInfo(line); });
        runOnUi([this, guard, ok = r.success, err = r.error, name, started] {
            if (!guard) return;
            if (ok) logSuccess(QStringLiteral("[可视刷写] 写入完成: %1").arg(name));
            else logError(QStringLiteral("[可视刷写] 写入失败: %1").arg(err));
            m_progressLabel->setText(ok ? QStringLiteral("写入完成") : QStringLiteral("写入失败"));
            ShellStatus::endTask();
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

void VisualFlashPage::erasePartition(const QString& name)
{
    if (m_busy.loadAcquire() != 0) return;
    logView()->appendSection(QStringLiteral("擦除分区"));
    setBusy(true);
    m_progressLabel->setText(QStringLiteral("擦除分区: %1").arg(name));
    QPointer<VisualFlashPage> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, name] {
        ShellStatus::beginTask(QStringLiteral("擦除分区"));
        ShellStatus::report(std::nullopt, name, std::nullopt, std::nullopt, std::nullopt, -1.0);
        const qint64 started = QDateTime::currentMSecsSinceEpoch();
        bool ok = false;
        QString err;
        if (!kc::MembershipService::guardAndConsume(QStringLiteral("擦除 / 格式化分区"),
                                                    QStringLiteral("edl.erase"))) {
            err = QStringLiteral("会员额度校验未通过，已取消");
        } else {
            PlatformToolsManager tools;
            tools.ensureToolsExist();
            const auto r = tools.executeFastboot(QStringLiteral("erase %1").arg(name), 120000);
            ok = r.success;
            err = r.error;
        }
        runOnUi([this, guard, ok, err, name, started] {
            if (!guard) return;
            if (ok) logSuccess(QStringLiteral("[可视刷写] 擦除完成: %1").arg(name));
            else logError(QStringLiteral("[可视刷写] 擦除失败: %1").arg(err));
            ShellStatus::endTask();
            m_progressLabel->setText(ok ? QStringLiteral("擦除完成") : QStringLiteral("擦除失败"));
            logView()->appendFooter(
                QStringLiteral("%1, 耗时: %2秒")
                    .arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"))
                    .arg((QDateTime::currentMSecsSinceEpoch() - started) / 1000.0, 0, 'f', 2));
            setBusy(false);
        });
    }));
}

} // namespace kc::ui
