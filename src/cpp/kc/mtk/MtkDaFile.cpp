#include "kc/mtk/MtkDaFile.h"

#include "kc/mtk/MtkTypes.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

namespace kc::mtk {
namespace {

constexpr quint64 kDaEntryRegionBytes = 20; // 5 × u32

uint16_t rdU16(const QByteArray& buf, int offset)
{
    if (offset < 0 || offset + 2 > buf.size()) return 0;
    return qFromLittleEndian<uint16_t>(buf.constData() + offset);
}

uint32_t rdU32(const QByteArray& buf, int offset)
{
    if (offset < 0 || offset + 4 > buf.size()) return 0;
    return qFromLittleEndian<uint32_t>(buf.constData() + offset);
}

} // namespace

bool DaRegion::isSane(quint64 fileSize) const
{
    if (len == 0) return false;
    if (sigLen > len) return false;
    if (static_cast<quint64>(buf) + static_cast<quint64>(len) > fileSize) return false;
    return true;
}

const DaRegion* DaEntry::regionAt(int index) const
{
    if (index < 0 || index >= regions.size()) return nullptr;
    return &regions.at(index);
}

bool DaFile::looksLikeDaFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 %1：%2").arg(path, file.errorString());
        return false;
    }
    const QByteArray head = file.read(0x30);
    file.close();
    if (head.contains("MTK_DOWNLOAD_AGENT") || head.contains("MTK_DA")) return true;
    if (error) {
        *error = QStringLiteral("%1 不像 DA 文件（头部没有 MTK_DOWNLOAD_AGENT / MTK_DA 标识）")
                     .arg(QFileInfo(path).fileName());
    }
    return false;
}

bool DaFile::parse(const QString& path, QVector<DaEntry>* entries, QString* error)
{
    if (entries) entries->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 %1：%2").arg(path, file.errorString());
        return false;
    }
    const quint64 fileSize = static_cast<quint64>(file.size());
    if (fileSize < kDaEntryTableOffset + kDaEntrySize) {
        if (error) {
            *error = QStringLiteral("%1 太小（%2 字节），不像 DA 文件")
                         .arg(QFileInfo(path).fileName())
                         .arg(fileSize);
        }
        return false;
    }

    const QByteArray head = file.read(static_cast<qint64>(kDaFileHeaderSize));
    if (head.size() != static_cast<int>(kDaFileHeaderSize)) {
        if (error) *error = QStringLiteral("读取 DA 文件头失败");
        return false;
    }
    const bool v6 = head.contains("MTK_DA_v6");

    const QByteArray countRaw = file.read(4);
    if (countRaw.size() != 4) {
        if (error) *error = QStringLiteral("读取 DA 条目数失败");
        return false;
    }
    const uint32_t countDa = qFromLittleEndian<uint32_t>(countRaw.constData());
    if (countDa == 0 || countDa > 256) {
        if (error) *error = QStringLiteral("DA 条目数异常：%1").arg(countDa);
        return false;
    }

    // 旧式 / 新式条目长度：用「第二个条目的 magic 位置」判定（参考实现同口径）。
    bool legacy = false;
    if (file.seek(static_cast<qint64>(kDaEntryTableOffset + kDaEntrySizeLegacy))) {
        const QByteArray probe = file.read(2);
        if (probe.size() == 2
            && static_cast<uint8_t>(probe.at(0)) == 0xDA
            && static_cast<uint8_t>(probe.at(1)) == 0xDA) {
            legacy = true;
        }
    }
    const uint32_t entrySize = legacy ? kDaEntrySizeLegacy : kDaEntrySize;

    QVector<DaEntry> parsed;
    parsed.reserve(static_cast<int>(countDa));

    for (uint32_t index = 0; index < countDa; ++index) {
        const quint64 offset = kDaEntryTableOffset + static_cast<quint64>(index) * entrySize;
        if (offset + entrySize > fileSize) break;
        if (!file.seek(static_cast<qint64>(offset))) break;
        const QByteArray record = file.read(entrySize);
        if (record.size() != static_cast<int>(entrySize)) break;

        DaEntry entry;
        entry.legacy = legacy;
        entry.v6 = v6;
        entry.magic = rdU16(record, 0x00);
        entry.hwCode = rdU16(record, 0x02);
        entry.hwSubCode = rdU16(record, 0x04);
        entry.hwVersion = rdU16(record, 0x06);

        int pos = legacy ? 0x08 : 0x0C;
        if (!legacy) entry.swVersion = rdU16(record, 0x08);
        entry.pagesize = rdU16(record, pos); pos += 2;
        // 跳过 reserved3
        pos += 2;
        entry.entryRegionIndex = rdU16(record, pos); pos += 2;
        entry.entryRegionCount = rdU16(record, pos); pos += 2;

        if (entry.magic != kDaEntryMagic) continue; // 空槽 / 错位，跳过
        if (entry.entryRegionCount == 0) continue;
        if (pos + static_cast<int>(entry.entryRegionCount * kDaEntryRegionBytes)
            > record.size()) {
            continue; // region 表越界 → 条目损坏，跳过而不是判死整包
        }

        entry.regions.reserve(entry.entryRegionCount);
        for (int r = 0; r < entry.entryRegionCount; ++r) {
            const int base = pos + r * static_cast<int>(kDaEntryRegionBytes);
            DaRegion region;
            region.buf = rdU32(record, base + 0x00);
            region.len = rdU32(record, base + 0x04);
            region.startAddr = rdU32(record, base + 0x08);
            region.startOffset = rdU32(record, base + 0x0C);
            region.sigLen = rdU32(record, base + 0x10);
            entry.regions.append(region);
        }
        parsed.append(entry);
    }

    if (parsed.isEmpty()) {
        if (error) {
            *error = QStringLiteral("%1 里没有可用的 DA 条目（magic 0xDADA 未命中）")
                         .arg(QFileInfo(path).fileName());
        }
        return false;
    }

    if (entries) *entries = parsed;
    return true;
}

