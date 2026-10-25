#include "protocol/Firehose.h"

#include "core/DeviceIdentity.h"
#include "transport/SerialTransport.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtEndian>

#include <limits>
#include <utility>

namespace swt::edl {
namespace {

QString xmlEnvelope(const QString& element)
{
    return QStringLiteral("<?xml version=\"1.0\" ?><data>%1</data>").arg(element);
}

QString joinedMessages(const FirehoseResponse& response)
{
    return response.messages.isEmpty() ? QStringLiteral("目标返回 NAK")
                                       : response.messages.join(QStringLiteral(" | "));
}

bool fixedAsciiField(QByteArrayView field, QString* value)
{
    qsizetype end = field.indexOf('\0');
    if (end < 0) end = field.size();
    for (qsizetype i = 0; i < end; ++i) {
        const uchar ch = static_cast<uchar>(field[i]);
        if (ch < 0x20 || ch > 0x7e) return false;
    }
    for (qsizetype i = end; i < field.size(); ++i) {
        if (field[i] != '\0') return false;
    }
    if (value) *value = QString::fromLatin1(field.first(end)).trimmed();
    return true;
}

QHash<QString, QString> parseVipSignedFilenames(const QString& digestPath)
{
    constexpr qsizetype recordSize = 132;
    constexpr qsizetype labelOffset = 32;
    constexpr qsizetype filenameOffset = 64;
    constexpr qsizetype fieldSize = 32;

    QFile file(digestPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = file.readAll();
    if (data.size() < recordSize) return {};

    // OPPO Digest variants wrap the same 132-byte partition records in several
    // different containers. Determine the record alignment from stable labels
    // instead of assuming one ELF/program-header layout.
    QHash<qsizetype, int> alignmentVotes;
    const QList<QByteArray> anchors{
        QByteArrayLiteral("ssd\0"), QByteArrayLiteral("persist\0"),
        QByteArrayLiteral("misc\0"), QByteArrayLiteral("PrimaryGPT\0"),
        QByteArrayLiteral("BackupGPT\0")
    };
    for (const QByteArray& anchor : anchors) {
        qsizetype position = 0;
        while ((position = data.indexOf(anchor, position)) >= 0) {
            if (position >= labelOffset) {
                alignmentVotes[(position - labelOffset) % recordSize] += 1;
            }
            position += anchor.size();
        }
    }

    qsizetype alignment = -1;
    int bestVotes = 0;
    for (auto it = alignmentVotes.cbegin(); it != alignmentVotes.cend(); ++it) {
        if (it.value() > bestVotes) {
            alignment = it.key();
            bestVotes = it.value();
        }
    }
    if (alignment < 0 || bestVotes < 2) return {};

    QHash<QString, QString> result;
    const QByteArrayView view(data);
    for (qsizetype record = alignment; record + recordSize <= data.size();
         record += recordSize) {
        QString label;
        QString filename;
        if (!fixedAsciiField(view.sliced(record + labelOffset, fieldSize), &label) ||
            !fixedAsciiField(view.sliced(record + filenameOffset, fieldSize), &filename) ||
            label.isEmpty()) {
            continue;
        }

        bool flagsValid = true;
        for (qsizetype offset = 96; offset < 116; offset += 4) {
            const quint32 flag = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(data.constData() + record + offset));
            if (flag > 1) {
                flagsValid = false;
                break;
            }
        }
        if (!flagsValid) continue;
        result.insert(label.toLower(), filename);
    }
    return result;
}

QString firehoseToken(QString value, const QString& fallback)
{
    value = QFileInfo(value).fileName().trimmed();
    value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    if (value.size() > 96) value.truncate(96);
    return value.isEmpty() ? fallback : value;
}

struct VipIdentity {
    QString label;
    QString filename;
};

void appendVipIdentity(QVector<VipIdentity>* identities, const QString& label,
                       const QString& filename)
{
    for (const VipIdentity& current : std::as_const(*identities)) {
        if (current.label.compare(label, Qt::CaseInsensitive) == 0 &&
            current.filename.compare(filename, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    identities->push_back({label, filename});
}

bool isBenignFirehoseNoise(const QString& message)
{
    // A USB zero-length packet may be reported as command id 0 by OPPO's
    // programmer. It is transport framing rather than an operation failure;
    // the following XML command is still parsed and acknowledged normally.
    return message.trimmed().compare(
               QStringLiteral("ERROR: Failed to read XML command 0"),
               Qt::CaseInsensitive) == 0;
}

QVector<VipIdentity> vipIdentityStrategies(const QHash<QString, QString>& signedFilenames,
                                           const QString& label, const QString& filename,
                                           quint32 lun, bool directGptRead = false)
{
    QVector<VipIdentity> result;
    if (directGptRead) {
        result.push_back({QStringLiteral("PrimaryGPT"),
                          QStringLiteral("gpt_main%1.bin").arg(lun)});
        return result;
    }

    const QString safeLabel = firehoseToken(label, QStringLiteral("buffer"));

    // PrimaryGPT/BackupGPT occur once per UFS LUN. A label-only hash keeps the
    // last record, so always derive these filenames from the requested LUN.
    if (safeLabel.compare(QStringLiteral("PrimaryGPT"), Qt::CaseInsensitive) == 0) {
        appendVipIdentity(&result, QStringLiteral("PrimaryGPT"),
                          QStringLiteral("gpt_main%1.bin").arg(lun));
    } else if (safeLabel.compare(QStringLiteral("BackupGPT"), Qt::CaseInsensitive) == 0) {
        appendVipIdentity(&result, QStringLiteral("BackupGPT"),
                          QStringLiteral("gpt_backup%1.bin").arg(lun));
    }

    const auto signedFile = signedFilenames.constFind(label.trimmed().toLower());
    if (signedFile != signedFilenames.cend() && !signedFile.value().isEmpty()) {
        appendVipIdentity(&result, safeLabel,
                          firehoseToken(signedFile.value(), QStringLiteral("buffer.bin")));
    }

    appendVipIdentity(&result, QStringLiteral("BackupGPT"),
                      QStringLiteral("gpt_backup%1.bin").arg(lun));
    appendVipIdentity(&result, safeLabel, QStringLiteral("gpt_backup0.bin"));
    appendVipIdentity(&result, safeLabel,
                      firehoseToken(filename, safeLabel + QStringLiteral(".bin")));
    appendVipIdentity(&result, safeLabel, safeLabel + QStringLiteral(".img"));
    appendVipIdentity(&result, safeLabel, safeLabel + QStringLiteral(".bin"));
    appendVipIdentity(&result, QStringLiteral("ssd"), QStringLiteral("ssd"));
    appendVipIdentity(&result, QStringLiteral("buffer"), QStringLiteral("buffer.bin"));
    appendVipIdentity(&result, {}, {});
    return result;
}

QString vipAttributes(const VipIdentity& identity, const QString& suffix = {})
{
    if (identity.label.isEmpty()) return suffix;
    if (identity.filename.isEmpty()) {
        return QStringLiteral(" label=\"%1\"%2").arg(identity.label, suffix);
    }
    return QStringLiteral(" filename=\"%1\" label=\"%2\"%3")
        .arg(identity.filename, identity.label, suffix);
}

} // namespace

FirehoseClient::FirehoseClient(SerialTransport& transport, std::atomic_bool& cancelled,
                               LogCallback log, ProgressCallback progress)
    : m_transport(transport), m_cancelled(cancelled), m_log(std::move(log)),
      m_progress(std::move(progress))
{
}

void FirehoseClient::observeMessages(const QStringList& messages)
{
    static const QRegularExpression chipPattern(
        QStringLiteral("Chip\\s+serial\\s+num\\s*\\(0x([0-9a-f]+)\\).*"
                       "Chip\\s+ID\\s*\\(0x([0-9a-f]+)\\)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression msmPattern(
        QStringLiteral("MSM[_ ]ID\\s*[:=]\\s*([A-Za-z0-9_-]+)"),
        QRegularExpression::CaseInsensitiveOption);

    for (const QString& message : messages) {
        const auto chipMatch = chipPattern.match(message);
        if (chipMatch.hasMatch()) {
            m_detectedSerial = chipMatch.captured(1).toLower();
            const QString chip = chipMatch.captured(2).toLower();
            bool ok = false;
            if (chip.toULongLong(&ok, 16) != 0 && ok) m_detectedChipId = chip;
        }
        const QString chipName = normalizeQualcommChipName(message);
        if (!chipName.isEmpty()) m_detectedModel = chipName;
        const auto msmMatch = msmPattern.match(message);
        if (msmMatch.hasMatch() && m_detectedModel.isEmpty()) {
            m_detectedModel = msmMatch.captured(1).toUpper();
        }

        const qsizetype jsonStart = message.indexOf(QLatin1Char('{'));
        if (jsonStart < 0) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            message.mid(jsonStart).toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
        const QJsonObject storage = document.object()
            .value(QStringLiteral("storage_info")).toObject();
        if (storage.isEmpty()) continue;
        const QString memory = storage.value(QStringLiteral("mem_type")).toString().trimmed();
        const QString product = storage.value(QStringLiteral("prod_name")).toString().trimmed();
        if (!memory.isEmpty()) m_detectedStorageType = memory.toUpper();
        if (!product.isEmpty()) m_detectedStorageProduct = product;
        const quint64 totalBlocks = storage.value(QStringLiteral("total_blocks"))
                                        .toVariant().toULongLong();
        const quint64 blockSize = storage.value(QStringLiteral("block_size"))
                                      .toVariant().toULongLong();
        if (totalBlocks > 0 && blockSize > 0 &&
            totalBlocks <= std::numeric_limits<quint64>::max() / blockSize) {
            m_detectedStorageBytes = totalBlocks * blockSize;
        }
    }
}

void FirehoseClient::enrichDeviceInfo(DeviceInfo* info) const
{
    if (!info) return;
    if (info->serial.isEmpty()) info->serial = m_detectedSerial;
    // Firehose runtime output describes the programmer actually accepted by
    // this device, so it takes precedence over the package-file hint.
    if (!m_detectedModel.isEmpty()) info->model = qualcommChipDisplayName(m_detectedModel);
    if (info->hwid.isEmpty()) {
        info->hwid = !m_detectedChipId.isEmpty()
            ? m_detectedChipId + m_detectedSerial : m_detectedSerial;
    }
    if (!m_detectedStorageType.isEmpty()) info->storage = m_detectedStorageType;
    info->storageProduct = m_detectedStorageProduct;
    info->storageBytes = m_detectedStorageBytes;
}

FirehoseResponse FirehoseClient::parseResponse(const QByteArray& frame)
{
    FirehoseResponse result;
    qsizetype xmlStart = frame.indexOf("<?xml");
    if (xmlStart < 0) xmlStart = frame.indexOf("<data");
    const QByteArray xmlFrame = xmlStart > 0 ? frame.mid(xmlStart) : frame;
    result.xml = QString::fromUtf8(xmlFrame);
    QXmlStreamReader reader(xmlFrame);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) continue;
        const auto attributes = reader.attributes();
        for (const auto& attribute : attributes) {
            const QString name = attribute.name().toString();
            const QString value = attribute.value().toString();
            if (name.compare(QStringLiteral("value"), Qt::CaseInsensitive) == 0) {
                if (value.compare(QStringLiteral("ACK"), Qt::CaseInsensitive) == 0) result.ack = true;
                if (value.compare(QStringLiteral("NAK"), Qt::CaseInsensitive) == 0) result.nak = true;
            }
            if (name.compare(QStringLiteral("rawmode"), Qt::CaseInsensitive) == 0) {
                result.rawMode = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
            }
            if (name.compare(QStringLiteral("MaxPayloadSizeToTargetInBytes"), Qt::CaseInsensitive) == 0) {
                bool ok = false;
                const quint64 payload = value.toULongLong(&ok);
                if (ok) result.negotiatedPayload = payload;
            }
            if (name.compare(QStringLiteral("value"), Qt::CaseInsensitive) == 0 &&
                value.compare(QStringLiteral("ACK"), Qt::CaseInsensitive) != 0 &&
                value.compare(QStringLiteral("NAK"), Qt::CaseInsensitive) != 0) {
                result.messages.push_back(value);
            }
        }
    }
    if (reader.hasError()) result.messages.push_back(reader.errorString());
    return result;
}

bool FirehoseClient::receiveResponse(FirehoseResponse* response, int timeoutMs, QString* error)
{
    *response = {};
    QElapsedTimer timer;
    timer.start();
    // Modern OPPO programmers emit one <log> frame per supported function before
    // acknowledging the first VIP blob. SM8550 commonly exceeds 32 XML frames.
    for (int frameIndex = 0; frameIndex < 1024; ++frameIndex) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (error) *error = QStringLiteral("等待 Firehose ACK 超时");
            return false;
        }
        QByteArray frame;
        if (!m_transport.readUntil(QByteArrayView("</data>"), &frame, 2 * 1024 * 1024,
                                   remaining, error)) {
            return false;
        }
        FirehoseResponse current = parseResponse(frame);
        observeMessages(current.messages);
        response->xml += current.xml;
        response->messages += current.messages;
        response->ack = response->ack || current.ack;
        response->nak = response->nak || current.nak;
        response->rawMode = current.rawMode;
        if (current.negotiatedPayload > 0) {
            response->negotiatedPayload = current.negotiatedPayload;
        }
        for (const QString& message : current.messages) {
            if (isBenignFirehoseNoise(message)) continue;
            if (m_log) m_log(QStringLiteral("Firehose: %1").arg(message));
        }
        if (current.ack || current.nak) return true;
    }
    if (error) *error = QStringLiteral("Firehose 日志帧超过 1024，未发现 ACK/NAK");
    return false;
}

bool FirehoseClient::waitForVipPartitionRequest(quint64* requestedCapacity,
                                                int timeoutMs, QString* error)
{
    if (requestedCapacity) *requestedCapacity = 0;
    QElapsedTimer timer;
    timer.start();
    const QRegularExpression requestPattern(
        QStringLiteral("receiving\\s+the\\s+partition\\s+info\\s+of\\s+size\\s+(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);

    for (int frameIndex = 0; frameIndex < 1024; ++frameIndex) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (error) *error = QStringLiteral("等待 VIP Partition Info 请求超时");
            return false;
        }
        QByteArray frame;
        if (!m_transport.readUntil(QByteArrayView("</data>"), &frame, 2 * 1024 * 1024,
                                   remaining, error)) {
            return false;
        }
        FirehoseResponse current = parseResponse(frame);
        observeMessages(current.messages);
        for (const QString& message : current.messages) {
            if (isBenignFirehoseNoise(message)) continue;
            if (m_log) m_log(QStringLiteral("Firehose: %1").arg(message));
            const QRegularExpressionMatch match = requestPattern.match(message);
            if (!match.hasMatch()) continue;
            bool ok = false;
            const quint64 requested = match.captured(1).toULongLong(&ok);
            if (!ok || requested == 0 || requested > 16 * 1024 * 1024) {
                if (error) *error = QStringLiteral("VIP Partition Info 容量异常：%1")
                                         .arg(match.captured(1));
                return false;
            }
            if (requestedCapacity) *requestedCapacity = requested;
            return true;
        }
        if (current.nak) {
            if (error) *error = QStringLiteral("Programmer 在 VIP 请求前返回 NAK：%1")
                                     .arg(joinedMessages(current));
            return false;
        }
    }
    if (error) *error = QStringLiteral("VIP 启动日志帧超过 1024，未发现 Partition Info 请求");
    return false;
}

bool FirehoseClient::receiveVipPartitionResult(FirehoseResponse* response,
                                               int timeoutMs, QString* error)
{
    *response = {};
    QElapsedTimer timer;
    timer.start();
    const QRegularExpression acceptedPattern(
        QStringLiteral("partition_num\\s*=\\s*(\\d+).*partition_info_size\\s*=\\s*(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);

    for (int frameIndex = 0; frameIndex < 256; ++frameIndex) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (error) *error = QStringLiteral("等待 VIP Partition Info 解析结果超时");
            return false;
        }
        QByteArray frame;
        if (!m_transport.readUntil(QByteArrayView("</data>"), &frame, 2 * 1024 * 1024,
                                   remaining, error)) {
            return false;
        }
        FirehoseResponse current = parseResponse(frame);
        observeMessages(current.messages);
        response->xml += current.xml;
        response->messages += current.messages;
        response->ack = response->ack || current.ack;
        response->nak = response->nak || current.nak;
        for (const QString& message : current.messages) {
            if (isBenignFirehoseNoise(message)) continue;
            if (m_log) m_log(QStringLiteral("Firehose: %1").arg(message));
            const QRegularExpressionMatch match = acceptedPattern.match(message);
            if (!match.hasMatch()) continue;
            response->ack = true;
            if (m_log) {
                m_log(QStringLiteral("VIP Partition Info 已接收：%1 个分区，记录 %2 字节")
                          .arg(match.captured(1), match.captured(2)));
            }
            return true;
        }
        if (current.ack || current.nak) return true;
    }
    if (error) *error = QStringLiteral("VIP Partition Info 响应帧超过 256");
    return false;
}

bool FirehoseClient::transact(const QString& xml, FirehoseResponse* response,
                              int timeoutMs, QString* error)
{
    const QByteArray request = xml.toUtf8();
    if (m_log) m_log(QStringLiteral("发送 Firehose 命令：%1").arg(xml.section(QLatin1Char('<'), 3, 3).section(QLatin1Char(' '), 0, 0)));
    if (!m_transport.writeAll(request, timeoutMs, error)) return false;
    return receiveResponse(response, timeoutMs, error);
}

bool FirehoseClient::checkAck(const FirehoseResponse& response, const QString& operation,
                              QString* error) const
{
    if (response.ack && !response.nak) return true;
    if (error) *error = QStringLiteral("%1失败：%2").arg(operation, joinedMessages(response));
    return false;
}

bool FirehoseClient::configure(StorageType storage, QString* error)
{
    const QString memory = storageName(storage);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const QString command = xmlEnvelope(QStringLiteral(
            "<configure MemoryName=\"%1\" Verbose=\"0\" AlwaysValidate=\"0\" "
            "MaxPayloadSizeToTargetInBytes=\"%2\" ZlpAwareHost=\"1\" SkipStorageInit=\"0\" "
            "CheckDevinfo=\"0\" EnableFlash=\"1\" />")
            .arg(memory).arg(m_maximumPayload));

        FirehoseResponse response;
        if (!transact(command, &response, 8000, error)) return false;
        if (response.negotiatedPayload > 0) {
            m_maximumPayload = qBound<quint64>(4096, response.negotiatedPayload, 16 * 1024 * 1024);
        }
        if (response.ack && !response.nak) {
            if (m_log) m_log(QStringLiteral("Firehose 已配置，Payload %1 KiB").arg(m_maximumPayload / 1024));
            return true;
        }
    }
    if (error) *error = QStringLiteral("Firehose Configure 返回 NAK");
    return false;
}

