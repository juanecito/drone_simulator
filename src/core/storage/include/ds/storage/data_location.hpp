// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string_view>

#include "ds/storage/storage_error.hpp"

/// @file
/// @brief Per-OS-user data directory `$HOME/.drone_simulation` (FR-DB-01, NFR-SEC-01).
/// @ingroup storage

namespace ds::storage {

/// @brief Name of the data directory inside the home directory.
inline constexpr std::string_view kDataDirectoryName = ".drone_simulation";
/// @brief File name of the user database inside the data directory.
inline constexpr std::string_view kDatabaseFileName = "drone_simulation.db";

/// @brief Home directory of the user running the process (`QDir::homePath()`, i.e. `$HOME`).
[[nodiscard]] std::filesystem::path homeDirectory();

/// @brief Creates `<home>/.drone_simulation` with mode 0700 if needed, tightens the mode of an
/// existing directory to 0700 and checks that it is writable.
/// @return The data directory, or a DataDirectory error (e.g. `$HOME` missing or read-only).
[[nodiscard]] StorageResult<std::filesystem::path> prepareDataDirectory(
    const std::filesystem::path& home);

/// @brief Creates @p file empty with mode 0600 if it does not exist, otherwise removes any
/// group/other permissions from it (NFR-SEC-01).
[[nodiscard]] StorageResult<void> ensurePrivateFile(const std::filesystem::path& file);

}  // namespace ds::storage
