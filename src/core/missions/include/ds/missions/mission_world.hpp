// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <optional>

#include "ds/common/error.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/items/registry.hpp"
#include "ds/missions/mission.hpp"

/// @file
/// @brief Placing a mission in the world (FR-MIS-03, FR-ITM-07, FR-ITM-11): mission positions
/// → engine world coordinates, and the mission's items as an ItemRegistry.
/// @ingroup missions

namespace ds::missions {

/// @brief True if the straight line between two world points is not blocked by the terrain.
using LineOfSightFunction = std::function<bool(const items::Vec3& from, const items::Vec3& to)>;

/// @brief What is needed to turn mission positions into world positions.
struct MissionPlacement {
    /// Geographic origin of world (0, 0); defaults to the mission's terrain origin. Needed for
    /// geo / UTM positions (for file terrain it may come from the files).
    std::optional<geo::LatLon> origin;
    items::HeightFunction ground;  ///< Terrain height at (x, z); empty ⇒ flat at 0.
    double originAltitudeM = 0.0;  ///< AMSL altitude of world height 0.
};

/// @brief World position of @p position: horizontal from the origin, vertical from its
/// altitude reference (AGL: terrain + altitude; AMSL: altitude − originAltitudeM).
[[nodiscard]] common::Result<items::Vec3> worldPosition(const MissionPosition& position,
                                                        const MissionDefinition& mission,
                                                        const MissionPlacement& placement);

/// @brief The mission's items, placed on the terrain. Item keys are the mission ids.
[[nodiscard]] common::Result<items::ItemRegistry> buildItems(const MissionDefinition& mission,
                                                             const MissionPlacement& placement);

/// @brief Serialises the registry's items back to mission `items` JSON with `local`
/// positions (FR-ITM-11; editor and scene export). Spawn and drone items are skipped.
[[nodiscard]] common::Json itemsToJson(const items::ItemRegistry& registry);

}  // namespace ds::missions