bool FirehoseClient::sendVipFile(const QString& path, const QString& label, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开 %1 失败：%2").arg(label, file.errorString());
        return false;
    }
    if (file.size() <= 0 || file.size() > 16 * 1024 * 1024) {
        if (error) *error = QStringLiteral("%1 文件大小异常：%2").arg(label).arg(file.size());
        return false;
    }
    const QByteArray payload = file.readAll();
    if (payload.size() != file.size()) {
        if (error) *error = QStringLiteral("读取 %1 失败：%2").arg(label, file.errorString());
        return false;
    }

    if (m_log) {
        m_log(QStringLiteral("原生 VIP RAW：发送 %1（%2 字节）")
                  .arg(QFileInfo(path).fileName()).arg(payload.size()));
    }
    if (!m_transport.writeAll(payload, 30000, error)) return false;

    FirehoseResponse response;
    if (!receiveResponse(&response, 20000, error)) return false;
    return checkAck(response, label, error);
}

bool FirehoseClient::sendInitialVipPartition(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开 Digest 失败：%1").arg(file.errorString());
        return false;
    }
    const QByteArray payload = file.readAll();
    if (payload.isEmpty() || payload.size() > 16 * 1024 * 1024) {
        if (error) *error = QStringLiteral("Digest 文件大小异常：%1").arg(payload.size());
        return false;
    }

    quint64 requestedCapacity = 0;
    if (m_log) m_log(QStringLiteral("等待 Programmer 请求 VIP Partition Info…"));
    QString requestError;
    const bool explicitRequest = waitForVipPartitionRequest(
        &requestedCapacity, 800, &requestError);
    if (!explicitRequest) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = requestError;
            return false;
        }
        // Several OPPO programmers enable a short boot watchdog before their
        // verbose Firehose banner has fully drained. Qualcomm host tools are
        // tolerant of the host queuing Digest early, so race the watchdog when
        // an explicit size request has not arrived in the first window.
        if (m_log) {
            m_log(QStringLiteral(
                "VIP 请求窗口内未见 Partition Info，立即预发送 Digest，避免 Programmer 看门狗复位（%1）")
                .arg(requestError));
        }
        requestedCapacity = qMax<quint64>(36864, static_cast<quint64>(payload.size()));
    }
    if (static_cast<quint64>(payload.size()) > requestedCapacity) {
        if (error) *error = QStringLiteral("Digest 大于 Programmer 缓冲区：%1/%2 字节")
                                 .arg(payload.size()).arg(requestedCapacity);
        return false;
    }

    if (m_log) {
        m_log(QStringLiteral("原生 VIP RAW：按设备请求发送 %1（%2/%3 字节）")
                  .arg(QFileInfo(path).fileName()).arg(payload.size()).arg(requestedCapacity));
    }
    // The size in the request is target buffer capacity, not the transfer size.
    // Qualcomm's host sends this 28,472-byte ELF as-is, without 36,864-byte padding.
    if (!m_transport.writeAll(payload, 30000, error)) return false;

    FirehoseResponse response;
    if (!receiveVipPartitionResult(&response, 20000, error)) return false;
    return checkAck(response, QStringLiteral("Digest"), error);
}

