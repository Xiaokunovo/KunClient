#include "oplus/OfpExtractor.h"

#include "kc/security/Digest.h"
#include "oplus/AesCfb.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QVector>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace swt::edl {
namespace {

using ProgressFn = std::function<void(int, const QString&)>;
using LogFn = std::function<void(const QString&)>;
using CancelFn = std::function<bool()>;

constexpr qint64 kCopyBuffer = 2 * 1024 * 1024;
constexpr qint64 kMaxXmlLength = 64LL * 1024 * 1024;

/// 参考实现里的固定口令（realme/OPPO 加密 zip 形态）。
constexpr char kZipPassword[] = "flash@realme$50E7F7D847732396F1582CD62DD385ED7ABB0897";

constexpr char kMtkShuffleKey[] = "geyixue";

struct KeyTriple {
    const char* mask;
    const char* key;
    const char* iv;
};

struct AsciiPair {
    const char* key;
    const char* iv;
};

// 高通 OFP 密钥表（顺序与参考实现一致，命中即停）。
constexpr KeyTriple kQcKeyTables[] = {
    { "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C",
      "422DD5399181E223813CD8ECDF2E4D72" },
    { "E11AA7BB558A436A8375FD15DDD4651F", "77DDF6A0696841F6B74782C097835169",
      "A739742384A44E8BA45207AD5C3700EA" },
    { "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C",
      "42F2D5399137E2B2813CD8ECDF2F4D72" },
    { "3C2D518D9BF2E4279DC758CD535147C3", "87C74A29709AC1BF2382276C4E8DF232",
      "598D92E967265E9BCABE2469FE4A915E" },
    { "8FB8FB261930260BE945B841AEFA9FD4", "E529E82B28F5A2F8831D860AE39E425D",
      "8A09DA60ED36F125D64709973372C1CF" },
    { "E8AE288C0192C54BF10C5707E9C4705B", "D64FC385DCD52A3C9B5FBA8650F92EDA",
      "79051FD8D8B6297E2E4559E997F63B7F" },
};

// 联发科 OFP 密钥表（前三元组 + 末位一组纯 ASCII key/iv）。
constexpr KeyTriple kMtkKeyTables[] = {
    { "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C",
      "42F2D5399137E2B2813CD8ECDF2F4D72" },
    { "9E4F32639D21357D37D226B69A495D21", "A3D8D358E42F5A9E931DD3917D9A3218",
      "386935399137416B67416BECF22F519A" },
    { "892D57E92A4D8A975E3C216B7C9DE189", "D26DF2D9913785B145D18C7219B89F26",
      "516989E4A1BFC78B365C6BC57D944391" },
    { "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C",
      "422DD5399181E223813CD8ECDF2E4D72" },
    { "3C4A618D9BF2E4279DC758CD535147C3", "87B13D29709AC1BF2382276C4E8DF232",
      "59B7A8E967265E9BCABE2469FE4A915E" },
    { "1C3288822BF824259DC852C1733127D3", "E7918D22799181CF2312176C9E2DF298",
      "3247F889A7B6DECBCA3E28693E4AAAFE" },
    { "1E4F32239D65A57D37D2266D9A775D43", "A332D3C3E42F5A3E931DD991729A321D",
      "3F2A35399A373377674155ECF28FD19A" },
    { "122D57E92A518AFF5E3C786B7C34E189", "DD6DF2D9543785674522717219989FB0",
      "12698965A132C76136CC88C5DD94EE91" },
};

constexpr AsciiPair kMtkAsciiKeys[] = {
    { "ab3f76d7989207f2", "2bf515b3a9737835" },
};

quint8 swapNibbles(quint8 value)
{
    return static_cast<quint8>(((value & 0x0F) << 4) | ((value & 0xF0) >> 4));
}

QByteArray fromHex(const char* hex)
{
    return QByteArray::fromHex(QByteArray(hex));
}

bool readAt(QFile& file, qint64 offset, qint64 length, QByteArray* out, QString* error)
{
    if (!out) return false;
    if (offset < 0 || length < 0 || offset + length > file.size()) {
        if (error) *error = QStringLiteral("OFP 文件表包含越界的镜像范围。");
        return false;
    }
    if (!file.seek(offset)) {
        if (error) *error = QStringLiteral("OFP 文件定位失败: %1").arg(file.errorString());
        return false;
    }
    *out = file.read(length);
    if (out->size() != length) {
        if (error) *error = QStringLiteral("OFP 文件读取不完整。");
        return false;
    }
    return true;
}

/// 把固件里的相对路径拼到输出目录下，并挡掉 ../ 之类的越界路径。
bool resolveOutputPath(const QString& root, const QString& relative, QString* out, QString* error)
{
    QString normalized = relative;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (normalized.startsWith(QLatin1Char('/'))) normalized.remove(0, 1);
    if (normalized.isEmpty()) {
        if (error) *error = QStringLiteral("固件表里出现了空的输出文件名。");
        return false;
    }

    const QString base = QDir::cleanPath(QDir(root).absolutePath());
    const QString destination = QDir::cleanPath(base + QLatin1Char('/') + normalized);
    if (!destination.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("固件表里有不安全的输出路径: %1").arg(relative);
        return false;
    }
    if (out) *out = destination;
    return true;
}

bool ensureParentDirectory(const QString& filePath, QString* error)
{
    const QDir parent = QFileInfo(filePath).absoluteDir();
    if (parent.exists()) return true;
    if (QDir().mkpath(parent.absolutePath())) return true;
    if (error) *error = QStringLiteral("无法创建输出目录: %1").arg(parent.absolutePath());
    return false;
}

bool copyRange(QFile& input, QFile& output, qint64 length, const QString& what, QString* error,
               const CancelFn& cancel)
{
    QByteArray buffer(static_cast<qsizetype>(std::min<qint64>(kCopyBuffer, std::max<qint64>(length, 1))),
                      '\0');
    while (length > 0) {
        if (cancel && cancel()) {
            if (error) *error = QStringLiteral("已取消: %1").arg(what);
            return false;
        }
        const qint64 requested = std::min<qint64>(buffer.size(), length);
        const qint64 read = input.read(buffer.data(), requested);
        if (read <= 0) {
            if (error) *error = QStringLiteral("读取镜像数据时文件意外结束: %1").arg(what);
            return false;
        }
        if (output.write(buffer.constData(), read) != read) {
            if (error) *error = QStringLiteral("写入失败: %1").arg(output.errorString());
            return false;
        }
        length -= read;
    }
    return true;
}

QString readCString(const QByteArray& value)
{
    const qsizetype end = value.indexOf('\0');
    return QString::fromUtf8(end < 0 ? value : value.left(end));
}

int percentOf(int current, int total)
{
    if (total <= 0) return 100;
    return static_cast<int>(current * 100LL / total);
}

// ── 高通 ProFile.xml 解析 ───────────────────────────────────────────────────

struct QcEntry {
    QString fileName;
    qint64 start = 0;
    qint64 readLength = 0;
    qint64 decryptLength = 0;
    bool copyWithoutDecrypt = false;
};

QString attributeOf(const QXmlStreamAttributes& attributes, const QString& name)
{
    for (const QXmlStreamAttribute& attribute : attributes) {
        if (attribute.name().toString().compare(name, Qt::CaseInsensitive) == 0) {
            return attribute.value().toString();
        }
    }
    return {};
}

bool tryLongAttribute(const QXmlStreamAttributes& attributes, const QString& name, qint64* value)
{
    const QString text = attributeOf(attributes, name);
    if (text.trimmed().isEmpty()) return false;
    bool ok = false;
    const qint64 parsed = text.trimmed().toLongLong(&ok);
    if (!ok) return false;
    if (value) *value = parsed;
    return true;
}

void addQcEntry(QVector<QcEntry>* entries, const QXmlStreamAttributes& attributes,
                const QString& category, int pageSize)
{
    QString fileName = attributeOf(attributes, QStringLiteral("Path"));
    if (fileName.isEmpty()) fileName = attributeOf(attributes, QStringLiteral("filename"));
    if (fileName.trimmed().isEmpty()) return;

    qint64 start = -1;
    qint64 value = 0;
    if (tryLongAttribute(attributes, QStringLiteral("FileOffsetInSrc"), &value)) {
        start = value * pageSize;
    } else if (tryLongAttribute(attributes, QStringLiteral("SizeInSectorInSrc"), &value)) {
        start = value * pageSize;
    }
    if (start < 0) return;

    qint64 rawLength = 0;
    tryLongAttribute(attributes, QStringLiteral("SizeInByteInSrc"), &rawLength);
    qint64 readLength = rawLength;

    qint64 sectors = 0;
    if (tryLongAttribute(attributes, QStringLiteral("SizeInSectorInSrc"), &sectors) && sectors > 0) {
        readLength = sectors * pageSize;
    }

    const bool rawLengthCategory = category == QLatin1String("Config")
                                   || category == QLatin1String("Provision")
                                   || category == QLatin1String("ChainedTableOfDigests")
                                   || category == QLatin1String("DigestsToSign")
                                   || category == QLatin1String("Firmware");
    if (rawLengthCategory && rawLength > 0) readLength = rawLength;

    const bool copyWithoutDecrypt = category == QLatin1String("DigestsToSign")
                                    || category == QLatin1String("ChainedTableOfDigests")
                                    || category == QLatin1String("Firmware");
    const qint64 decryptLength = category == QLatin1String("Sahara")
                                     ? rawLength
                                     : std::min<qint64>(0x40000, rawLength);

    if (readLength <= 0) return;

    QcEntry entry;
    entry.fileName = fileName;
    entry.start = start;
    entry.readLength = readLength;
    entry.decryptLength = std::max<qint64>(0, decryptLength);
    entry.copyWithoutDecrypt = copyWithoutDecrypt;
    entries->append(entry);
}

QVector<QcEntry> parseQcEntries(const QString& xml, int pageSize, QString* error)
{
    QVector<QcEntry> raw;
    QXmlStreamReader reader(xml);

    // 用元素名栈还原「父节点名 = 类别」：C# 那边是 item.Parent.Name。
    QStringList stack;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (!stack.isEmpty()) {
                addQcEntry(&raw, reader.attributes(), stack.last(), pageSize);
            }
            stack.append(reader.name().toString());
        } else if (token == QXmlStreamReader::EndElement) {
            if (!stack.isEmpty()) stack.removeLast();
        } else if (token == QXmlStreamReader::Invalid) {
            if (error) *error = QStringLiteral("ProFile.xml 解析失败: %1").arg(reader.errorString());
            return {};
        }
    }
    if (reader.hasError()) {
        if (error) *error = QStringLiteral("ProFile.xml 解析失败: %1").arg(reader.errorString());
        return {};
    }

    // 同一 (文件名, 起点, 长度) 只保留第一条（参考实现同样去重）。
    QVector<QcEntry> unique;
    QSet<QString> seen;
    for (const QcEntry& entry : raw) {
        const QString key = QStringLiteral("%1|%2|%3").arg(entry.fileName).arg(entry.start).arg(
            entry.readLength);
        if (seen.contains(key)) continue;
        seen.insert(key);
        unique.append(entry);
    }
    return unique;
}

