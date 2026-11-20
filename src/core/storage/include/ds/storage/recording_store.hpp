// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ds/storage/connection.hpp"
#include "ds/storage/session_store.hpp"

/// @file
/// @brief Telemetry samples and replays of flight sessions (M7 step 7: FR-X-06, FR-DB-07,
/// History tab).
/// @ingroup storage

namespace ds::storage {

/// @brief One row of `telemetry_samples` (20 Hz).
struct TelemetryRow {
    double tS = 0.0;        ///< Time since the session start.
    double posX = 0.0;      ///< World position, east.
    double posY = 0.0;      ///< World position, up.
    double posZ = 0.0;      ///< World position, south.
    double roll = 0.0;      ///< Roll, radians.
    double pitch = 0.0;     ///< Pitch, radians.
    double yaw = 0.0;       ///< Yaw, radians clockwise from north.
    double speed = 0.0;     ///< Ground speed, m/s.
    double altAgl = 0.0;    ///< Height above ground, m.
    double throttle = 0.0;  ///< Throttle (aerial) or drive (ground), −1 … 1.
    double steering = 0.0;  ///< Roll stick (aerial) or steering (ground), −1 … 1.
    double batteryV = 0.0;  ///< Battery voltage, V.
    double batteryA = 0.0;  ///< Battery current, A.
    /// @brief Equality (tests).
    bool operator==(const TelemetryRow&) const = default;
};

/// @brief One row of `replays`.
struct ReplayRecord {
    std::int64_t sessionId = 0;        ///< Session (0 before saving).
    std::string missionKey;            ///< Mission flown ("" = free flight).
    bool replayable = true;            ///< False when the log could not hold every change.
    double durationS = 0.0;            ///< Length.
    std::int64_t ticks = 0;            ///< Ticks in @ref inputs.
    std::string metaJson = "{}";       ///< Header, commands and flags (sim::ReplayLog).
    std::vector<std::uint8_t> inputs;  ///< Input stream (sim::InputEncoder).
    /// @brief Equality (tests).
    bool operator==(const ReplayRecord&) const = default;
};

/// @brief Saves and reads the recordings of flight sessions on the storage thread.
class RecordingStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit RecordingStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Saves a finished session in one transaction (FR-DB-07: one batch, on the
    /// storage thread): the `sessions` row, its telemetry samples, its replay and, when
    /// @p attemptId is given, the link from that mission attempt. Returns the session id.
    [[nodiscard]] StorageResult<std::int64_t> saveFlight(
        const SessionRecord& session, std::span<const TelemetryRow> samples,
        const std::optional<ReplayRecord>& replay,
        std::optional<std::int64_t> attemptId = std::nullopt);
    /// @brief The telemetry of session @p sessionId, in time order.
    [[nodiscard]] StorageResult<std::vector<TelemetryRow>> telemetry(std::int64_t sessionId);
    /// @brief The replay of session @p sessionId, if it has one.
    [[nodiscard]] StorageResult<std::optional<ReplayRecord>> replay(std::int64_t sessionId);

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
