// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "ds/common/error.hpp"
#include "ds/missions/mission_loader.hpp"

/// @file
/// @brief Mission validation set-up shared by the GUI and `--validate-mission` (FR-MIS-16).
/// @ingroup app

namespace ds::app {

/// @brief Loads `<assetDir>/missions/mission.schema.json` and the built-in keys (vehicle
/// profiles, data providers, procedural terrain presets) for mission validation. The schema
/// is kept alive by the returned object.
struct MissionValidation {
    common::JsonSchema schema;          ///< The mission schema.
    missions::ValidationContext context;  ///< Points at `schema`; do not copy the object.

    MissionValidation(const MissionValidation&) = delete;
    MissionValidation& operator=(const MissionValidation&) = delete;
    MissionValidation(MissionValidation&&) = delete;
    MissionValidation& operator=(MissionValidation&&) = delete;
    ~MissionValidation() = default;

    /// @brief Builds the validation set-up from the asset folder @p assetDir and the geodata
    /// library @p libraryDir (may be empty).
    [[nodiscard]] static common::Result<std::unique_ptr<MissionValidation>> create(
        const std::filesystem::path& assetDir, const std::filesystem::path& libraryDir);

private:
    explicit MissionValidation(common::JsonSchema s) : schema(std::move(s)) {}
};

/// @brief Runs `--validate-mission`: validates each file, prints errors and warnings to
/// @p out (one per line, `file: error: path: message`) and a summary.
/// @return 0 if every file is valid, 1 otherwise (FR-MIS-16).
int validateMissionFiles(std::span<const std::filesystem::path> files, std::string& out);

}  // namespace ds::app
