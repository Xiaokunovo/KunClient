#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkWorker — MTK 会话的**专用线程宿主**
//
//  为什么需要它（2026-10-13 架构结论）：
//   · QSerialPort 有线程亲和性 → 会话不能跨 QThreadPool 线程复用；
//   · 但 DA 引导（SEND_DA/JUMP_DA/BOOT_TO，几分钟量级）**绝不能每分区重做一次**。
//  两者一起逼出唯一正解：**一个长驻工作线程持有一个已引导的会话**，
//  UI 只往它投递作业（queued slot），取消走原子标志。
//
//  线程契约：
//   · 本对象 moveToThread(专用 QThread)；所有 slot 都在该线程执行；
//   · requestCancel() 是唯一允许从别的线程直接调的方法（原子标志）；
//   · 会话在 worker 线程创建、使用、销毁（closeSession 必须在同线程调）。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"

#include <QAtomicInt>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

namespace kc::mtk {

class MtkDeviceSession;

/// 一个刷写 / 读回作业（一个分区）。
struct MtkJob {
    int row = -1;             // 表格行号（回填状态用；-1 = 无 UI 上下文）
    QString name;             // 分区名
    QString filePath;         // 写：源镜像；读：目标文件（由页面算好）
    quint64 addr = 0;         // 存储偏移（scatter 的 physical_start_addr）
    quint64 size = 0;         // 写：分区容量（须容纳 512 补齐）；读/擦除：操作字节数
    QString region;           // EMMC_USER / EMMC_BOOT_1 / UFS_LU0 …
    StorageType storage = StorageType::None;
};

class MtkWorker : public QObject {
    Q_OBJECT

public:
    explicit MtkWorker(QObject* parent = nullptr);
    ~MtkWorker() override;

    /// 线程安全：UI 线程直接调，worker 在长循环里轮询。
    void requestCancel() { m_cancel.storeRelease(1); }

public slots:
    /// 打开 BROM 口 → 握手 → 读 HW code → 引导 DA（daPath 为空则只连不引导）。
    void openAndBoot(const QString& comPort, const QString& daPath, const QString& preloaderPath);

    /// 引导 DA（若尚未引导）—— 供「连接」与「下载」之间的显式步骤。
    void bootDa(const QString& daPath, const QString& preloaderPath);

    void writeJobs(const QVector<kc::mtk::MtkJob>& jobs);
    void readJobs(const QVector<kc::mtk::MtkJob>& jobs);

    /// 擦除（DA FORMAT）。整分区擦除时 addr/size 取 scatter 里的起始地址与大小；
    /// 与写入一样走 DA 高速通道，**不可回滚** —— 调用方必须先让用户确认。
    void formatJobs(const QVector<kc::mtk::MtkJob>& jobs);

    /// 释放会话（必须在 worker 线程执行）。
    void closeSession();

signals:
    void logLine(const QString& level, const QString& message);
    void progressChanged(qint64 done, qint64 total);
    void rowStatus(int row, const QString& text);
    void connectionResult(bool ok, uint16_t hwCode, const QString& comPort, const QString& message);
    void daResult(bool ok, const QString& message);
    void jobsFinished(bool ok, const QString& message);

private:
    bool cancelled() const { return m_cancel.loadAcquire() != 0; }
    void resetCancel() { m_cancel.storeRelease(0); }

    /// 确保会话 + DA 已就绪（否则按已记住的路径引导一次）。
    bool ensureReady(QString* error);
    void releaseFailedSession();

    MtkDeviceSession* m_session = nullptr;
    QString m_comPort;
    QString m_daPath;
    QString m_preloaderPath;
    bool m_daReady = false;
    QAtomicInt m_cancel{0};
};

} // namespace kc::mtk

Q_DECLARE_METATYPE(kc::mtk::MtkJob)
Q_DECLARE_METATYPE(QVector<kc::mtk::MtkJob>)
