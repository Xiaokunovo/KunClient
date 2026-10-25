#include "storage/SystemVersionReader.h"

#include <QByteArray>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>

namespace swt::edl {
namespace {

template <typename T> bool le(const QByteArray& data, qsizetype at, T* value)
{
    if (!value || at < 0 || at + static_cast<qsizetype>(sizeof(T)) > data.size()) return false;
    T raw{};
    std::memcpy(&raw, data.constData() + at, sizeof(T));
    *value = qFromLittleEndian(raw);
    return true;
}

QString fixedText(const QByteArray& data, qsizetype at, qsizetype count)
{
    if (at < 0 || count <= 0 || at + count > data.size()) return {};
    QByteArray text = data.mid(at, count);
    const qsizetype zero = text.indexOf('\0');
    if (zero >= 0) text.truncate(zero);
    return QString::fromLatin1(text).trimmed();
}

class Reader {
public:
    Reader(PartitionByteReader callback, quint64 size)
        : m_callback(std::move(callback)), m_size(size) {}

    QByteArray read(quint64 offset, quint32 length)
    {
        if (!m_callback || length == 0 || offset > m_size || length > m_size - offset) return {};
        QString error;
        QByteArray bytes = m_callback(offset, length, &error);
        if (bytes.size() != static_cast<qsizetype>(length)) {
            if (!error.isEmpty()) m_error = error;
            return {};
        }
        return bytes;
    }
    quint64 size() const { return m_size; }
    QString error() const { return m_error; }
private:
    PartitionByteReader m_callback;
    quint64 m_size = 0;
    QString m_error;
};

class Ext4Reader {
public:
    explicit Ext4Reader(Reader& reader) : m_reader(reader)
    {
        const QByteArray sb = m_reader.read(1024, 256);
        quint16 magic = 0; quint32 logBlock = 0, incompat = 0; quint16 inodeSize = 0;
        if (!le(sb, 56, &magic) || magic != 0xef53 || !le(sb, 24, &logBlock) || logBlock > 6 ||
            !le(sb, 40, &m_inodesPerGroup) || !le(sb, 88, &inodeSize) ||
            !le(sb, 96, &incompat)) return;
        m_blockSize = 1024U << logBlock;
        m_inodeSize = inodeSize ? inodeSize : 128;
        m_extents = (incompat & 0x40U) != 0;
        m_64bit = (incompat & 0x80U) != 0;
        quint16 descSize = 0;
        if (m_64bit) le(sb, 254, &descSize);
        m_descSize = qMax<quint32>(32, descSize);
        m_valid = m_inodesPerGroup > 0 && m_inodeSize >= 128;
    }
    bool valid() const { return m_valid; }
    QByteArray file(const QString& path)
    {
        quint32 inode = 2;
        for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            const QByteArray directory = inodeData(inode);
            // 🔴 不要用 isNull()：Qt6 已移除该 API，它只由 libQt6Core.a(removed_api.cpp.obj)
            //    提供，而那个目标文件又引用 QCryptographicHash::addData → 会把整个
            //    OpenSSL（实测 4.42 MB，占单文件 exe 11.3%）拖进产物。
            //    语义上「读失败」= 空，isEmpty() 覆盖 isNull() 且更严。
            if (directory.isEmpty()) return {};
            inode = findEntry(directory, part);
            if (!inode) return {};
        }
        return inodeData(inode);
    }
private:
    QByteArray inodeBytes(quint32 number)
    {
        if (!number) return {};
        const quint32 group = (number - 1) / m_inodesPerGroup;
        const quint32 index = (number - 1) % m_inodesPerGroup;
        const quint64 descAt = (m_blockSize == 1024 ? 2ULL * m_blockSize : m_blockSize) +
                               static_cast<quint64>(group) * m_descSize;
        const QByteArray desc = m_reader.read(descAt, m_descSize);
        quint32 tableLo = 0, tableHi = 0;
        if (!le(desc, 8, &tableLo)) return {};
        if (m_64bit && m_descSize >= 64) le(desc, 40, &tableHi);
        const quint64 table = tableLo | (static_cast<quint64>(tableHi) << 32);
        return m_reader.read(table * m_blockSize + static_cast<quint64>(index) * m_inodeSize,
                             m_inodeSize);
    }
    QByteArray inodeData(quint32 number)
    {
        const QByteArray inode = inodeBytes(number);
        quint32 lo = 0, hi = 0, flags = 0;
        if (!le(inode, 4, &lo) || !le(inode, 108, &hi) || !le(inode, 32, &flags)) return {};
        const quint64 size = lo | (static_cast<quint64>(hi) << 32);
        if (size > 64ULL * 1024ULL * 1024ULL) return {};
        if (size == 0) return QByteArray(0, '\0');
        const QByteArray blocks = inode.mid(40, 60);
        if (m_extents && (flags & 0x80000U)) return extentData(blocks, size);
        QByteArray result; result.reserve(static_cast<qsizetype>(size));
        for (int index = 0; index < 12 && static_cast<quint64>(result.size()) < size; ++index) {
            quint32 block = 0; if (!le(blocks, index * 4, &block) || !block) break;
            const quint32 count = static_cast<quint32>(qMin<quint64>(m_blockSize, size - result.size()));
            const QByteArray data = m_reader.read(static_cast<quint64>(block) * m_blockSize, count);
            if (data.size() != count) return {};
            result += data;
        }
        return result.size() == static_cast<qsizetype>(size) ? result : QByteArray{};
    }
    bool appendExtentNode(const QByteArray& node, quint64 maximum, QByteArray* out)
    {
        quint16 magic = 0, entries = 0, depth = 0;
        if (!le(node, 0, &magic) || magic != 0xf30a || !le(node, 2, &entries) ||
            !le(node, 6, &depth)) return false;
        for (quint16 index = 0; index < entries && static_cast<quint64>(out->size()) < maximum; ++index) {
            const qsizetype at = 12 + index * 12;
            if (depth == 0) {
                quint16 length = 0, startHi = 0; quint32 startLo = 0;
                if (!le(node, at + 4, &length) || !le(node, at + 6, &startHi) ||
                    !le(node, at + 8, &startLo)) return false;
                const quint64 block = startLo | (static_cast<quint64>(startHi) << 32);
                const quint32 blocks = length > 0x8000 ? length - 0x8000 : length;
                for (quint32 b = 0; b < blocks && static_cast<quint64>(out->size()) < maximum; ++b) {
                    const quint32 count = static_cast<quint32>(qMin<quint64>(m_blockSize, maximum - out->size()));
                    const QByteArray data = m_reader.read((block + b) * m_blockSize, count);
                    if (data.size() != count) return false;
                    out->append(data);
                }
            } else {
                quint32 leafLo = 0; quint16 leafHi = 0;
                if (!le(node, at + 4, &leafLo) || !le(node, at + 8, &leafHi)) return false;
                const quint64 leaf = leafLo | (static_cast<quint64>(leafHi) << 32);
                if (!appendExtentNode(m_reader.read(leaf * m_blockSize, m_blockSize), maximum, out)) return false;
            }
        }
        return true;
    }
    QByteArray extentData(const QByteArray& root, quint64 size)
    {
        QByteArray result; result.reserve(static_cast<qsizetype>(size));
        if (!appendExtentNode(root, size, &result) || result.size() != static_cast<qsizetype>(size)) return {};
        return result;
    }
    quint32 findEntry(const QByteArray& data, const QString& name)
    {
        qsizetype pos = 0;
        while (pos + 8 <= data.size()) {
            quint32 inode = 0; quint16 rec = 0;
            if (!le(data, pos, &inode) || !le(data, pos + 4, &rec) || rec < 8 || pos + rec > data.size()) break;
            const quint8 length = static_cast<quint8>(data.at(pos + 6));
            if (inode && length && pos + 8 + length <= data.size() &&
                QString::fromUtf8(data.constData() + pos + 8, length) == name) return inode;
            pos += rec;
        }
        return 0;
    }
    Reader& m_reader; quint32 m_blockSize = 0, m_inodeSize = 0, m_inodesPerGroup = 0,
        m_descSize = 0; bool m_extents = false, m_64bit = false, m_valid = false;
};

class ErofsReader {
public:
    explicit ErofsReader(Reader& reader) : m_reader(reader)
    {
        const QByteArray sb = m_reader.read(1024, 128);
        quint32 magic = 0;
        if (!le(sb, 0, &magic) || magic != 0xe0f5e1e2U || sb.size() < 81) return;
        m_bits = static_cast<quint8>(sb.at(12));
        if (m_bits < 9 || m_bits > 16) return;
        m_blockSize = 1U << m_bits;
        le(sb, 14, &m_root); le(sb, 40, &m_meta);
        quint32 incompat = 0; le(sb, 80, &incompat); m_zeroPadding = (incompat & 1U) != 0;
        m_valid = true;
    }
    bool valid() const { return m_valid; }
    QByteArray file(const QString& path)
    {
        quint64 nid = m_root;
        for (const QString& part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            const QByteArray directory = nidData(nid);
            // 同上：不用 isNull()（Qt6 已移除，会连带把 OpenSSL 拉进 exe）。
            if (directory.isEmpty()) return {};
            nid = findEntry(directory, part);
            if (nid == std::numeric_limits<quint64>::max()) return {};
        }
        return nidData(nid);
    }
private:
    struct CompactEntry {
        int type = -1;
        int clusterOffset = 0;
        qint64 physicalBlock = -1;
        quint64 nextPackOffset = 0;
    };

