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
/// @brief Named terrain configurations in the `terrain_presets` table (FR-TER-10).
/// @ingroup storage

namespace ds::storage {

/// @brief One row of `terrain_presets`.
/// @details Only the columns used so far are mapped; online and file sources (provider ids,
/// DEM/orthophoto file lists) arrive with milestone M4.
struct TerrainPresetRecord {
    std::optional<std::int64_t> id;         ///< Row id; empty for a record not yet saved.
    std::string key;                        ///< Stable unique key (`preset_key`).
    std::string name;                       ///< Display name.
    std::string sourceType = "procedural";  ///< `procedural`, `online` or `file`.
    /// Procedural seed (required for procedural presets). Stored bit-for-bit in SQLite's
    /// signed 64-bit INTEGER.
    std::optional<std::uint64_t> seed;
    double originLatDeg = 0.0;            ///< Geographic origin latitude.
    double originLonDeg = 0.0;            ///< Geographic origin longitude.
    double originAltM = 0.0;              ///< Geographic origin altitude (AMSL).
    std::optional<double> extentWidthM;   ///< Terrain width; set together with the length.
    std::optional<double> extentLengthM;  ///< Terrain length.
    std::string paramsJson = "{}";        ///< Source parameters as a JSON object.

    /// @brief Equality (tests).
    bool operator==(const TerrainPresetRecord&) const = default;
};

/// @brief Reads and writes terrain presets on a connection of the storage thread.
class TerrainPresetStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit TerrainPresetStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief All presets, sorted by name.
    [[nodiscard]] StorageResult<std::vector<TerrainPresetRecord>> list();
    /// @brief The preset with @p key, if any.
    [[nodiscard]] StorageResult<std::optional<TerrainPresetRecord>> find(std::string_view key);
    /// @brief Inserts @p preset or, if its key exists, replaces it. Returns the row id.
    /// @details Fails with InvalidValue if the key or name is empty or `paramsJson` is not a
    /// JSON object, and with Query if a table constraint rejects the row (e.g. a procedural
    /// preset without seed, or only one extent side).
    [[nodiscard]] StorageResult<std::int64_t> save(const TerrainPresetRecord& preset);
    /// @brief Deletes the preset with @p key. Returns false if there was none.
    [[nodiscard]] StorageResult<bool> remove(std::string_view key);

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