bool FirehoseClient::initializeOplusVip(const QString& digestPath,
                                        const QString& signaturePath,
    StorageType storage, QString* error)
{
    m_vipSignedFilenames = parseVipSignedFilenames(digestPath);
    if (m_log && !m_vipSignedFilenames.isEmpty()) {
        m_log(QStringLiteral("VIP Digest 元数据已解析：%1 个标签")
                  .arg(m_vipSignedFilenames.size()));
    }
    if (m_progress) m_progress(0, 5);
    if (m_log) m_log(QStringLiteral("OPPO VIP 1/5：等待请求并发送 Digest"));
    if (!sendInitialVipPartition(digestPath, error)) return false;
    if (m_progress) m_progress(1, 5);

    FirehoseResponse response;
    if (m_log) m_log(QStringLiteral("OPPO VIP 2/5：原生发送 Verify"));
    if (!transact(xmlEnvelope(QStringLiteral("<verify value=\"ping\" EnableVip=\"1\"/>")),
                  &response, 15000, error) ||
        !checkAck(response, QStringLiteral("VIP Verify"), error)) {
        return false;
    }
    if (m_progress) m_progress(2, 5);

    if (m_log) m_log(QStringLiteral("OPPO VIP 3/5：原生发送 Sign"));
    if (!sendVipFile(signaturePath, QStringLiteral("Sign"), error)) return false;
    if (m_progress) m_progress(3, 5);

    if (m_log) m_log(QStringLiteral("OPPO VIP 4/5：原生发送 SHA-256 Init"));
    response = {};
    if (!transact(xmlEnvelope(QStringLiteral("<sha256init Verbose=\"1\"/>")),
                  &response, 15000, error) ||
        !checkAck(response, QStringLiteral("SHA-256 Init"), error)) {
        return false;
    }
    if (m_progress) m_progress(4, 5);

    if (m_log) m_log(QStringLiteral("OPPO VIP 5/5：原生发送 Configure"));
    const QString command = xmlEnvelope(QStringLiteral(
        "<configure MemoryName=\"%1\" Verbose=\"0\" AlwaysValidate=\"0\" "
        "MaxDigestTableSizeInBytes=\"8192\" "
        "MaxPayloadSizeToTargetInBytes=\"1048576\" "
        "ZlpAwareHost=\"1\" SkipStorageInit=\"0\" "
        "CheckDevinfo=\"0\" EnableFlash=\"1\" />").arg(storageName(storage)));
    response = {};
    if (!transact(command, &response, 20000, error) ||
        !checkAck(response, QStringLiteral("VIP Configure"), error)) {
        return false;
    }
    if (response.negotiatedPayload > 0) {
        m_maximumPayload = qBound<quint64>(4096, response.negotiatedPayload, 16 * 1024 * 1024);
    }
    if (m_progress) m_progress(5, 5);
    m_oplusVip = true;
    if (m_log) m_log(QStringLiteral("OPPO VIP 原生协议初始化完成"));
    return true;
}

