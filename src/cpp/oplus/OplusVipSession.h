#pragma once

#include "core/Types.h"

namespace swt::edl {

// Package discovery for the native OPPO VIP protocol implemented by FirehoseClient.
class OplusVipSession final {
public:
    static bool discoverPackage(const QString& directory, StorageType storage,
                                OplusVipPackage* package, QString* error = nullptr,
                                bool recursive = true,
                                const QString& preferredProgrammerPath = {});
};

} // namespace swt::edl
