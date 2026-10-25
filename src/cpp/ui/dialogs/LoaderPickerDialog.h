#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  LoaderPickerDialog — 引导程序选择器（本地文件）。
//
//  移植自 tests/src/KunClient.App/Dialogs/LoaderPickerDialog.xaml.cs。
//
//  为什么不直接用 QFileDialog：真实的高通 firehose 引导文件**没有扩展名**
//  （firehose/mi/865、firehose/oppo/450 …），文件过滤器没有「无扩展名」这种
//  通配符，只能靠 *.* 兜底 —— 整个目录里上百个无关文件全糊在列表里。
//  这里改成自己递归扫目录 + 扩展名黑名单：无扩展名的文件反而是重点保留对象，
//  .txt/.json/.7z/.png 这类一律先藏起来，并给出「已隐藏 N 个」的提示。
//
//  扫描跑在 QThreadPool（大目录扫描不能卡 UI）；过滤纯内存操作在 UI 线程做。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QVector>
#include <QDialog>
#include "ui/dialogs/AnimatedDialog.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace kc::ui {

class LoaderPickerDialog : public AnimatedDialog {
    Q_OBJECT

public:
    /// 清单中的一行（对应一个候选文件）。
    struct LoaderCandidate {
        QString fullPath;
        QString relative;
        QString name;
        qint64 size = 0;
        QString ext;
        bool hidden = false;   // 命中黑名单（默认不显示）

        QString kindText() const;   // 无扩展名 = 「引导程序」，其余大写扩展名
    };

    explicit LoaderPickerDialog(QWidget* parent = nullptr,
                                const QString& current = QString());

    /// 已选中的完整路径；用户点「取消」时为空。
    QString selectedPath() const { return m_selectedPath; }

    /// 弹窗选文件；用户取消返回空串。
    static QString pick(QWidget* parent, const QString& current = QString());

    /// C# LoaderPickerDialog.FormatSize（CloudLoaderPickerDialog 也用它）。
    static QString formatSize(qint64 bytes);

private slots:
    void onSearchChanged();
    void onShowAllChanged();
    void onChangeRoot();
    void onBrowseAny();
    void confirm();

private:
    void buildUi();
    void startScan();          // QThreadPool 上跑 scanDirectory，回 UI 填表
    void applyFilter();
    void preselectIfPossible();
    void setHint(const QString& text);

    static QVector<LoaderCandidate> scanDirectory(const QString& root);
    static QString resolveInitialRoot(const QString& current);

    QVector<LoaderCandidate> m_all;
    QString m_root;
    QString m_preselect;
    QString m_selectedPath;
    bool m_scanning = false;

    QLineEdit* m_search = nullptr;
    QCheckBox* m_showAll = nullptr;
    QLabel* m_rootLabel = nullptr;
    QLabel* m_filterInfo = nullptr;
    QLabel* m_subtitle = nullptr;
    QLabel* m_hint = nullptr;
    QPushButton* m_changeRootButton = nullptr;
    QPushButton* m_browseAnyButton = nullptr;
    QPushButton* m_confirmButton = nullptr;
    QTableWidget* m_table = nullptr;
};

} // namespace kc::ui