void FirehoseClient::resumeOplusVip(const QString& digestPath)
{
    m_vipSignedFilenames = parseVipSignedFilenames(digestPath);
    m_oplusVip = true;
    if (m_log) {
        m_log(QStringLiteral("已接管现有 OPPO VIP Firehose 会话，载入 %1 个签名标签")
                  .arg(m_vipSignedFilenames.size()));
    }
}

bool FirehoseClient::ping(QString* error, int timeoutMs)
{
    FirehoseResponse response;
    if (!transact(xmlEnvelope(QStringLiteral("<nop value=\"ping\" />")), &response,
                  qMax(250, timeoutMs), error)) return false;
    return checkAck(response, QStringLiteral("NOP"), error);
}

bool FirehoseClient::getStorageInfo(quint32 lun, QString* error)
{
    FirehoseResponse response;
    const QString command = xmlEnvelope(QStringLiteral("<getstorageinfo physical_partition_number=\"%1\" />").arg(lun));
    if (!transact(command, &response, 8000, error)) return false;
    return checkAck(response, QStringLiteral("读取存储信息"), error);
}

bool FirehoseClient::readToDevice(QIODevice& output, quint64 startSector, quint64 sectorCount,
                                  quint32 lun, quint32 sectorSize, QString* error,
                                  const QString& label, const QString& filename)
{
    if (!output.isWritable() || sectorCount == 0 || sectorSize == 0) {
        if (error) *error = QStringLiteral("读取目标参数错误");
        return false;
    }
    if (sectorCount > std::numeric_limits<quint64>::max() / sectorSize) {
        if (error) *error = QStringLiteral("读取范围溢出");
        return false;
    }

    const quint64 totalBytes = sectorCount * sectorSize;
    QVector<VipIdentity> identities;
    if (m_oplusVip) {
        identities = vipIdentityStrategies(m_vipSignedFilenames, label, filename, lun,
                                           label.trimmed().isEmpty() && startSector <= 33);
    } else {
        identities.push_back({});
    }

    const QString sizeKb = QString::number(
        static_cast<double>(totalBytes) / 1024.0, 'f', 1);
    FirehoseResponse response;
    bool enteredRawMode = false;
    QString lastReject;
    for (qsizetype index = 0; index < identities.size(); ++index) {
        const VipIdentity& identity = identities[index];
        QString attributes;
        if (m_oplusVip) {
            attributes = vipAttributes(
                identity, QStringLiteral(" size_in_KB=\"%1\" sparse=\"false\"").arg(sizeKb));
            if (m_log) {
                m_log(identity.label.isEmpty()
                    ? QStringLiteral("VIP READ：尝试无标签身份")
                    : QStringLiteral("VIP READ：label=%1，filename=%2")
                          .arg(identity.label, identity.filename));
            }
        }

        const QString command = xmlEnvelope(QStringLiteral(
            "<read SECTOR_SIZE_IN_BYTES=\"%1\" num_partition_sectors=\"%2\" "
            "start_sector=\"%3\" physical_partition_number=\"%4\"%5 />")
            .arg(sectorSize).arg(sectorCount).arg(startSector).arg(lun).arg(attributes));

        response = {};
        QString attemptError;
        if (!transact(command, &response, 8000, &attemptError)) {
            if (error) *error = attemptError;
            return false;
        }
        if (response.ack && !response.nak && response.rawMode) {
            enteredRawMode = true;
            break;
        }
        lastReject = joinedMessages(response);
        if (m_oplusVip && index + 1 < identities.size() && m_log) {
            m_log(QStringLiteral("VIP READ：当前身份未通过，继续尝试"));
        }
    }
    if (!enteredRawMode) {
        if (error) {
            *error = lastReject.isEmpty()
                ? QStringLiteral("设备未进入 RAW 读取模式")
                : QStringLiteral("进入读取模式失败：%1").arg(lastReject);
        }
        return false;
    }

    quint64 completed = 0;
    while (completed < totalBytes) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("操作已停止");
            return false;
        }
        const qsizetype request = static_cast<qsizetype>(qMin<quint64>(m_maximumPayload, totalBytes - completed));
        QByteArray block;
        // Windows qcusbser may defer the final short transfer for several seconds
        // when a read ends on a USB packet boundary. Keep the RAW phase alive long
        // enough for the driver to deliver the tail instead of desynchronizing the
        // following XML command.
        if (!m_transport.readExactly(request, &block, 30000, error)) return false;
        if (output.write(block) != block.size()) {
            if (error) *error = QStringLiteral("写入输出文件失败：%1").arg(output.errorString());
            return false;
        }
        completed += static_cast<quint64>(block.size());
        if (m_progress) m_progress(completed, totalBytes);
    }

    if (!receiveResponse(&response, 15000, error)) return false;
    return checkAck(response, QStringLiteral("完成读取"), error);
}

