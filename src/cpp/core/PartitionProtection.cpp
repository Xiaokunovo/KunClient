#include "core/PartitionProtection.h"

#include <QFileInfo>
#include <QSet>

namespace swt::edl {

QString normalizedPartitionName(QString name)
{
    name = QFileInfo(name.trimmed()).completeBaseName().toLower();
    if (name.endsWith(QStringLiteral("_a")) ||
        name.endsWith(QStringLiteral("_b"))) {
        name.chop(2);
    }
    return name;
}

QStringList protectedPartitionNames()
{
    // Violet/SmartTool baseband fingerprint and calibration backup policy,
    // plus its EDL sensitive-image list.
    return {
        QStringLiteral("persist"),
        QStringLiteral("persistbak"),
        QStringLiteral("ocdt"),
        QStringLiteral("modemst1"),
        QStringLiteral("modemst2"),
        QStringLiteral("fsg"),
        QStringLiteral("fsc"),
        QStringLiteral("oplusdycnvbk"),
        QStringLiteral("oplusstanvbk"),
        QStringLiteral("oplusreserve4"),
        QStringLiteral("oplusreserve5"),
        QStringLiteral("param"),
        QStringLiteral("preisp_otp"),
        QStringLiteral("preisp_dt"),
        QStringLiteral("preisp_dt_bk")
    };
}

bool isProtectedPartitionName(const QString& name)
{
    static const QSet<QString> names = [] {
        const QStringList values = protectedPartitionNames();
        return QSet<QString>(values.cbegin(), values.cend());
    }();
    return names.contains(normalizedPartitionName(name));
}

bool isProtectedPartitionMapping(const QString& label, const QString& filename)
{
    return isProtectedPartitionName(label) ||
           (!filename.trimmed().isEmpty() && isProtectedPartitionName(filename));
}

} // namespace swt::edl
