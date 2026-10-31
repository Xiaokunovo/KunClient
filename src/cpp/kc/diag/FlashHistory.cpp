#include "kc/diag/FlashHistory.h"

#include "kc/config/AppStatePaths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace kc::diag {
namespace {

// 落盘文件名（在状态目录下）。改名会影响老用户的既有历史，别动。
constexpr char kFileName[] = "flash-history.jsonl";

// JSON 键名。统一常量，免得 format / parse 两处手滑拼错。
constexpr char kKeyTime[] = "time";
constexpr char kKeyOperation[] = "operation";
constexpr char kKeyDevice[] = "device";
constexpr char kKeyPackage[] = "package";
constexpr char kKeyPartitions[] = "partitions";
constexpr char kKeySuccess[] = "success";
constexpr char kKeyMessage[] = "message";
constexpr char kKeyDurationMs[] = "durationMs";

/// CSV 单字段转义（RFC4180）：含逗号 / 引号 / 换行时整段加引号，内部引号翻倍。
QString csvField(const QString& value)
{
    const bool needQuote = value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"'))
                           || value.contains(QLatin1Char('\n'))
                           || value.contains(QLatin1Char('\r'));
    if (!needQuote) return value;
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

} // namespace

QString FlashHistory::s_dirOverride;

// ─────────────────────────────────────────────────────────────────────────────
//  行级序列化 / 解析
// ─────────────────────────────────────────────────────────────────────────────

QString formatRecordLine(const FlashRecord& record)
{
    // 顺序固定写：人工排查时一眼能看出是哪台设备、成没成。
    // QJsonObject 内部按 key 排序序列化，所以实际输出顺序仍是字典序 —— 这里的
    // 书写顺序只是给读代码的人看的，稳定性由 QJsonDocument 保证。
    QJsonObject obj;
    obj.insert(QString::fromLatin1(kKeyTime),
               record.time.isValid() ? record.time.toString(Qt::ISODateWithMs) : QString());
    obj.insert(QString::fromLatin1(kKeyOperation), record.operation);
    obj.insert(QString::fromLatin1(kKeyDevice), record.device);
    obj.insert(QString::fromLatin1(kKeyPackage), record.package);
    obj.insert(QString::fromLatin1(kKeyPartitions), record.partitions);
    obj.insert(QString::fromLatin1(kKeySuccess), record.success);
    obj.insert(QString::fromLatin1(kKeyMessage), record.message);
    obj.insert(QString::fromLatin1(kKeyDurationMs),
               QJsonValue(static_cast<double>(record.durationMs)));

    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool parseRecordLine(const QByteArray& line, FlashRecord* out)
{
    if (!out) return false;

    // 尾随 \r / 空白容忍掉：文件被别的编辑器动过也不至于整表读不出来。
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) return false;

    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject obj = doc.object();

    // 字段缺一不可、类型必须对：宁可丢这一条，也不猜 / 不拿默认值凑一条假记录。
    const QJsonValue timeValue = obj.value(QString::fromLatin1(kKeyTime));
    const QJsonValue operationValue = obj.value(QString::fromLatin1(kKeyOperation));
    const QJsonValue deviceValue = obj.value(QString::fromLatin1(kKeyDevice));
    const QJsonValue packageValue = obj.value(QString::fromLatin1(kKeyPackage));
    const QJsonValue partitionsValue = obj.value(QString::fromLatin1(kKeyPartitions));
    const QJsonValue successValue = obj.value(QString::fromLatin1(kKeySuccess));
    const QJsonValue messageValue = obj.value(QString::fromLatin1(kKeyMessage));
    const QJsonValue durationValue = obj.value(QString::fromLatin1(kKeyDurationMs));

    if (!timeValue.isString() || !operationValue.isString() || !deviceValue.isString()
        || !packageValue.isString() || !partitionsValue.isString() || !successValue.isBool()
        || !messageValue.isString() || !durationValue.isDouble()) {
        return false;
    }

    // 时间串为空 = 写入方当时没有有效时间（异常路径）。这不算坏行，其余字段照样可信，
    // 记一个无效 QDateTime，UI 侧按「时间未知」显示。
    const QString timeText = timeValue.toString();
    QDateTime time;
    if (!timeText.isEmpty()) {
        time = QDateTime::fromString(timeText, Qt::ISODateWithMs);
        if (!time.isValid()) return false;
    }

    FlashRecord record;
    record.time = time;
    record.operation = operationValue.toString();
    record.device = deviceValue.toString();
    record.package = packageValue.toString();
    record.partitions = partitionsValue.toString();
    record.success = successValue.toBool();
    record.message = messageValue.toString();
    record.durationMs = static_cast<qint64>(durationValue.toDouble());

    // 全部校验通过才写出参：失败路径上 *out 保持调用方原值。
    *out = record;
    return true;
}

QString recordsToCsv(const QVector<FlashRecord>& records)
{
    QString csv = QStringLiteral("时间,操作,设备,来源包,分区,结果,耗时(秒),消息\n");
    csv.reserve(csv.size() + records.size() * 120);

    for (const FlashRecord& r : records) {
        const QString time = r.time.isValid()
                                 ? r.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                 : QString();
        csv += csvField(time);
        csv += QLatin1Char(',');
        csv += csvField(r.operation);
        csv += QLatin1Char(',');
        csv += csvField(r.device);
        csv += QLatin1Char(',');
        csv += csvField(r.package);
        csv += QLatin1Char(',');
        csv += csvField(r.partitions);
        csv += QLatin1Char(',');
        csv += r.success ? QStringLiteral("成功") : QStringLiteral("失败");
        csv += QLatin1Char(',');
        // 秒，两位小数：UI 里显示 "12.34s"，导出后人工对得上。
        csv += QString::number(static_cast<double>(r.durationMs) / 1000.0, 'f', 2);
        csv += QLatin1Char(',');
        csv += csvField(r.message);
        csv += QLatin1Char('\n');
    }
    return csv;
}

// ─────────────────────────────────────────────────────────────────────────────
//  FlashHistory
// ─────────────────────────────────────────────────────────────────────────────

FlashHistory& FlashHistory::instance()
{
    // 函数内 static：C++11 起保证线程安全的一次性初始化。
    static FlashHistory s;
    return s;
}

void FlashHistory::setDirForTest(const QString& dir)
{
    // 单例可能已经加载过别的目录（前一个用例），这里连同内存表一起复位，
    // 否则第二个用例会读到上一个临时目录的内容。
    FlashHistory& self = instance();
    QMutexLocker lock(&self.m_mutex);
    s_dirOverride = dir;
    self.m_records.clear();
    self.m_loaded = false;
}

QString FlashHistory::filePath() const
{
    if (!s_dirOverride.isEmpty()) {
        QDir().mkpath(s_dirOverride);
        return QDir(s_dirOverride).filePath(QString::fromLatin1(kFileName));
    }
    // AppStatePaths::Combine 内部已确保状态目录存在。
    return AppStatePaths::Combine(QString::fromLatin1(kFileName));
}

void FlashHistory::ensureLoadedLocked() const
{
    if (m_loaded) return;
    m_loaded = true;   // 先置位：下面即便读失败也不重复读盘

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) return;   // 没有文件 = 空表，不是错误
    const QByteArray content = file.readAll();
    file.close();
    if (content.isEmpty()) return;