bool FirehoseClient::readToByteArray(QByteArray* output, quint64 startSector,
                                     quint64 sectorCount, quint32 lun,
                                     quint32 sectorSize, QString* error,
                                     const QString& label, const QString& filename)
{
    if (!output || sectorCount > 1024 * 1024) {
        if (error) *error = QStringLiteral("内存读取范围异常");
        return false;
    }
    output->clear();
    QBuffer buffer(output);
    buffer.open(QIODevice::WriteOnly);
    return readToDevice(buffer, startSector, sectorCount, lun, sectorSize, error,
                        label, filename);
}

bool FirehoseClient::program(QIODevice& input, quint64 inputSize, quint64 startSector,
                             quint64 maximumSectors, quint32 lun, quint32 sectorSize,
                             QString* error, const QString& label, const QString& filename)
{
    if (!input.isReadable() || inputSize == 0 || sectorSize == 0) {
        if (error) *error = QStringLiteral("镜像参数错误");
        return false;
    }
    const quint64 sectors = (inputSize + sectorSize - 1) / sectorSize;
    if (maximumSectors > 0 && sectors > maximumSectors) {
        if (error) *error = QStringLiteral("镜像需要 %1 个扇区，目标分区仅有 %2 个扇区")
                                 .arg(sectors).arg(maximumSectors);
        return false;
    }

    QVector<VipIdentity> identities;
    if (m_oplusVip) {
        // OPPO's native gpt-backup RW mode uses a LUN-local BackupGPT identity
        // for arbitrary program ranges. Prefer it immediately; signed metadata
        // identities remain as compatibility fallbacks for other programmers.
        appendVipIdentity(&identities, QStringLiteral("BackupGPT"),
                          QStringLiteral("gpt_backup%1.bin").arg(lun));
        const QVector<VipIdentity> fallbacks = vipIdentityStrategies(
            m_vipSignedFilenames, label, filename, lun);
        for (const VipIdentity& identity : fallbacks) {
            appendVipIdentity(&identities, identity.label, identity.filename);
        }
    } else {
        identities.push_back({});
    }

    FirehoseResponse response;
    bool enteredRawMode = false;
    QString lastReject;
    for (qsizetype index = 0; index < identities.size(); ++index) {
        const VipIdentity& identity = identities[index];
        const QString attributes = m_oplusVip
            ? vipAttributes(identity, QStringLiteral(" sparse=\"false\"")) : QString();
        if (m_oplusVip && m_log) {
            m_log(identity.label.isEmpty()
                ? QStringLiteral("VIP PROGRAM：尝试无标签身份")
                : QStringLiteral("VIP PROGRAM：label=%1，filename=%2")
                      .arg(identity.label, identity.filename));
        }

        const QString command = xmlEnvelope(QStringLiteral(
            "<program SECTOR_SIZE_IN_BYTES=\"%1\" num_partition_sectors=\"%2\" "
            "start_sector=\"%3\" physical_partition_number=\"%4\"%5 />")
            .arg(sectorSize).arg(sectors).arg(startSector).arg(lun).arg(attributes));

        response = {};
        QString attemptError;
        if (!transact(command, &response, 8000, &attemptError)) {
            if (error) *error = attemptError;
            return false;
        }
        if (response.ack && !response.nak && response.rawMode) {
            enteredRawMode = true;
            break;
        }
        lastReject = joinedMessages(response);
        if (m_oplusVip && index + 1 < identities.size() && m_log) {
            m_log(QStringLiteral("VIP PROGRAM：当前身份未通过，继续尝试"));
        }
    }
    if (!enteredRawMode) {
        if (error) {
            *error = lastReject.isEmpty()
                ? QStringLiteral("设备未进入 RAW 写入模式")
                : QStringLiteral("进入写入模式失败：%1").arg(lastReject);
        }
        return false;
    }

    const quint64 transferBytes = sectors * sectorSize;
    quint64 completed = 0;
    while (completed < transferBytes) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("操作已停止");
            return false;
        }
        const qsizetype request = static_cast<qsizetype>(qMin<quint64>(m_maximumPayload, transferBytes - completed));
        const quint64 sourceRemaining = completed < inputSize ? inputSize - completed : 0;
        const qsizetype sourceRequest = static_cast<qsizetype>(
            qMin<quint64>(static_cast<quint64>(request), sourceRemaining));
        QByteArray block = sourceRequest > 0 ? input.read(sourceRequest) : QByteArray{};
        if (block.size() != sourceRequest && completed < inputSize) {
            if (error) *error = QStringLiteral("读取镜像失败：%1").arg(input.errorString());
            return false;
        }
        if (block.size() < request) block.append(request - block.size(), '\0');
        if (!m_transport.writeAll(block, 15000, error)) return false;
        completed += static_cast<quint64>(block.size());
        if (m_progress) m_progress(qMin(completed, inputSize), inputSize);
    }

    if (!receiveResponse(&response, 15000, error)) return false;
    return checkAck(response, QStringLiteral("完成写入"), error);
}

