// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/storage/mission_store.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <format>

#include "store_util.hpp"

namespace ds::storage {
namespace {

QVariant nullableText(std::string_view text) {
    return text.empty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(toQString(text));
}

bool isJsonArray(std::string_view text) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(text.data(), static_cast<qsizetype>(text.size())), &error);
    return error.error == QJsonParseError::NoError && doc.isArray();
}

StorageResult<void> validate(const MissionIndexRecord& r) {
    if (r.missionKey.empty() || r.title.empty() || r.filePath.empty()) {
        return storageError(StorageErrorCode::InvalidValue,
                            "a mission needs a key, a title and a file path");
    }
    if (!isVehicleType(r.vehicleType)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "vehicle type must be \"aerial\" or \"ground\"");
    }
    if (r.origin != "builtin" && r.origin != "user") {
        return storageError(StorageErrorCode::InvalidValue,
                            "mission origin must be \"builtin\" or \"user\"");
    }
    if (r.status != "valid" && r.status != "invalid" && r.status != "missing") {
        return storageError(StorageErrorCode::InvalidValue,
                            "mission status must be \"valid\", \"invalid\" or \"missing\"");
    }
    if (!r.validationErrorsJson.empty() && !isJsonArray(r.validationErrorsJson)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "validation errors must be a JSON array");
    }
    return {};
}

MissionIndexRecord readIndex(const QSqlQuery& q, int first) {
    MissionIndexRecord r;
    r.missionKey = q.value(first + 0).toString().toStdString();
    r.title = q.value(first + 1).toString().toStdString();
    r.vehicleType = q.value(first + 2).toString().toStdString();
    r.origin = q.value(first + 3).toString().toStdString();
    r.chapter = q.value(first + 4).toString().toStdString();
    r.sortOrder = q.value(first + 5).toInt();
    r.filePath = q.value(first + 6).toString().toStdString();
    r.fileSha256 = q.value(first + 7).toString().toStdString();
    r.schemaVersion = q.value(first + 8).toInt();
    r.status = q.value(first + 9).toString().toStdString();
    r.validationErrorsJson = q.value(first + 10).toString().toStdString();
    return r;
}

constexpr const char* kListColumns =
    "m.mission_key, m.title, m.vehicle_type, m.origin, COALESCE(m.chapter, ''), m.sort_order, "
    "m.file_path, m.file_sha256, m.schema_version, m.status, COALESCE(m.validation_errors, ''), "
    "p.attempts, p.successes, COALESCE(p.best_stars, 0), COALESCE(p.best_score, 0), "
    "p.best_time_s";

MissionListEntry readEntry(const QSqlQuery& q) {
    MissionListEntry e;
    e.mission = readIndex(q, 0);
    e.progress.attempts = q.value(11).toInt();
    e.progress.successes = q.value(12).toInt();
    e.progress.bestStars = q.value(13).toInt();
    e.progress.bestScore = q.value(14).toDouble();
    if (!q.value(15).isNull()) {
        e.progress.bestTimeS = q.value(15).toDouble();
    }
    return e;
}

constexpr const char* kAttemptColumns =
    "a.id, m.mission_key, a.session_id, a.started_at, a.outcome, COALESCE(a.failure_reason, ''), "
    "a.score, a.stars, COALESCE(a.duration_s, 0), COALESCE(a.terrain_data_hash, ''), "
    "a.results_json";

MissionAttemptRecord readAttempt(const QSqlQuery& q) {
    MissionAttemptRecord a;
    a.id = q.value(0).toLongLong();
    a.missionKey = q.value(1).toString().toStdString();
    if (!q.value(2).isNull()) {
        a.sessionId = q.value(2).toLongLong();
    }
    a.startedAt = q.value(3).toString().toStdString();
    a.outcome = q.value(4).toString().toStdString();
    a.failureReason = q.value(5).toString().toStdString();
    a.score = q.value(6).toDouble();
    a.stars = q.value(7).toInt();
    a.durationS = q.value(8).toDouble();
    a.terrainDataHash = q.value(9).toString().toStdString();
    a.resultsJson = q.value(10).toString().toStdString();
    return a;
}

}  // namespace

