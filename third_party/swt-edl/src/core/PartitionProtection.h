#pragma once

#include <QString>
#include <QStringList>

namespace swt::edl {

// Central partition protection policy.  Slot suffixes are ignored so the same
// policy covers A/B and non-A/B devices.
QString normalizedPartitionName(QString name);
QStringList protectedPartitionNames();
bool isProtectedPartitionName(const QString& name);
bool isProtectedPartitionMapping(const QString& label, const QString& filename = {});

} // namespace swt::edl
