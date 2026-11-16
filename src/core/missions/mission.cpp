// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/mission.hpp"

#include <algorithm>
#include <array>
#include <format>
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

std::string objectiveTitle(const MissionDefinition& mission, const ObjectiveSpec& o) {
    if (!o.title.empty()) {
        return o.title;
    }
    // Items by their display name when they have one.
    const auto name = [&](const std::string& itemId) {
        const ItemSpec* item = mission.item(itemId);
        return item != nullptr && !item->name.empty() ? item->name : itemId;
    };
    switch (o.type) {
        case ObjectiveType::ReachAltitude:
            return std::format("Climb to {:g} m", o.altitudeAglM.value_or(0.0));
        case ObjectiveType::ReachWaypoint:
            return o.target.empty() ? std::string("Reach the waypoint")
                                    : std::format("Reach {}", name(o.target));
        case ObjectiveType::Hover:
            return std::format("Hover for {:g} s", o.durationS.value_or(0.0));
        case ObjectiveType::Land: return std::format("Land on {}", name(o.target));
        case ObjectiveType::GateCourse:
            return o.laps > 1 ? std::format("Fly the gate course ({} laps)", o.laps)
                              : std::string("Fly the gate course");
        case ObjectiveType::Deliver:
            return std::format("Deliver {:g} kg from {} to {}", o.cargoKg, name(o.pickup),
                               name(o.drop));
        case ObjectiveType::Inspect: return std::format("Inspect {}", name(o.target));
        case ObjectiveType::Survey: return std::format("Survey {}", name(o.target));
        case ObjectiveType::ReturnHome: return "Return home";
        case ObjectiveType::Park: return std::format("Park in {}", name(o.target));
    }
    return o.id;
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
