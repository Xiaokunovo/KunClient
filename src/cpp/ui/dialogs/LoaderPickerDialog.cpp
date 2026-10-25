#include "ui/dialogs/LoaderPickerDialog.h"

#include "kc/config/AppSettings.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QtGlobal>

#include <functional>
#include <stack>

namespace kc::ui {
namespace {

// 这些扩展名一定不是引导程序，默认全部隐藏（与 C# 黑名单逐条一致）。
const char* kUselessExtensions[] = {
    // 文本 / 配置 / 日志
    ".txt", ".md", ".rtf", ".json", ".ini", ".cfg", ".conf", ".log", ".xml", ".csv", ".tsv",
    ".yaml", ".yml", ".properties",
    // 文档
    ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
    // 图片
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".ico", ".svg", ".tif", ".tiff", ".heic",
    // 程序 / 库
    ".dll", ".exe", ".msi", ".sys", ".ocx", ".so", ".dylib", ".pdb", ".lib", ".exp", ".jar",
    ".class",
    // 脚本
    ".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".sh", ".py", ".pyc", ".js", ".mjs", ".ts",
    ".html", ".htm", ".css", ".java", ".kt", ".c", ".h", ".cpp", ".cs", ".go", ".rs", ".php",
    // 压缩包 / 镜像
    ".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz", ".zst", ".iso", ".apk", ".deb", ".rpm",
    // 杂项
    ".lnk", ".url", ".db", ".sqlite", ".sqlite3", ".tmp", ".bak", ".old", ".part", ".crdownload",
};

const char* kUselessFileNames[] = {
    "desktop.ini", "thumbs.db", ".ds_store", ".gitignore", ".gitattributes",
};

constexpr int kMaxFiles = 4000;

bool isUselessExtension(const QString& ext)
{
    if (ext.isEmpty()) return false;
    for (const char* e : kUselessExtensions)
        if (QString::fromLatin1(e).compare(ext, Qt::CaseInsensitive) == 0) return true;
    return false;
}

bool isUselessFileName(const QString& name)
{
    for (const char* n : kUselessFileNames)
        if (QString::fromLatin1(n).compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

/// C# LambdaTask 等价物：QThreadPool 上跑一段函数。
class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

} // namespace

QString LoaderPickerDialog::LoaderCandidate::kindText() const
{
    return ext.isEmpty() ? QStringLiteral("引导程序")
                         : ext.mid(1).toUpper();
}

QString LoaderPickerDialog::formatSize(qint64 bytes)
{
    if (bytes <= 0) return QStringLiteral("-");
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 2);
}

LoaderPickerDialog::LoaderPickerDialog(QWidget* parent, const QString& current)
    : AnimatedDialog(parent)
    , m_preselect(current)
{
    setWindowTitle(QStringLiteral("选择引导程序"));
    setObjectName(QStringLiteral("loaderPickerDialog"));
    setModal(true);
    resize(760, 560);
    buildUi();

    // C# 是 Loaded → 扫描；Qt 里构造完直接开扫（对话框 exec 前事件循环未跑，
    // 扫描结果经排队回调落表，时序一致）。
    startScan();
}

void LoaderPickerDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    // ── 顶部：目录读数 + 更换目录 ───────────────────────────────────────────
    auto* rootRow = new QHBoxLayout;
    rootRow->setSpacing(8);
    m_rootLabel = new QLabel(this);
    m_rootLabel->setObjectName(QStringLiteral("dialogHintLabel"));
    m_changeRootButton = new QPushButton(QStringLiteral("更换目录"), this);
    m_changeRootButton->setCursor(Qt::PointingHandCursor);
    rootRow->addWidget(m_rootLabel, 1);
    rootRow->addWidget(m_changeRootButton);
    layout->addLayout(rootRow);

    // ── 工具行：搜索 + 显示全部 ────────────────────────────────────────────
    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(8);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("搜索：品牌 / 机型（如 mi、865、redmagic）"));
    m_search->setClearButtonEnabled(true);
    m_showAll = new QCheckBox(QStringLiteral("显示全部文件"), this);
    toolRow->addWidget(m_search, 1);
    toolRow->addWidget(m_showAll);
    layout->addLayout(toolRow);