StorageResult<std::int64_t> MissionStore::upsert(const MissionIndexRecord& r) {
    if (auto ok = validate(r); !ok) {
        return std::unexpected(ok.error());
    }
    auto query = connection_->prepare(
        "INSERT INTO missions (mission_key, title, vehicle_type, origin, chapter, sort_order, "
        "file_path, file_sha256, schema_version, status, validation_errors, updated_at) "
        "VALUES (:key, :title, :type, :origin, :chapter, :sort, :path, :sha, :version, :status, "
        ":errors, datetime('now')) "
        "ON CONFLICT (mission_key) DO UPDATE SET title = excluded.title, "
        "vehicle_type = excluded.vehicle_type, origin = excluded.origin, "
        "chapter = excluded.chapter, sort_order = excluded.sort_order, "
        "file_path = excluded.file_path, file_sha256 = excluded.file_sha256, "
        "schema_version = excluded.schema_version, status = excluded.status, "
        "validation_errors = excluded.validation_errors, updated_at = excluded.updated_at "
        "RETURNING id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(r.missionKey));
    query->bindValue(QStringLiteral(":title"), toQString(r.title));
    query->bindValue(QStringLiteral(":type"), toQString(r.vehicleType));
    query->bindValue(QStringLiteral(":origin"), toQString(r.origin));
    query->bindValue(QStringLiteral(":chapter"), nullableText(r.chapter));
    query->bindValue(QStringLiteral(":sort"), r.sortOrder);
    query->bindValue(QStringLiteral(":path"), toQString(r.filePath));
    query->bindValue(QStringLiteral(":sha"), toQString(r.fileSha256));
    query->bindValue(QStringLiteral(":version"), r.schemaVersion);
    query->bindValue(QStringLiteral(":status"), toQString(r.status));
    query->bindValue(QStringLiteral(":errors"), nullableText(r.validationErrorsJson));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return storageError(StorageErrorCode::Query, "saving the mission returned no id");
    }
    return static_cast<std::int64_t>(query->value(0).toLongLong());
}

StorageResult<void> MissionStore::sync(std::string_view origin,
                                       const std::vector<MissionIndexRecord>& records) {
    if (origin != "builtin" && origin != "user") {
        return storageError(StorageErrorCode::InvalidValue,
                            "mission origin must be \"builtin\" or \"user\"");
    }
    auto tx = Transaction::begin(*connection_);
    if (!tx) {
        return std::unexpected(tx.error());
    }
    for (const MissionIndexRecord& r : records) {
        if (r.origin != origin) {
            return storageError(StorageErrorCode::InvalidValue,
                                std::format("mission \"{}\" is not a {} mission", r.missionKey,
                                            origin));
        }
        if (auto id = upsert(r); !id) {
            return std::unexpected(id.error());
        }
    }
    // Rows of this origin that were not found: keep them (progress), mark them missing.
    auto all = connection_->prepare("SELECT mission_key FROM missions WHERE origin = :origin");
    if (!all) {
        return std::unexpected(all.error());
    }
    all->bindValue(QStringLiteral(":origin"), toQString(origin));
    if (auto ok = execute(*all); !ok) {
        return ok;
    }
    std::vector<QString> gone;
    while (all->next()) {
        const std::string key = all->value(0).toString().toStdString();
        if (std::ranges::none_of(records, [&](const MissionIndexRecord& r) { return r.missionKey == key; })) {
            gone.push_back(all->value(0).toString());
        }
    }
    for (const QString& key : gone) {
        auto mark = connection_->prepare(
            "UPDATE missions SET status = 'missing', updated_at = datetime('now') "
            "WHERE mission_key = :key");
        if (!mark) {
            return std::unexpected(mark.error());
        }
        mark->bindValue(QStringLiteral(":key"), key);
        if (auto ok = execute(*mark); !ok) {
            return ok;
        }
    }
    return tx->commit();
}

StorageResult<std::vector<MissionListEntry>> MissionStore::list() {
    auto query = connection_->prepare(
        std::format("SELECT {} FROM missions m JOIN mission_progress p ON p.mission_id = m.id "
                    "ORDER BY COALESCE(m.chapter, ''), m.sort_order, m.mission_key",
                    kListColumns));
    if (!query) {
        return std::unexpected(query.error());
    }
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<MissionListEntry> out;
    while (query->next()) {
        out.push_back(readEntry(*query));
    }
    return out;
}

StorageResult<std::optional<MissionListEntry>> MissionStore::find(std::string_view missionKey) {
    auto query = connection_->prepare(
        std::format("SELECT {} FROM missions m JOIN mission_progress p ON p.mission_id = m.id "
                    "WHERE m.mission_key = :key",
                    kListColumns));
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(missionKey));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<MissionListEntry>{};
    }
    return std::optional<MissionListEntry>(readEntry(*query));
}

