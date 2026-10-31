#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FlashHistory — 刷机历史落盘（2026-10-29，「刷机历史」页的数据源）
//
//  为什么单独存一份：LogView 里的日志记的是「过程」，用户真正要回看的是「结果」
//  ——哪次刷机、什么包、成没成、花了多久。这类记录要跨会话存活、要能导出给
//  客服/群友看，所以按行落到状态目录：
//      %LOCALAPPDATA%\KunClientState\flash-history.jsonl
//  用 JSONL 而不是直接存 CSV：一行一条、追加只写尾巴不重写全表、坏一行不影响
//  其它行（日志里混进半行也不会把历史整表带崩）。CSV 只在导出时由
//  recordsToCsv() 现算 —— 它是纯函数，不碰磁盘，单测可以直接钉格式。
//
//  容量：内存与文件都只留最近 MaxRecords 条。刷机记录再多也没人翻到第 501 条，
//  而「超限重写」要读全量，必须有个硬上限兜住磁盘与内存。
//
//  线程安全：QMutex 保护内部状态；实际调用点在 UI 线程（worker 的完成信号回到
//  UI 后才 append），与 SessionLog 的口径一致。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QVector>

namespace kc::diag {

struct FlashRecord {
    QDateTime time;
    QString operation;   // 操作类型：写入 / 擦除 / 读取 / XML 刷机 / 整盘擦除 / GPT 写入 / GPT 导出 / 恢复出厂 / 基带备份
    QString device;      // 设备摘要，如 "COM7 · 小米 23127PN0CC · 0x1234abcd"
    QString package;     // 来源包：XML 文件名 / 镜像名 / 输出目录，可为空
    QString partitions;  // 分区摘要，如 "boot, system, vendor 等 3 个" 或 "69 个分区"，可为空
    bool success = false;
    QString message;     // worker 的原始完成消息（用于排查）
    qint64 durationMs = 0;
};

/// 序列化一行（JSONL，不含换行符）。字段顺序固定，便于人工看。
QString formatRecordLine(const FlashRecord& record);
/// 解析一行；格式不合法返回 false 且不改 *out。
bool parseRecordLine(const QByteArray& line, FlashRecord* out);
/// 整表转 CSV（含表头）。时间列用 yyyy-MM-dd HH:mm:ss；字段内含逗号/引号/换行时按 RFC4180 加引号。
QString recordsToCsv(const QVector<FlashRecord>& records);

class FlashHistory {
public:
    static FlashHistory& instance();

    FlashHistory(const FlashHistory&) = delete;
    FlashHistory& operator=(const FlashHistory&) = delete;

    /// 落盘路径（目录不存在时会建）。
    QString filePath() const;
    /// 全部记录，**最新的在前**。
    QVector<FlashRecord> records() const;
    /// 追加一条：写进内存表（最新的在前）并同步追加到文件。超过 MaxRecords 时丢最旧的（文件也重写）。
    bool append(const FlashRecord& record);
    /// 清空内存 + 删除文件。
    void clear();

    /// 仅单测：把目录指到临时目录。
    static void setDirForTest(const QString& dir);

    static constexpr int MaxRecords = 500;

private:
    FlashHistory() = default;

    /// 首次访问时把文件读进内存（最新的在前）。调用方必须已持锁。
    void ensureLoadedLocked() const;
    /// 用内存表整文件重写（QSaveFile 原子替换）。调用方必须已持锁。
    bool rewriteAllLocked() const;

    mutable QMutex m_mutex;
    mutable QVector<FlashRecord> m_records;   // 最新的在前
    mutable bool m_loaded = false;
    static QString s_dirOverride;             // 仅测试用
};

} // namespace kc::diag