    static QPair<int, int> decodeCompactBits(int lowBits, const QByteArray& data,
                                              int bitPosition)
    {
        const int byteOffset = bitPosition / 8;
        const int bitOffset = bitPosition & 7;
        quint32 value = 0;
        for (int index = 0; index < 4 && byteOffset + index < data.size(); ++index) {
            value |= static_cast<quint32>(static_cast<quint8>(data.at(byteOffset + index)))
                     << (index * 8);
        }
        value >>= bitOffset;
        const quint32 mask = lowBits == 32 ? std::numeric_limits<quint32>::max()
                                           : ((1U << lowBits) - 1U);
        return {static_cast<int>(value & mask),
                static_cast<int>((value >> lowBits) & 3U)};
    }

    CompactEntry compactEntry(quint64 base, int logicalCluster, int total,
                              int clusterBits, bool compacted2Byte, bool bigCluster)
    {
        const int initial4Byte = static_cast<int>(((32ULL - base % 32ULL) / 4ULL) & 7ULL);
        int compacted2ByteCount = 0;
        if (compacted2Byte && initial4Byte < total)
            compacted2ByteCount = (total - initial4Byte) / 16 * 16;
        quint64 position = base;
        int shift = 2;
        int adjusted = logicalCluster;
        if (adjusted >= initial4Byte) {
            position += static_cast<quint64>(initial4Byte) * 4ULL;
            adjusted -= initial4Byte;
            if (adjusted < compacted2ByteCount) shift = 1;
            else {
                position += static_cast<quint64>(compacted2ByteCount) * 2ULL;
                adjusted -= compacted2ByteCount;
            }
        }
        position += static_cast<quint64>(adjusted) * (1ULL << shift);
        const int count = ((1 << shift) == 4 && clusterBits <= 14) ? 2
            : ((1 << shift) == 2 && clusterBits <= 12) ? 16 : 0;
        if (!count) return {};
        const int packSize = count << shift;
        const int bytesInPack = static_cast<int>(position & (packSize - 1));
        const quint64 packStart = position - bytesInPack;
        const int item = bytesInPack >> shift;
        const QByteArray pack = m_reader.read(packStart, packSize);
        if (pack.size() != packSize) return {};
        const int lowBits = qMax(clusterBits, 12);
        const int encodedBits = (packSize - 4) * 8 / count;
        const auto [low, type] = decodeCompactBits(lowBits, pack, encodedBits * item);
        if (type == 2) return {type, 1 << clusterBits, -1,
                              packStart + static_cast<quint64>(packSize)};
        int blocks = bigCluster ? 0 : 1;
        int cursor = item;
        while (cursor > 0) {
            --cursor;
            const auto [previousLow, previousType] =
                decodeCompactBits(lowBits, pack, encodedBits * cursor);
            if (previousType == 2) {
                if (bigCluster && (previousLow & 2048)) {
                    --cursor;
                    blocks += previousLow & ~2048;
                } else if (bigCluster) {
                    if (previousLow <= 1) break;
                    cursor -= previousLow - 2;
                } else {
                    cursor -= previousLow;
                }
            }
            if (!bigCluster && cursor >= 0) ++blocks;
            else if (bigCluster && previousType != 2) ++blocks;
        }
        quint32 firstBlock = 0;
        if (!le(pack, packSize - 4, &firstBlock)) return {};
        return {type, low, static_cast<qint64>(firstBlock) + blocks,
                packStart + static_cast<quint64>(packSize)};
    }

