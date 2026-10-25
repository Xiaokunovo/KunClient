#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ScreenMirrorPage — 屏幕镜像（scrcpy 投屏）
//
//  移植自 tests/src/KunClient.App/Pages/ScreenMirrorPage.{xaml,xaml.cs}。
//  C# 原页核心（此处保留）：
//   · scrcpy 就绪：缓存目录 %LOCALAPPDATA%\KunClientState\cache\scrcpy 递归探测
//     scrcpy.exe；缺失时从服务器下载 scrcpy.zip（二进制端点 /api/packages/scrcpy/file
//     → base64 回落 /api/packages/scrcpy），解压后再探测（跨会话保留）；
//   · 启动：DETACHED 进程（scrcpy 开自己的窗口，不 waitForFinished）；
//     参数：--max-size / --video-bit-rate / --max-fps / --audio-codec=aac 或
//     --no-audio / --stay-awake / --crop / --record / --fullscreen / --otg /
//     --turn-screen-off / -s IP:5555（scrcpy 2.0 起不接受位置参数 IP:端口）；
//   · 🔴 scrcpy 认 ADB 环境变量找 adb（服务端 scrcpy.zip 已剔除重复的 adb.exe），
//     启动前把 ADB 指到 platform-tools；
//   · 停止：taskkill /PID <pid> /T /F 杀整棵进程树；
//   · 裁剪：`shell wm size` + `shell dumpsys window` 的 stable insets 计算 --crop；
//   · 无线：`adb connect IP:5555`；快捷：截图（screencap+pull）/ 录屏 / 全屏 /
//     息屏（keyevent 26）/ 返回(4) / Home(3) / 最近任务(187)。
//  C# 的 MembershipService 额度守卫、DeviceStateHub/ShellStatus 左栏同步与
//  LogLineQueue 批量投递不移植（Qt 端 LogView 自带线程安全批量渲染）。
//
//  adb / 下载等阻塞操作走 QThreadPool，控件更新过 runOnUi。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace kc {
class PlatformToolsManager;
}

namespace kc::ui {

class NotificationBar;

/// 启动时的一次性设置快照（UI 线程读好，worker 只用值）。
struct MirrorSettings {
    QString mode;         // "usb" / "wifi"
    QString resolution;   // "原始分辨率" / "1920x1080" / …
    QString bitrate;      // "8M" / …
    QString fps;          // "60" / …
    bool audio = true;
    bool crop = false;
    bool stayAwake = true;
    bool otg = false;
    QString ip;
};

class ScreenMirrorPage : public BasePage {
    Q_OBJECT

public:
    explicit ScreenMirrorPage(QWidget* parent = nullptr);

    void onActivated() override;
    void onDeactivated() override;   // C# Unloaded → StopScrcpy

private:
    void buildUi();
    void ensureScrcpy();                       // UI 线程触发
    void ensureScrcpyCore();                   // 线程池
    void startScrcpy(const QString& mode, const QString& recordPath, bool fullscreen);
    void startScrcpyCore(const MirrorSettings& s, const QString& recordPath, bool fullscreen);
    void stopScrcpy();
    void stopScrcpyCore(qint64 pid);
    void connectWifi();
    void screenshot();
    void runKeyevent(int keycode, const QString& desc);

    // 线程池内使用的纯函数 / adb 组合。
    QString buildCropArgument(kc::PlatformToolsManager& tools);      // 线程池

    void setBusy(bool busy);

    NotificationBar* m_notify = nullptr;
    QComboBox* m_resolution = nullptr;
    QComboBox* m_bitrate = nullptr;
    QComboBox* m_fps = nullptr;
    QCheckBox* m_audio = nullptr;
    QCheckBox* m_crop = nullptr;
    QCheckBox* m_stayAwake = nullptr;
    QCheckBox* m_otg = nullptr;
    QLineEdit* m_deviceIp = nullptr;
    QLabel* m_stateLabel = nullptr;
    QLabel* m_scrcpyLabel = nullptr;
    QPushButton* m_downloadButton = nullptr;

    QVector<QPushButton*> m_busyButtons;       // 忙碌时禁用（停止投屏不在其中）
    QAtomicInt m_busy{0};
    QAtomicInt m_mirroring{0};
    bool m_isDownloading = false;              // 仅 UI 线程读写
    QString m_lastMode = QStringLiteral("usb");
    QString m_scrcpyPath;                      // UI 线程读写；worker 只读
    qint64 m_pid = 0;                          // 仅 UI 线程读写
};

} // namespace kc::ui
