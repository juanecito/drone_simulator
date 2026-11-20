// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/recording_store.hpp"

#include <QByteArray>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <cmath>

#include "store_util.hpp"

namespace ds::storage {

StorageResult<std::int64_t> RecordingStore::saveFlight(const SessionRecord& session,
                                                       std::span<const TelemetryRow> samples,
                                                       const std::optional<ReplayRecord>& replay,
                                                       std::optional<std::int64_t> attemptId) {
    if (replay && !isJsonObject(replay->metaJson)) {
        return storageError(StorageErrorCode::InvalidValue, "replay data must be a JSON object");
    }
    auto transaction = Transaction::begin(*connection_);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    auto id = SessionStore(*connection_).insert(session);
    if (!id) {
        return std::unexpected(id.error());
    }
    const auto sessionId = static_cast<qlonglong>(*id);

    if (!samples.empty()) {
        auto insert = connection_->prepare(
            "INSERT OR REPLACE INTO telemetry_samples (session_id, t_s, pos_x, pos_y, pos_z, "
            "roll, pitch, yaw, speed, alt_agl, throttle, steering, battery_v, battery_a) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        if (!insert) {
            return std::unexpected(insert.error());
        }
        for (const TelemetryRow& r : samples) {
            const std::array<double, 13> values{
                r.tS,    r.posX,   r.posY,     r.posZ,     r.roll,     r.pitch,   r.yaw,
                r.speed, r.altAgl, r.throttle, r.steering, r.batteryV, r.batteryA};
            insert->bindValue(0, sessionId);
            for (std::size_t i = 0; i < values.size(); ++i) {
                // NaN (a failed sensor model) is stored as NULL.
                insert->bindValue(static_cast<int>(i) + 1,
                                  std::isfinite(values[i]) ? QVariant(values[i]) : QVariant());
            }
            if (auto ok = execute(*insert); !ok) {
                return std::unexpected(ok.error());
            }
        }
    }

    if (replay) {
        auto insert = connection_->prepare(
            "INSERT INTO replays (session_id, mission_key, replayable, duration_s, tick_count, "
            "meta_json, inputs) VALUES (:id, :mission, :replayable, :duration, :ticks, :meta, "
            ":inputs)");
        if (!insert) {
            return std::unexpected(insert.error());
        }
        insert->bindValue(QStringLiteral(":id"), sessionId);
        insert->bindValue(
            QStringLiteral(":mission"),
            replay->missionKey.empty() ? QVariant() : QVariant(toQString(replay->missionKey)));
        insert->bindValue(QStringLiteral(":replayable"), replay->replayable ? 1 : 0);
        insert->bindValue(QStringLiteral(":duration"), replay->durationS);
        insert->bindValue(QStringLiteral(":ticks"), static_cast<qlonglong>(replay->ticks));
        insert->bindValue(QStringLiteral(":meta"), toQString(replay->metaJson));
        insert->bindValue(QStringLiteral(":inputs"),
                          QByteArray(reinterpret_cast<const char*>(replay->inputs.data()),
                                     static_cast<qsizetype>(replay->inputs.size())));
        if (auto ok = execute(*insert); !ok) {
            return std::unexpected(ok.error());
        }
    }

    if (attemptId) {
        auto link = connection_->prepare(
            "UPDATE mission_attempts SET session_id = :session WHERE id = :id");
        if (!link) {
            return std::unexpected(link.error());
        }
        link->bindValue(QStringLiteral(":session"), sessionId);
        link->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(*attemptId));
        if (auto ok = execute(*link); !ok) {
            return std::unexpected(ok.error());
        }
    }
    if (auto ok = transaction->commit(); !ok) {
        return std::unexpected(ok.error());
    }
    return *id;
}

StorageResult<std::vector<TelemetryRow>> RecordingStore::telemetry(std::int64_t sessionId) {
    auto query = connection_->prepare(
        "SELECT t_s, pos_x, pos_y, pos_z, roll, pitch, yaw, speed, alt_agl, throttle, steering, "
        "battery_v, battery_a FROM telemetry_samples WHERE session_id = :id ORDER BY t_s");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(sessionId));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<TelemetryRow> rows;
    while (query->next()) {
        const auto v = [&](int i) {
            const QVariant value = query->value(i);
            return value.isNull() ? std::nan("") : value.toDouble();
        };
        rows.push_back({.tS = v(0),
                        .posX = v(1),
                        .posY = v(2),
                        .posZ = v(3),
                        .roll = v(4),
                        .pitch = v(5),
                        .yaw = v(6),
                        .speed = v(7),
                        .altAgl = v(8),
                        .throttle = v(9),
                        .steering = v(10),
                        .batteryV = v(11),
                        .batteryA = v(12)});
    }
    return rows;
}

StorageResult<std::optional<ReplayRecord>> RecordingStore::replay(std::int64_t sessionId) {
    auto query = connection_->prepare(
        "SELECT COALESCE(mission_key, ''), replayable, duration_s, tick_count, meta_json, inputs "
        "FROM replays WHERE session_id = :id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":id"), static_cast<qlonglong>(sessionId));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<ReplayRecord>();
    }
    ReplayRecord r;
    r.sessionId = sessionId;
    r.missionKey = query->value(0).toString().toStdString();
    r.replayable = query->value(1).toInt() != 0;
    r.durationS = query->value(2).toDouble();
    r.ticks = query->value(3).toLongLong();
    r.metaJson = query->value(4).toString().toStdString();
    const QByteArray bytes = query->value(5).toByteArray();
    r.inputs.assign(bytes.begin(), bytes.end());
    return std::optional<ReplayRecord>(std::move(r));
}

}  // namespace ds::storage
