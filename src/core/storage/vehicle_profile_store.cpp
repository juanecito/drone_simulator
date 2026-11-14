// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/vehicle_profile_store.hpp"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include "store_util.hpp"

namespace ds::storage {
namespace {

constexpr const char* kSelect =
    "SELECT id, profile_key, vehicle_type, name, origin, params_json FROM vehicle_profiles ";

VehicleProfileRecord readRow(const QSqlQuery& query) {
    VehicleProfileRecord r;
    r.id = query.value(0).toLongLong();
    r.key = query.value(1).toString().toStdString();
    r.vehicleType = query.value(2).toString().toStdString();
    r.name = query.value(3).toString().toStdString();
    r.origin = query.value(4).toString().toStdString();
    r.paramsJson = query.value(5).toString().toStdString();
    return r;
}

StorageResult<void> validate(const VehicleProfileRecord& p) {
    if (p.key.empty() || p.name.empty()) {
        return storageError(StorageErrorCode::InvalidValue,
                            "vehicle profile key and name must not be empty");
    }
    if (!isVehicleType(p.vehicleType)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "vehicle type must be \"aerial\" or \"ground\"");
    }
    if (!isJsonObject(p.paramsJson)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "vehicle profile parameters must be a JSON object");
    }
    return {};
}

/// Inserts or updates one row; @p origin is stored as given.
StorageResult<std::int64_t> upsert(DbConnection& db, const VehicleProfileRecord& p,
                                   std::string_view origin) {
    auto query = db.prepare(
        "INSERT INTO vehicle_profiles (profile_key, vehicle_type, name, origin, params_json) "
        "VALUES (:key, :type, :name, :origin, :params) "
        "ON CONFLICT (profile_key) DO UPDATE SET vehicle_type = excluded.vehicle_type, "
        "name = excluded.name, origin = excluded.origin, params_json = excluded.params_json, "
        "updated_at = datetime('now') "
        "RETURNING id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(p.key));
    query->bindValue(QStringLiteral(":type"), toQString(p.vehicleType));
    query->bindValue(QStringLiteral(":name"), toQString(p.name));
    query->bindValue(QStringLiteral(":origin"), toQString(origin));
    query->bindValue(QStringLiteral(":params"), toQString(p.paramsJson));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return storageError(StorageErrorCode::Query, "saving the vehicle profile returned no id");
    }
    return static_cast<std::int64_t>(query->value(0).toLongLong());
}

}  // namespace

StorageResult<std::vector<VehicleProfileRecord>> VehicleProfileStore::list(
    std::string_view vehicleType) {
    auto query = connection_->prepare(std::string(kSelect) +
                                      "WHERE :all = 1 OR vehicle_type = :type "
                                      "ORDER BY origin = 'user', name COLLATE NOCASE, id");
    if (!query) {
        return std::unexpected(query.error());
    }
    // Distinct placeholders: Qt binds a repeated name only once.
    query->bindValue(QStringLiteral(":all"), vehicleType.empty() ? 1 : 0);
    query->bindValue(QStringLiteral(":type"), toQString(vehicleType));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<VehicleProfileRecord> records;
    while (query->next()) {
        records.push_back(readRow(*query));
    }
    return records;
}

StorageResult<std::optional<VehicleProfileRecord>> VehicleProfileStore::find(std::string_view key) {
    auto query = connection_->prepare(std::string(kSelect) + "WHERE profile_key = :key");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<VehicleProfileRecord>{};
    }
    return std::optional<VehicleProfileRecord>{readRow(*query)};
}

StorageResult<ProfileSyncReport> VehicleProfileStore::syncBuiltIn(
    std::span<const VehicleProfileRecord> profiles) {
    for (const VehicleProfileRecord& p : profiles) {
        if (auto ok = validate(p); !ok) {
            return std::unexpected(ok.error());
        }
    }
    auto transaction = Transaction::begin(*connection_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    ProfileSyncReport report;
    for (const VehicleProfileRecord& p : profiles) {
        auto existing = find(p.key);
        if (!existing) {
            return std::unexpected(existing.error());
        }
        if (*existing && (*existing)->origin != "builtin") {
            ++report.skipped;
            continue;
        }
        if (*existing && (*existing)->name == p.name && (*existing)->paramsJson == p.paramsJson &&
            (*existing)->vehicleType == p.vehicleType) {
            continue;  // Unchanged: keep updated_at.
        }
        if (auto id = upsert(*connection_, p, "builtin"); !id) {
            return std::unexpected(id.error());
        }
        ++(*existing ? report.updated : report.inserted);
    }
    if (auto ok = transaction->commit(); !ok) {
        return std::unexpected(ok.error());
    }
    return report;
}

StorageResult<std::int64_t> VehicleProfileStore::saveUser(const VehicleProfileRecord& p) {
    if (auto ok = validate(p); !ok) {
        return std::unexpected(ok.error());
    }
    auto existing = find(p.key);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (*existing && (*existing)->origin == "builtin") {
        return storageError(StorageErrorCode::InvalidValue,
                            "built-in vehicle profiles are read-only; save a copy instead");
    }
    return upsert(*connection_, p, "user");
}

StorageResult<bool> VehicleProfileStore::remove(std::string_view key) {
    auto existing = find(key);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (!*existing) {
        return false;
    }
    if ((*existing)->origin == "builtin") {
        return storageError(StorageErrorCode::InvalidValue,
                            "built-in vehicle profiles cannot be deleted");
    }
    auto query = connection_->prepare("DELETE FROM vehicle_profiles WHERE profile_key = :key");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(key));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    return query->numRowsAffected() > 0;
}

}  // namespace ds::storage
