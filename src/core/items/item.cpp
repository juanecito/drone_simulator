// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/items/item.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace ds::items {
namespace {

constexpr double kBig = 1e7;  // Stand-in for infinite bounds.

struct TypeName {
    ItemType type;
    std::string_view name;
};

constexpr std::array<TypeName, 17> kTypeNames{{
    {ItemType::Drone, "drone"},
    {ItemType::Spawn, "spawn"},
    {ItemType::Obstacle, "obstacle"},
    {ItemType::Gate, "gate"},
    {ItemType::Waypoint, "waypoint"},
    {ItemType::LandingPad, "landingPad"},
    {ItemType::Payload, "payload"},
    {ItemType::PickupZone, "pickupZone"},
    {ItemType::DropZone, "dropZone"},
    {ItemType::InspectionTarget, "inspectionTarget"},
    {ItemType::SurveyArea, "surveyArea"},
    {ItemType::NoFlyZone, "noFlyZone"},
    {ItemType::Geofence, "geofence"},
    {ItemType::PilotPosition, "pilot"},
    {ItemType::ParkingZone, "parkingZone"},
    {ItemType::Ramp, "ramp"},
    {ItemType::Bridge, "bridge"},
}};

double num(const common::Json& j, const char* key, double fallback) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) {
        const double v = j[key].get<double>();
        if (std::isfinite(v)) {
            return v;
        }
    }
    return fallback;
}

double positive(const common::Json& j, const char* key, double fallback) {
    const double v = num(j, key, fallback);
    return v > 0.0 ? v : fallback;
}

// `sizeM`: [width, length] or [width, height, length].
std::optional<Vec3> size3(const common::Json& j) {
    if (!j.is_object() || !j.contains("sizeM") || !j["sizeM"].is_array()) {
        return std::nullopt;
    }
    const common::Json& s = j["sizeM"];
    const auto at = [&](std::size_t i) {
        return s[i].is_number() ? std::max(s[i].get<double>(), 0.01) : 1.0;
    };
    if (s.size() == 2) {
        return Vec3(at(0), std::numeric_limits<double>::quiet_NaN(), at(1));
    }
    if (s.size() == 3) {
        return Vec3(at(0), at(1), at(2));
    }
    return std::nullopt;
}

bool pointInPolygon(const std::vector<Vec2>& poly, double x, double z) noexcept {
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if ((a.y > z) != (b.y > z)) {
            const double xCross = a.x + (z - a.y) / (b.y - a.y) * (b.x - a.x);
            if (x <= xCross) {
                inside = !inside;
            }
        }
    }
    return inside;
}

}  // namespace

std::string_view itemTypeName(ItemType type) noexcept {
    for (const auto& t : kTypeNames) {
        if (t.type == type) {
            return t.name;
        }
    }
    return "unknown";
}

std::optional<ItemType> parseItemType(std::string_view name) noexcept {
    for (const auto& t : kTypeNames) {
        if (t.name == name) {
            return t.type;
        }
    }
    return std::nullopt;
}

const std::vector<ItemType>& allItemTypes() {
    static const std::vector<ItemType> kAll = [] {
        std::vector<ItemType> v;
        for (const auto& t : kTypeNames) {
            v.push_back(t.type);
        }
        return v;
    }();
    return kAll;
}

bool isZoneType(ItemType type) noexcept {
    switch (type) {
        case ItemType::Waypoint:
        case ItemType::LandingPad:
        case ItemType::PickupZone:
        case ItemType::DropZone:
        case ItemType::SurveyArea:
        case ItemType::NoFlyZone:
        case ItemType::Geofence:
        case ItemType::ParkingZone: return true;
        default: return false;
    }
}

bool isSolidType(ItemType type) noexcept {
    return type == ItemType::Obstacle || type == ItemType::Ramp || type == ItemType::Bridge;
}

Shape Shape::sphere(double r) {
    Shape s;
    s.kind = Kind::Sphere;
    s.radiusM = r;
    return s;
}

Shape Shape::box(const Vec3& half, const Vec3& offset) {
    Shape s;
    s.kind = Kind::Box;
    s.halfExtentsM = half;
    s.offsetM = offset;
    return s;
}

