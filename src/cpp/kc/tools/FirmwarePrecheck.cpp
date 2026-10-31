#include "kc/tools/FirmwarePrecheck.h"

#include "kc/security/Digest.h"
#include "kc/tools/FirmwareChecksum.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
#include <utility>

namespace kc::tools {
namespace {

/// 汇总文案里最多列几个文件名 —— 列太多会把状态行撑成一段。
constexpr int kMaxListedNames = 4;

// ─────────────────────────────────────────────────────────────────────────────
//  小工具
// ─────────────────────────────────────────────────────────────────────────────

/// 追加一条明细并同步计数。
void addDetail(PrecheckResult* result, PrecheckLevel level, const QString& path,
               const QString& message)
{
    if (!result) return;
    result->details.push_back({ level, path, message });
    switch (level) {
    case PrecheckLevel::Error:   ++result->errorCount;   break;
    case PrecheckLevel::Warning: ++result->warningCount; break;
    case PrecheckLevel::Info:    break;
    }
}

/// 文件名列表拼成「a、b、c 等 N 个」（超出 kMaxListedNames 就收口）。
QString joinNames(const QStringList& names)
{
    if (names.size() <= kMaxListedNames) return names.join(QStringLiteral("、"));
    const QStringList head = names.mid(0, kMaxListedNames);
    return QStringLiteral("%1 等 %2 个").arg(head.join(QStringLiteral("、"))).arg(names.size());
}

/// 压缩包判定。只看后缀，不读文件头 —— 本层不解包，认后缀已经够用。
bool isArchivePath(const QString& path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    return name.endsWith(QStringLiteral(".zip")) || name.endsWith(QStringLiteral(".7z"))
           || name.endsWith(QStringLiteral(".tgz")) || name.endsWith(QStringLiteral(".tar.gz"))
           || name.endsWith(QStringLiteral(".tar"));
}

bool isRawProgramXml(const QString& path)
{
    const QString name = QFileInfo(path).fileName().toLower();
    return name.startsWith(QStringLiteral("rawprogram")) && name.endsWith(QStringLiteral(".xml"));
}

/// 旁挂校验文件 → 算法。认不出来返回 false。
bool algoForChecksumFile(const QString& path, kc::crypto::HashAlgo* algo)
{
    const QString name = QFileInfo(path).fileName().toLower();
    if (name == QStringLiteral("sha256sums") || name.endsWith(QStringLiteral(".sha256"))) {
        if (algo) *algo = kc::crypto::HashAlgo::Sha256;
        return true;
    }
    if (name == QStringLiteral("md5sums") || name.endsWith(QStringLiteral(".md5"))) {
        if (algo) *algo = kc::crypto::HashAlgo::Md5;
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  目录扫描
// ─────────────────────────────────────────────────────────────────────────────

/// 目录（含一级 images 子目录，大小写不敏感）下的镜像文件。
/// 只扫一层：固件包的镜像要么在根，要么在 images/，再深的目录属于用户自己
/// 整理过，猜错了反而会拿别的包的文件去核对。
QVector<QFileInfo> collectImages(const QString& dir)
{
    QVector<QFileInfo> images;
    QSet<QString> seen;
    QStringList dirs{ dir };

    const QDir root(dir);
    const QStringList subs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sub : subs) {
        if (sub.compare(QStringLiteral("images"), Qt::CaseInsensitive) == 0)
            dirs.append(root.filePath(sub));
    }

    for (const QString& one : std::as_const(dirs)) {
        const QDir qdir(one);
        const QFileInfoList files = qdir.entryInfoList(
            { QStringLiteral("*.img"), QStringLiteral("*.bin"), QStringLiteral("*.mbn") },
            QDir::Files, QDir::Name);
        for (const QFileInfo& info : files) {
            const QString key = info.absoluteFilePath().toLower();
            if (seen.contains(key)) continue;
            seen.insert(key);
            images.push_back(info);
        }
    }
    return images;
}

/// 目录（含一级 images 子目录）里的 rawprogram*.xml。
QStringList collectRawProgramXml(const QString& dir)
{
    QStringList out;
    QStringList dirs{ dir };
    const QDir root(dir);
    const QStringList subs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& sub : subs) {
        if (sub.compare(QStringLiteral("images"), Qt::CaseInsensitive) == 0)
            dirs.append(root.filePath(sub));
    }
    for (const QString& one : std::as_const(dirs)) {
        const QDir qdir(one);
        const QFileInfoList files = qdir.entryInfoList({ QStringLiteral("rawprogram*.xml") },
                                                       QDir::Files, QDir::Name);
        for (const QFileInfo& info : files) out.push_back(info.absoluteFilePath());
    }
    return out;
}

/// 目录里的批量校验文件（coreutils 的 SHA256SUMS / MD5SUMS）。
QStringList collectBatchManifests(const QString& dir)
{
    QStringList out;
    const QDir qdir(dir);
    const QFileInfoList files = qdir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        const QString name = info.fileName().toLower();
        if (name == QStringLiteral("sha256sums") || name == QStringLiteral("md5sums"))
            out.push_back(info.absoluteFilePath());
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  摘要比对
// ─────────────────────────────────────────────────────────────────────────────

/// 核对一份批量校验文件（SHA256SUMS / MD5SUMS）里列出的全部条目。
void verifyManifest(const QString& manifestPath, kc::crypto::HashAlgo algo,
                    const PrecheckOptions& options, PrecheckResult* result)
{
    QFile in(manifestPath);
    if (!in.open(QIODevice::ReadOnly)) {
        addDetail(result, PrecheckLevel::Error, manifestPath,
                  QStringLiteral("打不开校验文件：%1").arg(in.errorString()));
        return;
    }
    const QVector<ChecksumRecord> records = parseChecksumText(in.readAll());
    in.close();

    if (records.isEmpty()) {
        addDetail(result, PrecheckLevel::Warning, manifestPath,
                  QStringLiteral("校验文件里没有可解析的记录（格式应为「摘要 + 两个空格 + 文件名」），"
                                 "本次没有做摘要比对"));
        return;
    }
    if (!options.verifyChecksums) {
        addDetail(result, PrecheckLevel::Info, manifestPath,
                  QStringLiteral("找到校验文件（%1 条记录），按设置本次未做摘要比对")
                      .arg(records.size()));
        return;
    }

    const QDir baseDir = QFileInfo(manifestPath).absoluteDir();
    const int expectedHex = kc::crypto::hashAlgoLength(algo) * 2;
    int matched = 0;
    int mismatched = 0;
    int missing = 0;
    int skipped = 0;
    QStringList mismatchNames;

    for (const ChecksumRecord& record : records) {
        // 记录里的摘要长度和文件后缀给的算法对不上：这条跳过（例如 SHA-1 记录
        // 混进了 .sha256 文件）。不静默 —— 汇总里报条数。
        if (record.hash.size() != expectedHex) {
            ++skipped;
            continue;
        }
        const QString target = baseDir.filePath(record.name);
        const QFileInfo targetInfo(target);
        if (!targetInfo.exists() || !targetInfo.isFile()) {
            ++missing;
            addDetail(result, PrecheckLevel::Error, target,
                      QStringLiteral("校验文件里列出的文件不存在：%1（包不完整）").arg(record.name));
            continue;
        }
        QString error;
        const QString actual = kc::crypto::hashFile(target, algo, &error);
        if (actual.isEmpty()) {
            ++mismatched;
            mismatchNames.append(record.name);
            addDetail(result, PrecheckLevel::Error, target,
                      QStringLiteral("计算摘要失败：%1（%2）").arg(record.name, error));
            continue;
        }
        if (actual.compare(record.hash, Qt::CaseInsensitive) == 0) {
            ++matched;
            continue;
        }
        ++mismatched;
        mismatchNames.append(record.name);
        result->checksumMismatch = true;
        addDetail(result, PrecheckLevel::Error, target,
                  QStringLiteral("摘要不一致：%1（期望 %2，实际 %3）")
                      .arg(record.name, record.hash, actual));
    }

    const QString algoName = kc::crypto::hashAlgoName(algo).toUpper();
    if (mismatched > 0) {
        addDetail(result, PrecheckLevel::Error, manifestPath,
                  QStringLiteral("%1 校验不一致 %2 项：%3")
                      .arg(algoName).arg(mismatched).arg(joinNames(mismatchNames)));
    } else if (missing > 0) {
        addDetail(result, PrecheckLevel::Error, manifestPath,
                  QStringLiteral("%1 校验清单里有 %2 个文件缺失").arg(algoName).arg(missing));
    } else if (matched > 0) {
        addDetail(result, PrecheckLevel::Info, manifestPath,
                  QStringLiteral("%1 校验一致（%2 项）").arg(algoName).arg(matched));
    }
    if (skipped > 0) {
        addDetail(result, PrecheckLevel::Warning, manifestPath,
                  QStringLiteral("有 %1 条记录的摘要长度与 %2 不符，已跳过").arg(skipped).arg(algoName));
    }
}

/// 核对「一个镜像 + 同名 .sha256/.md5」这一对。
void verifySidecar(const QFileInfo& image, const QString& sidecarPath, kc::crypto::HashAlgo algo,
                   const PrecheckOptions& options, PrecheckResult* result)
{
    QFile in(sidecarPath);
    if (!in.open(QIODevice::ReadOnly)) {
        addDetail(result, PrecheckLevel::Warning, sidecarPath,
                  QStringLiteral("打不开旁挂校验文件：%1").arg(in.errorString()));
        return;
    }
    const QVector<ChecksumRecord> records = parseChecksumText(in.readAll());
    in.close();
    if (records.isEmpty()) {
        addDetail(result, PrecheckLevel::Warning, sidecarPath,
                  QStringLiteral("旁挂校验文件里没有可解析的记录，本次没有做摘要比对"));
        return;
    }
    if (!options.verifyChecksums) {
        addDetail(result, PrecheckLevel::Info, sidecarPath,
                  QStringLiteral("找到旁挂校验文件，按设置本次未做摘要比对"));
        return;
    }

    const int expectedHex = kc::crypto::hashAlgoLength(algo) * 2;
    const auto found = std::find_if(records.cbegin(), records.cend(),
                                    [expectedHex](const ChecksumRecord& record) {
                                        return record.hash.size() == expectedHex;
                                    });
    if (found == records.cend()) {
        addDetail(result, PrecheckLevel::Warning, sidecarPath,
                  QStringLiteral("旁挂校验文件里的摘要长度与 %1 不符，已跳过")
                      .arg(kc::crypto::hashAlgoName(algo).toUpper()));
        return;
    }

    QString error;
    const QString actual = kc::crypto::hashFile(image.absoluteFilePath(), algo, &error);
    if (actual.isEmpty()) {
        result->checksumMismatch = true;
        addDetail(result, PrecheckLevel::Error, image.absoluteFilePath(),
                  QStringLiteral("计算摘要失败：%1（%2）").arg(image.fileName(), error));
        return;
    }
    if (actual.compare(found->hash, Qt::CaseInsensitive) == 0) {
        addDetail(result, PrecheckLevel::Info, sidecarPath,
                  QStringLiteral("%1 与旁挂 %2 一致")
                      .arg(image.fileName(), QFileInfo(sidecarPath).fileName()));
        return;
    }
    result->checksumMismatch = true;
    addDetail(result, PrecheckLevel::Error, image.absoluteFilePath(),
              QStringLiteral("摘要不一致：%1（期望 %2，实际 %3）")
                  .arg(image.fileName(), found->hash, actual));
}

/// 单文件（镜像 / 压缩包 / XML）的同名旁挂校验文件路径：`<文件名>.sha256` / `.md5`。
QStringList sidecarCandidates(const QString& filePath)
{
    QStringList out;
    for (const QString& suffix : { QStringLiteral(".sha256"), QStringLiteral(".md5") }) {
        const QString candidate = filePath + suffix;
        if (QFileInfo::exists(candidate)) out.push_back(candidate);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  rawprogram XML：按声明 size 判断截断
// ─────────────────────────────────────────────────────────────────────────────

/// XML 里的一个 program 条目（只取本层要用的字段）。
struct XmlProgramEntry {
    QString filename;
    QString label;
    qint64 declaredFileBytes = -1;   ///< 明确声明的「文件应有字节数」，-1 = 未声明
    QString declaredFrom;            ///< 声明来自哪个属性（报错文案用）
    qint64 fileOffsetBytes = 0;      ///< file_sector_offset × 扇区大小
    bool sparse = false;
    bool capacityOnly = false;       ///< 只声明了分区容量，不能据此判截断
};

/// 解析一个 rawprogram XML 的 program 条目。返回 false 表示 XML 读不动。
bool readXmlPrograms(const QString& xmlPath, QVector<XmlProgramEntry>* entries, QString* error)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开 XML 失败：%1").arg(file.errorString());
        return false;
    }

    QXmlStreamReader reader(&file);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) continue;
        if (reader.name().compare(QLatin1String("program"), Qt::CaseInsensitive) != 0) continue;

        const QXmlStreamAttributes attrs = reader.attributes();
        const auto attr = [&attrs](const char* name) {
            return attrs.value(QLatin1String(name)).toString().trimmed();
        };
        const auto toNumber = [](QString text, qint64* out) {
            text = text.trimmed();
            while (text.endsWith(QLatin1Char('.'))) text.chop(1);
            if (text.isEmpty()) return false;
            bool ok = false;
            const int base = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10;
            const qint64 value = text.toLongLong(&ok, base);
            if (!ok || value < 0) return false;
            *out = value;
            return true;
        };

        XmlProgramEntry entry;
        entry.filename = attr("filename");
        entry.label = attr("label");
        entry.sparse = attr("sparse").compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        if (entry.filename.isEmpty()) continue;   // 空 filename = 擦除标记，不涉及文件

        qint64 sectorSize = 0;
        qint64 sectorCount = 0;
        const bool haveSectorSize = toNumber(attr("SECTOR_SIZE_IN_BYTES"), &sectorSize);
        const bool haveSectorCount = toNumber(attr("num_partition_sectors"), &sectorCount);

        // 明确声明的文件字节数优先：size_in_bytes > size_in_KB。
        // 🔴 num_partition_sectors × SECTOR_SIZE_IN_BYTES 是**分区容量**，不是文件
        //    应有长度（boot.img 常常比分区小），拿它判截断会把好包全判成坏包。
        qint64 declared = -1;
        if (toNumber(attr("size_in_bytes"), &declared) && declared > 0) {
            entry.declaredFileBytes = declared;
            entry.declaredFrom = QStringLiteral("size_in_bytes");
        } else if (toNumber(attr("size_in_KB"), &declared) && declared > 0) {
            entry.declaredFileBytes = declared * 1024;
            entry.declaredFrom = QStringLiteral("size_in_KB");
        } else {
            entry.capacityOnly = haveSectorSize && haveSectorCount && sectorCount > 0;
        }

        qint64 fileSectorOffset = 0;
        if (toNumber(attr("file_sector_offset"), &fileSectorOffset) && haveSectorSize
            && sectorSize > 0) {
            entry.fileOffsetBytes = fileSectorOffset * sectorSize;
        }
        entries->push_back(entry);
    }

