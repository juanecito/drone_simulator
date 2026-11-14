// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "ds/storage/storage_error.hpp"
#include "ds/storage/storage_service.hpp"

/// @file
/// @brief Opens the per-user database at start-up, with the in-memory fallback (FR-DB-01/02).
/// @ingroup app

namespace ds::app {

/// @brief The storage service the application runs with.
struct UserStorage {
    std::unique_ptr<storage::StorageService> service;  ///< Never null on success.
    /// Non-empty if the user database could not be used and an in-memory database was opened
    /// instead: explains why, for a warning dialog. Settings are then not persisted.
    std::string warning;
};

/// @brief Opens the user database and applies migrations.
/// @param databaseOverride `--db <path>`; if set, it is used instead of the default location.
/// @param home Home directory holding `.drone_simulation` (normally storage::homeDirectory()).
/// @return The service, possibly on an in-memory database with UserStorage::warning set; an
///         error only if not even an in-memory database can be opened (e.g. no QSQLITE driver).
[[nodiscard]] storage::StorageResult<UserStorage> openUserStorage(
    const std::optional<std::filesystem::path>& databaseOverride,
    const std::filesystem::path& home);

}  // namespace ds::app
