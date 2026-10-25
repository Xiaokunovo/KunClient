#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkDeviceSession — 整机编排：BROM 会话 → 引导 DA → XFlash（DA2）会话
//
//  这是 UI 唯一需要打交道的 MTK 对象。一次完整的 DA 引导（bootDa）流程：
//    ① 解析 DA 文件（MtkDaFile）→ 按 hw code 选条目；
//    ② 取 region[1] = DA1 → BROM 的 SEND_DA / JUMP_DA；
//    ③ 🔴 设备跳转后会以 PID 0x2001 **重新枚举**（Windows 下 COM 口号会变）
//       → 轮询发现 DA 口并换掉 transport；
//    ④ 等 0xC0 → SYNC → SETUP_ENVIRONMENT → SETUP_HW_INIT_PARAMS → 读回 SYNC；
//    ⑤ 若 connection agent == "brom"：需要 DRAM 参数（EMI）—— 从 preloader 里抠；
//    ⑥ 取 region[2] = DA2（去掉尾部签名）→ BOOT_TO 引导到 stage2；
//    ⑦ SLA 状态探一下：启用则明确告知"不支持 SLA"而不是静默失败。
//
//  🔴 全程在 worker 线程执行（阻塞 IO）。一个实例一个线程。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

namespace kc::mtk {

class BromSession;
class XflashSession;

class MtkDeviceSession : public QObject {
    Q_OBJECT

public:
    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(qint64 done, qint64 total)>;

    /// 打开 COM 口并完成 BROM 握手 + 读 HW code。失败返回 nullptr。
    static MtkDeviceSession* openBrom(const QString& comPort, QString* error,
                                      CancelFn cancel = CancelFn());

    ~MtkDeviceSession() override;

    BromSession* brom() const { return m_brom; }
    XflashSession* xflash() const { return m_xflash; }
    bool daReady() const;

    uint16_t hwCode() const { return m_hwCode; }
    QString comPort() const { return m_comPort; }

    void setCancelFn(CancelFn fn);
    void setProgressFn(ProgressFn fn);

    /// 从 preloader 镜像里抠出 DRAM 参数（MTK_BLOADER_INFO_v* / MTK_BIN 段）。
    /// 抠不出来返回空（调用方只警告，不判死）。
    static QByteArray extractEmi(const QByteArray& preloader);

    /**
     * 完整引导 DA。
     * @param daPath        MTK_AllInOne_DA.bin / MTK_DA_*.bin
     * @param preloaderPath preloader 分区镜像（可空；仅当设备要求 EMI 时才需要）
     * @param error         失败原因
     * @return 成功返回 true（此后 xflash() 非空）
     */
    bool bootDa(const QString& daPath, const QString& preloaderPath, QString* error);

signals:
    void logLine(const QString& level, const QString& message);
    void progressChanged(qint64 done, qint64 total);

private:
    explicit MtkDeviceSession(QObject* parent = nullptr);

    bool cancelled() const;
    void log(const QString& level, const QString& message);
    bool reopenDaTransport(QString* error);

    BromSession* m_brom = nullptr;
    XflashSession* m_xflash = nullptr;
    bool m_daReady = false;
    CancelFn m_cancel;
    ProgressFn m_progress;
    QString m_comPort;
    uint16_t m_hwCode = 0;
};

} // namespace kc::mtk