StorageResult<std::int64_t> MissionStore::recordAttempt(const MissionAttemptRecord& a) {
    if (a.outcome != "succeeded" && a.outcome != "failed" && a.outcome != "aborted") {
        return storageError(StorageErrorCode::InvalidValue,
                            "outcome must be \"succeeded\", \"failed\" or \"aborted\"");
    }
    if (a.stars < 0 || a.stars > 3 || (a.outcome != "succeeded" && a.stars != 0)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "stars must be 0–3, and 0 unless the mission succeeded");
    }
    if (!std::isfinite(a.score) || a.score < 0.0 || !std::isfinite(a.durationS) ||
        a.durationS < 0.0) {
        return storageError(StorageErrorCode::InvalidValue,
                            "score and duration must be finite and not negative");
    }
    if (a.startedAt.empty() || !isJsonObject(a.resultsJson)) {
        return storageError(StorageErrorCode::InvalidValue,
                            "an attempt needs a start time and a results JSON object");
    }
    auto query = connection_->prepare(
        "INSERT INTO mission_attempts (mission_id, session_id, started_at, outcome, "
        "failure_reason, score, stars, duration_s, terrain_data_hash, results_json) "
        "SELECT id, :session, :started, :outcome, :reason, :score, :stars, :duration, :hash, "
        ":results FROM missions WHERE mission_key = :key "
        "RETURNING id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(a.missionKey));
    query->bindValue(QStringLiteral(":session"),
                     a.sessionId ? QVariant(static_cast<qlonglong>(*a.sessionId))
                                 : QVariant(QMetaType(QMetaType::LongLong)));
    query->bindValue(QStringLiteral(":started"), toQString(a.startedAt));
    query->bindValue(QStringLiteral(":outcome"), toQString(a.outcome));
    query->bindValue(QStringLiteral(":reason"), nullableText(a.failureReason));
    query->bindValue(QStringLiteral(":score"), a.score);
    query->bindValue(QStringLiteral(":stars"), a.stars);
    query->bindValue(QStringLiteral(":duration"), a.durationS);
    query->bindValue(QStringLiteral(":hash"), nullableText(a.terrainDataHash));
    query->bindValue(QStringLiteral(":results"), toQString(a.resultsJson));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return storageError(StorageErrorCode::InvalidValue,
                            std::format("unknown mission \"{}\"", a.missionKey));
    }
    return static_cast<std::int64_t>(query->value(0).toLongLong());
}

StorageResult<std::vector<MissionAttemptRecord>> MissionStore::attempts(std::string_view missionKey,
                                                                        int limit) {
    auto query = connection_->prepare(
        std::format("SELECT {} FROM mission_attempts a JOIN missions m ON m.id = a.mission_id "
                    "WHERE :all = 1 OR m.mission_key = :key "
                    "ORDER BY a.started_at DESC, a.id DESC LIMIT :limit",
                    kAttemptColumns));
    if (!query) {
        return std::unexpected(query.error());
    }
    // Each named placeholder appears once: QSQLITE does not bind repeated names.
    query->bindValue(QStringLiteral(":all"), missionKey.empty() ? 1 : 0);
    query->bindValue(QStringLiteral(":key"), toQString(missionKey));
    query->bindValue(QStringLiteral(":limit"), std::max(limit, 0));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    std::vector<MissionAttemptRecord> out;
    while (query->next()) {
        out.push_back(readAttempt(*query));
    }
    return out;
}

StorageResult<std::optional<MissionAttemptRecord>> MissionStore::bestAttempt(
    std::string_view missionKey) {
    auto query = connection_->prepare(
        std::format("SELECT {} FROM mission_attempts a JOIN missions m ON m.id = a.mission_id "
                    "WHERE m.mission_key = :key "
                    "ORDER BY a.stars DESC, a.score DESC, a.duration_s ASC, a.id ASC LIMIT 1",
                    kAttemptColumns));
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bindValue(QStringLiteral(":key"), toQString(missionKey));
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    if (!query->next()) {
        return std::optional<MissionAttemptRecord>{};
    }
    return std::optional<MissionAttemptRecord>(readAttempt(*query));
}

StorageResult<int> MissionStore::clearAttempts() {
    auto query = connection_->prepare("DELETE FROM mission_attempts");
    if (!query) {
        return std::unexpected(query.error());
    }
    if (auto ok = execute(*query); !ok) {
        return std::unexpected(ok.error());
    }
    return query->numRowsAffected();
}

}  // namespace ds::storage
