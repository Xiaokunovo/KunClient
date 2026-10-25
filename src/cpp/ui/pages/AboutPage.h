#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AboutPage — 关于页
//
//  移植自 tests/src/KunClient.App/Pages/AboutPage.{xaml,xaml.cs}。
//  C# 原页是纯静态信息页（版本 / 合规承诺 / 功能范围 / 联系方式 / 鸣谢 /
//  反馈人员 / 收费 / 免责声明 / 许可证），这里保留核心部分：
//   · logo + KunClient 名称与版本（kc::kVersionString）；
//   · EDL 引擎版本（kc::kEdlEngineVersion）；
//   · 官网链接（https://kunawa.cn）与版权行；
//   · 鸣谢（kCreditList）与反馈人员（kFeedbackList）两张名片 —— 与 C# 参照页
//     同名单，两块刻意分开（贡献形式不同：代码/资源 vs 缺陷上报）；
//   · 免责声明摘要；
//   · 检查更新 —— kc::UpdateService::checkForUpdate 在线程池异步执行，
//     结果经 BasePage::runOnUi 回 UI 线程，走 NotificationBar 反馈。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QPointer>

class QFrame;
class QLabel;
class QPushButton;
class QTimer;

namespace kc::ui {

class NotificationBar;

class AboutPage : public BasePage {
    Q_OBJECT

public:
    explicit AboutPage(QWidget* parent = nullptr);

protected:
    /// 彩蛋：监听「版本:」那一行的 QLabel 左键点击（见 triggerVersionEgg）。
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    QFrame* makeCard(const QString& title);

    /// 彩蛋：版本号连点 7 次（相邻间隔 < 800ms）→ Toast + 版本号样式脉冲。
    void triggerVersionEgg();

    NotificationBar* m_notify = nullptr;
    QLabel* m_versionLabel = nullptr;

    // ── 彩蛋：连点版本号 7 次 ────────────────────────────────────────────
    //  刻意做成「有意的」而不是「疑似 bug」：维护者看到这几个成员就知道
    //  版本号能点是有设计的（触发方式见 AboutPage.cpp 的 eventFilter）。
    int m_versionClickCount = 0;             ///< 已连点次数（超时 / 触发后清零）
    qint64 m_versionLastClickMs = 0;         ///< 上一次点击的毫秒时间戳
    QTimer* m_versionPulseTimer = nullptr;   ///< 样式脉冲的阶段推进（不碰 geometry）
    int m_versionPulseStage = 0;             ///< 0 = 未在脉冲；1/2 = 脉冲的两个阶段
};

} // namespace kc::ui