bool FirehoseClient::erase(quint64 startSector, quint64 sectorCount, quint32 lun,
                           quint32 sectorSize, QString* error,
                           const QString& label, const QString& filename)
{
    if (sectorCount == 0 || sectorSize == 0) {
        if (error) *error = QStringLiteral("擦除范围参数错误");
        return false;
    }

    QVector<VipIdentity> identities;
    if (m_oplusVip) {
        // fh_loader's OPPO special RW mode forwards erase with the real label and
        // deliberately omits filename. This differs from read/program matching.
        appendVipIdentity(&identities,
                          firehoseToken(label, QStringLiteral("partition")), {});
        const QVector<VipIdentity> fallbacks = vipIdentityStrategies(
            m_vipSignedFilenames, label, filename, lun);
        for (const VipIdentity& identity : fallbacks) {
            appendVipIdentity(&identities, identity.label, identity.filename);
        }
    } else {
        identities.push_back({});
    }

    QString lastReject;
    for (qsizetype index = 0; index < identities.size(); ++index) {
        const VipIdentity& identity = identities[index];
        const QString attributes = m_oplusVip ? vipAttributes(identity) : QString();
        if (m_oplusVip && m_log) {
            m_log(identity.label.isEmpty()
                ? QStringLiteral("VIP ERASE：尝试无标签身份")
                : identity.filename.isEmpty()
                    ? QStringLiteral("VIP ERASE：label=%1，无 filename")
                          .arg(identity.label)
                    : QStringLiteral("VIP ERASE：label=%1，filename=%2")
                          .arg(identity.label, identity.filename));
        }
        const QString command = xmlEnvelope(QStringLiteral(
            "<erase SECTOR_SIZE_IN_BYTES=\"%1\" num_partition_sectors=\"%2\" "
            "start_sector=\"%3\" physical_partition_number=\"%4\"%5 />")
            .arg(sectorSize).arg(sectorCount).arg(startSector).arg(lun).arg(attributes));

        FirehoseResponse response;
        QString attemptError;
        if (!transact(command, &response, 30000, &attemptError)) {
            if (error) *error = attemptError;
            return false;
        }
        if (response.ack && !response.nak) return true;
        lastReject = joinedMessages(response);
        if (m_oplusVip && index + 1 < identities.size() && m_log) {
            m_log(QStringLiteral("VIP ERASE：当前身份未通过，继续尝试"));
        }
    }
    if (error) *error = QStringLiteral("擦除分区失败：%1").arg(lastReject);
    return false;
}

