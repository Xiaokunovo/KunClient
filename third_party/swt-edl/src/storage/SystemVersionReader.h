#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>

namespace swt::edl {

using PartitionByteReader =
    std::function<QByteArray(quint64 offset, quint32 length, QString* error)>;

struct SystemVersionInfo {
    QString brand;
    QString marketName;
    QString model;
    QString device;
    QString project;
    QString platform;
    QString androidVersion;
    QString systemVersion;
    QString otaVersion;
    QString securityPatch;
    QString region;
    QString buildDate;
    QString fingerprint;
    QStringList fileSystems;
    QHash<QString, QString> properties;

    [[nodiscard]] bool isEmpty() const { return properties.isEmpty(); }
};

class SystemVersionReader final {
public:
    static bool readPhysical(const PartitionByteReader& reader, quint64 partitionBytes,
                             SystemVersionInfo* result, QString* error);
    static bool readSuper(const PartitionByteReader& superReader, quint64 superBytes,
                          SystemVersionInfo* result, QString* error,
                          const std::function<void(const QString&)>& status = {});
};

} // namespace swt::edl