    static int lz4(const QByteArray& source, int sourceAt, int sourceLength,
                   QByteArray* target, int targetAt, int targetLength)
    {
        int s = sourceAt, d = targetAt, sEnd = sourceAt + sourceLength,
            dEnd = targetAt + targetLength;
        while (s < sEnd && d < dEnd) {
            const quint8 token = static_cast<quint8>(source.at(s++));
            int literal = token >> 4, match = token & 15;
            if (literal == 15) while (s < sEnd) { const quint8 v = source.at(s++); literal += v; if (v != 255) break; }
            if (s + literal > sEnd || d + literal > dEnd) return d - targetAt;
            std::memcpy(target->data() + d, source.constData() + s, literal); s += literal; d += literal;
            if (s + 2 > sEnd) break;
            const int distance = static_cast<quint8>(source.at(s)) |
                                 (static_cast<quint8>(source.at(s + 1)) << 8); s += 2;
            if (!distance || d - distance < targetAt) return -1;
            match += 4; if (match == 19) while (s < sEnd) { const quint8 v = source.at(s++); match += v; if (v != 255) break; }
            const int count = qMin(match, dEnd - d);
            for (int i = 0; i < count; ++i) (*target)[d + i] = (*target)[d - distance + i];
            d += count;
        }
        return d - targetAt;
    }
    quint64 inodeAt(quint64 nid) const { return static_cast<quint64>(m_meta) * m_blockSize + nid * 32ULL; }
    QByteArray nidData(quint64 nid)
    {
        const quint64 at = inodeAt(nid); const QByteArray hdr = m_reader.read(at, 64);
        quint16 format = 0, xattrs = 0; quint32 rawBlock = 0;
        if (!le(hdr, 0, &format) || !le(hdr, 2, &xattrs) || !le(hdr, 16, &rawBlock)) return {};
        const bool extended = (format & 1U) != 0; const int layout = (format >> 1) & 7;
        quint64 size = 0; if (extended) le(hdr, 8, &size); else { quint32 s = 0; le(hdr, 8, &s); size = s; }
        if (size > 64ULL * 1024ULL * 1024ULL) return {};
        if (!size) return QByteArray(0, '\0');
        const quint32 inodeSize = extended ? 64 : 32;
        const quint32 xattrSize = xattrs ? 12 + (xattrs - 1) * 4 : 0;
        const quint64 inlineAt = at + inodeSize + xattrSize;
        if (layout == 0 || layout == 4) return m_reader.read(static_cast<quint64>(rawBlock) * m_blockSize, static_cast<quint32>(size));
        if (layout == 2) {
            const quint32 tail = static_cast<quint32>(size % m_blockSize); const quint64 blockBytes = size - tail;
            QByteArray result; if (blockBytes) result = m_reader.read(static_cast<quint64>(rawBlock) * m_blockSize, static_cast<quint32>(blockBytes));
            if (tail) result += m_reader.read(inlineAt, tail);
            return result.size() == static_cast<qsizetype>(size) ? result : QByteArray{};
        }
        if (layout != 1 && layout != 3) return {};
        // Small build.prop files commonly use EROFS inline tail packing.
        const QByteArray map = m_reader.read(inlineAt, 8); quint16 inlineSize = 0, advise = 0;
        if (!le(map, 2, &inlineSize) || !le(map, 4, &advise) || map.size() < 8 || !inlineSize) return {};
        const int clusterBits = m_bits + (static_cast<quint8>(map.at(7)) & 15);
        const quint64 indexes = (inlineAt + 15ULL) & ~7ULL;
        const int totalIndexes = static_cast<int>((size + (1ULL << clusterBits) - 1) >> clusterBits);
        quint64 inlineData = indexes + static_cast<quint64>(totalIndexes) * 8ULL;
        int tailType = 1;
        if (layout == 3) {
            const CompactEntry tail = compactEntry(indexes, qMax(0, totalIndexes - 1),
                                                   totalIndexes, clusterBits,
                                                   (advise & 1U) != 0,
                                                   (advise & 10U) != 0);
            if (tail.type < 0 || !tail.nextPackOffset) return {};
            inlineData = tail.nextPackOffset;
            tailType = tail.type;
        }
        const QByteArray compressed = m_reader.read(inlineData, inlineSize);
        QByteArray output(static_cast<qsizetype>(size), '\0');
        if (tailType == 0 && compressed.size() >= output.size())
            return compressed.left(output.size());
        int sourceOffset = 0;
        if (m_zeroPadding) {
            while (sourceOffset < compressed.size() && compressed.at(sourceOffset) == 0)
                ++sourceOffset;
        }
        if (sourceOffset < compressed.size() &&
            lz4(compressed, sourceOffset, compressed.size() - sourceOffset,
                &output, 0, output.size()) >= output.size()) return output;
        if (compressed.size() >= output.size()) return compressed.left(output.size());
        return {};
    }
    quint64 findEntry(const QByteArray& data, const QString& name)
    {
        qsizetype pos = 0;
        while (pos < data.size()) {
            const qsizetype start = pos / m_blockSize * m_blockSize;
            const qsizetype end = qMin(start + static_cast<qsizetype>(m_blockSize), data.size());
            if (start + 12 > end) break;
            quint16 firstName = 0; if (!le(data, start + 8, &firstName) || firstName < 12 || start + firstName > end) { pos = end; continue; }
            const int count = firstName / 12;
            for (int i = 0; i < count; ++i) {
                const qsizetype d = start + i * 12; quint64 nid = 0; quint16 nameAt = 0, nextAt = 0;
                if (!le(data, d, &nid) || !le(data, d + 8, &nameAt)) continue;
                if (i + 1 < count) le(data, start + (i + 1) * 12 + 8, &nextAt); else nextAt = static_cast<quint16>(end - start);
                QByteArray n = data.mid(start + nameAt, nextAt - nameAt); while (n.endsWith('\0')) n.chop(1);
                if (QString::fromUtf8(n) == name) return nid;
            }
            pos = end;
        }
        return std::numeric_limits<quint64>::max();
    }
    Reader& m_reader; quint32 m_blockSize = 0, m_meta = 0; quint16 m_root = 0;
    int m_bits = 0; bool m_zeroPadding = false, m_valid = false;
};

QHash<QString, QString> parseProps(const QByteArray& bytes)
{
    QHash<QString, QString> props;
    for (const QByteArray& raw : bytes.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const qsizetype equals = line.indexOf('=');
        if (equals > 0) props.insert(QString::fromUtf8(line.left(equals)).trimmed().toLower(),
                                    QString::fromUtf8(line.mid(equals + 1)).trimmed());
    }
    return props;
}

QHash<QString, QString> readProperties(Reader& reader, QString* fileSystem)
{
    const QStringList paths{QStringLiteral("build.prop"), QStringLiteral("etc/build.prop"),
                            QStringLiteral("system/build.prop"), QStringLiteral("system/etc/build.prop")};
    const QByteArray extMagic = reader.read(1080, 2); quint16 em = 0; le(extMagic, 0, &em);
    if (em == 0xef53) {
        Ext4Reader ext4(reader); if (ext4.valid()) for (const QString& path : paths) {
            const QByteArray data = ext4.file(path); if (!data.isEmpty()) { if (fileSystem) *fileSystem = QStringLiteral("EXT4"); return parseProps(data); }
        }
    }
    const QByteArray eroMagic = reader.read(1024, 4); quint32 erm = 0; le(eroMagic, 0, &erm);
    if (erm == 0xe0f5e1e2U) {
        ErofsReader erofs(reader); if (erofs.valid()) for (const QString& path : paths) {
            const QByteArray data = erofs.file(path); if (!data.isEmpty()) { if (fileSystem) *fileSystem = QStringLiteral("EROFS"); return parseProps(data); }
        }
    }
    // Vendor images sometimes store a plain property block outside a mounted FS.
    const quint64 scanBytes = qMin<quint64>(reader.size(), 4ULL * 1024ULL * 1024ULL);
    for (quint64 at = 0; at < scanBytes; at += 1024 * 1024) {
        const QByteArray data = reader.read(at, static_cast<quint32>(qMin<quint64>(1024 * 1024, scanBytes - at)));
        if (data.contains("ro.build.version.release=") || data.contains("ro.product.brand=")) {
            if (fileSystem) *fileSystem = QStringLiteral("RAW");
            return parseProps(data);
        }
    }
    return {};
}

QString prop(const QHash<QString, QString>& props, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) { const QString value = props.value(QString::fromLatin1(key)); if (!value.trimmed().isEmpty()) return value.trimmed(); }
    return {};
}