Shape Shape::cylinder(double r, double bottom, double top) {
    Shape s;
    s.kind = Kind::Cylinder;
    s.radiusM = r;
    s.bottomM = bottom;
    s.topM = top;
    return s;
}

Shape Shape::prism(std::vector<Vec2> outline, double bottom, double top) {
    Shape s;
    s.kind = Kind::Prism;
    s.polygon = std::move(outline);
    s.bottomM = bottom;
    s.topM = top;
    return s;
}

Shape Shape::gate(double width, double height) {
    Shape s;
    s.kind = Kind::Gate;
    s.halfExtentsM = Vec3(0.5 * width, 0.5 * height, 0.0);
    return s;
}

bool Shape::contains(const Vec3& p) const noexcept {
    switch (kind) {
        case Kind::Sphere: {
            const Vec3 d = p - offsetM;
            return glm::dot(d, d) <= radiusM * radiusM;
        }
        case Kind::Box: {
            const Vec3 d = p - offsetM;
            return std::abs(d.x) <= halfExtentsM.x && std::abs(d.y) <= halfExtentsM.y &&
                   std::abs(d.z) <= halfExtentsM.z;
        }
        case Kind::Cylinder:
            return p.y >= bottomM && p.y <= topM && p.x * p.x + p.z * p.z <= radiusM * radiusM;
        case Kind::Prism:
            return polygon.size() >= 3 && p.y >= bottomM && p.y <= topM &&
                   pointInPolygon(polygon, p.x, p.z);
        case Kind::Point:
        case Kind::Gate: return false;
    }
    return false;
}

void Shape::localBounds(Vec3& lo, Vec3& hi) const noexcept {
    const auto clampY = [](double v) { return std::clamp(v, -kBig, kBig); };
    switch (kind) {
        case Kind::Point:
            lo = hi = Vec3(0.0);
            return;
        case Kind::Sphere:
            lo = offsetM - Vec3(radiusM);
            hi = offsetM + Vec3(radiusM);
            return;
        case Kind::Box:
            lo = offsetM - halfExtentsM;
            hi = offsetM + halfExtentsM;
            return;
        case Kind::Gate: {
            // The frame around the opening.
            const double t = gateFrameThicknessM(halfExtentsM.x, halfExtentsM.y);
            lo = -halfExtentsM - Vec3(t);
            hi = halfExtentsM + Vec3(t);
            return;
        }
        case Kind::Cylinder:
            lo = Vec3(-radiusM, clampY(bottomM), -radiusM);
            hi = Vec3(radiusM, clampY(topM), radiusM);
            return;
        case Kind::Prism: {
            lo = Vec3(kBig, clampY(bottomM), kBig);
            hi = Vec3(-kBig, clampY(topM), -kBig);
            for (const Vec2& v : polygon) {
                lo.x = std::min(lo.x, v.x);
                lo.z = std::min(lo.z, v.y);
                hi.x = std::max(hi.x, v.x);
                hi.z = std::max(hi.z, v.y);
            }
            if (polygon.empty()) {
                lo.x = lo.z = hi.x = hi.z = 0.0;
            }
            return;
        }
    }
}

Vec3 Transform::toLocal(const Vec3& world) const noexcept {
    return (glm::conjugate(rotation) * (world - position)) / scale;
}

Vec3 Transform::toWorld(const Vec3& local) const noexcept {
    return position + rotation * (local * scale);
}

Vec3 Transform::directionToWorld(const Vec3& local) const noexcept { return rotation * local; }

Quat headingRotation(double headingDeg) noexcept {
    // Rotating the local forward (−z, north) clockwise seen from above is a negative rotation
    // about +y.
    return glm::angleAxis(-headingDeg * std::numbers::pi / 180.0, Vec3(0.0, 1.0, 0.0));
}

double headingOf(const Quat& rotation) noexcept {
    const Vec3 f = rotation * Vec3(0.0, 0.0, -1.0);
    // Heading clockwise from north: east = +x, north = −z.
    double deg = std::atan2(f.x, -f.z) * 180.0 / std::numbers::pi;
    if (deg < 0.0) {
        deg += 360.0;
    }
    return deg >= 360.0 ? 0.0 : deg;
}

