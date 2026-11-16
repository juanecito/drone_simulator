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

/// @brief Mission files directly in @p folder (`*.mission.json`) and in its sub-folders
/// (one mission folder each, with its `dem/` and `ortho/` files), sorted by path.
[[nodiscard]] std::vector<std::filesystem::path> findMissionFiles(const std::filesystem::path& folder);

/// @brief Loads every mission of the built-in folder and the user folder. Invalid files are
/// kept as invalid entries; a user mission whose id is already taken (by a built-in mission or
/// an earlier file) is invalid with an explanation.
[[nodiscard]] std::vector<CatalogEntry> scanMissions(const std::filesystem::path& builtInDir,
                                                     const std::filesystem::path& userDir,
                                                     const ValidationContext& context);

/// @brief Copies a mission into @p userDir (FR-MIS-01): a `.mission.json` file alone, or a
/// mission folder (exactly one `.mission.json` at its top) with everything in it. Symlinks are
/// not copied. Fails if the target exists, unless @p overwrite.
/// @return The path of the imported `.mission.json`.
[[nodiscard]] common::Result<std::filesystem::path> importMission(
    const std::filesystem::path& source, const std::filesystem::path& userDir,
    bool overwrite = false);

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
