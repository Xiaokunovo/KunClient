#include "storage/OcdtGenerator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace swt::edl {
namespace {

constexpr char kGeyixueKey[] = "geyixue";   // 7 字节，末尾不带 '\0' 参与运算
constexpr int kGeyixueKeyLength = 7;

/// 空 OSIG 块里的默认 Device ID（16 字节）。
constexpr unsigned char kDefaultDeviceId[16] = { 0x30, 0, 0, 0, 0, 0, 0, 0,
                                                 0,    0, 0, 0, 0, 0, 0, 0 };

/// 机型代号 → 数字 Project ID（顺序与原表一致；分析时按顺序取首个匹配项）。
constexpr OcdtModelEntry kProjectIdMap[] = {
    // OnePlus
    { "LE2100", 20828 }, { "LE2110", 19825 }, { "PGKM10", 21861 }, { "PHP110", 22823 },
    { "PJE110", 23801 }, { "PGZ110", 22801 },
    // OPPO
    { "PDEM10", 19065 }, { "PDEM30", 19066 }, { "PDHM00", 19161 }, { "PDPM00", 19015 },
    { "PDRM00", 20135 }, { "PDSM00", 20131 }, { "PDYM20", 20001 }, { "PECM20", 20041 },
    { "PEDM00", 20061 }, { "PEFM00", 20091 }, { "PEHM00", 20121 }, { "PELM00", 20151 },
    { "PENM00", 20161 }, { "PEQM00", 20181 }, { "PESM10", 21091 }, { "PEYM00", 21061 },
    { "PFCM00", 21081 }, { "PFGM00", 21041 }, { "PFJM10", 21031 }, { "PFTM20", 21102 },
    { "PFVM10", 21037 }, { "PGAM10", 21125 }, { "PGBM10", 21127 }, { "PGCM10", 4256 },
    { "PGFM10", 21135 }, { "PGJM10", 21143 }, { "PHJ110", 22083 }, { "PHM110", 22055 },
    { "PJB110", 22087 }, { "PJU110", 23054 }, { "PJV110", 23081 },
    // Realme
    { "RMX2117", 20613 }, { "RMX3031", 20615 }, { "RMX3370", 21619 }, { "RMX3372", 21623 },
    { "RMX3461", 21644 }, { "RMX3560", 21641 }, { "RMX3610", 22604 }, { "RMX3823", 23603 },
};

/// 8MB 平台里需要 OSIG 块的旧款联发科机型（OSIG Version = 0）。
constexpr OcdtModelEntry kMtkWithOsig[] = {
    { "PDYM20", 20001 }, { "PECM20", 20041 }, { "PDSM00", 20131 },
    { "PELM00", 20151 }, { "RMX2117", 20613 },
};

struct SpecialConfigEntry {
    const char* model;
    const char* hex;
};

/// 无法用公式生成的固定配置块（已是加密后的 52 字节）。
constexpr SpecialConfigEntry kSpecialConfigDb[] = {
    { "PGZ110",
      "67c3979696c256762fb2968757567656979687575676569796875756765697968757567656979687575676569796875756765697" },
    { "RMX3461",
      "be1397963f125676eed2968757567656979687575676569796875756765697968757567656979687575676569796875756765697" },
};

struct VariantEntry {
    const char* model;
    OcdtVariant variant;
};

/// 机型代号 → 推荐变体。
constexpr VariantEntry kVariantMap[] = {
    { "PGCM10", OcdtVariant::Fill02 },
    { "PJU110", OcdtVariant::GeyixueFill },
    { "PJV110", OcdtVariant::GeyixueFill },
    { "PGZ110", OcdtVariant::MixedProjectId },
    { "RMX3461", OcdtVariant::MixedProjectId },
};

template <typename T>
void putLe(QByteArray& data, qsizetype offset, T value)
{
    const T little = qToLittleEndian(value);
    std::memcpy(data.data() + offset, &little, sizeof(T));
}

void putBytes(QByteArray& data, qsizetype offset, const QByteArray& bytes)
{
    if (offset < 0 || offset + bytes.size() > data.size()) return;
    std::memcpy(data.data() + offset, bytes.constData(), static_cast<size_t>(bytes.size()));
}

QByteArray hexToBytes(const char* hex)
{
    const qsizetype length = static_cast<qsizetype>(std::strlen(hex));
    QByteArray bytes(static_cast<qsizetype>(length / 2), '\0');
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const QByteArray pair = QByteArray(hex + i * 2, 2);
        bytes[static_cast<int>(i)] = static_cast<char>(pair.toUShort(nullptr, 16) & 0xFF);
    }
    return bytes;
}