void fill(SystemVersionInfo* out)
{
    const auto& p = out->properties;
    out->brand = prop(p, {"ro.product.brand", "ro.product.odm.brand", "ro.product.vendor.brand"});
    out->marketName = prop(p, {"ro.vendor.oplus.market.enname", "ro.vendor.oplus.market.name", "ro.config.marketing_name"});
    out->model = prop(p, {"ro.product.model", "ro.product.odm.model", "ro.product.vendor.model"});
    out->device = prop(p, {"ro.product.device", "ro.product.odm.device", "ro.vendor.product.device.oem"});
    out->project = prop(p, {"ro.separate.soft", "ro.vendor.product.oem"});
    out->platform = prop(p, {"ro.board.platform", "ro.product.oplus.cpuinfo"});
    out->androidVersion = prop(p, {"ro.build.version.release", "ro.product.build.version.release", "ro.build.version.release_or_codename"});
    out->systemVersion = prop(p, {"ro.build.display.id.show", "ro.build.display.id", "ro.build.version.oplusrom", "ro.build.version.oplusrom.display"});
    out->otaVersion = prop(p, {"ro.build.version.ota", "ro.build.display.ota"});
    out->securityPatch = prop(p, {"ro.build.version.security_patch", "ro.vendor.build.security_patch"});
    out->region = prop(p, {"ro.vendor.oplus.regionmark", "ro.oplus.image.system_ext.area"});
    out->buildDate = prop(p, {"ro.build.date", "ro.product.build.date", "ro.system.build.date"});
    out->fingerprint = prop(p, {"ro.build.fingerprint", "ro.product.build.fingerprint", "ro.system.build.fingerprint", "ro.vendor.build.fingerprint"});
}