void Item::worldBounds(Vec3& lo, Vec3& hi) const noexcept {
    Vec3 llo;
    Vec3 lhi;
    shape.localBounds(llo, lhi);
    lo = Vec3(std::numeric_limits<double>::max());
    hi = Vec3(std::numeric_limits<double>::lowest());
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner((i & 1) != 0 ? lhi.x : llo.x, (i & 2) != 0 ? lhi.y : llo.y,
                          (i & 4) != 0 ? lhi.z : llo.z);
        const Vec3 w = transform.toWorld(corner);
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
}

Shape defaultShape(ItemType type, const common::Json& p) {
    const double radius = [&] {
        switch (type) {
            case ItemType::Waypoint: return positive(p, "radiusM", 2.0);
            case ItemType::LandingPad: return positive(p, "radiusM", 1.0);
            case ItemType::InspectionTarget: return positive(p, "radiusM", 0.5);
            default: return positive(p, "radiusM", 3.0);
        }
    }();
    const auto polygon = [&]() -> std::vector<Vec2> {
        std::vector<Vec2> out;
        if (p.is_object() && p.contains("polygon") && p["polygon"].is_array()) {
            for (const auto& v : p["polygon"]) {
                if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) {
                    out.emplace_back(v[0].get<double>(), v[1].get<double>());
                }
            }
        }
        return out;
    };
    const auto volume = [&](double defaultTop) {
        const double bottom = num(p, "bottomM", -kBig);
        const double top = num(p, "topM", defaultTop);
        if (auto poly = polygon(); poly.size() >= 3) {
            return Shape::prism(std::move(poly), bottom, top);
        }
        return Shape::cylinder(radius, bottom, top);
    };
    switch (type) {
        case ItemType::Drone:
        case ItemType::Spawn:
        case ItemType::PilotPosition: return Shape::point();
        case ItemType::Waypoint:
        case ItemType::InspectionTarget: return Shape::sphere(radius);
        case ItemType::LandingPad: return Shape::cylinder(radius, -0.5, 0.5);
        case ItemType::PickupZone:
        case ItemType::DropZone: {
            const double height = positive(p, "heightM", 10.0);
            if (const auto s = size3(p)) {
                // From 1 m below the origin (terrain slope) to `heightM` above it.
                return Shape::box(Vec3(0.5 * s->x, 0.5 * (height + 1.0), 0.5 * s->z),
                                  Vec3(0.0, 0.5 * (height - 1.0), 0.0));
            }
            return Shape::cylinder(radius, -1.0, height);
        }
        case ItemType::ParkingZone: {
            const auto s = size3(p).value_or(Vec3(2.0, 0.0, 3.0));
            const double height = positive(p, "heightM", 2.0);
            return Shape::box(Vec3(0.5 * s.x, 0.5 * (height + 0.5), 0.5 * s.z),
                              Vec3(0.0, 0.5 * (height - 0.5), 0.0));
        }
        case ItemType::SurveyArea: return volume(kBig);
        case ItemType::NoFlyZone:
        case ItemType::Geofence: return volume(kBig);
        case ItemType::Gate:
            return Shape::gate(positive(p, "widthM", 2.0), positive(p, "heightM", 2.0));
        case ItemType::Payload: return Shape::box(Vec3(0.15));
        case ItemType::Obstacle:
        case ItemType::Ramp:
        case ItemType::Bridge: {
            const std::string kind = p.is_object() && p.contains("shape") && p["shape"].is_string()
                                         ? p["shape"].get<std::string>()
                                         : std::string("box");
            if (kind == "cylinder" || kind == "tree" || kind == "windTurbine") {
                const double height = positive(p, "heightM", kind == "windTurbine" ? 100.0 : 8.0);
                const double r = positive(p, "radiusM", kind == "tree" ? 2.0 : 1.0);
                return Shape::cylinder(r, 0.0, height);
            }
            const auto s = size3(p).value_or(Vec3(2.0));
            const double h = std::isnan(s.y) ? positive(p, "heightM", 2.0) : s.y;
            // Origin on the ground at the centre of the base.
            return Shape::box(Vec3(0.5 * s.x, 0.5 * h, 0.5 * s.z), Vec3(0.0, 0.5 * h, 0.0));
        }
    }
    return Shape::point();
}