QString toHex(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

bool isBlank(const QByteArray& bytes)
{
    bool allZero = true;
    bool allFf = true;
    for (char ch : bytes) {
        const auto value = static_cast<unsigned char>(ch);
        if (value != 0x00) allZero = false;
        if (value != 0xFF) allFf = false;
        if (!allZero && !allFf) return false;
    }
    return true;
}

QString printableAscii(const QByteArray& text)
{
    QString out;
    out.reserve(text.size());
    for (char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        out.append(value >= 32 && value <= 126 ? QChar(value) : QChar(u'.'));
    }
    return out;
}

/// 是否写 8MB 平台的 OSIG 块：机型代号命中，或项目号命中。
bool needsOsigBlock(int projectId, const QString& modelCode)
{
    if (!modelCode.isEmpty()) {
        for (const auto& entry : kMtkWithOsig) {
            if (modelCode == QString::fromLatin1(entry.model)) return true;
        }
    }
    for (const auto& entry : kMtkWithOsig) {
        if (entry.projectId == projectId) return true;
    }
    return false;
}

/// 空 OSIG 块（两块平台的空块内容不同：128KB 多一份内嵌 TDCO 头副本）。
QByteArray emptyOsigBlock(OcdtPlatform platform)
{
    QByteArray block(OcdtGenerator::OsigBlockSize, '\0');
    putBytes(block, 0, QByteArray("OSIG"));
    // Device ID 在 OSIG 块内的 0x10 处（不是块首）。
    putBytes(block, 0x10, QByteArray(reinterpret_cast<const char*>(kDefaultDeviceId), 16));
    if (platform == OcdtPlatform::Qualcomm128Kb) {
        constexpr int kEmbedded = 0x50;
        putBytes(block, kEmbedded, QByteArray("TDCO"));
        putLe<quint32>(block, kEmbedded + 4, 0x0001);
        putLe<quint32>(block, kEmbedded + 8, 0x0010);
        putLe<quint32>(block, kEmbedded + 12, 0x0034);
    }
    return block;
}

QByteArray generateConfigBlock(int projectId, OcdtVariant variant)
{
    if (variant == OcdtVariant::MixedProjectId) {
        // 按 ProjectIdMap 的原始顺序取第一个命中且存在于特殊配置库的机型。
        for (const auto& entry : kProjectIdMap) {
            if (entry.projectId != projectId) continue;
            for (const auto& special : kSpecialConfigDb) {
                if (QString::fromLatin1(special.model) == QString::fromLatin1(entry.model)) {
                    return hexToBytes(special.hex);
                }
            }
        }
    }

    QByteArray plain(OcdtGenerator::ConfigSize, '\0');
    const auto lo = static_cast<char>(projectId & 0xFF);
    const auto hi = static_cast<char>((projectId >> 8) & 0xFF);
    plain[0] = lo;
    plain[1] = hi;
    plain[4] = lo;
    plain[5] = hi;
    plain[8] = lo;
    plain[9] = hi;

    if (variant == OcdtVariant::Fill02) {
        plain[2] = 0x02;
        plain[6] = 0x02;
        plain[10] = 0x02;
    } else if (variant == OcdtVariant::GeyixueFill) {
        for (int i = OcdtGenerator::ConfigTailOffset; i < OcdtGenerator::ConfigSize; ++i) {
            plain[i] = kGeyixueKey[i % kGeyixueKeyLength];
        }
    }
    return OcdtGenerator::geyixueEncrypt(plain);
}

} // namespace

