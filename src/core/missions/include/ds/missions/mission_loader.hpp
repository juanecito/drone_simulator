// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ds/common/json.hpp"
#include "ds/common/json_schema.hpp"
#include "ds/missions/mission.hpp"

/// @file
/// @brief Loading and validating mission files (FR-MIS-01, FR-MIS-03a, FR-MIS-16): the JSON
/// Schema first, then the semantic checks (references between ids, objectives allowed for the
/// vehicle type, positions inside the terrain extent, reachable thresholds, weather
/// plausibility). Mission files are untrusted input: no error ever throws or crashes.
/// @ingroup missions

namespace ds::missions {

/// @brief One problem found in a mission file.
struct MissionIssue {
    std::string path;     ///< Dotted JSON path, e.g. `objectives[1].target` ("(root)" for the file).
    std::string message;  ///< e.g. `unknown item "pad2"`.

    /// @brief `path: message`.
    [[nodiscard]] std::string text() const;
};

/// @brief What the semantic checks may compare against. Empty sets are not checked.
struct ValidationContext {
    const common::JsonSchema* schema = nullptr;  ///< `mission.schema.json` (required).
    std::set<std::string> aerialProfiles;   ///< Built-in aerial profile keys.
    std::set<std::string> groundProfiles;   ///< Built-in ground profile keys.
    std::set<std::string> providers;        ///< Elevation / imagery provider keys.
    std::set<std::string> terrainPresets;   ///< Procedural preset names ("rolling-hills").
    /// Geodata library for `library:` paths; empty ⇒ terrain file locations are not checked.
    std::filesystem::path libraryDir;
};

/// @brief Result of loading or validating one mission.
struct MissionLoadResult {
    std::filesystem::path file;                   ///< The `.mission.json` ("" for JSON input).
    std::string sha256;                           ///< Hash of the file bytes ("" for JSON input).
    std::optional<MissionDefinition> definition;  ///< Set when there are no errors.
    std::vector<MissionIssue> errors;             ///< Make the mission invalid.
    std::vector<MissionIssue> warnings;           ///< Shown, but the mission stays valid.
    /// Reasons the mission cannot be played right now although valid (e.g. a missing
    /// elevation file, FR-MIS-02).
    std::vector<std::string> unavailable;

    /// @brief True if the mission is valid.
    [[nodiscard]] bool valid() const noexcept { return errors.empty() && definition.has_value(); }
    /// @brief Every error and warning, one per line, prefixed with the file name.
    [[nodiscard]] std::string report() const;
};

/// @brief Validates the parsed document @p json. @p missionDir is the folder used to resolve
/// relative terrain file paths ("" ⇒ not checked).
[[nodiscard]] MissionLoadResult validateMission(const common::Json& json,
                                                const ValidationContext& context,
                                                const std::filesystem::path& missionDir = {});

/// @brief Reads (at most kMaxMissionFileBytes), hashes and validates @p file.
[[nodiscard]] MissionLoadResult loadMissionFile(const std::filesystem::path& file,
                                                const ValidationContext& context);

/// @brief Keys of the built-in vehicle profiles found in `<assetDir>/vehicles/<type>/*.json`,
/// the providers in `<assetDir>/providers/*.json`; fills the matching sets of @p context.
void addBuiltInKeys(ValidationContext& context, const std::filesystem::path& assetDir);

/// @brief World (x, z) of @p position, or nullopt if it is geographic and @p origin is
/// unknown or the UTM coordinate is invalid.
[[nodiscard]] std::optional<items::Vec2> horizontalPosition(
    const MissionPosition& position, const std::optional<geo::LatLon>& origin);

}  // namespace ds::missions