struct LpExtent { quint64 sectors = 0, physicalSector = 0; quint32 type = 0; };
struct LpPart { QString name; QVector<LpExtent> extents; quint64 sectors = 0; };

QVector<LpPart> parseLp(Reader& super)
{
    const QByteArray geometry = super.read(4096, 4096); quint32 gm = 0, maxMeta = 0;
    if (!le(geometry, 0, &gm) || gm != 0x616c4467U || !le(geometry, 40, &maxMeta) || maxMeta < 128 || maxMeta > 4 * 1024 * 1024) return {};
    const QByteArray meta = super.read(12288, maxMeta); quint32 mm = 0, header = 0;
    if (!le(meta, 0, &mm) || mm != 0x414c5030U || !le(meta, 8, &header) || header < 128 || header > static_cast<quint32>(meta.size())) return {};
    quint32 po=0,pn=0,ps=0,eo=0,en=0,es=0;
    if (!le(meta,80,&po)||!le(meta,84,&pn)||!le(meta,88,&ps)||!le(meta,92,&eo)||!le(meta,96,&en)||!le(meta,100,&es)||ps<52||es<24) return {};
    QVector<LpExtent> all; all.reserve(en);
    for (quint32 i=0;i<en;++i) { const qsizetype at=header+eo+static_cast<qsizetype>(i)*es; LpExtent e; quint32 source=0; if(!le(meta,at,&e.sectors)||!le(meta,at+8,&e.type)||!le(meta,at+12,&e.physicalSector)||!le(meta,at+20,&source)) return {}; all.push_back(e); }
    QVector<LpPart> parts;
    for (quint32 i=0;i<pn;++i) { const qsizetype at=header+po+static_cast<qsizetype>(i)*ps; quint32 first=0,count=0; if(at+52>meta.size()||!le(meta,at+40,&first)||!le(meta,at+44,&count)||first+count>static_cast<quint32>(all.size())) return {}; LpPart part; part.name=fixedText(meta,at,36); for(quint32 x=0;x<count;++x){ part.extents.push_back(all.at(first+x)); part.sectors += all.at(first+x).sectors; } if(part.sectors) parts.push_back(part); }
    return parts;
}