const QVector<OcdtModelEntry>& OcdtGenerator::modelTable()
{
    static const QVector<OcdtModelEntry> table = [] {
        QVector<OcdtModelEntry> rows;
        rows.reserve(static_cast<int>(std::size(kProjectIdMap)));
        for (const auto& entry : kProjectIdMap) rows.append(entry);
        return rows;
    }();
    return table;
}

bool OcdtGenerator::tryGetProjectId(const QString& modelCode, int* projectId)
{
    if (projectId) *projectId = 0;
    const QString key = modelCode.trimmed();
    if (key.isEmpty()) return false;
    for (const auto& entry : kProjectIdMap) {
        if (key == QString::fromLatin1(entry.model)) {
            if (projectId) *projectId = entry.projectId;
            return true;
        }
    }
    return false;
}

OcdtVariant OcdtGenerator::recommendedVariant(const QString& modelCode)
{
    const QString key = modelCode.trimmed();
    if (!key.isEmpty()) {
        for (const auto& entry : kVariantMap) {
            if (key == QString::fromLatin1(entry.model)) return entry.variant;
        }
    }
    return OcdtVariant::Standard;
}

bool OcdtGenerator::needsOsigBackup(int projectId)
{
    for (const auto& entry : kMtkWithOsig) {
        if (entry.projectId == projectId) return true;
    }
    return false;
}

QString OcdtGenerator::platformKey(OcdtPlatform platform)
{
    return platform == OcdtPlatform::Mtk8Mb ? QStringLiteral("8mb") : QStringLiteral("128kb");
}

QString OcdtGenerator::variantKey(OcdtVariant variant)
{
    switch (variant) {
    case OcdtVariant::Fill02: return QStringLiteral("fill02");
    case OcdtVariant::GeyixueFill: return QStringLiteral("geyixue_fill");
    case OcdtVariant::MixedProjectId: return QStringLiteral("mixed_projid");
    case OcdtVariant::Standard: break;
    }
    return QStringLiteral("standard");
}

bool OcdtGenerator::parsePlatform(const QString& text, OcdtPlatform* platform)
{
    const QString key = text.trimmed().toLower();
    if (key == QStringLiteral("8mb") || key == QStringLiteral("mtk")) {
        if (platform) *platform = OcdtPlatform::Mtk8Mb;
        return true;
    }
    if (key == QStringLiteral("128kb") || key == QStringLiteral("qc")
        || key == QStringLiteral("qualcomm")) {
        if (platform) *platform = OcdtPlatform::Qualcomm128Kb;
        return true;
    }
    return false;
}

bool OcdtGenerator::parseVariant(const QString& text, OcdtVariant* variant)
{
    const QString key = text.trimmed().toLower();
    if (key.isEmpty() || key == QStringLiteral("standard")) {
        if (variant) *variant = OcdtVariant::Standard;
        return true;
    }
    if (key == QStringLiteral("fill02")) {
        if (variant) *variant = OcdtVariant::Fill02;
        return true;
    }
    if (key == QStringLiteral("geyixue_fill")) {
        if (variant) *variant = OcdtVariant::GeyixueFill;
        return true;
    }
    if (key == QStringLiteral("mixed_projid")) {
        if (variant) *variant = OcdtVariant::MixedProjectId;
        return true;
    }
    return false;
}

QString OcdtGenerator::defaultFileName(int projectId, OcdtPlatform platform)
{
    return QStringLiteral("ocdt_%1_%2.img").arg(projectId).arg(platformKey(platform));
}

