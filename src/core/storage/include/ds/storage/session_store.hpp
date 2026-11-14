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
/// @brief Flight sessions in the `sessions` table (FR-LOOP-07, History tab in M7).
/// @ingroup storage

namespace ds::storage {

/// @brief One row of `sessions`.
struct SessionRecord {
    std::optional<std::int64_t> id;      ///< Row id; empty for a record not yet saved.
    std::string vehicleProfileKey;       ///< Profile flown ("" if unknown or deleted).
    std::string vehicleType = "aerial";  ///< `aerial` or `ground`.
    std::string startedAt;               ///< Simulation UTC, ISO 8601 ("2026-06-21T10:00:00Z").
    double durationS = 0.0;              ///< Simulated duration.
    double maxAltitudeAglM = 0.0;        ///< Highest height above ground.
    double maxSpeedMs = 0.0;             ///< Highest ground speed.
    double distanceM = 0.0;              ///< Distance flown.
    double energyUsedWh = 0.0;           ///< Battery energy used.
    int crashes = 0;                     ///< Number of crashes.
    /// @brief Equality (tests).
    bool operator==(const SessionRecord&) const = default;
};

/// @brief Reads and writes sessions on a connection of the storage thread.
class SessionStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit SessionStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Inserts a new session; returns its id. The profile key is resolved to the
    /// `vehicle_profiles` row (NULL if no such profile). Fails with InvalidValue for an unknown
    /// vehicle type, an empty start time or a negative figure.
    [[nodiscard]] StorageResult<std::int64_t> insert(const SessionRecord& session);
    /// @brief Updates the figures of session @p session.id (written again when it ends).
    /// Returns false if the row does not exist.
    [[nodiscard]] StorageResult<bool> update(const SessionRecord& session);
    /// @brief The @p limit most recent sessions, newest first.
    [[nodiscard]] StorageResult<std::vector<SessionRecord>> recent(int limit = 50);
    /// @brief Deletes the row @p id (its telemetry samples go with it). False if none.
    [[nodiscard]] StorageResult<bool> remove(std::int64_t id);

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
