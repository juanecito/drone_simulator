// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief Error type of the storage API (FR-DB-06): `QSqlError` and file-system errors are
/// mapped to `StorageResult<T>` at the API boundary.
/// @ingroup storage

namespace ds::storage {

/// @brief What kind of storage operation failed.
enum class StorageErrorCode {
    DriverUnavailable,  ///< The Qt SQLite driver (QSQLITE) is not installed.
    CannotOpen,         ///< The database file could not be created or opened.
    DataDirectory,      ///< `~/.drone_simulation` could not be created or is not writable.
    Query,              ///< An SQL statement failed.
    Transaction,        ///< BEGIN/COMMIT failed.
    Migration,          ///< A schema migration failed (the database is left unchanged).
    NewerSchema,        ///< The database was written by a newer version of the application.
    InvalidValue,       ///< A stored value is not valid JSON or has the wrong type.
    Stopped,            ///< The storage service is not running.
};

/// @brief A storage failure with a message meant for logs and, where useful, the user.
struct StorageError {
    StorageErrorCode code = StorageErrorCode::Query;  ///< Category.
    std::string message;                              ///< Human-readable description.
};

/// @brief Result of a storage operation.
template <typename T>
using StorageResult = std::expected<T, StorageError>;

/// @brief Convenience factory for a failed StorageResult.
[[nodiscard]] inline std::unexpected<StorageError> storageError(StorageErrorCode code,
                                                                std::string message) {
    return std::unexpected<StorageError>(StorageError{code, std::move(message)});
}

/// @brief Name of an error code, e.g. "query".
[[nodiscard]] std::string_view toString(StorageErrorCode code) noexcept;

}  // namespace ds::storage
