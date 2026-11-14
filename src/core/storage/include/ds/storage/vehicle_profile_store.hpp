// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ds/storage/connection.hpp"

/// @file
/// @brief Vehicle profiles in the `vehicle_profiles` table (FR-PHY-15, FR-DB-09).
/// @ingroup storage

namespace ds::storage {

/// @brief One row of `vehicle_profiles`.
/// @details The parameters are kept as the profile's JSON document; parsing and validating it
/// is the job of the physics module (`ds::physics::aerialProfileFromJson`), which storage does
/// not depend on.
struct VehicleProfileRecord {
    std::optional<std::int64_t> id;      ///< Row id; empty for a record not yet saved.
    std::string key;                     ///< Stable unique key (`profile_key`).
    std::string vehicleType = "aerial";  ///< `aerial` or `ground`.
    std::string name;                    ///< Display name.
    std::string origin = "user";         ///< `builtin` (shipped, read-only) or `user`.
    std::string paramsJson = "{}";       ///< Profile document (JSON object).
    /// @brief Equality (tests).
    bool operator==(const VehicleProfileRecord&) const = default;
};

/// @brief Result of VehicleProfileStore::syncBuiltIn().
struct ProfileSyncReport {
    int inserted = 0;  ///< New built-in rows.
    int updated = 0;   ///< Built-in rows whose name or parameters changed.
    int skipped = 0;   ///< Keys taken by a user profile (left untouched).
    /// @brief Equality (tests).
    bool operator==(const ProfileSyncReport&) const = default;
};

/// @brief Reads and writes vehicle profiles on a connection of the storage thread.
class VehicleProfileStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit VehicleProfileStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Profiles of @p vehicleType (all types if empty): built-in first, then by name.
    [[nodiscard]] StorageResult<std::vector<VehicleProfileRecord>> list(
        std::string_view vehicleType = {});
    /// @brief The profile with @p key, if any.
    [[nodiscard]] StorageResult<std::optional<VehicleProfileRecord>> find(std::string_view key);

    /// @brief Inserts or updates the shipped profiles by key in one transaction (FR-DB-09).
    /// @details A key already used by a user profile is never overwritten; built-in rows that
    /// are no longer shipped are kept (sessions may refer to them). The records' `origin` is
    /// ignored (always `builtin`).
    [[nodiscard]] StorageResult<ProfileSyncReport> syncBuiltIn(
        std::span<const VehicleProfileRecord> profiles);

    /// @brief Inserts or updates a user profile; returns the row id.
    /// @details Fails with InvalidValue if the key or name is empty, the type is unknown,
    /// `paramsJson` is not a JSON object, or the key belongs to a built-in profile (those are
    /// read-only: save a copy under a new key).
    [[nodiscard]] StorageResult<std::int64_t> saveUser(const VehicleProfileRecord& profile);

    /// @brief Deletes the user profile @p key. Returns false if there was none; fails with
    /// InvalidValue for a built-in profile.
    [[nodiscard]] StorageResult<bool> remove(std::string_view key);

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
