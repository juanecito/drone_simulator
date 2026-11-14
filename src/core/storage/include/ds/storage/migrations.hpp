// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include "ds/storage/connection.hpp"

/// @file
/// @brief Schema versioning and ordered migrations (FR-DB-03, FR-DB-08).
/// @ingroup storage

namespace ds::storage {

/// @brief One schema step: the statements that turn version `version − 1` into `version`.
struct Migration {
    int version = 0;                               ///< Target version (1, 2, …, contiguous).
    std::string_view description;                  ///< Short description for logs.
    std::span<const std::string_view> statements;  ///< One SQL statement per element.
};

/// @brief What migrate() did.
struct MigrationReport {
    int fromVersion = 0;                          ///< Version before (0 = empty database).
    int toVersion = 0;                            ///< Version after.
    std::optional<std::filesystem::path> backup;  ///< Backup file written before migrating.
};

/// @brief The application's migrations, in order.
[[nodiscard]] std::span<const Migration> schemaMigrations() noexcept;

/// @brief Migrations of the separate tile-cache index database `cache.db` (FR-NET-04).
[[nodiscard]] std::span<const Migration> cacheSchemaMigrations() noexcept;

/// @brief Version the application's migrations lead to.
[[nodiscard]] int latestSchemaVersion() noexcept;

/// @brief Current version stored in `schema_version` (0 for an empty database).
[[nodiscard]] StorageResult<int> schemaVersion(DbConnection& connection);

/// @brief Applies the pending @p migrations in one transaction: either all of them or none.
/// @details Before migrating a file database that already has a schema, a consistent copy is
/// written to `<db>.bak-v<old version>` (FR-DB-08). A database newer than the last migration is
/// refused (NewerSchema) and left untouched.
[[nodiscard]] StorageResult<MigrationReport> migrate(DbConnection& connection,
                                                     std::span<const Migration> migrations);

/// @brief Applies the application's own migrations (schemaMigrations()).
[[nodiscard]] StorageResult<MigrationReport> migrate(DbConnection& connection);

}  // namespace ds::storage
