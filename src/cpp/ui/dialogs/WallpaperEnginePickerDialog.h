#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  WallpaperEnginePickerDialog — Wallpaper Engine 动态壁纸选择器。
//
//  移植自 tests/src/KunClient.App/Dialogs/WallpaperEnginePickerDialog.xaml.cs。
//  Wallpaper Engine 把「壁纸」放在 Steam 创意工坊的 431960 目录下，每张壁纸一个
//  文件夹，里面有 project.json 描述类型与资源文件。type=video 的壁纸可以直接取
//  它的 mp4/webm 来播，完全不需要 Wallpaper Engine 在后台跑。
//
//  scene / web 两类是私有格式，无法还原其原始渲染；只拿官方预览素材顶上会发虚，
//  按用户决定：与其给一个糊的假背景，不如根本不出现在列表里（在提示里说明隐藏
//  了多少张）。所以列表**只保留视频类**，扫描跑在 QThreadPool 上。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QDialog>
#include "ui/dialogs/AnimatedDialog.h"
#include <QVector>

#include "kc/tools/WallpaperEngineService.h"

class QLabel;
class QPushButton;
class QTableWidget;

namespace kc::ui {

class WallpaperEnginePickerDialog : public AnimatedDialog {
    Q_OBJECT

public:
    explicit WallpaperEnginePickerDialog(QWidget* parent = nullptr);

    /// 用户选中的素材路径（视频/动图/图片）；取消或选了不可用项时为空。
    QString selectedPath() const { return m_selectedPath; }

    /// 弹出选择器，返回选中素材的绝对路径（取消返回空）。
    static QString pick(QWidget* parent);

private slots:
    void rescan(const QString& extraFolder = QString());

private:
    void buildUi();
    void showResult(const QVector<kc::WallpaperEngineService::WallpaperItem>& list,
                    const QString& extraFolder, int hidden);
    void setHint(const QString& text);
    void confirm();

    QVector<kc::WallpaperEngineService::WallpaperItem> m_items;
    QString m_selectedPath;
    bool m_scanning = false;

    QLabel* m_hint = nullptr;
    QLabel* m_empty = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_pickFolderButton = nullptr;
    QPushButton* m_confirmButton = nullptr;
    QTableWidget* m_table = nullptr;
};

} // namespace kc::ui
