#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  XiaomiEdlPage — FDL 通道（原「小米EDL」，去掉平台前缀；照「FDL 计算端」流程）
//
//  移植自 tests/src/KunClient.App/Pages/XiaomiEdlPage.{xaml,xaml.cs}。
//  C# 原页核心流程：
//   · fastboot getvar unlocked —— yes 直接 oem edl（签名流程全省）；
//   · no → getvar token 取 blob（命令可由 AppSettings::xiaomiEdlReadCommand 配置）
//        → 弹「FDL Auth」窗（FdlAuthDialog）：原 token 只读 + 一键复制，sig 粘贴框，
//          点「写入」才继续（本工具不算签名，sig 只能从别处拿来）；
//        → sig 十六进制/Base64 解析为字节，落临时 bin（🔴 文件里**只有 sig**，不拼 blob
//          —— 2026-09-29 实机：blob+sig 会让设备回 Edl Token Verify Failed）；
//        → fastboot stage <文件>（xiaomiEdlWriteCommand，默认 "stage"）→ 立即删临时文件；
//        → oem edl（xiaomiEdlEdlCommand，默认 "oem edl"）；
//        → 以 9008 端口是否出现为唯一判据（oem edl 成功时设备当场断 USB，
//          fastboot 退出码反而常常非 0，不能当判据）。
//  自有平台（账号密码 + Bearer 令牌）于 2026-10-16 下线；其后的卡密自动求签
//  （FdlApiClient + FdlKamiStore + 页内卡密行）也于 2026-10-17 按用户要求整体删除，
//  签名来源只剩「FDL Auth 窗里手工粘贴 sig」这一条。
//
//  所有 fastboot / 平台请求走 QThreadPool（同步阻塞实现），控件更新过 runOnUi；
//  日志经 LogView 的线程安全 append* 直写。忙碌闸门 + 停止（原子标志）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QString>

class QLabel;
class QPushButton;
class QWidget;

namespace kc::ui {

class NotificationBar;

class XiaomiEdlPage : public BasePage {
    Q_OBJECT

public:
    explicit XiaomiEdlPage(QWidget* parent = nullptr);

    void onActivated() override;

    /// 立即开跑「一键进入 EDL」完整流程（内部有忙碌闸门，重复调用安全）。
    /// 2026-10-16 起本页不再作为独立页面注册，而是被塞进一个弹窗，由 EDL 右下角
    /// 「重启设备 ▾」菜单的 FDL 项触发 —— 打开即调它，实现「点击自动执行命令」。
    void startFlow();

private:
    void buildUi();

    // ── UI 动作（UI 线程）─────────────────────────────────────────────────
    void refreshDevice();
    void runFlow();
    void stopFlow();
    void showResult(bool ok, const QString& title, const QString& sub);
    void setResultVisible(bool visible);
    void setStep(const QString& text);
    void setBusy(bool busy);

    // ── 线程池执行体（阻塞实现，禁止在 UI 线程调用）────────────────────────
    void refreshDeviceCore();
    /// 弹「FDL Auth」窗等用户粘贴 sig（worker 线程调用：内部把开窗搬到 UI 线程并阻塞等结果）。
    /// 返回 false = 用户取消 / 没填出可解析的 sig。
    bool requestSig(const QString& blob, QString* sigOut);
    /// 返回 true = 步骤走到「完成：…」（设备进入 EDL）。
    bool runFlowCore();

    // ── 成员 ──────────────────────────────────────────────────────────────
    NotificationBar* m_notify = nullptr;
    QLabel* m_deviceValue = nullptr;
    QLabel* m_productValue = nullptr;
    QLabel* m_stepLabel = nullptr;
    QWidget* m_resultBox = nullptr;
    QLabel* m_resultIcon = nullptr;
    QLabel* m_resultTitle = nullptr;
    QLabel* m_resultSub = nullptr;

    QPushButton* m_runButton = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QVector<QPushButton*> m_busyButtons;

    QAtomicInt m_busy{0};
    QAtomicInt m_cancel{0};

    // AppSettings 读入的三条命令（读 blob / stage / 进 EDL）。
    QString m_readCmd;
    QString m_writeCmd;
    QString m_edlCmd;
};

} // namespace kc::ui
