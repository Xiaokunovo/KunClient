#include "core/EdlMaintenance.h"

#include <cstring>

namespace swt::edl {

QByteArray buildBcbPayload(const QString& command)
{
    // command[32]：最多 31 字符 + NUL。超长一律拒绝 —— 截断会写出一个
    // bootloader 不认识的命令，设备下一轮启动行为完全不可预期。
    const QByteArray utf8 = command.toUtf8();
    if (utf8.isEmpty() || utf8.size() > 31) return {};
    QByteArray payload(static_cast<qsizetype>(BcbBlockBytes), '\0');
    std::memcpy(payload.data(), utf8.constData(), static_cast<size_t>(utf8.size()));
    return payload;
}

DevinfoUnlockResult applyDevinfoUnlock(QByteArray* sector, QString* error)
{
    if (!sector) {
        if (error) *error = QStringLiteral("devinfo 扇区缓冲区为空");
        return DevinfoUnlockResult::Rejected;
    }
    if (sector->size() <= static_cast<qsizetype>(DevinfoUnlockByteOffset)) {
        if (error) {
            *error = QStringLiteral(
                         "devinfo 扇区只有 %1 字节，够不到解锁开关（偏移 %2）")
                         .arg(sector->size())
                         .arg(DevinfoUnlockByteOffset);
        }
        return DevinfoUnlockResult::Rejected;
    }
    const uchar current = static_cast<uchar>(sector->at(
        static_cast<qsizetype>(DevinfoUnlockByteOffset)));
    if (current == 0x01) return DevinfoUnlockResult::AlreadyUnlocked;
    if (current != 0x00) {
        if (error) {
            *error = QStringLiteral(
                         "devinfo 解锁字节为 0x%1，既非锁定（0x00）也非解锁（0x01）——"
                         "格式与已知实现不一致，拒绝改写")
                         .arg(current, 2, 16, QLatin1Char('0'));
        }
        return DevinfoUnlockResult::Rejected;
    }
    (*sector)[static_cast<qsizetype>(DevinfoUnlockByteOffset)] =
        static_cast<char>(0x01);
    return DevinfoUnlockResult::Changed;
}

QStringList factoryResetPartitionNames()
{
    return {
        QStringLiteral("userdata"),
        QStringLiteral("metadata"),
    };
}

QStringList basebandBackupPartitionNames()
{
    return {
        QStringLiteral("modem"),
        QStringLiteral("modemst1"),
        QStringLiteral("modemst2"),
        QStringLiteral("fsg"),
        QStringLiteral("fsc"),
        QStringLiteral("bluetooth"),
    };
}

QString gptFileName(quint32 lun, bool backup)
{
    return backup ? QStringLiteral("gpt_backup%1.bin").arg(lun)
                  : QStringLiteral("gpt_main%1.bin").arg(lun);
}

} // namespace swt::edl
