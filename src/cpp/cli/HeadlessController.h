#pragma once

#include "core/Types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QThread>

#include <memory>

namespace swt::edl {

class EdlWorker;

/// headless（`--cli`）模式的控制器：把 `EdlWorker` 的能力暴露成
/// **NDJSON**（每行一个 JSON）的 stdin/stdout 协议，供外部程序（如 KunClient）驱动。
///
/// ## 为什么是「一行一个 JSON」
///
/// 跨进程解析最稳的形态就是行分隔：不依赖读多少字节才算一帧，也不会因为日志里
/// 出现换行就断帧（换行在 JSON 字符串里会被转义成 `\n`，不会真的断行）。
/// **不要**改用多行美化输出 —— 那需要自己实现一个流式 JSON 分帧器，白增风险。
///
/// ## 事件（stdout）
///
/// ```text
/// {"event":"ready","version":"0.11.0"}
/// {"event":"state","value":"ready","text":"EDL 已就绪","detail":""}
/// {"event":"log","level":"info","text":"…"}
/// {"event":"progress","completed":123,"total":456}
/// {"event":"overallProgress","completed":1,"total":2}
/// {"event":"device","port":"COM4","storage":"ufs","serial":"…"}
/// {"event":"partitions","items":[{"name":"super","lun":0,"firstLba":…,"lastLba":…,"sectors":…}]}
/// {"event":"finished","success":true,"message":"…"}
/// {"event":"error","message":"…"}          ← 只表示「这条指令本身没被接受」，业务失败走 finished
/// {"event":"pong","seq":1}
/// ```
///
/// ## 指令（stdin，一行一条）
///
/// ```text
/// {"cmd":"ping","seq":1}
/// {"cmd":"connect","port":"COM4","programmer":"…","storage":"ufs",
///  "vip":{"enabled":true,"packageDirectory":"…","programmer":"…","digest":"…","signature":"…"}}
/// {"cmd":"disconnect"}
/// {"cmd":"readPartitions"}
/// {"cmd":"readVersionInfo"}
/// {"cmd":"backup","name":"super","lun":0,"output":"…"}      / {"cmd":"backup","names":[…],"outputDir":"…","generateXml":true}
/// {"cmd":"flashXml","paths":["…\\rawprogram0.xml"],"allowProtected":false}
/// {"cmd":"flash","name":"boot","image":"…"}                  / {"cmd":"flash","names":[…],"images":[…],"allowProtected":false}
/// {"cmd":"erase","name":"userdata"}                          / {"cmd":"erase","names":[…],"allowProtected":false}
/// {"cmd":"reboot","toEdl":false}
/// {"cmd":"cancel"}
/// {"cmd":"quit"}
/// ```
///
/// ## 退出码
///
/// 只表达「CLI 自身」是否正常：0 = 正常收尾，2 = 参数/启动问题。
/// **刷机失败不是 CLI 崩溃**，一律走 `{"event":"finished","success":false}`。
class HeadlessController final : public QObject {
    Q_OBJECT

public:
    explicit HeadlessController(QObject* parent = nullptr);
    ~HeadlessController() override;

    /// 起 worker 线程并打印 ready 事件。
    bool start();

    /// 收尾：让 worker 断开设备（`BlockingQueuedConnection`，等价于 GUI 关闭时的处理），
    /// 再停线程。**幂等**。
    void stop();

public slots:
    /// 收到一行指令（由 StdinReader 跨线程投递过来）。
    void handleLine(const QString& line);

    /// stdin 关闭（EOF）：当作 quit 处理 —— 父进程没了就不该继续挂着。
    void handleEof();

private:
    void emitJson(const QJsonObject& obj);
    void fail(const QString& message);
    void errorEvent(const QString& message);

    /// 把 `QVector<PartitionInfo>` 转成事件里的 items 数组。
    static QJsonArray partitionsToJson(const QVector<PartitionInfo>& partitions);

    /// 按名字/lun 在当前分区表里找一项；找不到给出人话错误。
    bool resolvePartition(const QString& name, int lun, PartitionInfo* out, QString* error) const;

    QThread m_workerThread;
    EdlWorker* m_worker = nullptr;
    QVector<PartitionInfo> m_partitions;   // 最近一次 partitionsReady 的快照（供按名字定位）
    bool m_started = false;
    bool m_stopping = false;
};

} // namespace swt::edl
