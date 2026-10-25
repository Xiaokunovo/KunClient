#include "kc/devices/StorageBrandProbe.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVector>

#include <algorithm>

namespace kc::devices {
namespace {

/// 厂商串规整：去首尾空白、折叠内部空白、去掉行尾标点、统一大写。
/// loader 的打印形态很杂（"SAMSUNG" / "Samsung " / "SK HYNIX"），
/// 不规整的话同一种芯片会显示成好几种牌子。
QString cleanVendor(QString raw)
{
    raw = raw.trimmed();
    while (!raw.isEmpty()) {
        const QChar last = raw.at(raw.size() - 1);
        if (last == QLatin1Char('.') || last == QLatin1Char(',') ||
            last == QLatin1Char(';') || last == QLatin1Char('"')) {
            raw.chop(1);
            raw = raw.trimmed();
        } else {
            break;
        }
    }
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    raw.replace(spaces, QStringLiteral(" "));
    return raw.toUpper();
}

struct BrandEntry {
    const char* key;   ///< 厂商串里出现的特征子串（大写）
    const char* zh;
    const char* en;
};

/// 存储芯片厂牌表。key 用**包含**匹配：loader 常打印
/// "SAMSUNG ELECTRONICS" / "SK HYNIX INC." 这种带后缀的形态。
/// 顺序有意义 —— "HYNIX" 必须排在 "SK HYNIX" 之前才能覆盖两种写法。
const BrandEntry kBrands[] = {
    { "SAMSUNG",         "三星",       "Samsung" },
    { "MICRON",          "美光",       "Micron" },
    { "HYNIX",           "SK 海力士",  "SK hynix" },
    { "KIOXIA",          "铠侠",       "Kioxia" },
    { "TOSHIBA",         "铠侠",       "Kioxia" },      // 东芝存储 2019 改名铠侠
    { "SANDISK",         "闪迪",       "SanDisk" },
    { "WESTERN",         "西部数据",   "Western Digital" },
    { "WDC",             "西部数据",   "Western Digital" },
    { "YMTC",            "长江存储",   "YMTC" },
    { "YANGTZE",         "长江存储",   "YMTC" },
    { "LONGSYS",         "江波龙",     "Longsys" },
    { "FORESEE",         "江波龙",     "Longsys" },
    { "BIWIN",           "佰维",       "BIWIN" },
    { "KINGSTON",        "金士顿",     "Kingston" },
    { "NANYA",           "南亚",       "Nanya" },
    { "WINBOND",         "华邦",       "Winbond" },
    { "MACRONIX",        "旺宏",       "Macronix" },
    { "ADATA",           "威刚",       "ADATA" },
    { "PHISON",          "群联",       "Phison" },
    { "SILICON MOTION",  "慧荣",       "Silicon Motion" },
    { "SILICONMOTION",   "慧荣",       "Silicon Motion" },
};

const BrandEntry* findBrand(const QString& vendor)
{
    if (vendor.isEmpty()) return nullptr;
    const QString upper = vendor.toUpper();
    for (const BrandEntry& entry : kBrands) {
        if (upper.contains(QLatin1String(entry.key))) return &entry;
    }
    return nullptr;
}

int naturalPortNumber(const QString& name)
{
    static const QRegularExpression digits(QStringLiteral("(\\d+)"));
    const auto match = digits.match(name);
    bool ok = false;
    const int value = match.hasMatch() ? match.captured(1).toInt(&ok) : 0;
    return ok ? value : 9999;
}

} // namespace

StorageBrandResult parseStorageBrand(const QString& text)
{
    StorageBrandResult result;
    if (text.isEmpty()) return result;

    // 带厂商：`INFO: ufs: SAMSUNG`。厂商串止于引号 / 尖括号 / 换行 —— 与
    // MiUfsProbe 的 StorageProbeResult.FromText 同一口径。
    static const QRegularExpression vendorPattern(
        QStringLiteral("INFO:\\s*(ufs|emmc)\\s*:\\s*([^\"<\\r\\n]+)"),
        QRegularExpression::CaseInsensitiveOption);
    // 只有类型：`INFO: emmc` 单独成行（尾引号可能已被 XML 解析器吃掉，所以
    // 末尾也要算边界）。
    static const QRegularExpression typePattern(
        QStringLiteral("INFO:\\s*(ufs|emmc)(?=[\"<\\r\\n]|$)"),
        QRegularExpression::CaseInsensitiveOption);
    // 芯片序列号：有些 loader 只打这一行（没有 Chip ID）。
    static const QRegularExpression serialPattern(
        QStringLiteral("Chip\\s+serial\\s+num\\s*\\(([^)]+)\\)"),
        QRegularExpression::CaseInsensitiveOption);

    const auto vendorMatch = vendorPattern.match(text);
    if (vendorMatch.hasMatch()) {
        result.storageType = vendorMatch.captured(1).toUpper();
        result.vendor = cleanVendor(vendorMatch.captured(2));
        result.rawLine = vendorMatch.captured(0).trimmed();
    } else {
        const auto typeMatch = typePattern.match(text);
        if (typeMatch.hasMatch()) {
            result.storageType = typeMatch.captured(1).toUpper();
            result.rawLine = typeMatch.captured(0).trimmed();
        }
    }

    const auto serialMatch = serialPattern.match(text);
    if (serialMatch.hasMatch()) result.chipSerial = serialMatch.captured(1).trimmed();

    return result;
}

QString storageBrandChineseName(const QString& vendor)
{
    const BrandEntry* entry = findBrand(vendor);
    return entry ? QString::fromUtf8(entry->zh) : QString();
}

QString storageBrandEnglishName(const QString& vendor)
{
    const BrandEntry* entry = findBrand(vendor);
    return entry ? QString::fromUtf8(entry->en) : QString();
}

QString storageBrandTitle(const StorageBrandResult& result)
{
    if (result.vendor.isEmpty()) return QString();
    const QString zh = storageBrandChineseName(result.vendor);
    const QString en = storageBrandEnglishName(result.vendor);
    if (zh.isEmpty()) return result.vendor;
    if (en.isEmpty()) return zh;
    return QStringLiteral("%1 %2").arg(zh, en);
}

QString findProgrammerInDirectory(const QString& dir)
{
    if (dir.trimmed().isEmpty()) return QString();

    QDir root(dir);
    if (!root.exists()) return QString();

    // ROM 根目录 → 下钻到 images（名字里带 images 的子目录都算，小米包常见
    // "<机型>_images_<版本>" 这种命名）。
    QDir imageDir = root;
    if (root.dirName().compare(QStringLiteral("images"), Qt::CaseInsensitive) != 0) {
        const QStringList subDirs =
            root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& sub : subDirs) {
            if (!sub.contains(QStringLiteral("images"), Qt::CaseInsensitive)) continue;
            imageDir = QDir(root.filePath(sub));
            break;
        }
    }