// ── 加密 ZIP 形态 ──────────────────────────────────────────────────────────

QString findSevenZip()
{
    QStringList candidates;
    if (QCoreApplication::instance()) {
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("7z.exe"));
        candidates << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("7za.exe"));
    }
    const QString programFiles = QString::fromLocal8Bit(qgetenv("ProgramFiles"));
    if (!programFiles.isEmpty())
        candidates << QDir(programFiles).filePath(QStringLiteral("7-Zip/7z.exe"));
    const QString programFilesX86 = QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)"));
    if (!programFilesX86.isEmpty())
        candidates << QDir(programFilesX86).filePath(QStringLiteral("7-Zip/7z.exe"));
    for (const QString& name : { QStringLiteral("7z"), QStringLiteral("7za") }) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) candidates << found;
    }
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}

bool extractEncryptedZip(const QString& sourcePath, const QString& outputDirectory,
                         OfpExtractResult* result, QString* error, const ProgressFn& progress,
                         const LogFn& log)
{
    const QString sevenZip = findSevenZip();
    if (sevenZip.isEmpty()) {
        if (error) {
            *error = QStringLiteral(
                "该 OFP 是「带口令的加密 ZIP」形态，解包需要 7-Zip。\n"
                "请安装 7-Zip（或把 7z.exe 放到程序目录）后重试；\n"
                "口令为 flash@realme$50E7F7D847732396F1582CD62DD385ED7ABB0897");
        }
        return false;
    }

    if (log) log(QStringLiteral("[OFP] 加密 ZIP 形态，调用 %1 解压").arg(sevenZip));
    QProcess process;
    process.setProgram(sevenZip);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setArguments({ QStringLiteral("x"), QStringLiteral("-y"),
                           QStringLiteral("-p%1").arg(QString::fromLatin1(kZipPassword)),
                           QStringLiteral("-o%1").arg(QDir::toNativeSeparators(outputDirectory)),
                           QDir::toNativeSeparators(sourcePath) });
    process.start();
    if (!process.waitForStarted(15000)) {
        if (error) *error = QStringLiteral("无法启动 7-Zip: %1").arg(sevenZip);
        return false;
    }
    process.waitForFinished(-1);
    const QString output = QString::fromLocal8Bit(process.readAll());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("7-Zip 解压失败（退出码 %1）:\n%2")
                         .arg(process.exitCode())
                         .arg(output.right(800));
        }
        return false;
    }

    QDirIterator iterator(outputDirectory, QDir::Files | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        result->files << info.fileName();
        result->totalBytes += info.size();
    }
    result->fileCount = result->files.size();
    if (progress) progress(100, QStringLiteral("已解包 %1 个文件").arg(result->fileCount));
    if (log) log(output.trimmed().right(1500));
    return true;
}

