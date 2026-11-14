// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/session_store.hpp"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cmath>

#include "store_util.hpp"

namespace ds::storage {
namespace {

StorageResult<void> validate(const SessionRecord& s) {
    if (!isVehicleType(s.vehicleType)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "vehicle type must be \"aerial\" or \"ground\"");
    }
    if (s.startedAt.empty()) {
        return storageError(StorageErrorCode::InvalidValue, "session start time is missing");
    }
    for (const double v :
         {s.durationS, s.maxAltitudeAglM, s.maxSpeedMs, s.distanceM, s.energyUsedWh}) {
        if (!std::isfinite(v)) {
            return storageError(StorageErrorCode::InvalidValue,
                                "session figures must be finite numbers");
        }
    }
    if (s.durationS < 0.0 || s.maxSpeedMs < 0.0 || s.distanceM < 0.0 || s.energyUsedWh < 0.0 ||
        s.crashes < 0) {
        return storageError(StorageErrorCode::InvalidValue, "session figures must not be negative");
    }
    return {};
}

void bindFigures(QSqlQuery& q, const SessionRecord& s) {
    q.bindValue(QStringLiteral(":duration"), s.durationS);
    q.bindValue(QStringLiteral(":agl"), s.maxAltitudeAglM);
    q.bindValue(QStringLiteral(":speed"), s.maxSpeedMs);
    q.bindValue(QStringLiteral(":distance"), s.distanceM);
    q.bindValue(QStringLiteral(":energy"), s.energyUsedWh);
    q.bindValue(QStringLiteral(":crashes"), s.crashes);
}

}  // namespace

StorageResult<std::int64_t> SessionStore::insert(const SessionRecord& s) {
    if (auto ok = validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    auto query = connection_->prepare(
        "INSERT INTO sessions (vehicle_profile_id, vehicle_type, started_at, duration_s, "
        "max_altitude_agl_m, max_speed_ms, distance_m, energy_used_wh, crashes) "
        "VALUES ((SELECT id FROM vehicle_profiles WHERE profile_key = :key), :type, :started, "
        ":duration, :agl, :speed, :distance, :energy, :crashes) "
        "RETURNING id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(s.vehicleProfileKey));
    query->bindValue(QStringLiteral(":type"), toQString(s.vehicleType));
    query->bindValue(QStringLiteral(":started"), toQString(s.startedAt));
    bindFigures(*query, s);
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return storageError(StorageErrorCode::Query, "saving the session returned no id");
    }
    return static_cast<std::int64_t>(query->value(0).toLongLong());
}

StorageResult<bool> SessionStore::update(const SessionRecord& s) {
    if (!s.id) {
        return storageError(StorageErrorCode::InvalidValue, "session has no id");
    }
    if (auto ok = validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    auto query = connection_->prepare(
        "UPDATE sessions SET duration_s = :duration, max_altitude_agl_m = :agl, "
        "max_speed_ms = :speed, distance_m = :distance, energy_used_wh = :energy, "
        "crashes = :crashes WHERE id = :id");
    if (!query) {
        return std::unexpected(query.error());
    }
    bindFigures(*query, s);
    query->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(*s.id));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    return query->numRowsAffected() > 0;
}

StorageResult<std::vector<SessionRecord>> SessionStore::recent(int limit) {
    auto query = connection_->prepare(
        "SELECT s.id, COALESCE(v.profile_key, ''), s.vehicle_type, s.started_at, "
        "COALESCE(s.duration_s, 0), COALESCE(s.max_altitude_agl_m, 0), "
        "COALESCE(s.max_speed_ms, 0), COALESCE(s.distance_m, 0), "
        "COALESCE(s.energy_used_wh, 0), s.crashes "
        "FROM sessions s LEFT JOIN vehicle_profiles v ON v.id = s.vehicle_profile_id "
        "ORDER BY s.started_at DESC, s.id DESC LIMIT :limit");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":limit"), std::max(limit, 0));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<SessionRecord> records;
    while (query->next()) {
        SessionRecord r;
        r.id = query->value(0).toLongLong();
        r.vehicleProfileKey = query->value(1).toString().toStdString();
        r.vehicleType = query->value(2).toString().toStdString();
        r.startedAt = query->value(3).toString().toStdString();
        r.durationS = query->value(4).toDouble();
        r.maxAltitudeAglM = query->value(5).toDouble();
        r.maxSpeedMs = query->value(6).toDouble();
        r.distanceM = query->value(7).toDouble();
        r.energyUsedWh = query->value(8).toDouble();
        r.crashes = query->value(9).toInt();
        records.push_back(std::move(r));
    }
    return records;
}

StorageResult<bool> SessionStore::remove(std::int64_t id) {
    auto query = connection_->prepare("DELETE FROM sessions WHERE id = :id");
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