QByteArray OcdtGenerator::geyixueEncrypt(const QByteArray& plain)
{
    QByteArray result(plain.size(), '\0');
    for (qsizetype i = 0; i < plain.size(); ++i) {
        const auto xored = static_cast<unsigned char>(plain[i]) ^ static_cast<unsigned char>(kGeyixueKey[i % kGeyixueKeyLength]);
        result[static_cast<int>(i)] = static_cast<char>(((xored << 4) | (xored >> 4)) & 0xFF);
    }
    return result;
}

QByteArray OcdtGenerator::geyixueDecrypt(const QByteArray& encrypted)
{
    QByteArray result(encrypted.size(), '\0');
    for (qsizetype i = 0; i < encrypted.size(); ++i) {
        const auto value = static_cast<unsigned char>(encrypted[i]);
        const auto rotated = static_cast<unsigned char>(((value >> 4) | (value << 4)) & 0xFF);
        result[static_cast<int>(i)] = static_cast<char>(rotated ^ static_cast<unsigned char>(kGeyixueKey[i % kGeyixueKeyLength]));
    }
    return result;
}

QByteArray OcdtGenerator::buildImage(OcdtPlatform platform, int projectId, OcdtVariant variant,
                                     const QString& modelCode, const QByteArray& osigBackup)
{
    const bool mtk = platform == OcdtPlatform::Mtk8Mb;
    QByteArray data(static_cast<qsizetype>(mtk ? FileSize8Mb : FileSize128Kb), '\0');

    // TDCO 头（两个平台完全相同）
    putBytes(data, 0, QByteArray("TDCO"));
    putLe<quint16>(data, 4, 0x0001);
    putLe<quint16>(data, 6, 0x0000);
    putLe<quint32>(data, 8, 0x0010);
    putLe<quint32>(data, 12, 0x0034);

    putBytes(data, ConfigOffset, generateConfigBlock(projectId, variant));

    if (mtk) {
        if (needsOsigBlock(projectId, modelCode)) {
            if (!osigBackup.isEmpty()) {
                // 只搬运备份镜像 [0x1000, 0x1200) 这一段；长度不足时**不写任何 OSIG**
                // （与参考实现一致，不是笔误）。
                if (osigBackup.size() >= OsigBackupMinLength) {
                    putBytes(data, OsigOffset, osigBackup.mid(OsigOffset, OsigBlockSize));
                }
            } else {
                // 空 OSIG：Version = 0，无签名。
                putBytes(data, OsigOffset, emptyOsigBlock(platform));
            }
        }
        return data;
    }

    // 128KB 高通：OSIG 块同时写进 0x1000 与 0x2000 两份。
    QByteArray osigBlock;
    if (!osigBackup.isEmpty() && osigBackup.size() >= OsigBackupMinLength) {
        const qsizetype copyLength = std::min<qsizetype>(OsigBlockSize, osigBackup.size() - OsigOffset);
        osigBlock = osigBackup.mid(OsigOffset, copyLength);
        if (osigBlock.size() < OsigBlockSize) osigBlock.append(QByteArray(OsigBlockSize - osigBlock.size(), '\0'));
    } else {
        osigBlock = emptyOsigBlock(platform);
    }
    putBytes(data, OsigOffset, osigBlock);
    putBytes(data, OsigBackupOffset, osigBlock);
    return data;
}

bool OcdtGenerator::generate(OcdtPlatform platform, int projectId, OcdtVariant variant,
                             const QString& outputPath, const QString& modelCode,
                             const QByteArray& osigBackup, QString* error)
{
    if (outputPath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("输出路径不能为空。");
        return false;
    }

    const QFileInfo info(outputPath);
    const QDir directory = info.absoluteDir();
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        if (error) *error = QStringLiteral("无法创建输出目录: %1").arg(directory.absolutePath());
        return false;
    }

    const QByteArray data = buildImage(platform, projectId, variant, modelCode, osigBackup);
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法写入文件: %1").arg(file.errorString());
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error) *error = QStringLiteral("写入不完整: %1").arg(file.errorString());
        return false;
    }
    file.close();
    return true;
}