QString baseName(QString name) { name=name.toLower(); if(name.endsWith("_a")||name.endsWith("_b")) name.chop(2); return name; }

} // namespace

bool SystemVersionReader::readPhysical(const PartitionByteReader& callback, quint64 bytes,
                                       SystemVersionInfo* result, QString* error)
{
    if (!result) return false;
    Reader reader(callback, bytes); QString fs; const auto props = readProperties(reader, &fs);
    if (props.isEmpty()) { if (error) *error = reader.error().isEmpty() ? QStringLiteral("分区中未识别到 build.prop") : reader.error(); return false; }
    for (auto it=props.cbegin();it!=props.cend();++it) if(!result->properties.contains(it.key())) result->properties.insert(it.key(),it.value());
    if (!fs.isEmpty() && !result->fileSystems.contains(fs)) result->fileSystems.push_back(fs);
    fill(result); return true;
}

bool SystemVersionReader::readSuper(const PartitionByteReader& callback, quint64 bytes,
                                    SystemVersionInfo* result, QString* error,
                                    const std::function<void(const QString&)>& status)
{
    if (!result) return false;
    Reader super(callback, bytes); const QVector<LpPart> parts=parseLp(super);
    if(parts.isEmpty()){ if(error)*error=QStringLiteral("super 动态分区元数据解析失败"); return false; }
    const QStringList priority{QStringLiteral("my_manifest"),QStringLiteral("odm"),QStringLiteral("my_product"),QStringLiteral("vendor"),QStringLiteral("product"),QStringLiteral("system_ext"),QStringLiteral("system")};
    for(const QString& wanted:priority){ for(const LpPart& part:parts){ if(baseName(part.name)!=wanted) continue; if(status)status(QStringLiteral("解析 %1 版本信息…").arg(part.name));
        quint64 logicalSize=part.sectors*512ULL;
        PartitionByteReader logical=[callback,part](quint64 offset,quint32 length,QString* readError){ QByteArray out; out.reserve(length); quint64 logical=0; quint64 current=offset; quint32 remaining=length; for(const LpExtent& extent:part.extents){ const quint64 extentBytes=extent.sectors*512ULL; if(current>=logical+extentBytes){ logical+=extentBytes; continue; } const quint64 within=current-logical; const quint32 take=static_cast<quint32>(qMin<quint64>(remaining,extentBytes-within)); if(extent.type==1) out.append(QByteArray(take,'\0')); else if(extent.type==0){ QByteArray data=callback(extent.physicalSector*512ULL+within,take,readError); if(data.size()!=take)return QByteArray{}; out+=data; } else return QByteArray{}; current+=take; remaining-=take; logical+=extentBytes; if(!remaining)break; } return remaining?QByteArray{}:out; };
        SystemVersionInfo one; QString localError; if(readPhysical(logical,logicalSize,&one,&localError)){ for(auto it=one.properties.cbegin();it!=one.properties.cend();++it)if(!result->properties.contains(it.key()))result->properties.insert(it.key(),it.value()); for(const QString& fs:one.fileSystems)if(!result->fileSystems.contains(fs))result->fileSystems.push_back(fs); }
    }}
    if(result->properties.isEmpty()){if(error)*error=QStringLiteral("动态分区中未读取到 build.prop");return false;} fill(result); return true;
}

} // namespace swt::edl