bool FirehoseClient::patch(quint32 sectorSize, quint64 byteOffset, quint32 lun,
                           quint64 sizeInBytes, const QString& startSector,
                           const QString& value, QString* error)
{
    if (sectorSize == 0 || sizeInBytes == 0 || startSector.isEmpty() || value.isEmpty()) {
        if (error) *error = QStringLiteral("Patch 参数错误");
        return false;
    }
    const QString command = xmlEnvelope(QStringLiteral(
        "<patch SECTOR_SIZE_IN_BYTES=\"%1\" byte_offset=\"%2\" filename=\"DISK\" "
        "physical_partition_number=\"%3\" size_in_bytes=\"%4\" "
        "start_sector=\"%5\" value=\"%6\" />")
        .arg(sectorSize).arg(byteOffset).arg(lun).arg(sizeInBytes)
        .arg(startSector, value));
    FirehoseResponse response;
    if (!transact(command, &response, 30000, error)) return false;
    return checkAck(response, QStringLiteral("应用 GPT Patch"), error);
}

bool FirehoseClient::setBootableStorageDrive(quint32 value, QString* error)
{
    FirehoseResponse response;
    const QString command = xmlEnvelope(
        QStringLiteral("<setbootablestoragedrive value=\"%1\" />").arg(value));
    if (!transact(command, &response, 30000, error)) return false;
    return checkAck(response, QStringLiteral("设置启动存储器"), error);
}

