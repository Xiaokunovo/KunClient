#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PackageImageListDialog — 「包内镜像清单」。
//
//  移植自 tests/src/KunClient.App/Dialogs/PackageImageListDialog.xaml.cs。
//  把资料目录里所有 rawprogram*.xml 的 <program> 条目逐个列出来：
//  分区名 / 第几块 / LUN / 起始扇区（表达式原样保留）/ 扇区数 / 镜像 / 状态。
//
//  为什么需要它：主界面的分区表来自**设备** GPT，而 QFIL 等工具展示的是**包内**
//  条目。出厂包会把 super / userdata 拆成多块镜像（共用同一个 label），只按
//  「分区名」看就会以为解析不全。纯只读展示，不改变任何刷写行为；「占位」条目
//  在 rawprogram 里没有镜像文件，按 QFIL 语义只是登记分区，清单里照样列出来
//  只是为了对齐原包。
//
//  C# 版没有多选交互（Pick 为 void、纯展示），这里保持一致：只有
//  「只看会刷写的条目」过滤勾选，不提供 checkbox 多选。解析用
//  swt::edl::RawProgramParser（storage/，与 C# QfilXmlParser 同源），
//  解析在 QThreadPool 上跑，UI 不卡。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <QDialog>
#include "ui/dialogs/AnimatedDialog.h"
#include <QVector>

class QCheckBox;
class QLabel;
class QTableWidget;

namespace kc::ui {

class PackageImageListDialog : public AnimatedDialog {
    Q_OBJECT

public:
    /// 清单中的一行（对应一个 <program> 条目）。
    struct ImageRow {
        QString label;
        QString chunkText;        // "#i/n"，单块条目为空
        quint32 lun = 0;
        QString lunText() const { return QStringLiteral("LUN%1").arg(lun); }
        QString startSectorText;  // 表达式原样（NUM_DISK_SECTORS-5. 等）
        QString numSectorsText;
        QString fileText;
        QString statusText;
        /// 这次刷写是否会真正下发数据（供「只看会刷写的条目」过滤）。
        bool willFlash = false;
    };

    /// 空构造：离屏巡检会直接构造本窗口（无数据时列表为空）。
    explicit PackageImageListDialog(QWidget* parent = nullptr);

    /// 打开清单。excludedLabels 为本次「保数据刷写」排除的分区。
    static void showList(QWidget* parent, const QString& imageDir,
                         const QStringList& excludedLabels = QStringList());

private slots:
    void applyFilter();

private:
    void buildUi();
    void loadPackage(const QString& imageDir, const QStringList& excludedLabels);
    void setEmpty(const QString& text);
    void setStatusColor(int row, const QString& status);

    QVector<ImageRow> m_all;
    QString m_extraSummary;

    QCheckBox* m_onlyFlash = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_empty = nullptr;
    QTableWidget* m_table = nullptr;
};

} // namespace kc::ui
