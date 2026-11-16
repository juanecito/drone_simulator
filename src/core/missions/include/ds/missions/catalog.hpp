// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/missions/mission_loader.hpp"

/// @file
/// @brief The mission catalogue (FR-MIS-01, FR-MIS-01a, FR-MIS-12): scanning the built-in and
/// the user's mission folders, importing missions, and campaign unlock rules.
/// @ingroup missions

namespace ds::missions {

/// @brief Where a mission comes from.
enum class MissionOrigin { BuiltIn, User };

/// @brief One mission file found by scan().
struct CatalogEntry {
    MissionOrigin origin = MissionOrigin::User;  ///< Folder it was found in.
    MissionLoadResult load;                      ///< Validation result (file, hash, issues).
    std::string key;  ///< Mission id (from the file even when invalid, else the file stem).
};

/// @brief Whether @p file is taken as a mission file (FR-MIS-01): every `*.mission.json`, and
/// every other `*.json` that is malformed or whose top-level object has `schemaVersion` or
/// `objectives` (so other JSON files in a mission folder are left alone). Names starting with
/// '.' and `*.schema.json` files are never missions.
[[nodiscard]] bool isMissionFile(const std::filesystem::path& file);

/// @brief Mission files (isMissionFile()) directly in @p folder and in its sub-folders (one
/// mission folder each, with its `dem/` and `ortho/` files), sorted by path. Symbolic links
/// are skipped.
[[nodiscard]] std::vector<std::filesystem::path> findMissionFiles(const std::filesystem::path& folder);

/// @brief Loads every mission of the built-in folder and the user folder. Invalid files are
/// kept as invalid entries; a user mission whose id is already taken (by a built-in mission or
/// an earlier file) is invalid with an explanation.
[[nodiscard]] std::vector<CatalogEntry> scanMissions(const std::filesystem::path& builtInDir,
                                                     const std::filesystem::path& userDir,
                                                     const ValidationContext& context);

/// @brief Copies a mission into @p userDir (FR-MIS-01): a mission file alone, or a mission
/// folder (exactly one mission file at its top) with everything in it. Symlinks are
/// not copied. Fails if the target exists, unless @p overwrite.
/// @return The path of the imported mission file.
[[nodiscard]] common::Result<std::filesystem::path> importMission(
    const std::filesystem::path& source, const std::filesystem::path& userDir,
    bool overwrite = false);

/// @brief Installs the default missions (FR-MIS-01b): copies each mission file of
/// @p defaultsDir into @p userDir (created if needed) unless a file of that name is already
/// there, or a mission with the same id already exists in @p userDir. Existing files are
/// never overwritten, so the user's edits are kept; a deleted default mission is installed
/// again.
/// @return The files created (empty if nothing had to be done); an error only if @p userDir
///         cannot be created. A file that cannot be copied is skipped.
[[nodiscard]] common::Result<std::vector<std::filesystem::path>> installDefaultMissions(
    const std::filesystem::path& defaultsDir, const std::filesystem::path& userDir);

/// @brief A mission's place in the campaign.
struct CampaignMission {
    std::string key;             ///< Mission id.
    std::string chapter;         ///< Chapter ("" ⇒ free missions, always unlocked).
    int order = 0;               ///< Position in the chapter.
    std::optional<UnlockSpec> unlock;  ///< Explicit rule.
};

/// @brief Which missions are unlocked (FR-MIS-12), given the best stars per mission key.
/// @details With an explicit `unlock` rule every listed mission needs `minStars`. Otherwise
/// the first mission of each chapter (by `order`, then key) is unlocked and every other one
/// needs at least one star on the mission before it.
[[nodiscard]] std::map<std::string, bool> unlockedMissions(
    const std::vector<CampaignMission>& missions, const std::map<std::string, int>& bestStars);

}  // namespace ds::missions