// ── 联发科 / 高通 ─────────────────────────────────────────────────────────

bool tryGetMtkKey(QFile& input, QByteArray* key, QByteArray* iv)
{
    QByteArray head;
    QString ignored;
    if (!readAt(input, 0, 16, &head, &ignored)) return false;

    auto matches = [&head](const QByteArray& candidateKey, const QByteArray& candidateIv) {
        const QByteArray decrypted = AesCfb::decrypt(head, candidateKey, candidateIv);
        return decrypted.size() >= 3 && decrypted.left(3) == QByteArray("MMM");
    };

    for (const KeyTriple& table : kMtkKeyTables) {
        const QByteArray candidateKey = OfpExtractor::deriveKeyMaterial(fromHex(table.mask),
                                                                       fromHex(table.key));
        const QByteArray candidateIv = OfpExtractor::deriveKeyMaterial(fromHex(table.mask),
                                                                      fromHex(table.iv));
        if (matches(candidateKey, candidateIv)) {
            *key = candidateKey;
            *iv = candidateIv;
            return true;
        }
    }
    for (const AsciiPair& pair : kMtkAsciiKeys) {
        const QByteArray candidateKey(pair.key);
        const QByteArray candidateIv(pair.iv);
        if (matches(candidateKey, candidateIv)) {
            *key = candidateKey;
            *iv = candidateIv;
            return true;
        }
    }
    return false;
}

