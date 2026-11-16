// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/mission_world.hpp"

#include <cmath>
#include <format>

#include "ds/missions/mission_loader.hpp"

namespace ds::missions {
namespace {

std::optional<geo::LatLon> originOf(const MissionDefinition& m, const MissionPlacement& p) {
    if (p.origin) {
        return p.origin;
    }
    if (m.terrain.origin) {
        return m.terrain.origin;
    }
    if (m.terrain.file && m.terrain.file->originLatDeg && m.terrain.file->originLonDeg) {
        return geo::LatLon{*m.terrain.file->originLatDeg, *m.terrain.file->originLonDeg};
    }
    return std::nullopt;
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

}  // namespace

common::Result<items::Vec3> worldPosition(const MissionPosition& position,
                                          const MissionDefinition& mission,
                                          const MissionPlacement& placement) {
    const auto xz = horizontalPosition(position, originOf(mission, placement));
    if (!xz) {
        return common::makeError("geographic position without a terrain origin");
    }
    const double y = position.altitudeRef == items::AltitudeRef::Agl
                         ? (placement.ground ? placement.ground(xz->x, xz->y) : 0.0) +
                               position.altitudeM
                         : position.altitudeM - placement.originAltitudeM;
    return items::Vec3(xz->x, y, xz->y);
}

common::Result<items::ItemRegistry> buildItems(const MissionDefinition& mission,
                                               const MissionPlacement& placement) {
    items::ItemRegistry registry;
    for (const ItemSpec& spec : mission.items) {
        auto pos = worldPosition(spec.position, mission, placement);
        if (!pos) {
            return common::makeError(std::format("item \"{}\": {}", spec.id, pos.error().message));
        }
        items::Item item;
        item.type = spec.type;
        item.key = spec.id;
        item.name = spec.name.empty() ? spec.id : spec.name;
        item.transform.position = *pos;
        item.transform.rotation = items::headingRotation(spec.headingDeg);
        item.altitudeRef = spec.position.altitudeRef;
        item.altitudeM = spec.position.altitudeM;
        if (spec.position.kind != MissionPosition::Kind::Local) {
            if (spec.position.kind == MissionPosition::Kind::Geo) {
                item.geo = items::GeoPoint{spec.position.latitudeDeg, spec.position.longitudeDeg};
            } else if (const auto ll = geo::fromUtm(spec.position.utm)) {
                item.geo = items::GeoPoint{ll->latitudeDeg, ll->longitudeDeg};
            }
        }
        item.shape = items::defaultShape(spec.type, spec.properties);
        item.solid = items::isSolidType(spec.type);
        item.properties = spec.properties;
        if (auto id = registry.add(std::move(item)); !id) {
            return std::unexpected(id.error());
        }
    }
    registry.clearHistory();  // Loading is not an edit.
    return registry;
}

common::Json itemsToJson(const items::ItemRegistry& registry) {
    common::Json list = common::Json::array();
    for (const items::Item* item : registry.items()) {
        if (item->type == items::ItemType::Drone || item->type == items::ItemType::Spawn) {
            continue;
        }
        common::Json j = item->properties.is_object() ? item->properties : common::Json::object();
        j["type"] = items::itemTypeName(item->type);
        j["id"] = item->key;
        if (!item->name.empty() && item->name != item->key) {
            j["name"] = item->name;
        }
        const items::Vec3& p = item->transform.position;
        // The vertical value keeps the item's own reference (AGL items follow the terrain).
        j["local"] = {round3(p.x), round3(item->altitudeM), round3(p.z)};
        j["altRef"] = item->altitudeRef == items::AltitudeRef::Agl ? "AGL" : "AMSL";
        if (const double h = items::headingOf(item->transform.rotation); std::abs(h) > 1e-9) {
            j["headingDeg"] = round3(h);
        }
        list.push_back(std::move(j));
    }
    return list;
}

}  // namespace ds::missions
