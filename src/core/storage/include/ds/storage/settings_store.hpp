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
/// @brief Key/value settings persisted in the `settings` table as JSON values (FR-UI-03).
/// @ingroup storage

namespace ds::storage {

/// @brief Reads and writes settings. Values are stored JSON-encoded; typed accessors convert.
/// @details Keys are dotted names such as `ui.mainWindow.geometry`. A missing key gives
/// `std::nullopt`; a key holding a value of another type gives an InvalidValue error.
class SettingsStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit SettingsStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Raw JSON text of @p key.
    [[nodiscard]] StorageResult<std::optional<std::string>> json(std::string_view key);
    /// @brief Stores @p json (any JSON value: object, array, string, number, bool, null).
    [[nodiscard]] StorageResult<void> setJson(std::string_view key, std::string_view json);

    /// @brief String value.
    [[nodiscard]] StorageResult<std::optional<std::string>> string(std::string_view key);
    /// @brief Stores a string value.
    [[nodiscard]] StorageResult<void> setString(std::string_view key, std::string_view value);

    /// @brief Integer value (JSON number without fraction).
    [[nodiscard]] StorageResult<std::optional<std::int64_t>> integer(std::string_view key);
    /// @brief Stores an integer (exact up to ±2^53).
    [[nodiscard]] StorageResult<void> setInteger(std::string_view key, std::int64_t value);

    /// @brief Floating-point value.
    [[nodiscard]] StorageResult<std::optional<double>> number(std::string_view key);
    /// @brief Stores a finite floating-point value.
    [[nodiscard]] StorageResult<void> setNumber(std::string_view key, double value);

    /// @brief Boolean value.
    [[nodiscard]] StorageResult<std::optional<bool>> boolean(std::string_view key);
    /// @brief Stores a boolean.
    [[nodiscard]] StorageResult<void> setBoolean(std::string_view key, bool value);

    /// @brief Binary value (stored as a base64 JSON string), e.g. Qt window state.
    [[nodiscard]] StorageResult<std::optional<std::vector<std::uint8_t>>> bytes(
        std::string_view key);
    /// @brief Stores binary data.
    [[nodiscard]] StorageResult<void> setBytes(std::string_view key,
                                               const std::vector<std::uint8_t>& value);

    /// @brief Deletes @p key (no error if absent).
    [[nodiscard]] StorageResult<void> remove(std::string_view key);
    /// @brief All keys, sorted.
    [[nodiscard]] StorageResult<std::vector<std::string>> keys();

private:
    DbConnection* connection_;
};

}  // namespace ds::storage