bool extractMtk(QFile& input, const QString& outputDirectory, const QByteArray& key,
                const QByteArray& iv, OfpExtractResult* result, QString* error,
                const ProgressFn& progress, const CancelFn& cancel)
{
    constexpr qint64 kHeaderLength = 0x6C;
    const qint64 fileLength = input.size();
    if (fileLength < kHeaderLength) {
        if (error) *error = QStringLiteral("OFP 文件过小，无法读取联发科文件表。");
        return false;
    }

    QByteArray header;
    if (!readAt(input, fileLength - kHeaderLength, kHeaderLength, &header, error)) return false;
    OfpExtractor::mtkShuffleDecode(QByteArray(kMtkShuffleKey), &header);

    const quint16 entryCount = qFromLittleEndian<quint16>(header.constData() + 72);
    const qint64 tableLength = static_cast<qint64>(entryCount) * 0x60;
    if (entryCount == 0 || tableLength > fileLength - kHeaderLength) {
        if (error) *error = QStringLiteral("联发科 OFP 文件表无效。");
        return false;
    }

    QByteArray table;
    if (!readAt(input, fileLength - kHeaderLength - tableLength, tableLength, &table, error)) {
        return false;
    }
    OfpExtractor::mtkShuffleDecode(QByteArray(kMtkShuffleKey), &table);

    for (int index = 0; index < entryCount; ++index) {
        if (cancel && cancel()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        const char* entry = table.constData() + index * 0x60;
        const qint64 start = static_cast<qint64>(qFromLittleEndian<quint64>(entry + 32));
        const qint64 length = static_cast<qint64>(qFromLittleEndian<quint64>(entry + 40));
        const qint64 encryptedLength = static_cast<qint64>(qFromLittleEndian<quint64>(entry + 48));
        const QString fileName = readCString(QByteArray(entry + 56, 32));
        if (fileName.isEmpty()) continue;

        if (start < 0 || length < 0 || start > fileLength || length > fileLength - start) {
            if (error) *error = QStringLiteral("OFP 文件表包含越界的镜像范围: %1").arg(fileName);
            return false;
        }

        QString destination;
        if (!resolveOutputPath(outputDirectory, fileName, &destination, error)) return false;
        if (!ensureParentDirectory(destination, error)) return false;

        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法创建输出文件 %1: %2").arg(destination,
                                                                              output.errorString());
            return false;
        }

        const qint64 safeEncrypted = std::min<qint64>(encryptedLength, length);
        if (safeEncrypted > 0) {
            QByteArray encrypted;
            if (!readAt(input, start, safeEncrypted, &encrypted, error)) return false;
            const QByteArray decrypted = AesCfb::decrypt(encrypted, key, iv);
            if (output.write(decrypted) != decrypted.size()) {
                if (error) *error = QStringLiteral("写入失败: %1").arg(output.errorString());
                return false;
            }
        }
        if (!input.seek(start + safeEncrypted)) {
            if (error) *error = QStringLiteral("OFP 文件定位失败。");
            return false;
        }
        if (!copyRange(input, output, length - safeEncrypted, fileName, error, cancel)) return false;
        output.close();

        result->fileCount += 1;
        result->totalBytes += length;
        result->files << fileName;
        if (progress) {
            progress(percentOf(index + 1, entryCount),
                     QStringLiteral("已解包 %1").arg(fileName));
        }
    }
    return true;
}

