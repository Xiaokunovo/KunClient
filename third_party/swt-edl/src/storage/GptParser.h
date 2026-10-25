#pragma once

#include "core/Types.h"

#include <QByteArrayView>
#include <QString>

namespace swt::edl {

class GptParser final {
public:
    static bool parse(QByteArrayView data, quint32 sectorSize, quint32 lun,
                      QVector<PartitionInfo>* partitions, QString* error);
};

} // namespace swt::edl

