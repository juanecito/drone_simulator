// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/input_profile_store.hpp"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include "store_util.hpp"

namespace ds::storage {
namespace {

constexpr const char* kSelect =
    "SELECT id, device_guid, vehicle_type, name, mapping_json FROM input_profiles ";

InputProfileRecord readRow(const QSqlQuery& query) {
    InputProfileRecord r;
    r.id = query.value(0).toLongLong();
    r.deviceGuid = query.value(1).toString().toStdString();
    r.vehicleType = query.value(2).toString().toStdString();
    r.name = query.value(3).toString().toStdString();
    r.mappingJson = query.value(4).toString().toStdString();
    return r;
}

}  // namespace

StorageResult<std::vector<InputProfileRecord>> InputProfileStore::list(
    std::optional<std::string_view> deviceGuid) {
    auto query = connection_->prepare(std::string(kSelect) +
                                      "WHERE :all = 1 OR device_guid = :guid "
                                      "ORDER BY device_guid, vehicle_type, name COLLATE NOCASE");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":all"), deviceGuid ? 0 : 1);
    query->bindValue(QStringLiteral(":guid"), toQString(deviceGuid.value_or("")));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<InputProfileRecord> records;
    while (query->next()) {
        records.push_back(readRow(*query));
    }
    return records;
}

StorageResult<std::optional<InputProfileRecord>> InputProfileStore::find(
    std::string_view deviceGuid, std::string_view vehicleType, std::string_view name) {
    auto query =
        connection_->prepare(std::string(kSelect) +
                             "WHERE device_guid = :guid AND vehicle_type = :type AND name = :name");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":guid"), toQString(deviceGuid));
    query->bindValue(QStringLiteral(":type"), toQString(vehicleType));
    query->bindValue(QStringLiteral(":name"), toQString(name));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<InputProfileRecord>{};
    }
    return std::optional<InputProfileRecord>{readRow(*query)};
}

StorageResult<std::int64_t> InputProfileStore::save(const InputProfileRecord& p) {
    if (p.name.empty()) {
        return storageError(StorageErrorCode::InvalidValue, "input profile name must not be empty");
    }
    if (!isVehicleType(p.vehicleType)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "vehicle type must be \"aerial\" or \"ground\"");
    }
    if (!isJsonObject(p.mappingJson)) {
        return storageError(StorageErrorCode::InvalidValue, "input mapping must be a JSON object");
    }
    auto query = connection_->prepare(
        "INSERT INTO input_profiles (device_guid, vehicle_type, name, mapping_json) "
        "VALUES (:guid, :type, :name, :mapping) "
        "ON CONFLICT (device_guid, vehicle_type, name) DO UPDATE SET "
        "mapping_json = excluded.mapping_json "
        "RETURNING id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":guid"), toQString(p.deviceGuid));
    query->bindValue(QStringLiteral(":type"), toQString(p.vehicleType));
    query->bindValue(QStringLiteral(":name"), toQString(p.name));
    query->bindValue(QStringLiteral(":mapping"), toQString(p.mappingJson));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return storageError(StorageErrorCode::Query, "saving the input profile returned no id");
    }
    return static_cast<std::int64_t>(query->value(0).toLongLong());
}

StorageResult<bool> InputProfileStore::remove(std::int64_t id) {
    auto query = connection_->prepare("DELETE FROM input_profiles WHERE id = :id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(id));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    return query->numRowsAffected() > 0;
}

}  // namespace ds::storage
