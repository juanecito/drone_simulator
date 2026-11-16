// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/storage/connection.hpp"

/// @file
/// @brief The mission index (`missions`), attempts (`mission_attempts`) and progress
/// (`mission_progress`) — FR-DB-09, FR-DB-10, FR-MIS-01a, FR-MIS-12. Mission files stay the
/// source of truth; the database only keeps their index and the user's results.
/// @ingroup storage

namespace ds::storage {

/// @brief One row of `missions`.
struct MissionIndexRecord {
    std::string missionKey;               ///< `id` of the mission file.
    std::string title;                    ///< Title.
    std::string vehicleType = "aerial";   ///< `aerial` or `ground`.
    std::string origin = "user";          ///< `builtin` or `user`.
    std::string chapter;                  ///< "" ⇒ NULL.
    int sortOrder = 0;                    ///< Order in the chapter.
    std::string filePath;                 ///< Absolute path of the `.mission.json`.
    std::string fileSha256;               ///< Hash of the file (changes are picked up).
    int schemaVersion = 1;                ///< `schemaVersion` of the file.
    std::string status = "valid";         ///< `valid`, `invalid` or `missing`.
    std::string validationErrorsJson;     ///< JSON array of messages ("" ⇒ NULL).
    /// @brief Equality (tests).
    bool operator==(const MissionIndexRecord&) const = default;
};

/// @brief Progress of one mission (`mission_progress` view).
struct MissionProgress {
    int attempts = 0;                  ///< Attempts.
    int successes = 0;                 ///< Succeeded attempts.
    int bestStars = 0;                 ///< 0 if none.
    double bestScore = 0.0;            ///< 0 if none.
    std::optional<double> bestTimeS;   ///< Fastest success.
};

/// @brief A mission with its progress.
struct MissionListEntry {
    MissionIndexRecord mission;  ///< Index row.
    MissionProgress progress;    ///< Progress.
};

/// @brief One row of `mission_attempts`.
struct MissionAttemptRecord {
    std::optional<std::int64_t> id;          ///< Row id (empty before saving).
    std::string missionKey;                  ///< Mission.
    std::optional<std::int64_t> sessionId;   ///< Flight session, if recorded.
    std::string startedAt;                   ///< ISO 8601 UTC.
    std::string outcome = "failed";          ///< `succeeded`, `failed` or `aborted`.
    std::string failureReason;               ///< "" ⇒ NULL.
    double score = 0.0;                      ///< Total score.
    int stars = 0;                           ///< 0–3.
    double durationS = 0.0;                  ///< Mission time.
    std::string terrainDataHash;             ///< FR-TER-O14 ("" ⇒ NULL, procedural terrain).
    std::string resultsJson = "{}";          ///< Per-objective results, score breakdown.
    /// @brief Equality (tests).
    bool operator==(const MissionAttemptRecord&) const = default;
};

/// @brief Reads and writes the mission tables on a connection of the storage thread.
class MissionStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit MissionStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Inserts or updates the row of @p record.missionKey (attempts are kept).
    [[nodiscard]] StorageResult<std::int64_t> upsert(const MissionIndexRecord& record);
    /// @brief Synchronises the index with a scan (FR-MIS-01a, FR-DB-09): upserts @p records in
    /// one transaction and marks rows of the same @p origin that were not found as `missing`
    /// (rows are kept so progress survives).
    [[nodiscard]] StorageResult<void> sync(std::string_view origin,
                                           const std::vector<MissionIndexRecord>& records);
    /// @brief Every mission with its progress, ordered by chapter, order and key.
    [[nodiscard]] StorageResult<std::vector<MissionListEntry>> list();
    /// @brief One mission's row, or nullopt.
    [[nodiscard]] StorageResult<std::optional<MissionListEntry>> find(std::string_view missionKey);
    /// @brief Saves an attempt (FR-MIS-11, FR-MIS-12); returns its id. Fails with InvalidValue
    /// for an unknown mission, outcome or star count, or results that are not a JSON object.
    [[nodiscard]] StorageResult<std::int64_t> recordAttempt(const MissionAttemptRecord& attempt);
    /// @brief The @p limit most recent attempts of a mission ("" ⇒ all missions), newest first.
    [[nodiscard]] StorageResult<std::vector<MissionAttemptRecord>> attempts(
        std::string_view missionKey = {}, int limit = 50);
    /// @brief The best attempt of a mission (most stars, then highest score), for "personal
    /// best" comparisons.
    [[nodiscard]] StorageResult<std::optional<MissionAttemptRecord>> bestAttempt(
        std::string_view missionKey);
    /// @brief Deletes every attempt (Settings → clear history). Returns the number deleted.
    [[nodiscard]] StorageResult<int> clearAttempts();

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