// --- Obstacle contacts ------------------------------------------------------------------------

namespace {

// Contact of a local point inside the box centred on @p centre with half sizes @p half: the
// face with the least penetration.
std::optional<SurfaceContact> boxContact(const Vec3& q, const Vec3& centre, const Vec3& half) {
    const Vec3 d = q - centre;
    double best = kBig;
    SurfaceContact c;
    for (int axis = 0; axis < 3; ++axis) {
        const double depth = half[axis] - std::abs(d[axis]);
        if (depth < 0.0) {
            return std::nullopt;
        }
        if (depth < best) {
            best = depth;
            c.normal = Vec3(0.0);
            c.normal[axis] = d[axis] >= 0.0 ? 1.0 : -1.0;
            c.depthM = depth;
        }
    }
    return c;
}

}  // namespace

double gateFrameThicknessM(double halfWidthM, double halfHeightM) noexcept {
    return std::clamp(0.06 * (halfWidthM + halfHeightM), 0.05, 0.3);
}

bool blocksVehicle(const Item& item) noexcept {
    return item.enabled && (item.solid || item.type == ItemType::Gate);
}

std::optional<SurfaceContact> obstacleContact(const Item& item, const Vec3& p) {
    if (!blocksVehicle(item)) {
        return std::nullopt;
    }
    const Vec3 q = item.transform.toLocal(p);
    const Shape& s = item.shape;
    std::optional<SurfaceContact> local;
    switch (s.kind) {
        case Shape::Kind::Box: local = boxContact(q, s.offsetM, s.halfExtentsM); break;
        case Shape::Kind::Cylinder: {
            const double rho = std::hypot(q.x, q.z);
            if (q.y < s.bottomM || q.y > s.topM || rho > s.radiusM) {
                break;
            }
            SurfaceContact c;
            c.depthM = s.radiusM - rho;
            c.normal = rho > 1e-9 ? Vec3(q.x / rho, 0.0, q.z / rho) : Vec3(1.0, 0.0, 0.0);
            if (s.topM - q.y < c.depthM) {
                c = {Vec3(0.0, 1.0, 0.0), s.topM - q.y};
            }
            if (q.y - s.bottomM < c.depthM) {
                c = {Vec3(0.0, -1.0, 0.0), q.y - s.bottomM};
            }
            local = c;
            break;
        }
        case Shape::Kind::Gate: {
            // Top and bottom bars, and the side posts down to the ground.
            const double hw = s.halfExtentsM.x;
            const double hh = s.halfExtentsM.y;
            const double t = gateFrameThicknessM(hw, hh);
            const double below = item.altitudeRef == AltitudeRef::Agl
                                     ? std::max(item.altitudeM - hh - t, 0.0)
                                     : 0.0;
            const double postBottom = -hh - t - below;
            for (const auto& [centre, half] :
                 {std::pair{Vec3(0.0, hh + 0.5 * t, 0.0), Vec3(hw + t, 0.5 * t, 0.5 * t)},
                  std::pair{Vec3(0.0, -hh - 0.5 * t, 0.0), Vec3(hw + t, 0.5 * t, 0.5 * t)},
                  std::pair{Vec3(-hw - 0.5 * t, 0.5 * (postBottom + hh + t), 0.0),
                            Vec3(0.5 * t, 0.5 * (hh + t - postBottom), 0.5 * t)},
                  std::pair{Vec3(hw + 0.5 * t, 0.5 * (postBottom + hh + t), 0.0),
                            Vec3(0.5 * t, 0.5 * (hh + t - postBottom), 0.5 * t)}}) {
                if (auto c = boxContact(q, centre, half)) {
                    local = c;
                    break;
                }
            }
            break;
        }
        case Shape::Kind::Point:
        case Shape::Kind::Sphere:
        case Shape::Kind::Prism: break;  // Markers and zones: not obstacles.
    }
    if (!local) {
        return std::nullopt;
    }
    // Back to the world frame (scaled shapes: depth along the scaled normal).
    const Vec3 n = item.transform.rotation * (local->normal * item.transform.scale);
    const double len = glm::length(n);
    return SurfaceContact{n / len, local->depthM * len};
}

}  // namespace ds::items