    // MiUfsProbe 的 ProgrammerPattern（原样照抄，含 xbl_*devprg* 那条）。
    static const QRegularExpression programmerPattern(
        QStringLiteral("^(prog_.*_firehose_.*|prog_.*firehose_.*|prog_firehose_.*\\.elf|"
                       "xbl_.*devprg_.*)$"),
        QRegularExpression::CaseInsensitiveOption);

    struct Candidate {
        QString path;
        int rank;
        QString name;
    };
    QVector<Candidate> candidates;
    const QFileInfoList files = imageDir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& info : files) {
        if (!programmerPattern.match(info.fileName()).hasMatch()) continue;
        const QString lower = info.fileName().toLower();
        int rank = 2;
        if (lower == QStringLiteral("prog_firehose_lite.elf")) {
            rank = 0;   // lite 版最通用：小米多数机型都吃得下
        } else if (lower.contains(QStringLiteral("devprg"))) {
            rank = 1;
        }
        candidates.push_back({ info.absoluteFilePath(), rank, lower });
    }
    if (candidates.isEmpty()) return QString();

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.rank != b.rank) return a.rank < b.rank;
                  return a.name < b.name;
              });
    return candidates.first().path;
}

bool isQualcommEdlPort(const QSerialPortInfo& port)
{
    if (port.hasVendorIdentifier() && port.hasProductIdentifier()) {
        if (port.vendorIdentifier() == 0x05c6 && port.productIdentifier() == 0x9008) {
            return true;
        }
    }
    const QString fingerprint = port.description() + QLatin1Char(' ') + port.manufacturer() +
                                QLatin1Char(' ') + port.portName();
    static const char* const kHints[] = {
        "9008", "QDLoader", "Qualcomm HS-USB", "QUSB_BULK", "Sahara", "Firehose",
    };
    for (const char* hint : kHints) {
        if (fingerprint.contains(QLatin1String(hint), Qt::CaseInsensitive)) return true;
    }
    return false;
}

QStringList availableEdlPorts()
{
    QStringList ports;
    const auto available = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& port : available) {
        if (isQualcommEdlPort(port)) ports.push_back(port.portName());
    }
    std::sort(ports.begin(), ports.end(), [](const QString& a, const QString& b) {
        const int left = naturalPortNumber(a);
        const int right = naturalPortNumber(b);
        if (left != right) return left < right;
        return a < b;
    });
    return ports;
}

} // namespace kc::devices
