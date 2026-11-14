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
/// @brief Controller mappings and calibrations in the `input_profiles` table (FR-INP-03).
/// @ingroup storage

namespace ds::storage {

/// @brief One row of `input_profiles`.
/// @details The mapping is the JSON document of `ds::input::InputMapping::toJson()`.
struct InputProfileRecord {
    std::optional<std::int64_t> id;      ///< Row id; empty for a record not yet saved.
    std::string deviceGuid;              ///< SDL GUID of the device ("" for the keyboard).
    std::string vehicleType = "aerial";  ///< `aerial` or `ground`.
    std::string name = "Default";        ///< Profile name (several per device).
    std::string mappingJson = "{}";      ///< Mapping document (JSON object).
    /// @brief Equality (tests).
    bool operator==(const InputProfileRecord&) const = default;
};

/// @brief Reads and writes input profiles on a connection of the storage thread.
class InputProfileStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit InputProfileStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Profiles of @p deviceGuid (all devices if empty), sorted by device and name.
    [[nodiscard]] StorageResult<std::vector<InputProfileRecord>> list(
        std::optional<std::string_view> deviceGuid = std::nullopt);
    /// @brief The profile for (@p deviceGuid, @p vehicleType, @p name), if any.
    [[nodiscard]] StorageResult<std::optional<InputProfileRecord>> find(
        std::string_view deviceGuid, std::string_view vehicleType, std::string_view name);
    /// @brief Inserts or replaces (by device, type and name); returns the row id. Fails with
    /// InvalidValue for an empty name, an unknown type or a mapping that is not a JSON object.
    [[nodiscard]] StorageResult<std::int64_t> save(const InputProfileRecord& profile);
    /// @brief Deletes the row @p id. Returns false if there was none.
    [[nodiscard]] StorageResult<bool> remove(std::int64_t id);

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