bool DaFile::readRegion(const QString& path, const DaRegion& region, QByteArray* out, QString* error)
{
    if (out) out->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 %1：%2").arg(path, file.errorString());
        return false;
    }
    const quint64 fileSize = static_cast<quint64>(file.size());
    if (!region.isSane(fileSize)) {
        if (error) {
            *error = QStringLiteral("DA region 越界（offset=0x%1 len=0x%2 文件=0x%3）")
                         .arg(region.buf, 0, 16)
                         .arg(region.len, 0, 16)
                         .arg(fileSize, 0, 16);
        }
        return false;
    }
    if (!file.seek(static_cast<qint64>(region.buf))) {
        if (error) *error = QStringLiteral("seek 到 0x%1 失败").arg(region.buf, 0, 16);
        return false;
    }
    const QByteArray data = file.read(static_cast<qint64>(region.len));
    if (data.size() != static_cast<int>(region.len)) {
        if (error) {
            *error = QStringLiteral("读取 DA region 不完整（%1/%2）").arg(data.size()).arg(region.len);
        }
        return false;
    }
    if (out) *out = data;
    return true;
}

const DaEntry* DaFile::pickForHwCode(const QVector<DaEntry>& entries, uint16_t hwCode, QString* why){
    const DaEntry* best = nullptr;
    for (const DaEntry& entry : entries) {
        if (entry.hwCode != hwCode) continue;
        if (entry.da1() == nullptr || entry.da2() == nullptr) continue;
        if (best == nullptr) {
            best = &entry;
            continue;
        }
        // 同 hw code 取 hw_version 最大、再取 sw_version 最大
        if (entry.hwVersion > best->hwVersion
            || (entry.hwVersion == best->hwVersion && entry.swVersion > best->swVersion)) {
            best = &entry;
        }
    }
    if (best) return best;

    if (why) {
        QStringList available;
        for (const DaEntry& entry : entries) {
            available << QStringLiteral("0x%1").arg(entry.hwCode, 4, 16, QLatin1Char('0'));
        }
        *why = QStringLiteral("DA 文件里没有 hw code 0x%1 的条目（现有：%2）")
                   .arg(hwCode, 4, 16, QLatin1Char('0'))
                   .arg(available.isEmpty() ? QStringLiteral("无") : available.join(QStringLiteral(", ")));
    }
    return nullptr;
}

QByteArray extractEmiFromPreloader(const QByteArray& preloader)
{
    QByteArray data = preloader;

    const QByteArray emiMagic("\x4D\x4D\x4D\x01\x38\x00\x00\x00", 8);
    const int idx = data.indexOf(emiMagic);
    if (idx >= 0) {
        data = data.mid(idx);
        if (data.size() < 0x30) return QByteArray();
        const uint32_t mlen = rdU32(data, 0x20);
        const uint32_t siglen = rdU32(data, 0x2C);
        if (mlen <= siglen || mlen > static_cast<uint32_t>(data.size())) return QByteArray();
        data = data.left(static_cast<int>(mlen - siglen));

        if (data.size() < 4) return QByteArray();
        uint32_t dramsize = rdU32(data, data.size() - 4);
        if (dramsize == 0) {
            if (data.size() <= 0x800) return QByteArray();
            data = data.left(data.size() - 0x800);
            dramsize = rdU32(data, data.size() - 4);
        }
        const int start = data.size() - static_cast<int>(dramsize) - 4;
        if (dramsize == 0 || start < 0) return QByteArray();
        data = data.mid(start, static_cast<int>(dramsize));
    }

    const QByteArray bldr("MTK_BLOADER_INFO_v");
    const int bldrIdx = data.indexOf(bldr);
    if (bldrIdx < 0) return QByteArray();

    // xflash 体系：bldr 串就在开头 → EMI 数据就是本身
    if (bldrIdx == 0) return data;

    const int binIdx = data.indexOf("MTK_BIN");
    if (binIdx >= 0 && binIdx + 0xC < data.size()) return data.mid(binIdx + 0xC);
    return QByteArray();
}

} // namespace kc::mtk
