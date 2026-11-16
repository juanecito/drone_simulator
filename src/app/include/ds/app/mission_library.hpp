// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "ds/app/mission_validation.hpp"
#include "ds/common/error.hpp"
#include "ds/missions/catalog.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/storage/storage_service.hpp"

/// @file
/// @brief The list of available missions, built at start-up (FR-MIS-01, FR-MIS-01b, FR-DB-09).
/// @ingroup app

namespace ds::app {

/// @brief Name of the folder of default missions inside the asset directory.
inline constexpr std::string_view kDefaultMissionsDirName = "default-missions";

/// @brief Folders the mission library reads.
struct MissionFolders {
    std::filesystem::path assetDir;  ///< Holds `missions/` (schema, built-in pack) and `default-missions/`.
    std::filesystem::path userDir;   ///< `~/.drone_simulation/missions` ("" ⇒ no user missions).
    std::filesystem::path libraryDir;  ///< `~/.drone_simulation/library` (may be empty).
};

/// @brief The missions found at start-up.
struct MissionLibrary {
    std::unique_ptr<MissionValidation> validation;  ///< Schema and built-in keys.
    std::vector<missions::CatalogEntry> entries;    ///< Built-in first, then the user's.
    std::vector<std::filesystem::path> installed;   ///< Default missions copied this time.
};

/// @brief Index row of a catalogue entry (`missions` table).
[[nodiscard]] storage::MissionIndexRecord toIndexRecord(const missions::CatalogEntry& entry);

/// @brief Builds the list of available missions: installs the default missions into the
/// user folder if they are missing (FR-MIS-01b), scans the built-in and the user folders
/// (FR-MIS-01) and, if @p storage is given, synchronises the mission index in the database
/// (FR-DB-09; a mission whose id is already taken keeps no row of its own).
/// @return The library; an error only if the mission schema cannot be loaded. Problems with
///         the user folder or the database are logged and do not stop the start-up.
[[nodiscard]] common::Result<MissionLibrary> loadMissionLibrary(const MissionFolders& folders,
                                                                storage::StorageService* storage);

}  // namespace ds::app
