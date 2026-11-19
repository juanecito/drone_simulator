// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "ds/app/scene_config.hpp"
#include "ds/common/error.hpp"
#include "ds/missions/mission.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/vehicle_factory.hpp"

/// @file
/// @brief Turning a mission definition into the scene it is flown in (FR-MIS-02, §4.9.1):
/// terrain configuration, environment (time, weather, wind, air) and vehicle profile.
/// @ingroup app

namespace ds::app {

/// @brief Terrain and environment of a mission.
struct MissionScene {
    TerrainConfig terrain;          ///< Procedural terrain of the mission.
    EnvironmentConfig environment;  ///< Time, weather, wind and air of the mission.
};

/// @brief Why this build cannot fly @p mission, or nullopt if it can.
/// @details Ground vehicles arrive with milestone M6; online and file terrain need the raster
/// sources of M4 (GDAL), which this build does not have yet.
[[nodiscard]] std::optional<std::string> unplayableReason(
    const missions::MissionDefinition& mission);

/// @brief Visibility of a weather preset ("fog" ⇒ 500 m, "clear" ⇒ 50 km), overridden by
/// `visibilityM`.
[[nodiscard]] double weatherVisibilityM(const missions::WeatherSpec& weather);

/// @brief Terrain of a procedural mission: the built-in preset `terrain.preset` (rolling hills
/// if none), then the mission's seed and `params`, origin and extent.
[[nodiscard]] common::Result<TerrainConfig> missionTerrain(
    const missions::MissionDefinition& mission);

/// @brief Scene of @p mission. Exposure and the time-animation settings are taken from
/// @p current; everything else comes from the mission.
[[nodiscard]] common::Result<MissionScene> missionScene(const missions::MissionDefinition& mission,
                                                        const EnvironmentConfig& current);

/// @brief The vehicle profile of @p mission: the embedded `customProfile`, else the profile
/// found by @p lookup (built-in or user key).
template <typename Lookup>
[[nodiscard]] common::Result<physics::VehicleProfile> missionProfile(
    const missions::MissionDefinition& mission, Lookup&& lookup) {
    if (!mission.vehicle.customProfile.is_null()) {
        auto profile = physics::vehicleProfileFromJson(mission.vehicle.customProfile);
        if (!profile) {
            return common::makeError("vehicle.customProfile: " + profile.error().message);
        }
        std::visit(
            [&](auto& p) {
                if (p.key.empty()) {
                    p.key = "mission." + mission.id;
                }
            },
            *profile);
        return profile;
    }
    return lookup(mission.vehicle.profile);
}

}  // namespace ds::app
