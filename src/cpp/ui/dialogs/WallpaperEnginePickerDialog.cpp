#include "ui/dialogs/WallpaperEnginePickerDialog.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace kc::ui {
namespace {

class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

} // namespace

WallpaperEnginePickerDialog::WallpaperEnginePickerDialog(QWidget* parent)
    : AnimatedDialog(parent)
{
    setWindowTitle(QStringLiteral("Wallpaper Engine 壁纸"));
    setObjectName(QStringLiteral("wallpaperEnginePickerDialog"));
    setModal(true);
    resize(720, 520);
    buildUi();
    rescan();
}

void WallpaperEnginePickerDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(8);

    auto* toolRow = new QHBoxLayout;
    toolRow->setSpacing(8);
    m_refreshButton = new QPushButton(QStringLiteral("重新扫描"), this);
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_pickFolderButton = new QPushButton(QStringLiteral("手动指定文件夹…"), this);
    m_pickFolderButton->setToolTip(
        QStringLiteral("选择 Wallpaper Engine 壁纸目录（创意工坊 431960，"
                       "或 projects/myprojects）"));
    toolRow->addWidget(m_refreshButton);
    toolRow->addWidget(m_pickFolderButton);
    toolRow->addStretch();
    layout->addLayout(toolRow);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("标题"), QStringLiteral("类型") });
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    layout->addWidget(m_table, 1);

    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("dialogHintLabel"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    layout->addWidget(m_empty, 1);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("dialogHintLabel"));
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);
    buttonRow->addStretch();
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    m_confirmButton = new QPushButton(QStringLiteral("使用"), this);
    m_confirmButton->setObjectName(QStringLiteral("primaryButton"));
    m_confirmButton->setDefault(true);
    m_confirmButton->setEnabled(false);
    buttonRow->addWidget(cancel);
    buttonRow->addWidget(m_confirmButton);
    layout->addLayout(buttonRow);

    connect(m_refreshButton, &QPushButton::clicked, this,
            [this] { rescan(); });
    connect(m_pickFolderButton, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("选择 Wallpaper Engine 壁纸目录"
                           "（创意工坊 431960，或 projects\\myprojects）"),
            QString());
        if (!folder.isEmpty()) rescan(folder);
    });
    connect(m_confirmButton, &QPushButton::clicked, this, &WallpaperEnginePickerDialog::confirm);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { confirm(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_table->currentRow();
        const bool ok = row >= 0 && row < m_items.size()
                        && m_items.at(row).usable();
        m_confirmButton->setEnabled(ok && !m_scanning);
        if (row < 0 || row >= m_items.size()) {
            m_hint->setText(QStringLiteral("双击一行即可直接使用"));
            return;
        }
        const auto& item = m_items.at(row);
        m_hint->setText(item.usable()
            ? QStringLiteral("将使用：%1（%2）").arg(item.sourcePath, item.typeText())
            : QStringLiteral("%1 —— 该壁纸目录里没有可直接使用的素材，"
                             "请在 Wallpaper Engine 中导出后再用「选择文件」载入")
                  .arg(item.typeText()));
    });
}

void WallpaperEnginePickerDialog::setHint(const QString& text)
{
    m_hint->setText(text);
}

