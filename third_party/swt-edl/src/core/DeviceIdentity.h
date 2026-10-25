#pragma once

#include <QString>
#include <QStringList>

namespace swt::edl {

// Extracts and normalizes a Qualcomm marketing chip code such as SM8250.
QString normalizeQualcommChipName(const QString& text);

// Converts the Sahara MSM_ID field to a known Qualcomm marketing chip code.
QString qualcommChipFromMsmId(const QString& msmId);

// Adds the Snapdragon family name when it is known.
QString qualcommChipDisplayName(const QString& chipName);

// Uses directory/file-name hints first, then scans the programmer's embedded
// ASCII metadata. This is useful for VIP sessions where entering Sahara command
// mode would consume the programmer watchdog window.
QString detectProgrammerChip(const QString& programmerPath,
                             const QStringList& pathHints = {});

} // namespace swt::edl