    // 文件里是「旧 → 新」的追加顺序，读进来先按原序收，最后整体翻转成「最新在前」。
    QVector<FlashRecord> loaded;
    const QList<QByteArray> lines = content.split('\n');
    loaded.reserve(lines.size());
    for (const QByteArray& line : lines) {
        FlashRecord record;
        // 坏行直接跳过：一行写了一半（断电）不该连累整份历史。
        if (!parseRecordLine(line, &record)) continue;
        loaded.push_back(record);
    }

    std::reverse(loaded.begin(), loaded.end());
    if (loaded.size() > MaxRecords) loaded.resize(MaxRecords);   // 兜底：手改大的文件
    m_records = std::move(loaded);
}

bool FlashHistory::rewriteAllLocked() const
{
    // 整文件重写走 QSaveFile：写临时文件 + 原子 rename，中途崩了旧文件还在。
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) return false;

    // 内存是「最新在前」，文件要「旧 → 新」，所以倒着写。
    for (auto it = m_records.crbegin(); it != m_records.crend(); ++it) {
        const QByteArray line = formatRecordLine(*it).toUtf8();
        if (file.write(line) != line.size()) {
            file.cancelWriting();
            return false;
        }
        if (file.write("\n") != 1) {
            file.cancelWriting();
            return false;
        }
    }
    return file.commit();
}

QVector<FlashRecord> FlashHistory::records() const
{
    QMutexLocker lock(&m_mutex);
    ensureLoadedLocked();
    return m_records;
}

bool FlashHistory::append(const FlashRecord& record)
{
    QMutexLocker lock(&m_mutex);
    ensureLoadedLocked();

    // 先落内存：即使磁盘写失败，UI 也还能看到这次记录。
    m_records.prepend(record);

    if (m_records.size() > MaxRecords) {
        m_records.resize(MaxRecords);   // 从尾部丢最旧的
        return rewriteAllLocked();
    }

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    const QByteArray line = formatRecordLine(record).toUtf8();
    const bool ok = file.write(line) == line.size() && file.write("\n") == 1;
    file.close();
    return ok;
}

void FlashHistory::clear()
{
    QMutexLocker lock(&m_mutex);
    m_records.clear();
    m_loaded = true;   // 清空后不必再回读磁盘（文件马上删掉）
    QFile::remove(filePath());
}

} // namespace kc::diag
