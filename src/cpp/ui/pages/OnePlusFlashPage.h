#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  OnePlusFlashPage — 一加（欧加）线刷页
//
//  移植自 tests/src/KunClient.App/Pages/OnePlusFlashPage.{xaml,xaml.cs}。
//  ⚠️ C# 原页**不是** EDL/swt-edl 流程，而是 adb / fastboot shell 流程（核对了
//     .xaml.cs 全文：.ofp/.ops → `adb edl`；.ozip/.zip → 本地解压 → payload.bin
//     或 rawprogram*.xml 或 *.img → fastboot；.bin → fastboot 逐分区）。照此移植：
//   · 本地刷写包选择（.ofp/.ops/.ozip/.zip/.bin）+ 云端 URL 下载
//     （ChunkedDownloader，对应 C# BackgroundDownloader，可取消）；
//   · 一键刷写：按扩展名分流（逻辑同 C# BtnStartFlash_Click / FlashPayloadAsync）：
//       .ofp/.ops  → adb edl "<file>"
//       .ozip/.zip → 本地解压（系统 bsdtar 支持 zip）→ 目录里有 payload.bin →
//                    flashPayload（flash_all.bat 优先，其次 rawprogram*.xml，
//                    最后逐个 *.img）；没有 payload.bin → 逐个 *.img
//       .bin       → flashPayload
//   · 停止（取消下载 / 置取消旗标，逐分区间隙生效）；
//   · 重启 FastbootD（adb reboot fastboot）、格式化数据（fastboot -w）、
//     提取 Payload（adb payload unpack）。
//   ⚠️ 2026-10-20：原「擦除 FRP」按钮已下线 —— FRP 是厂商防盗机制，工具不提供针对它的
//      专项操作（服务端 oneplus.frp 功能项同步删除，旧版本调用会被服务端拒绝）。
//
//  全部长操作走 QThreadPool；忙碌闸门同其它两页。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;

namespace kc {
class CancelToken;
}

namespace kc::ui {

class NotificationBar;
class ToolProgressBar;

class OnePlusFlashPage : public BasePage {
    Q_OBJECT

public:
    explicit OnePlusFlashPage(QWidget* parent = nullptr);
    ~OnePlusFlashPage() override;

private:
    void buildUi();
    void browseLocal();
    void startFlash();
    void stopFlash();
    void rebootFastbootD();
    void formatData();
    void extractPayload();

    // ── 线程池执行体（阻塞实现，禁止在 UI 线程调用）────────────────────────
    void flashCore(const QString& file);
    void runAdbCore(const QString& args, const QString& desc);
    void runFastbootCore(const QString& args, const QString& desc);

    /// C# FlashPayloadAsync 的对应实现：flash_all.bat → rawprogram*.xml → *.img。
    /// 返回是否成功；进度经 runOnUi 回 UI。
    bool flashPayloadCore(const QString& payloadPath);

    void setBusy(bool busy);

    NotificationBar* m_notify = nullptr;
    QLineEdit* m_localFileEdit = nullptr;
    ToolProgressBar* m_progress = nullptr;
    QLabel* m_progressLabel = nullptr;
    QPushButton* m_browseButton = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_fastbootDButton = nullptr;
    QPushButton* m_formatDataButton = nullptr;
    QPushButton* m_extractButton = nullptr;
    QVector<QPushButton*> m_busyButtons;

    QAtomicInt m_flashCancel{0};                    // 逐分区刷写取消旗标
    QAtomicInt m_busy{0};
};

} // namespace kc::ui