bool tryReadQcXml(QFile& input, int pageSize, const QByteArray& key, const QByteArray& iv,
                  QString* xml)
{
    const qint64 fileLength = input.size();
    const qint64 xmlHeader = fileLength - pageSize;
    QByteArray metadata;
    QString ignored;
    if (!readAt(input, xmlHeader + 0x14, 8, &metadata, &ignored)) return false;

    const quint32 pageNumber = qFromLittleEndian<quint32>(metadata.constData());
    const quint32 declaredLength = qFromLittleEndian<quint32>(metadata.constData() + 4);
    qint64 offset = static_cast<qint64>(pageNumber) * pageSize;
    qint64 length = declaredLength;
    if (length < 200) length = xmlHeader - offset - 0x57;
    if (length <= 0 || length > kMaxXmlLength || offset < 0 || offset + length > fileLength) {
        return false;
    }

    QByteArray encrypted;
    if (!readAt(input, offset, length, &encrypted, &ignored)) return false;
    const QByteArray decrypted = AesCfb::decrypt(encrypted, key, iv);
    if (decrypted.isEmpty()) return false;

    const qsizetype xmlStart = decrypted.indexOf(QByteArray("<?xml"));
    const qsizetype xmlEnd = decrypted.lastIndexOf('>');
    if (xmlStart < 0 || xmlEnd < xmlStart) return false;

    const QString candidate =
        QString::fromUtf8(decrypted.constData() + xmlStart, xmlEnd - xmlStart + 1);
    // 校验一下确实是 XML：错密钥偶尔也能凑出 "<?xml"，但不能解析。
    QXmlStreamReader validator(candidate);
    while (!validator.atEnd()) validator.readNext();
    if (validator.hasError()) return false;

    *xml = candidate;
    return true;
}