QString OcdtGenerator::analyze(const QString& imagePath)
{
    if (imagePath.trimmed().isEmpty()) return QStringLiteral("[错误] 未指定 OCDT 镜像路径。");

    QFile file(imagePath);
    if (!file.exists()) return QStringLiteral("[错误] 文件不存在: %1").arg(imagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("[错误] 读取镜像失败: %1").arg(file.errorString());
    }
    const QByteArray data = file.readAll();
    file.close();

    constexpr qsizetype kMaxAnalyzeSize = 64LL * 1024 * 1024;
    if (data.size() < ConfigOffset + ConfigSize) {
        return QStringLiteral("[错误] OCDT 镜像长度不足 0x44 字节（实际 %1 字节），无法读取 TDCO 配置。")
            .arg(data.size());
    }
    if (data.size() > kMaxAnalyzeSize) {
        return QStringLiteral("[错误] 所选文件超过 64MB（实际 %1 字节），不符合 OCDT 镜像特征。")
            .arg(data.size());
    }

    const QByteArray magic = data.left(4);
    if (magic != QByteArray("TDCO")) {
        return QStringLiteral("[错误] 无 TDCO 文件头（实际为 \"%1\"），不是有效的 OCDT 镜像。")
            .arg(printableAscii(magic));
    }

    const quint16 version = qFromLittleEndian<quint16>(data.constData() + 4);
    const quint32 dataOffset = qFromLittleEndian<quint32>(data.constData() + 8);
    const quint32 dataLength = qFromLittleEndian<quint32>(data.constData() + 12);
    const quint64 configEnd = static_cast<quint64>(dataOffset) + dataLength;
    if (dataOffset > static_cast<quint64>(data.size()) || configEnd > static_cast<quint64>(data.size())) {
        return QStringLiteral("[错误] TDCO 配置偏移 0x%1 或长度 %2 超出文件范围（共 %3 字节）。")
            .arg(dataOffset, 0, 16)
            .arg(dataLength)
            .arg(data.size());
    }

    const QByteArray encryptedConfig = data.mid(ConfigOffset, ConfigSize);
    const QByteArray decryptedConfig = geyixueDecrypt(encryptedConfig);

    const int projectId1 = static_cast<unsigned char>(decryptedConfig[0])
                           | (static_cast<unsigned char>(decryptedConfig[1]) << 8);
    const int projectId2 = static_cast<unsigned char>(decryptedConfig[4])
                           | (static_cast<unsigned char>(decryptedConfig[5]) << 8);
    const int projectId3 = static_cast<unsigned char>(decryptedConfig[8])
                           | (static_cast<unsigned char>(decryptedConfig[9]) << 8);

    const bool configAreaBlank = isBlank(encryptedConfig);
    const bool projectIdEmpty = configAreaBlank
                                || (projectId1 == 0 && projectId2 == 0 && projectId3 == 0);
    const bool projectIdsMatch = projectId1 == projectId2 && projectId1 == projectId3;

    OcdtVariant variant = OcdtVariant::Standard;
    if (decryptedConfig[2] == 0x02) {
        variant = OcdtVariant::Fill02;
    } else {
        const QByteArray tail = decryptedConfig.mid(ConfigTailOffset);
        bool tailNonZero = false;
        for (char ch : tail) {
            if (ch != 0) {
                tailNonZero = true;
                break;
            }
        }
        const QByteArray key(reinterpret_cast<const char*>(kGeyixueKey), kGeyixueKeyLength);
        if (tailNonZero && tail.left(kGeyixueKeyLength) == key) {
            variant = OcdtVariant::GeyixueFill;
        }
    }
    if (!projectIdsMatch) variant = OcdtVariant::MixedProjectId;

    const bool is8Mb = data.size() > 1024 * 1024;
    const bool hasOsig = data.size() >= OsigOffset + OsigBlockSize
                         && data.mid(OsigOffset, 4) == QByteArray("OSIG");
    const quint32 osigVersion =
        hasOsig ? qFromLittleEndian<quint32>(data.constData() + OsigOffset + 4) : 0;

    QString fileType;
    if (is8Mb) {
        fileType = hasOsig ? QStringLiteral("8MB 联发科 + OSIG（旧款 2020-2021）")
                           : QStringLiteral("8MB 联发科（新款，无 OSIG）");
    } else {
        fileType = QStringLiteral("128KB 高通");
    }

    QStringList report;
    if (projectIdEmpty) report << QStringLiteral("[错误] 项目号为空，OCDT 无效。");

    report << QStringLiteral("================= OCDT 镜像分析报告 =================");
    report << QStringLiteral("文件: %1").arg(imagePath);
    report << QStringLiteral("大小: %1 字节 (%2 KB)")
                  .arg(data.size())
                  .arg(QString::number(data.size() / 1024.0, 'f', 1));

    report << QStringLiteral("[TDCO 头部]");
    report << QStringLiteral("  魔数: %1").arg(QString::fromLatin1(magic));
    report << QStringLiteral("  版本: %1").arg(version);
    report << QStringLiteral("  配置偏移: 0x%1").arg(dataOffset, 0, 16);
    report << QStringLiteral("  配置长度: %1").arg(dataLength);
    report << QStringLiteral("  配置结束: 0x%1").arg(configEnd, 0, 16);

    report << QStringLiteral("[配置数据]");
    report << QStringLiteral("  加密: %1").arg(toHex(encryptedConfig));
    report << QStringLiteral("  明文: %1").arg(toHex(decryptedConfig));
    report << QStringLiteral("  项目号: [%1, %2, %3]").arg(projectId1).arg(projectId2).arg(projectId3);
    report << QStringLiteral("  一致性: %1")
                  .arg(projectIdsMatch ? QStringLiteral("一致") : QStringLiteral("不一致"));
    report << QStringLiteral("  空判定: %1")
                  .arg(projectIdEmpty ? QStringLiteral("空（全 0x00 / 全 0xFF 或项目号为 0）")
                                      : QStringLiteral("非空"));
    report << QStringLiteral("  变体: %1").arg(variantKey(variant));

    QStringList describe;
    describe << (projectIdsMatch
                     ? QStringLiteral("项目号在 0x00/0x04/0x08 三处重复 (%1)").arg(projectId1)
                     : QStringLiteral("项目号不一致 (%1, %2, %3)")
                           .arg(projectId1)
                           .arg(projectId2)
                           .arg(projectId3));
    const bool fill02Mark = decryptedConfig[2] == 0x02 || decryptedConfig[6] == 0x02
                            || decryptedConfig[10] == 0x02;
    describe << QStringLiteral("0x02 标记: %1")
                    .arg(fill02Mark ? QStringLiteral("有") : QStringLiteral("无"));
    bool tailAllZero = true;
    bool tailIsKey = true;
    for (int i = ConfigTailOffset; i < decryptedConfig.size(); ++i) {
        if (decryptedConfig[i] != 0) tailAllZero = false;
        if (decryptedConfig[i] != kGeyixueKey[i % kGeyixueKeyLength]) tailIsKey = false;
    }
    describe << QStringLiteral("尾部 [0x16..0x33]: %1")
                    .arg(tailAllZero ? QStringLiteral("全零")
                                     : (tailIsKey ? QStringLiteral("geyixue 密钥填充")
                                                  : QStringLiteral("其它数据")));
    report << QStringLiteral("  摘要: %1").arg(describe.join(QStringLiteral("; ")));

    report << QStringLiteral("[文件类型]");
    report << QStringLiteral("  %1").arg(fileType);

    const char* matchedModel = nullptr;
    for (const auto& entry : kProjectIdMap) {
        if (entry.projectId == projectId1) {
            matchedModel = entry.model;
            break;
        }
    }
    report << QStringLiteral("[匹配设备]");
    if (matchedModel) {
        const QString model = QString::fromLatin1(matchedModel);
        report << QStringLiteral("  %1 (projId=%2)").arg(model).arg(projectId1);
        for (const auto& entry : kMtkWithOsig) {
            if (model == QString::fromLatin1(entry.model)) {
                report << QStringLiteral("  警告: 该机型属于旧款联发科设备，需要有效 OSIG 签名。");
                break;
            }
        }
    } else {
        report << QStringLiteral("  未收录 (projId=%1)").arg(projectId1);
    }

    report << QStringLiteral("[OSIG 签名块]");
    if (hasOsig) {
        const QByteArray deviceId = data.mid(OsigOffset + 0x10, 16);
        const QByteArray md5Bytes = data.mid(OsigOffset + 0x20, 32);
        QByteArray md5Trimmed = md5Bytes;
        while (md5Trimmed.endsWith('\0')) md5Trimmed.chop(1);
        bool md5IsText = !md5Trimmed.isEmpty();
        for (char ch : md5Trimmed) {
            const auto value = static_cast<unsigned char>(ch);
            if (value < 32 || value > 126) {
                md5IsText = false;
                break;
            }
        }

        const QByteArray tdcoCopy = data.mid(OsigOffset + 0x40, ConfigSize);
        bool hasTdcoCopy = false;
        for (char ch : tdcoCopy) {
            if (ch != 0) {
                hasTdcoCopy = true;
                break;
            }
        }

        const QByteArray signature = data.mid(OsigOffset + 0x100, 0x100);
        bool hasRsaSignature = false;
        for (char ch : signature) {
            if (ch != 0) {
                hasRsaSignature = true;
                break;
            }
        }

        const bool hasBackupOsig = data.size() >= OsigBackupOffset + OsigBlockSize
                                   && data.mid(OsigBackupOffset, 4) == QByteArray("OSIG");

        report << QStringLiteral("  位置: 0x%1").arg(OsigOffset, 0, 16);
        report << QStringLiteral("  OSIG版本: %1").arg(osigVersion);
        report << QStringLiteral("  Device ID: %1").arg(toHex(deviceId));
        report << QStringLiteral("  OSIG MD5: %1")
                      .arg(md5IsText ? QString::fromLatin1(md5Trimmed) : toHex(md5Bytes));
        report << QStringLiteral("  TDCO副本: %1")
                      .arg(hasTdcoCopy ? QStringLiteral("存在 (%1)").arg(toHex(tdcoCopy))
                                       : QStringLiteral("全零"));
        report << QStringLiteral("  RSA签名: %1")
                      .arg(hasRsaSignature
                               ? QStringLiteral("存在（256 字节，前 16 字节 %1）")
                                     .arg(toHex(signature.left(16)))
                               : QStringLiteral("不存在（全零）"));
        report << QStringLiteral("  备份OSIG: %1")
                      .arg(hasBackupOsig ? QStringLiteral("存在 (0x%1)").arg(OsigBackupOffset, 0, 16)
                                         : QStringLiteral("不存在"));
    } else {
        report << QStringLiteral("  OSIG版本: 无");
        report << QStringLiteral("  说明: 0x%1 处没有 \"OSIG\" 魔数，该镜像不含 OSIG 块。")
                      .arg(OsigOffset, 0, 16);
    }

    report << QStringLiteral("[检测结果]");
    report << (projectIdEmpty ? QStringLiteral("  项目号为空，OCDT 无效。")
                              : QStringLiteral("  项目号非空，OCDT 有效。"));

    return report.join(QLatin1Char('\n'));
}

} // namespace swt::edl