    auto* infoRow = new QHBoxLayout;
    infoRow->setSpacing(8);
    m_filterInfo = new QLabel(this);
    m_filterInfo->setObjectName(QStringLiteral("dialogHintLabel"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("dialogHintLabel"));
    infoRow->addWidget(m_filterInfo, 1);
    infoRow->addWidget(m_subtitle);
    layout->addLayout(infoRow);

    // ── 文件表 ─────────────────────────────────────────────────────────────
    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("文件"), QStringLiteral("类型"),
                                         QStringLiteral("大小"), QStringLiteral("相对路径") });
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    layout->addWidget(m_table, 1);

    // ── 底部：提示 + 兜底出口 + 确定/取消 ───────────────────────────────────
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("dialogHintLabel"));
    layout->addWidget(m_hint);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    m_browseAnyButton = new QPushButton(QStringLiteral("浏览任意文件…"), this);
    m_browseAnyButton->setToolTip(
        QStringLiteral("过滤规则万一误伤时的兜底出口：可以选任意文件"));
    auto* spacer = new QWidget(this);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    m_confirmButton = new QPushButton(QStringLiteral("加载"), this);
    m_confirmButton->setObjectName(QStringLiteral("primaryButton"));
    m_confirmButton->setDefault(true);
    buttonRow->addWidget(m_browseAnyButton);
    buttonRow->addWidget(spacer, 1);
    buttonRow->addWidget(cancel);
    buttonRow->addWidget(m_confirmButton);
    layout->addLayout(buttonRow);

    connect(m_search, &QLineEdit::textChanged, this, &LoaderPickerDialog::onSearchChanged);
    connect(m_showAll, &QCheckBox::toggled, this, &LoaderPickerDialog::onShowAllChanged);
    connect(m_changeRootButton, &QPushButton::clicked, this, &LoaderPickerDialog::onChangeRoot);
    connect(m_browseAnyButton, &QPushButton::clicked, this, &LoaderPickerDialog::onBrowseAny);
    connect(m_confirmButton, &QPushButton::clicked, this, &LoaderPickerDialog::confirm);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { confirm(); });
}

QString LoaderPickerDialog::resolveInitialRoot(const QString& current)
{
    // 初始目录优先级：调用方给的当前文件所在目录 → 上次用过的目录 → 常见
    // firehose 目录（OneToolBox 工具包、程序目录下的 firehose / loaders）→ 下载目录。
    if (!current.trimmed().isEmpty()) {
        const QString dir = QFileInfo(current).absolutePath();
        if (QFileInfo::exists(dir)) return QDir(dir).absolutePath();
    }

    const QString last = AppSettings::current().lastLoaderDir;
    if (!last.trimmed().isEmpty() && QDir(last).exists()) return last;

    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString exeDir = QCoreApplication::applicationDirPath();

    const QStringList candidates = {
        home + QStringLiteral("/Downloads/OneToolBox/tool/edl/firehose"),
        home + QStringLiteral("/Downloads/OneToolBox/tool/edl"),
        exeDir + QStringLiteral("/firehose"),
        exeDir + QStringLiteral("/loaders"),
        home + QStringLiteral("/Downloads"),
    };
    for (const QString& dir : candidates)
        if (QDir(dir).exists()) return dir;

    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

QVector<LoaderPickerDialog::LoaderCandidate> LoaderPickerDialog::scanDirectory(
    const QString& root)
{
    QVector<LoaderCandidate> list;
    if (root.trimmed().isEmpty() || !QDir(root).exists()) return list;

    const QString rootAbs = QDir(root).absolutePath();
    std::stack<QString> stack;
    stack.push(rootAbs);

    while (!stack.empty()) {
        const QDir dir(stack.top());
        stack.pop();

        const QFileInfoList files =
            dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& fi : files) {
            const QString name = fi.fileName();
            if (isUselessFileName(name)) continue;

            const QString ext = fi.suffix().isEmpty()
                                    ? QString()
                                    : QStringLiteral(".") + fi.suffix();

            LoaderCandidate item;
            item.fullPath = QDir::fromNativeSeparators(fi.absoluteFilePath());
            item.relative = dir.relativeFilePath(item.fullPath);
            item.name = name;
            item.size = fi.size();
            item.ext = ext;
            item.hidden = isUselessExtension(ext);
            list.append(item);

            if (list.size() >= kMaxFiles) return list;
        }

        const QFileInfoList subs =
            dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& sub : subs) stack.push(sub.absoluteFilePath());
    }

    return list;
}