bool extractQualcomm(QFile& input, const QString& outputDirectory, OfpExtractResult* result,
                     QString* error, const ProgressFn& progress, const LogFn& log,
                     const CancelFn& cancel)
{
    QString ignored;
    int detected = 0;
    for (const int candidate : { 0x200, 0x1000 }) {
        const qint64 position = input.size() - candidate + 0x10;
        if (position < 0) continue;
        QByteArray marker;
        if (!readAt(input, position, 4, &marker, &ignored)) continue;
        if (qFromLittleEndian<quint32>(marker.constData()) == 0x7CEF) {
            detected = candidate;
            break;
        }
    }
    if (detected == 0) {
        if (error) *error = QStringLiteral("无法识别 OFP 页大小或文件格式。");
        return false;
    }

    QByteArray selectedKey;
    QByteArray selectedIv;
    QString xml;
    for (const KeyTriple& table : kQcKeyTables) {
        const QByteArray key = OfpExtractor::deriveKeyMaterial(fromHex(table.mask),
                                                               fromHex(table.key));
        const QByteArray iv = OfpExtractor::deriveKeyMaterial(fromHex(table.mask),
                                                              fromHex(table.iv));
        if (tryReadQcXml(input, detected, key, iv, &xml)) {
            selectedKey = key;
            selectedIv = iv;
            break;
        }
    }
    if (selectedKey.isEmpty() || xml.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未找到匹配的高通 OFP 密钥，该固件格式可能过新。");
        return false;
    }

    QString profilePath;
    if (!resolveOutputPath(outputDirectory, QStringLiteral("ProFile.xml"), &profilePath, error)) {
        return false;
    }
    if (!ensureParentDirectory(profilePath, error)) return false;
    {
        QFile profile(profilePath);
        if (!profile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法写入 ProFile.xml: %1").arg(profile.errorString());
            return false;
        }
        profile.write(xml.toUtf8());
        profile.close();
    }
    result->profileXmlPath = profilePath;

    const QVector<QcEntry> entries = parseQcEntries(xml, detected, error);
    if (entries.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("ProFile.xml 中没有可提取的镜像条目。");
        return false;
    }

    int superCount = 0;
    for (const QcEntry& entry : entries) {
        if (QFileInfo(entry.fileName).fileName().startsWith(QStringLiteral("super"),
                                                            Qt::CaseInsensitive)) {
            ++superCount;
        }
    }
    if (progress) {
        progress(3, superCount > 0
                        ? QStringLiteral("识别到 %1 个文件，其中 super 分段 %2 个")
                              .arg(entries.size())
                              .arg(superCount)
                        : QStringLiteral("识别到 %1 个文件，未发现 super 分段")
                              .arg(entries.size()));
    }
    if (log) log(QStringLiteral("[OFP] 页大小 0x%1，ProFile.xml 已解出").arg(detected, 0, 16));

    for (int index = 0; index < entries.size(); ++index) {
        if (cancel && cancel()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        const QcEntry& entry = entries.at(index);
        if (entry.start < 0 || entry.readLength < 0 || entry.start > input.size()
            || entry.readLength > input.size() - entry.start) {
            if (error) *error = QStringLiteral("OFP 文件表包含越界的镜像范围: %1").arg(entry.fileName);
            return false;
        }

        QString destination;
        if (!resolveOutputPath(outputDirectory, entry.fileName, &destination, error)) return false;
        if (!ensureParentDirectory(destination, error)) return false;

        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法创建输出文件 %1: %2").arg(destination,
                                                                              output.errorString());
            return false;
        }
        if (!input.seek(entry.start)) {
            if (error) *error = QStringLiteral("OFP 文件定位失败。");
            return false;
        }

        if (entry.copyWithoutDecrypt) {
            if (!copyRange(input, output, entry.readLength, entry.fileName, error, cancel)) {
                return false;
            }
        } else {
            const qint64 decryptLength = std::min<qint64>(entry.decryptLength, entry.readLength);
            if (decryptLength > 0) {
                QByteArray encrypted;
                if (!readAt(input, entry.start, decryptLength, &encrypted, error)) return false;
                const QByteArray decrypted = AesCfb::decrypt(encrypted, selectedKey, selectedIv);
                if (output.write(decrypted) != decrypted.size()) {
                    if (error) *error = QStringLiteral("写入失败: %1").arg(output.errorString());
                    return false;
                }
                if (!input.seek(entry.start + decryptLength)) {
                    if (error) *error = QStringLiteral("OFP 文件定位失败。");
                    return false;
                }
            }
            if (!copyRange(input, output, entry.readLength - decryptLength, entry.fileName, error,
                           cancel)) {
                return false;
            }
        }
        output.close();

        result->fileCount += 1;
        result->totalBytes += entry.readLength;
        result->files << entry.fileName;
        if (progress) {
            progress(percentOf(index + 1, entries.size()),
                     QStringLiteral("已解包 %1").arg(entry.fileName));
        }
    }
    return true;
}

} // namespace