    if (reader.hasError()) {
        if (error) {
            *error = QStringLiteral("XML 解析失败：%1（第 %2 行）")
                         .arg(reader.errorString()).arg(reader.lineNumber());
        }
        return false;
    }
    return true;
}

/// 按 XML 声明核对引用的镜像：存在性 + 截断 + 多余字节。
void checkXmlDeclarations(const QString& xmlPath, const PrecheckOptions& options,
                          PrecheckResult* result)
{
    Q_UNUSED(options);
    QVector<XmlProgramEntry> entries;
    QString parseError;
    if (!readXmlPrograms(xmlPath, &entries, &parseError)) {
        addDetail(result, PrecheckLevel::Error, xmlPath, parseError);
        return;
    }
    if (entries.isEmpty()) {
        addDetail(result, PrecheckLevel::Warning, xmlPath,
                  QStringLiteral("XML 里没有可核对的 program 条目"));
        return;
    }

    const QDir baseDir = QFileInfo(xmlPath).absoluteDir();
    int truncated = 0;
    int extra = 0;
    int checked = 0;
    int capacityOnly = 0;
    QStringList truncatedNames;

    for (const XmlProgramEntry& entry : entries) {
        const QString target = QDir::cleanPath(baseDir.filePath(entry.filename));
        const QFileInfo info(target);
        const QString shown = entry.label.isEmpty() ? entry.filename : entry.label;

        if (!info.exists() || !info.isFile()) {
            addDetail(result, PrecheckLevel::Error, target,
                      QStringLiteral("XML 声明的镜像不存在：%1（%2）").arg(shown, entry.filename));
            continue;
        }
        if (entry.sparse) {
            // sparse 镜像在磁盘上是压缩形态，声明的是展开后大小 —— 不能直接比。
            continue;
        }
        if (entry.declaredFileBytes < 0) {
            if (entry.capacityOnly) ++capacityOnly;
            continue;
        }

        const qint64 actual = info.size();
        if (actual < entry.fileOffsetBytes) {
            addDetail(result, PrecheckLevel::Error, target,
                      QStringLiteral("%1 的文件偏移超出文件长度（%2 < %3 字节）")
                          .arg(shown).arg(actual).arg(entry.fileOffsetBytes));
            ++truncated;
            truncatedNames.append(shown);
            continue;
        }
        const qint64 available = actual - entry.fileOffsetBytes;
        ++checked;
        if (available < entry.declaredFileBytes) {
            ++truncated;
            truncatedNames.append(shown);
            addDetail(result, PrecheckLevel::Error, target,
                      QStringLiteral("%1 被截断：XML 声明 %2 字节（%3），实际只有 %4 字节")
                          .arg(shown, QString::number(entry.declaredFileBytes),
                               entry.declaredFrom, QString::number(available)));
        } else if (available > entry.declaredFileBytes) {
            ++extra;
            addDetail(result, PrecheckLevel::Warning, target,
                      QStringLiteral("%1 比 XML 声明多 %2 字节（%3，通常无害，可能是补零）")
                          .arg(shown, QString::number(available - entry.declaredFileBytes),
                               entry.declaredFrom));
        }
    }

    if (truncated > 0) {
        addDetail(result, PrecheckLevel::Error, xmlPath,
                  QStringLiteral("XML 声明 size 与磁盘不符：%1 个镜像被截断（%2）")
                      .arg(truncated).arg(joinNames(truncatedNames)));
    } else if (checked > 0 && extra == 0) {
        addDetail(result, PrecheckLevel::Info, xmlPath,
                  QStringLiteral("XML 声明的 size 与磁盘一致（%1 个镜像）").arg(checked));
    }
    if (capacityOnly > 0) {
        addDetail(result, PrecheckLevel::Info, xmlPath,
                  QStringLiteral("有 %1 个条目只声明了分区容量（num_partition_sectors），"
                                 "无法据此判断文件是否被截断")
                      .arg(capacityOnly));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  关键镜像
// ─────────────────────────────────────────────────────────────────────────────

QStringList defaultKeyImages()
{
    return { QStringLiteral("boot"), QStringLiteral("init_boot"),
             QStringLiteral("vendor_boot"), QStringLiteral("dtbo"),
             QStringLiteral("recovery"), QStringLiteral("system"),
             QStringLiteral("super"), QStringLiteral("vbmeta") };
}

/// 检查「包内是否缺关键镜像」。口径：
///   · 一个镜像都没有 → Error（这压根不是固件目录）；
///   · boot / init_boot / vendor_boot 三个引导镜像全缺 → Warning（刷了也开不了机）；
///   · 其它关键镜像缺 → Info 列出（很多机型本来就没有 recovery.img）。
void checkKeyImages(const QVector<QFileInfo>& images, const PrecheckOptions& options,
                    PrecheckResult* result)
{
    if (images.isEmpty()) return;   // 调用方已经报过「没有镜像」

    const QStringList wanted = options.keyImages.isEmpty() ? defaultKeyImages()
                                                           : options.keyImages;
    QSet<QString> present;
    for (const QFileInfo& info : images) present.insert(info.completeBaseName().toLower());

    QStringList missing;
    for (const QString& name : wanted) {
        if (!present.contains(name.toLower())) missing.push_back(name);
    }
    if (missing.isEmpty()) {
        addDetail(result, PrecheckLevel::Info, QString(),
                  QStringLiteral("关键镜像齐全（%1 个）").arg(wanted.size()));
        return;
    }

    const bool noBootImage = !present.contains(QStringLiteral("boot"))
                             && !present.contains(QStringLiteral("init_boot"))
                             && !present.contains(QStringLiteral("vendor_boot"));
    if (noBootImage) {
        addDetail(result, PrecheckLevel::Warning, QString(),
                  QStringLiteral("缺关键引导镜像：boot / init_boot / vendor_boot 一个都没有"));
    }
    addDetail(result, PrecheckLevel::Info, QString(),
              QStringLiteral("未找到的常见镜像：%1（部分机型本来就没有，属正常）")
                  .arg(joinNames(missing)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  结果收口
// ─────────────────────────────────────────────────────────────────────────────

/// 汇总明细 → level / ok / blocksFlashing / summary。
void finalize(PrecheckResult* result, const QString& passSummary)
{
    result->level = PrecheckLevel::Info;
    for (const PrecheckDetail& detail : std::as_const(result->details)) {
        if (static_cast<int>(detail.level) > static_cast<int>(result->level))
            result->level = detail.level;
    }
    result->ok = result->level != PrecheckLevel::Error;
    // 校验不一致一定是 Error（verify* 里已保证），这里再兜一道，
    // 免得将来有人只改一处就出现「标红了但不拦」的错位。
    if (result->checksumMismatch) {
        result->level = PrecheckLevel::Error;
        result->ok = false;
    }
    result->blocksFlashing = result->level == PrecheckLevel::Error;

    if (result->level == PrecheckLevel::Error) {
        QString first;
        for (const PrecheckDetail& detail : std::as_const(result->details)) {
            if (detail.level == PrecheckLevel::Error) { first = detail.message; break; }
        }
        result->summary = result->errorCount == 1
            ? QStringLiteral("预检失败：%1").arg(first)
            : QStringLiteral("预检失败：%1 项错误，%2").arg(result->errorCount).arg(first);
        return;
    }
    if (result->level == PrecheckLevel::Warning) {
        QString first;
        for (const PrecheckDetail& detail : std::as_const(result->details)) {
            if (detail.level == PrecheckLevel::Warning) { first = detail.message; break; }
        }
        result->summary = QStringLiteral("预检提醒：%1").arg(first);
        return;
    }
    result->summary = passSummary;
}

// ─────────────────────────────────────────────────────────────────────────────
//  三种输入各走一条
// ─────────────────────────────────────────────────────────────────────────────

PrecheckResult precheckDirectory(const QString& dirPath, const PrecheckOptions& options)
{
    PrecheckResult result;
    const QDir dir(dirPath);
    const QVector<QFileInfo> images = collectImages(dirPath);
    result.imageCount = images.size();

    // ── 1) 镜像本身：0 字节 ──────────────────────────────────────────────
    // 空 super.img 是动态分区包里的合法占位（见 RawProgramParser 的同类判断），
    // 其余镜像 0 字节就是坏包。
    for (const QFileInfo& info : images) {
        if (info.size() != 0) continue;
        const bool isSuper = info.completeBaseName().compare(QStringLiteral("super"),
                                                             Qt::CaseInsensitive) == 0;
        if (isSuper) {
            addDetail(&result, PrecheckLevel::Warning, info.absoluteFilePath(),
                      QStringLiteral("%1 是 0 字节占位（动态分区包常见，需包内带 META 定义）")
                          .arg(info.fileName()));
        } else {
            addDetail(&result, PrecheckLevel::Error, info.absoluteFilePath(),
                      QStringLiteral("%1 是 0 字节空文件").arg(info.fileName()));
        }
    }

    // ── 2) 关键镜像 ─────────────────────────────────────────────────────
    if (images.isEmpty()) {
        addDetail(&result, PrecheckLevel::Error, dirPath,
                  QStringLiteral("所选目录里没有 .img / .bin / .mbn 镜像文件"));
    } else {
        checkKeyImages(images, options, &result);
    }

    // ── 3) rawprogram XML 声明 size（有才查）────────────────────────────
    const QStringList xmlPaths = collectRawProgramXml(dirPath);
    for (const QString& xmlPath : xmlPaths) checkXmlDeclarations(xmlPath, options, &result);

    // ── 4) 旁挂 / 批量校验文件 ──────────────────────────────────────────
    const QStringList manifests = collectBatchManifests(dirPath);
    if (!manifests.isEmpty()) {
        const QString manifest = manifests.first();
        kc::crypto::HashAlgo algo = kc::crypto::HashAlgo::Sha256;
        algoForChecksumFile(manifest, &algo);
        result.checksumFile = manifest;
        verifyManifest(manifest, algo, options, &result);
        if (manifests.size() > 1) {
            addDetail(&result, PrecheckLevel::Info, manifests.at(1),
                      QStringLiteral("另有校验文件 %1，本次只核对了 %2")
                          .arg(QFileInfo(manifests.at(1)).fileName(),
                               QFileInfo(manifest).fileName()));
        }
    } else {
        // 逐文件同名旁挂（<镜像>.sha256 / <镜像>.md5）。
        int sidecarCount = 0;
        for (const QFileInfo& info : images) {
            const QStringList candidates = sidecarCandidates(info.absoluteFilePath());
            if (candidates.isEmpty()) continue;
            ++sidecarCount;
            kc::crypto::HashAlgo algo = kc::crypto::HashAlgo::Sha256;
            algoForChecksumFile(candidates.first(), &algo);
            if (result.checksumFile.isEmpty()) result.checksumFile = candidates.first();
            verifySidecar(info, candidates.first(), algo, options, &result);
        }
        if (sidecarCount == 0) {
            addDetail(&result, PrecheckLevel::Warning, dirPath,
                      QStringLiteral("未找到 .md5 / .sha256 校验文件（SHA256SUMS / MD5SUMS 或 "
                                     "「文件名.sha256」），无法确认镜像是否被截断或篡改"));
        }
    }

    const QString passSummary =
        QStringLiteral("预检通过：%1 个镜像%2")
            .arg(result.imageCount)
            .arg(result.checksumFile.isEmpty()
                     ? QStringLiteral("，未找到校验文件")
                     : QStringLiteral("，校验文件核对一致"));
    finalize(&result, passSummary);
    return result;
}

PrecheckResult precheckArchive(const QString& archivePath, const PrecheckOptions& options)
{
    PrecheckResult result;
    const QFileInfo info(archivePath);
    result.imageCount = 0;

    if (info.size() == 0) {
        addDetail(&result, PrecheckLevel::Error, archivePath,
                  QStringLiteral("压缩包是 0 字节空文件"));
        finalize(&result, QString());
        return result;
    }

    // 不解包：只能核对整包的旁挂摘要。
    const QStringList candidates = sidecarCandidates(archivePath);
    if (candidates.isEmpty()) {
        addDetail(&result, PrecheckLevel::Warning, archivePath,
                  QStringLiteral("压缩包只能核对整包摘要：未找到 %1.sha256 / .md5，"
                                 "包内文件无法逐个检查（本工程不解压 zip/7z）")
                      .arg(info.fileName()));
    } else {
        result.checksumFile = candidates.first();
        kc::crypto::HashAlgo algo = kc::crypto::HashAlgo::Sha256;
        algoForChecksumFile(candidates.first(), &algo);
        // 把压缩包自身当作「镜像」喂给旁挂核对：语义就是整包摘要。
        verifySidecar(info, candidates.first(), algo, options, &result);
    }

    const QString passSummary =
        QStringLiteral("预检通过：整包摘要一致（%1）").arg(info.fileName());
    finalize(&result, passSummary);
    return result;
}

PrecheckResult precheckSingleFile(const QString& filePath, const PrecheckOptions& options)
{
    PrecheckResult result;
    const QFileInfo info(filePath);
    result.imageCount = 1;

    if (info.size() == 0) {
        addDetail(&result, PrecheckLevel::Error, filePath,
                  QStringLiteral("%1 是 0 字节空文件").arg(info.fileName()));
    }

    const QStringList candidates = sidecarCandidates(filePath);
    if (candidates.isEmpty()) {
        addDetail(&result, PrecheckLevel::Warning, filePath,
                  QStringLiteral("未找到 %1.sha256 / .md5，无法确认文件是否完整")
                      .arg(info.fileName()));
    } else {
        result.checksumFile = candidates.first();
        kc::crypto::HashAlgo algo = kc::crypto::HashAlgo::Sha256;
        algoForChecksumFile(candidates.first(), &algo);
        verifySidecar(info, candidates.first(), algo, options, &result);
    }

    const QString passSummary = QStringLiteral("预检通过：%1").arg(info.fileName());
    finalize(&result, passSummary);
    return result;
}

} // namespace

QString precheckLevelText(PrecheckLevel level)
{
    switch (level) {
    case PrecheckLevel::Info:    return QStringLiteral("通过");
    case PrecheckLevel::Warning: return QStringLiteral("提醒");
    case PrecheckLevel::Error:   return QStringLiteral("失败");
    }
    return QStringLiteral("未知");
}

PrecheckResult precheckFirmware(const QString& path, const PrecheckOptions& options)
{
    PrecheckResult result;
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        addDetail(&result, PrecheckLevel::Error, QString(),
                  QStringLiteral("没有选择固件包"));
        finalize(&result, QString());
        return result;
    }

    const QFileInfo info(trimmed);
    if (!info.exists()) {
        addDetail(&result, PrecheckLevel::Error, trimmed,
                  QStringLiteral("路径不存在：%1").arg(QDir::toNativeSeparators(trimmed)));
        finalize(&result, QString());
        return result;
    }

    if (info.isDir()) return precheckDirectory(trimmed, options);
    if (isArchivePath(trimmed)) return precheckArchive(trimmed, options);
    if (isRawProgramXml(trimmed)) {
        // 单个 XML：按它声明的 size 逐个核对同目录的镜像。
        PrecheckResult xmlResult;
        const QString dirPath = info.absolutePath();
        const QVector<QFileInfo> images = collectImages(dirPath);
        xmlResult.imageCount = images.size();
        checkXmlDeclarations(info.absoluteFilePath(), options, &xmlResult);

        // XML 场景下也顺手核对一下同目录的批量校验文件（有就比）。
        const QStringList manifests = collectBatchManifests(dirPath);
        if (!manifests.isEmpty()) {
            const QString manifest = manifests.first();
            kc::crypto::HashAlgo algo = kc::crypto::HashAlgo::Sha256;
            algoForChecksumFile(manifest, &algo);
            xmlResult.checksumFile = manifest;
            verifyManifest(manifest, algo, options, &xmlResult);
        } else {
            addDetail(&xmlResult, PrecheckLevel::Warning, dirPath,
                      QStringLiteral("同目录没有 SHA256SUMS / MD5SUMS 校验文件，无法核对摘要"));
        }
        finalize(&xmlResult,
                 QStringLiteral("预检通过：%1 的 size 声明与磁盘一致").arg(info.fileName()));
        return xmlResult;
    }

    // 单个镜像文件 / 其它普通文件。
    return precheckSingleFile(trimmed, options);
}

} // namespace kc::tools