void LoaderPickerDialog::startScan()
{
    if (m_scanning) return;
    m_scanning = true;
    m_root = resolveInitialRoot(m_preselect);
    m_rootLabel->setText(m_root);
    m_filterInfo->setText(QStringLiteral("正在扫描…"));
    setHint(QStringLiteral("正在扫描目录…"));
    m_confirmButton->setEnabled(false);

    QPointer<LoaderPickerDialog> guard(this);
    const QString root = m_root;
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, root] {
        const QVector<LoaderCandidate> found = scanDirectory(root);
        QMetaObject::invokeMethod(this, [this, guard, found] {
            if (!guard) return;
            m_scanning = false;
            m_all = found;
            std::sort(m_all.begin(), m_all.end(),
                      [](const LoaderCandidate& a, const LoaderCandidate& b) {
                          return a.relative.compare(b.relative, Qt::CaseInsensitive) < 0;
                      });
            m_confirmButton->setEnabled(true);
            applyFilter();
        }, Qt::QueuedConnection);
    }));
}

void LoaderPickerDialog::applyFilter()
{
    // 填表期间先摘开表格内容再一次性重建：QTableWidget 逐行 setItem 会触发
    // 逐行排版，上千候选文件时是 O(n²)。setRowCount(0) → 一次 setRowCount(n)。
    const bool showAll = m_showAll->isChecked();
    const QString keyword = m_search->text().trimmed();

    QVector<LoaderCandidate> view;
    int hiddenCount = 0;
    for (const LoaderCandidate& item : m_all) {
        if (!showAll && item.hidden) { ++hiddenCount; continue; }
        if (!keyword.isEmpty() && !item.relative.contains(keyword, Qt::CaseInsensitive))
            continue;
        view.append(item);
    }

    m_table->clearContents();
    m_table->setRowCount(view.size());
    for (int i = 0; i < view.size(); ++i) {
        const LoaderCandidate& item = view.at(i);
        auto* nameItem = new QTableWidgetItem(item.name);
        nameItem->setToolTip(item.fullPath);
        m_table->setItem(i, 0, nameItem);
        m_table->setItem(i, 1, new QTableWidgetItem(item.kindText()));
        m_table->setItem(i, 2, new QTableWidgetItem(formatSize(item.size)));
        m_table->setItem(i, 3, new QTableWidgetItem(item.relative));
    }

    const int total = m_all.size();
    m_filterInfo->setText(showAll
        ? QStringLiteral("共 %1 个文件（未过滤）").arg(total)
        : QStringLiteral("可选中 %1 个 · 已隐藏 %2 个无关文件")
              .arg(view.size()).arg(hiddenCount));

    setHint(view.isEmpty()
        ? QStringLiteral("没有可选的引导文件，试试换个目录，或勾选「显示全部文件」")
        : QStringLiteral("双击一行即可加载"));
    m_subtitle->setText(showAll ? QStringLiteral("显示全部文件")
                                : QStringLiteral("已自动滤掉无关文件"));

    preselectIfPossible();
}

void LoaderPickerDialog::preselectIfPossible()
{
    if (m_preselect.trimmed().isEmpty()) return;
    for (int i = 0; i < m_table->rowCount(); ++i) {
        auto* it = m_table->item(i, 0);
        if (it && it->toolTip().compare(m_preselect, Qt::CaseInsensitive) == 0) {
            m_table->selectRow(i);
            m_table->scrollToItem(it);
            return;
        }
    }
}

void LoaderPickerDialog::setHint(const QString& text)
{
    m_hint->setText(text);
}

void LoaderPickerDialog::onSearchChanged() { applyFilter(); }
void LoaderPickerDialog::onShowAllChanged() { applyFilter(); }

void LoaderPickerDialog::onChangeRoot()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择引导文件所在目录"), m_root);
    if (dir.isEmpty()) return;

    m_root = dir;
    m_rootLabel->setText(m_root);
    startScan();
}

void LoaderPickerDialog::onBrowseAny()
{
    // 兜底出口：过滤规则万一误伤，这里可以选任意文件（C# FilterIndex=2 → 所有文件）。
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择引导程序"), m_root,
        QStringLiteral("引导程序 (*.mbn *.bin *.elf *.img);;所有文件 (*)"));
    if (file.isEmpty()) return;
    m_selectedPath = file;
    accept();
}

void LoaderPickerDialog::confirm()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        setHint(QStringLiteral("请先在列表里选中一个文件"));
        return;
    }
    auto* it = m_table->item(row, 0);
    if (!it) return;
    const QString fullPath = it->toolTip();

    // 记住这个目录，下次打开直接落在这里。
    const QString dir = QFileInfo(fullPath).absolutePath();
    if (QDir(dir).exists()) {
        AppSettings::current().lastLoaderDir = dir;
        AppSettings::current().save();
    }

    m_selectedPath = fullPath;
    accept();
}

QString LoaderPickerDialog::pick(QWidget* parent, const QString& current)
{
    LoaderPickerDialog dialog(parent, current);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedPath() : QString();
}

} // namespace kc::ui
