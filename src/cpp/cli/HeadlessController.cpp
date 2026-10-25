#include "cli/HeadlessController.h"

#include "app/EdlWorker.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QMetaObject>
#include <QStringList>
#include <QTextStream>

#include <algorithm>

namespace swt::edl {
namespace {

/// 统一的小端 UTF-8 单行输出。
///
/// 🔴 三件事必须一起做，少一件就会被外部读方坑到：
/// 1. **每行都 flush** —— 用缓冲时父进程会长时间收不到事件，表现为「界面卡住不动」；
/// 2. **显式 UTF-8** —— Windows 控制台默认 GBK，中文分区名会整成乱码**且不报错**；
/// 3. **一行一条** —— 换行只出现在行尾（JSON 字符串里的换行会被转义）。
void writeLine(const QJsonObject& obj)
{
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    QFile out;
    if (out.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        out.write(bytes);
        out.flush();
    }
}

QString levelOf(const QString& message)
{
    // EdlWorker 的日志是纯文本（没有级别字段），这里按常见前缀做一次轻量归类。
    // 认不出来就是 info —— 级别只影响颜色，不能因为归类失败丢日志。
    if (message.contains(QStringLiteral("失败"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("错误"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("error"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("ERROR"))) {
        return QStringLiteral("error");
    }
    if (message.contains(QStringLiteral("警告"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("warn"), Qt::CaseInsensitive)) {
        return QStringLiteral("warn");
    }
    if (message.contains(QStringLiteral("完成"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("成功"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("OK"))) {
        return QStringLiteral("ok");
    }
    return QStringLiteral("info");
}

StorageType storageFromString(const QString& value, StorageType fallback)
{
    const QString v = value.trimmed().toLower();
    if (v == QStringLiteral("emmc")) return StorageType::Emmc;
    if (v == QStringLiteral("ufs")) return StorageType::Ufs;
    return fallback;
}

} // namespace

HeadlessController::HeadlessController(QObject* parent)
    : QObject(parent)
{
}

HeadlessController::~HeadlessController()
{
    stop();
}

QJsonArray HeadlessController::partitionsToJson(const QVector<PartitionInfo>& partitions)
{
    QJsonArray items;
    for (const PartitionInfo& p : partitions) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), p.name);
        o.insert(QStringLiteral("lun"), static_cast<double>(p.lun));
        o.insert(QStringLiteral("firstLba"), static_cast<double>(p.firstLba));
        o.insert(QStringLiteral("lastLba"), static_cast<double>(p.lastLba));
        o.insert(QStringLiteral("sectors"), static_cast<double>(p.sectorCount()));
        o.insert(QStringLiteral("typeGuid"), p.typeGuid);
        o.insert(QStringLiteral("uniqueGuid"), p.uniqueGuid);
        o.insert(QStringLiteral("attributes"), static_cast<double>(p.attributes));
        items.append(o);
    }
    return items;
}

void HeadlessController::emitJson(const QJsonObject& obj)
{
    writeLine(obj);
}

void HeadlessController::errorEvent(const QString& message)
{
    QJsonObject o;
    o.insert(QStringLiteral("event"), QStringLiteral("error"));
    o.insert(QStringLiteral("message"), message);
    emitJson(o);
}

bool HeadlessController::start()
{
    if (m_started) return true;

    // 与 GUI 完全同一套线程模型：worker 移到独立线程，指令靠队列连过去。
    // 差别只在这里没有界面 —— 所以 EdlWorker 那些**阻塞**的槽不会卡住读 stdin 的那条路径。
    m_worker = new EdlWorker;
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    // 事件：worker → stdout
    connect(m_worker, &EdlWorker::logMessage, this, [this](const QString& text) {
        QJsonObject o;
        o.insert(QStringLiteral("event"), QStringLiteral("log"));
        o.insert(QStringLiteral("level"), levelOf(text));
        o.insert(QStringLiteral("text"), text);
        emitJson(o);
    });
    connect(m_worker, &EdlWorker::stateChanged, this,
            [this](SessionState state, const QString& detail) {
                QJsonObject o;
                o.insert(QStringLiteral("event"), QStringLiteral("state"));
                // value 用**稳定机器标识**（stateKey），text 才是给人看的中文。
                // 别把 QVariant::fromValue(state).toString() 当 id 用 —— 对枚举它返回空串。
                o.insert(QStringLiteral("value"), stateKey(state));
                o.insert(QStringLiteral("state"), static_cast<int>(state));
                o.insert(QStringLiteral("text"), stateText(state));
                o.insert(QStringLiteral("detail"), detail);
                emitJson(o);
            });
    connect(m_worker, &EdlWorker::progressChanged, this, [this](quint64 completed, quint64 total) {
        QJsonObject o;
        o.insert(QStringLiteral("event"), QStringLiteral("progress"));
        o.insert(QStringLiteral("completed"), static_cast<double>(completed));
        o.insert(QStringLiteral("total"), static_cast<double>(total));
        emitJson(o);
    });
    connect(m_worker, &EdlWorker::overallProgressChanged, this,
            [this](quint64 completed, quint64 total) {
                QJsonObject o;
                o.insert(QStringLiteral("event"), QStringLiteral("overallProgress"));
                o.insert(QStringLiteral("completed"), static_cast<double>(completed));
                o.insert(QStringLiteral("total"), static_cast<double>(total));
                emitJson(o);
            });
    connect(m_worker, &EdlWorker::deviceInfoChanged, this, [this](const DeviceInfo& info) {
        QJsonObject o;
        o.insert(QStringLiteral("event"), QStringLiteral("device"));
        o.insert(QStringLiteral("port"), info.portName);
        o.insert(QStringLiteral("storage"), info.storage);
        o.insert(QStringLiteral("programmer"), info.programmer);
        o.insert(QStringLiteral("serial"), info.serial);
        o.insert(QStringLiteral("hwid"), info.hwid);
        o.insert(QStringLiteral("pkHash"), info.pkHash);
        o.insert(QStringLiteral("vipMode"), info.vipMode);
        o.insert(QStringLiteral("brand"), info.brand);
        o.insert(QStringLiteral("model"), info.model);
        o.insert(QStringLiteral("msmId"), info.msmId);
        o.insert(QStringLiteral("saharaVersion"), info.saharaVersion);
        o.insert(QStringLiteral("sblVersion"), info.sblVersion);
        o.insert(QStringLiteral("storageProduct"), info.storageProduct);
        o.insert(QStringLiteral("storageVendor"), info.storageVendor);
        o.insert(QStringLiteral("productName"), info.productName);
        o.insert(QStringLiteral("androidVersion"), info.androidVersion);
        o.insert(QStringLiteral("systemVersion"), info.systemVersion);
        o.insert(QStringLiteral("securityPatch"), info.securityPatch);
        o.insert(QStringLiteral("buildFingerprint"), info.buildFingerprint);
        o.insert(QStringLiteral("storageBytes"), static_cast<double>(info.storageBytes));
        emitJson(o);
    });
    connect(m_worker, &EdlWorker::partitionsReady, this,
            [this](const QVector<PartitionInfo>& partitions) {
                // 留一份快照：后面的 flash/erase/backup 允许只给分区名，靠它定位 LUN/LBA。
                m_partitions = partitions;
                QJsonObject o;
                o.insert(QStringLiteral("event"), QStringLiteral("partitions"));
                o.insert(QStringLiteral("count"), partitions.size());
                o.insert(QStringLiteral("items"), partitionsToJson(partitions));
                emitJson(o);
            });
    connect(m_worker, &EdlWorker::operationFinished, this,
            [this](bool success, const QString& message) {
                QJsonObject o;
                o.insert(QStringLiteral("event"), QStringLiteral("finished"));
                o.insert(QStringLiteral("success"), success);
                o.insert(QStringLiteral("message"), message);
                emitJson(o);
            });

    m_workerThread.start();
    m_started = true;

    QJsonObject ready;
    ready.insert(QStringLiteral("event"), QStringLiteral("ready"));
    ready.insert(QStringLiteral("version"), QStringLiteral("0.11.0"));
    ready.insert(QStringLiteral("protocol"), QStringLiteral("ndjson"));
    ready.insert(QStringLiteral("commands"), QJsonArray{
        QStringLiteral("ping"), QStringLiteral("connect"), QStringLiteral("disconnect"),
        QStringLiteral("readPartitions"), QStringLiteral("readVersionInfo"),
        QStringLiteral("backup"), QStringLiteral("flash"), QStringLiteral("flashXml"),
        QStringLiteral("erase"), QStringLiteral("reboot"), QStringLiteral("cancel"),
        QStringLiteral("quit")});
    emitJson(ready);
    return true;
}

void HeadlessController::stop()
{
    if (m_stopping) return;
    m_stopping = true;
    if (!m_started || m_worker == nullptr) return;

    // 与 GUI 关闭时同一处理：让 worker 自己断开设备再退线程。
    // BlockingQueuedConnection 是必要的 —— QueuedConnection 会在 worker 还在跑长任务时
    // 直接进线程销毁，等于把正在刷写的设备晾在那儿。
    QMetaObject::invokeMethod(m_worker, "disconnectDevice", Qt::BlockingQueuedConnection);
    m_workerThread.quit();
    m_workerThread.wait(5000);
    m_worker = nullptr;
    m_started = false;
}

bool HeadlessController::resolvePartition(const QString& name, int lun,
                                          PartitionInfo* out, QString* error) const
{
    if (m_partitions.isEmpty()) {
        *error = QStringLiteral("还没有分区表：请先 connect 再 readPartitions");
        return false;
    }
    for (const PartitionInfo& p : m_partitions) {
        if (p.name.compare(name, Qt::CaseInsensitive) != 0) continue;
        if (lun >= 0 && static_cast<int>(p.lun) != lun) continue;
        *out = p;
        return true;
    }
    *error = QStringLiteral("分区表里找不到分区 '%1'%2（可先 readPartitions 看清单）")
                 .arg(name, lun >= 0 ? QStringLiteral("（LUN %1）").arg(lun) : QString());
    return false;
}

void HeadlessController::fail(const QString& message)
{
    errorEvent(message);
}

void HeadlessController::handleLine(const QString& line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(QStringLiteral("指令不是合法 JSON 对象：%1（偏移 %2）")
                 .arg(parseError.errorString())
                 .arg(parseError.offset));
        return;
    }

    const QJsonObject cmd = doc.object();
    const QString name = cmd.value(QStringLiteral("cmd")).toString();
    if (name.isEmpty()) {
        fail(QStringLiteral("指令缺少 \"cmd\" 字段"));
        return;
    }

    if (name == QStringLiteral("ping")) {
        QJsonObject o;
        o.insert(QStringLiteral("event"), QStringLiteral("pong"));
        o.insert(QStringLiteral("seq"), cmd.value(QStringLiteral("seq")));
        emitJson(o);
        return;
    }
    if (name == QStringLiteral("quit")) {
        emitJson(QJsonObject{{QStringLiteral("event"), QStringLiteral("bye")}});
        stop();
        QCoreApplication::quit();
        return;
    }
    if (name == QStringLiteral("cancel")) {
        QMetaObject::invokeMethod(m_worker, "requestStop", Qt::QueuedConnection);
        emitJson(QJsonObject{{QStringLiteral("event"), QStringLiteral("cancelRequested")}});
        return;
    }

    if (name == QStringLiteral("connect")) {
        const QString port = cmd.value(QStringLiteral("port")).toString();
        if (port.isEmpty()) { fail(QStringLiteral("connect 需要 \"port\"（如 COM4）")); return; }

        OplusVipPackage vip;
        const QJsonObject vipObj = cmd.value(QStringLiteral("vip")).toObject();
        if (!vipObj.isEmpty()) {
            vip.enabled = vipObj.value(QStringLiteral("enabled")).toBool(false);
            vip.packageDirectory = vipObj.value(QStringLiteral("packageDirectory")).toString();
            vip.programmerPath = vipObj.value(QStringLiteral("programmer")).toString();
            vip.digestPath = vipObj.value(QStringLiteral("digest")).toString();
            vip.signaturePath = vipObj.value(QStringLiteral("signature")).toString();
            for (const QJsonValue& v : vipObj.value(QStringLiteral("programmerCandidates")).toArray()) {
                vip.programmerCandidates.append(v.toString());
            }
        }

        const StorageType storage = storageFromString(
            cmd.value(QStringLiteral("storage")).toString(), StorageType::Ufs);
        const QString programmer = cmd.value(QStringLiteral("programmer")).toString();

        QMetaObject::invokeMethod(m_worker, "connectDevice", Qt::QueuedConnection,
                                  Q_ARG(QString, port), Q_ARG(QString, programmer),
                                  Q_ARG(StorageType, storage), Q_ARG(OplusVipPackage, vip));
        return;
    }
    if (name == QStringLiteral("disconnect")) {
        QMetaObject::invokeMethod(m_worker, "disconnectDevice", Qt::QueuedConnection);
        return;
    }
    if (name == QStringLiteral("readPartitions")) {
        QMetaObject::invokeMethod(m_worker, "readPartitionTable", Qt::QueuedConnection);
        return;
    }
    if (name == QStringLiteral("readVersionInfo")) {
        QMetaObject::invokeMethod(m_worker, "readSystemVersionInfo", Qt::QueuedConnection);
        return;
    }
    if (name == QStringLiteral("reboot")) {
        const bool toEdl = cmd.value(QStringLiteral("toEdl")).toBool(false);
        QMetaObject::invokeMethod(m_worker, "rebootDevice", Qt::QueuedConnection,
                                  Q_ARG(bool, toEdl));
        return;
    }
    if (name == QStringLiteral("flashXml")) {
        QStringList paths;
        for (const QJsonValue& v : cmd.value(QStringLiteral("paths")).toArray()) {
            paths.append(v.toString());
        }
        if (paths.isEmpty()) { fail(QStringLiteral("flashXml 需要非空 \"paths\"")); return; }
        const bool allowProtected = cmd.value(QStringLiteral("allowProtected")).toBool(false);
        QMetaObject::invokeMethod(m_worker, "flashXml", Qt::QueuedConnection,
                                  Q_ARG(QStringList, paths), Q_ARG(bool, allowProtected));
        return;
    }

    // ── 需要先在分区表里定位的指令：flash / erase / backup ──
    const bool isFlash = name == QStringLiteral("flash");
    const bool isErase = name == QStringLiteral("erase");
    const bool isBackup = name == QStringLiteral("backup");
    if (!isFlash && !isErase && !isBackup) {
        fail(QStringLiteral("未知指令 '%1'（ping/connect/disconnect/readPartitions/"
                            "readVersionInfo/backup/flash/flashXml/erase/reboot/cancel/quit）")
                 .arg(name));
        return;
    }

    // 单件（name）与批量（names）两种写法
    QStringList names;
    const QString single = cmd.value(QStringLiteral("name")).toString();
    if (!single.isEmpty()) names.append(single);
    for (const QJsonValue& v : cmd.value(QStringLiteral("names")).toArray()) {
        names.append(v.toString());
    }
    if (names.isEmpty()) {
        fail(QStringLiteral("%1 需要 \"name\" 或 \"names\"").arg(name));
        return;
    }

    const int lun = cmd.contains(QStringLiteral("lun"))
                        ? cmd.value(QStringLiteral("lun")).toInt(-1)
                        : -1;

    QVector<PartitionInfo> targets;
    for (const QString& n : names) {
        PartitionInfo p;
        QString error;
        if (!resolvePartition(n, lun, &p, &error)) { fail(error); return; }
        targets.append(p);
    }

    const bool allowProtected = cmd.value(QStringLiteral("allowProtected")).toBool(false);

    if (isErase) {
        QMetaObject::invokeMethod(m_worker, "erasePartitions", Qt::QueuedConnection,
                                  Q_ARG(QVector<PartitionInfo>, targets),
                                  Q_ARG(bool, allowProtected));
        return;
    }

    if (isBackup) {
        if (targets.size() == 1) {
            const QString output = cmd.value(QStringLiteral("output")).toString();
            if (output.isEmpty()) { fail(QStringLiteral("单分区 backup 需要 \"output\" 路径")); return; }
            QMetaObject::invokeMethod(m_worker, "backupPartition", Qt::QueuedConnection,
                                      Q_ARG(PartitionInfo, targets.first()),
                                      Q_ARG(QString, output));
            return;
        }
        const QString outputDir = cmd.value(QStringLiteral("outputDir")).toString();
        if (outputDir.isEmpty()) { fail(QStringLiteral("多分区 backup 需要 \"outputDir\" 目录")); return; }
        const bool generateXml = cmd.value(QStringLiteral("generateXml")).toBool(false);
        QMetaObject::invokeMethod(m_worker, "backupPartitions", Qt::QueuedConnection,
                                  Q_ARG(QVector<PartitionInfo>, targets),
                                  Q_ARG(QString, outputDir), Q_ARG(bool, generateXml));
        return;
    }

    // flash：单分区给 image，多分区给 images（顺序与 names 对齐）
    if (targets.size() == 1) {
        const QString image = cmd.value(QStringLiteral("image")).toString();
        if (image.isEmpty()) { fail(QStringLiteral("单分区 flash 需要 \"image\" 路径")); return; }
        QMetaObject::invokeMethod(m_worker, "flashPartition", Qt::QueuedConnection,
                                  Q_ARG(PartitionInfo, targets.first()),
                                  Q_ARG(QString, image));
        return;
    }

    QStringList images;
    for (const QJsonValue& v : cmd.value(QStringLiteral("images")).toArray()) {
        images.append(v.toString());
    }
    if (images.size() != targets.size()) {
        fail(QStringLiteral("多分区 flash 的 \"images\" 个数（%1）必须与分区数（%2）一致")
                 .arg(images.size()).arg(targets.size()));
        return;
    }
    QMetaObject::invokeMethod(m_worker, "flashPartitions", Qt::QueuedConnection,
                              Q_ARG(QVector<PartitionInfo>, targets),
                              Q_ARG(QStringList, images), Q_ARG(bool, allowProtected));
}

void HeadlessController::handleEof()
{
    // 父进程没了就不该继续挂着 —— 当 quit 处理（幂等）。
    stop();
    QCoreApplication::quit();
}

} // namespace swt::edl