void WallpaperEnginePickerDialog::rescan(const QString& extraFolder)
{
    if (m_scanning) return;
    m_scanning = true;
    setHint(QStringLiteral("正在扫描本机的 Wallpaper Engine 壁纸…"));
    m_confirmButton->setEnabled(false);
    m_items.clear();
    m_table->clearContents();
    m_table->setRowCount(0);

    QPointer<WallpaperEnginePickerDialog> guard(this);
    QThreadPool::globalInstance()->start(new LambdaTask([this, guard, extraFolder] {
        QVector<kc::WallpaperEngineService::WallpaperItem> found;
        try {
            found += kc::WallpaperEngineService::discover();
            if (!extraFolder.trimmed().isEmpty())
                found += kc::WallpaperEngineService::discoverIn(extraFolder);
        } catch (...) {
        }

        // 去重（discover 与 discoverIn 可能命中同一张壁纸）。
        QVector<kc::WallpaperEngineService::WallpaperItem> deduped;
        QStringList seen;
        for (const auto& it : found) {
            const QString key = it.sourcePath.isEmpty() ? it.folder : it.sourcePath;
            if (!key.isEmpty() && !seen.contains(key, Qt::CaseInsensitive)) {
                seen.append(key);
                deduped.append(it);
            }
        }
        std::sort(deduped.begin(), deduped.end(),
                  [](const kc::WallpaperEngineService::WallpaperItem& a,
                     const kc::WallpaperEngineService::WallpaperItem& b) {
                      const int byUsable = (b.usable() ? 1 : 0) - (a.usable() ? 1 : 0);
                      return byUsable != 0 ? byUsable > 0
                                           : a.title.compare(b.title, Qt::CaseInsensitive) < 0;
                  });

        // 只保留**本工具能真正播放**的壁纸（视频类）。
        // scene/web 类是 WE 私有格式，本体没有可播的视频文件，只能拿官方预览素材
        // 顶上：铺满窗口后明显发虚。按用户决定：与其给一个糊的假背景，不如根本
        // 不出现在列表里（会在提示里说明隐藏了多少张）。
        QVector<kc::WallpaperEngineService::WallpaperItem> playable;
        int hidden = 0;
        for (const auto& it : deduped) {
            if (it.sourceKind == kc::WallpaperSourceKind::Video)
                playable.append(it);
            else
                ++hidden;
        }

        QMetaObject::invokeMethod(this, [this, guard, playable, extraFolder, hidden] {
            if (!guard) return;
            m_scanning = false;
            showResult(playable, extraFolder, hidden);
        }, Qt::QueuedConnection);
    }));
}

void WallpaperEnginePickerDialog::showResult(
    const QVector<kc::WallpaperEngineService::WallpaperItem>& list,
    const QString& extraFolder, int hidden)
{
    m_items = list;
    m_table->clearContents();
    m_table->setRowCount(list.size());
    for (int i = 0; i < list.size(); ++i) {
        auto* titleItem = new QTableWidgetItem(list.at(i).title);
        titleItem->setToolTip(list.at(i).sourcePath);
        m_table->setItem(i, 0, titleItem);
        m_table->setItem(i, 1, new QTableWidgetItem(list.at(i).typeText()));
    }

    const QString hiddenNote =
        hidden > 0
            ? QStringLiteral("\n・另有 %1 张场景/网页(scene/web)类壁纸已隐藏："
                             "它们是 WE 私有格式，本工具只能拿官方预览素材顶上，"
                             "铺满窗口会发虚。").arg(hidden)
            : QString();

    if (m_items.isEmpty()) {
        m_empty->setVisible(true);
        m_table->setVisible(false);
        m_empty->setText(
            kc::WallpaperEngineService::isInstalled()
                ? QStringLiteral("扫描到 Wallpaper Engine，但这个目录里没有找到可用素材。\n\n"
                                 "・本工具只列**视频类**壁纸（mp4/webm 等，直接播放，"
                                 "清晰度最高）；\n・如果壁纸存在别的磁盘，可以点右上角"
                                 "「手动指定文件夹」直接选它的目录。")
                + hiddenNote
                : QStringLiteral("没有在本机找到 Wallpaper Engine 的壁纸目录。\n\n"
                                 "・如果确实装了，请点右上角「手动指定文件夹」，\n"
                                 "  选择 Steam\\steamapps\\workshop\\content\\431960，\n"
                                 "  或 Wallpaper Engine\\projects\\myprojects；\n"
                                 "・也可以直接用「选择文件」挑一个本地图片/视频当背景。"));
        setHint(QStringLiteral("未找到可用壁纸"));
        return;
    }

    m_empty->setVisible(false);
    m_table->setVisible(true);
    setHint((extraFolder.isEmpty()
                 ? QStringLiteral("共 %1 张可用壁纸（双击一行即可）").arg(m_items.size())
                 : QStringLiteral("已合并手动目录：共 %1 张可用壁纸").arg(m_items.size()))
            + hiddenNote);
}

void WallpaperEnginePickerDialog::confirm()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_items.size()) return;
    const auto& item = m_items.at(row);
    if (!item.usable()) {
        setHint(item.typeText() + QStringLiteral(" —— 不能直接使用"));
        return;
    }
    m_selectedPath = item.sourcePath;
    accept();
}

QString WallpaperEnginePickerDialog::pick(QWidget* parent)
{
    WallpaperEnginePickerDialog dialog(parent);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedPath() : QString();
}

} // namespace kc::ui
