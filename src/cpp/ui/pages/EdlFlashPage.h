#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  EdlFlashPage — EDL 刷写页（承载既有 SWT-EDL 界面）
//
//  既有 swt::edl::MainWindow 是一个完整的 QMainWindow（无边框、自带标题栏 /
//  左配置卡 / 右分区表 + 日志 / 底部状态 / 独立 EdlWorker 工作线程）。
//
//  最稳的宿主方式：**把它当成一个普通子控件嵌进来**（setWindowFlags(Qt::Widget)
//  去掉它自己的窗口属性），它内部的一切 —— 连接、读分区表、刷写、备份、擦除、
//  重启、日志、进度 —— 全部原样可用，不需要动 EDL 引擎一行代码。
//
//  ⚠️ 不采用「把 centralWidget 摘出来重挂」的做法：MainWindow 的顶层是
//     FramelessWindowHint + WA_TranslucentBackground，它的标题栏绑在窗口上做
//     拖动/缩放（nativeEvent 的 WM_NCHITTEST），摘 centralWidget 会让这些
//     窗口级行为失去宿主；整窗当子控件嵌入则这些代码路径全部保留。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

class QLabel;

namespace swt::edl {
class MainWindow;
}

namespace kc::ui {

class EdlFlashPage : public BasePage {
    Q_OBJECT

public:
    explicit EdlFlashPage(QWidget* parent = nullptr);
    ~EdlFlashPage() override;

    /// 暴露内嵌的 EDL 窗口（供外壳需要时取用，如转发主题切换）。
    swt::edl::MainWindow* edlWindow() const { return m_edl; }

signals:
    /// 请求外壳跳转到指定页面（key 见 MainShell::registerPages）。
    /// 注：2026-10-16 前由本页顶部「FDL ›」快捷入口发送；该入口已撤（FDL 改由
    /// 内嵌 EDL 窗右下角重启列表触发完整流程），信号保留但暂无发送方。
    void navigationRequested(const QString& key);

protected:
    /// 第一次显示才建内嵌 EDL 窗（构造要 2s+，见 .cpp 里的说明）。
    void showEvent(QShowEvent* event) override;
    void onActivated() override;

private:
    /// 幂等：建一次内嵌窗并替换掉占位标签。
    void ensureEdlBuilt();

    swt::edl::MainWindow* m_edl = nullptr;
    QWidget* m_edlHost = nullptr;      ///< 内嵌窗的容器（构造期只放占位标签）
    QLabel* m_edlPlaceholder = nullptr;
    bool m_edlBuilt = false;
};

} // namespace kc::ui