bool FirehoseClient::reboot(bool toEdl, QString* error)
{
    const QString value = toEdl ? QStringLiteral("reset_to_edl") : QStringLiteral("reset");
    const QString attributes = toEdl
        ? QStringLiteral("value=\"%1\" verbose=\"0\" DelayInSeconds=\"1\"").arg(value)
        : QStringLiteral("value=\"%1\" verbose=\"0\"").arg(value);
    const QByteArray command = xmlEnvelope(
        QStringLiteral("<power %1 />").arg(attributes)).toUtf8();
    if (m_log) {
        m_log(QStringLiteral("发送 Firehose 命令：power %1").arg(value));
    }
    if (!m_transport.writeAll(command, 3000, error)) return false;

    // Power commands are different from data commands: several programmers
    // drop USB as soon as the command is accepted, before an XML ACK reaches
    // the host. OneToolBox uses the same fire-and-close behavior. Consume an
    // ACK when present, reject an explicit NAK, and otherwise treat a completed
    // write followed by port transition as a successfully submitted command.
    FirehoseResponse response;
    QString responseError;
    if (!receiveResponse(&response, toEdl ? 2500 : 1500, &responseError)) {
        if (m_log) {
            m_log(QStringLiteral("Firehose Power 已提交，设备切换端口时未返回最终 ACK"));
        }
        return true;
    }
    if (response.nak) {
        if (error) *error = QStringLiteral("Firehose Power 返回 NAK");
        return false;
    }
    return response.ack;
}

} // namespace swt::edl