QString OfpExtractResult::formatName(Format format)
{
    switch (format) {
    case Format::EncryptedZip: return QStringLiteral("加密 ZIP");
    case Format::Mtk: return QStringLiteral("联发科 OFP");
    case Format::Qualcomm: return QStringLiteral("高通 OFP");
    case Format::Unknown: break;
    }
    return QStringLiteral("未知");
}

QByteArray OfpExtractor::deriveKeyMaterial(const QByteArray& mask, const QByteArray& encoded)
{
    if (mask.isEmpty() || encoded.isEmpty()) return {};
    QByteArray mixed(encoded.size(), '\0');
    for (qsizetype i = 0; i < encoded.size(); ++i) {
        const auto value = static_cast<quint8>(encoded[i]) ^ static_cast<quint8>(mask[i % mask.size()]);
        mixed[i] = static_cast<char>(swapNibbles(value));
    }
    const QByteArray digest = kc::crypto::hashBytes(mixed, kc::crypto::HashAlgo::Md5);
    return digest.toHex().left(16);
}

void OfpExtractor::mtkShuffleDecode(const QByteArray& key, QByteArray* data)
{
    if (!data || key.isEmpty()) return;
    for (qsizetype i = 0; i < data->size(); ++i) {
        const auto value = static_cast<quint8>((*data)[i]);
        (*data)[i] = static_cast<char>(static_cast<quint8>(key[i % key.size()])
                                       ^ swapNibbles(value));
    }
}

int OfpExtractor::detectQualcommPageSize(const QByteArray& file)
{
    if (file.size() < 0x210) return 0;
    for (const int candidate : { 0x200, 0x1000 }) {
        const qsizetype position = file.size() - candidate + 0x10;
        if (position < 0 || position + 4 > file.size()) continue;
        if (qFromLittleEndian<quint32>(file.constData() + position) == 0x7CEF) return candidate;
    }
    return 0;
}

bool OfpExtractor::extract(const QString& sourcePath, const QString& outputDirectory,
                           OfpExtractResult* result, QString* error, const ProgressFn& progress,
                           const LogFn& log, const CancelFn& cancel)
{
    if (result) *result = OfpExtractResult{};
    if (error) error->clear();
    if (sourcePath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未指定 OFP 固件路径。");
        return false;
    }
    if (outputDirectory.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未指定输出目录。");
        return false;
    }
    if (!QFileInfo::exists(sourcePath)) {
        if (error) *error = QStringLiteral("OFP 固件不存在: %1").arg(sourcePath);
        return false;
    }
    if (!QDir().mkpath(outputDirectory)) {
        if (error) *error = QStringLiteral("无法创建输出目录: %1").arg(outputDirectory);
        return false;
    }

    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 OFP: %1").arg(input.errorString());
        return false;
    }

    QByteArray signature = input.read(3);
    input.seek(0);

    if (signature.size() >= 2 && signature[0] == 'P' && signature[1] == 'K') {
        if (result) result->format = OfpExtractResult::Format::EncryptedZip;
        if (progress) progress(1, QStringLiteral("识别为加密 ZIP 形态"));
        return extractEncryptedZip(sourcePath, outputDirectory, result, error, progress, log);
    }

    QByteArray mtkKey;
    QByteArray mtkIv;
    if (tryGetMtkKey(input, &mtkKey, &mtkIv)) {
        if (result) result->format = OfpExtractResult::Format::Mtk;
        if (progress) progress(2, QStringLiteral("识别为联发科 OFP"));
        return extractMtk(input, outputDirectory, mtkKey, mtkIv, result, error, progress, cancel);
    }

    if (result) result->format = OfpExtractResult::Format::Qualcomm;
    if (progress) progress(2, QStringLiteral("正在识别高通 OFP 密钥"));
    return extractQualcomm(input, outputDirectory, result, error, progress, log, cancel);
}

} // namespace swt::edl
