// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/mission.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace ds::missions {
namespace {

constexpr std::array<std::pair<ObjectiveType, std::string_view>, 10> kObjectiveNames{{
    {ObjectiveType::ReachAltitude, "reachAltitude"},
    {ObjectiveType::ReachWaypoint, "reachWaypoint"},
    {ObjectiveType::Hover, "hover"},
    {ObjectiveType::Land, "land"},
    {ObjectiveType::GateCourse, "gateCourse"},
    {ObjectiveType::Deliver, "deliver"},
    {ObjectiveType::Inspect, "inspect"},
    {ObjectiveType::Survey, "survey"},
    {ObjectiveType::ReturnHome, "returnHome"},
    {ObjectiveType::Park, "park"},
}};

}  // namespace

std::string_view objectiveTypeName(ObjectiveType type) noexcept {
    for (const auto& [t, name] : kObjectiveNames) {
        if (t == type) {
            return name;
        }
    }
    return "unknown";
}

std::optional<ObjectiveType> parseObjectiveType(std::string_view name) noexcept {
    for (const auto& [t, n] : kObjectiveNames) {
        if (n == name) {
            return t;
        }
    }
    return std::nullopt;
}

bool objectiveAllowed(ObjectiveType type, physics::VehicleType vehicle) noexcept {
    if (vehicle == physics::VehicleType::Aerial) {
        return type != ObjectiveType::Park;
    }
    return type != ObjectiveType::ReachAltitude && type != ObjectiveType::Hover &&
           type != ObjectiveType::Land;
}

const ItemSpec* MissionDefinition::item(std::string_view itemId) const noexcept {
    const auto it = std::ranges::find(items, itemId, &ItemSpec::id);
    return it == items.end() ? nullptr : &*it;
}

const ObjectiveSpec* MissionDefinition::objective(std::string_view objectiveId) const noexcept {
    const auto it = std::ranges::find(objectives, objectiveId, &ObjectiveSpec::id);
    return it == objectives.end() ? nullptr : &*it;
}

}  // namespace ds::missions
