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

bool isDrivableType(ItemType type) noexcept {
    return type == ItemType::Ramp || type == ItemType::Bridge;
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

// --- Materials and obstacle figures ----------------------------------------------------------

SurfaceProperties surfaceOf(Material material) noexcept {
    switch (material) {
        case Material::Concrete: return {0.2, 0.5};
        case Material::Masonry: return {0.2, 0.6};
        case Material::RoofTiles: return {0.25, 0.5};
        case Material::Wood: return {0.25, 0.6};
        case Material::Foliage: return {0.05, 0.9};  // Branches give way and catch.
        case Material::Hide:
        case Material::Skin: return {0.1, 0.6};  // Soft bodies absorb the impact.
        case Material::Clothing: return {0.1, 0.7};
    }
    return {};
}

double SolidPart::volumeM3() const noexcept {
    switch (kind) {
        case Kind::Box: return 8.0 * halfExtentsM.x * halfExtentsM.y * halfExtentsM.z;
        case Kind::Cylinder: return std::numbers::pi * radiusM * radiusM * heightM;
        case Kind::Sphere: return 4.0 / 3.0 * std::numbers::pi * radiusM * radiusM * radiusM;
        case Kind::Gable:
        case Kind::Wedge: return 2.0 * halfExtentsM.x * heightM * halfExtentsM.z;
    }
    return 0.0;
}

namespace {

SolidPart boxPart(const Vec3& centre, const Vec3& half, Material material) {
    return {.kind = SolidPart::Kind::Box,
            .centreM = centre,
            .halfExtentsM = half,
            .material = material};
}

SolidPart cylinderPart(const Vec3& bottomCentre, double radius, double height, Material material) {
    return {.kind = SolidPart::Kind::Cylinder,
            .centreM = bottomCentre,
            .radiusM = radius,
            .heightM = height,
            .material = material};
}

SolidPart spherePart(const Vec3& centre, double radius, Material material) {
    return {.kind = SolidPart::Kind::Sphere,
            .centreM = centre,
            .radiusM = radius,
            .material = material};
}

// House of width w (x), height h and length l (z): walls, gable roof along z with eaves, chimney.
std::vector<SolidPart> houseParts(double w, double h, double l) {
    const double eaves = std::min(0.3, 0.05 * std::min(w, l));
    const double wallTop = 0.62 * h;
    const double chimney = std::clamp(0.035 * w, 0.15, 0.5);
    return {
        boxPart(Vec3(0.0, 0.5 * wallTop, 0.0),
                Vec3(0.5 * w - eaves, 0.5 * wallTop, 0.5 * l - eaves), Material::Masonry),
        {.kind = SolidPart::Kind::Gable,
         .centreM = Vec3(0.0, wallTop, 0.0),
         .halfExtentsM = Vec3(0.5 * w, 0.0, 0.5 * l),
         .heightM = h - wallTop,
         .material = Material::RoofTiles},
        // Through the roof half-way up its east slope, up to 95 % of the height.
        boxPart(Vec3(0.25 * w, 0.5 * (wallTop + 0.95 * h), 0.2 * l),
                Vec3(chimney, 0.5 * (0.95 * h - wallTop), chimney), Material::Masonry),
    };
}

// Tree of crown radius r and height h (the drawn tree: trunk and two crown spheres).
std::vector<SolidPart> treeParts(double r, double h) {
    return {
        cylinderPart(Vec3(0.0), std::max(0.12 * r, 0.05), 0.62 * h, Material::Wood),
        spherePart(Vec3(0.0, 0.62 * h, 0.0), r, Material::Foliage),
        spherePart(Vec3(0.0, 0.86 * h, 0.0), 0.6 * r, Material::Foliage),
    };
}

// Cow of width w, height h (to the top of the head) and length l, head towards −z.
std::vector<SolidPart> cowParts(double w, double h, double l) {
    std::vector<SolidPart> parts{
        boxPart(Vec3(0.0, 0.675 * h, 0.07 * l), Vec3(0.5 * w, 0.225 * h, 0.37 * l),
                Material::Hide),  // Body, from the chest (−0.30 l) to the rump (+0.44 l).
        boxPart(Vec3(0.0, 0.82 * h, -0.39 * l), Vec3(0.3 * w, 0.13 * h, 0.1 * l),
                Material::Hide),  // Head.
    };
    for (const double x : {-0.3 * w, 0.3 * w}) {
        for (const double z : {-0.22 * l, 0.34 * l}) {
            parts.push_back(cylinderPart(Vec3(x, 0.0, z), 0.09 * w, 0.47 * h, Material::Hide));
        }
    }
    return parts;
}

// Standing man of height h, facing −z.
std::vector<SolidPart> manParts(double h) {
    std::vector<SolidPart> parts;
    for (const double x : {-0.05 * h, 0.05 * h}) {
        parts.push_back(boxPart(Vec3(x, 0.235 * h, 0.0), Vec3(0.04 * h, 0.235 * h, 0.045 * h),
                                Material::Clothing));  // Legs.
    }
    parts.push_back(boxPart(Vec3(0.0, 0.62 * h, 0.0), Vec3(0.125 * h, 0.15 * h, 0.065 * h),
                            Material::Clothing));  // Torso.
    for (const double x : {-0.155 * h, 0.155 * h}) {
        parts.push_back(boxPart(Vec3(x, 0.6 * h, 0.0), Vec3(0.03 * h, 0.15 * h, 0.035 * h),
                                Material::Clothing));  // Arms.
    }
    parts.push_back(cylinderPart(Vec3(0.0, 0.77 * h, 0.0), 0.03 * h, 0.11 * h, Material::Skin));
    parts.push_back(spherePart(Vec3(0.0, 0.937 * h, 0.0), 0.063 * h, Material::Skin));
    return parts;
}

// Wedge of width w and length l rising by h towards −z (+z when flipped), base centred on c.
SolidPart wedgePart(const Vec3& c, double w, double h, double l, bool flipped, Material material) {
    return {.kind = SolidPart::Kind::Wedge,
            .centreM = c,
            .halfExtentsM = Vec3(0.5 * w, 0.0, 0.5 * l),
            .heightM = h,
            .material = material,
            .flipped = flipped};
}

// Bridge along z: deck of width w, top at h over the length l, approach ramps of length r at
// both ends, kerbs along the deck.
std::vector<SolidPart> bridgeParts(double w, double h, double l, double r, bool kerbs) {
    const double thickness = std::min(0.3, h);
    std::vector<SolidPart> parts{
        boxPart(Vec3(0.0, h - 0.5 * thickness, 0.0), Vec3(0.5 * w, 0.5 * thickness, 0.5 * l),
                Material::Concrete),
        wedgePart(Vec3(0.0, 0.0, 0.5 * (l + r)), w, h, r, false, Material::Concrete),
        wedgePart(Vec3(0.0, 0.0, -0.5 * (l + r)), w, h, r, true, Material::Concrete),
    };
    if (kerbs) {
        const double kerbW = std::clamp(0.05 * w, 0.04, 0.2);
        const double kerbH = std::clamp(0.1 * w, 0.08, 0.25);
        for (const double side : {-1.0, 1.0}) {
            parts.push_back(boxPart(Vec3(side * 0.5 * (w - kerbW), h + 0.5 * kerbH, 0.0),
                                    Vec3(0.5 * kerbW, 0.5 * kerbH, 0.5 * l), Material::Concrete));
        }
    }
    return parts;
}

// Bounding box of @p parts, as a shape that keeps them.
Shape compound(std::vector<SolidPart> parts) {
    Vec3 lo(kBig);
    Vec3 hi(-kBig);
    for (const SolidPart& part : parts) {
        Vec3 a;
        Vec3 b;
        switch (part.kind) {
            case SolidPart::Kind::Box:
                a = part.centreM - part.halfExtentsM;
                b = part.centreM + part.halfExtentsM;
                break;
            case SolidPart::Kind::Cylinder:
                a = part.centreM - Vec3(part.radiusM, 0.0, part.radiusM);
                b = part.centreM + Vec3(part.radiusM, part.heightM, part.radiusM);
                break;
            case SolidPart::Kind::Sphere:
                a = part.centreM - Vec3(part.radiusM);
                b = part.centreM + Vec3(part.radiusM);
                break;
            case SolidPart::Kind::Gable:
            case SolidPart::Kind::Wedge:
                a = part.centreM - Vec3(part.halfExtentsM.x, 0.0, part.halfExtentsM.z);
                b = part.centreM + Vec3(part.halfExtentsM.x, part.heightM, part.halfExtentsM.z);
                break;
        }
        lo = glm::min(lo, a);
        hi = glm::max(hi, b);
    }
    Shape s = Shape::box(0.5 * (hi - lo), 0.5 * (hi + lo));
    s.parts = std::move(parts);
    return s;
}

std::string obstacleKind(const common::Json& p) {
    return p.is_object() && p.contains("shape") && p["shape"].is_string()
               ? p["shape"].get<std::string>()
               : std::string("box");
}

// Shape of an obstacle figure; origin on the ground at the centre of the base.
Shape obstacleShape(const std::string& kind, const common::Json& p) {
    // `sizeM` [width, height, length] or [width, length] with `heightM`.
    const auto size = [&](const Vec3& fallback) {
        Vec3 s = size3(p).value_or(
            Vec3(fallback.x, std::numeric_limits<double>::quiet_NaN(), fallback.z));
        if (std::isnan(s.y)) {
            s.y = positive(p, "heightM", fallback.y);
        }
        return s;
    };
    if (kind == "house") {
        const Vec3 s = size(Vec3(8.0, 7.0, 10.0));
        return compound(houseParts(s.x, s.y, s.z));
    }
    if (kind == "tree") {
        return compound(treeParts(positive(p, "radiusM", 2.0), positive(p, "heightM", 8.0)));
    }
    if (kind == "cow") {
        const Vec3 s = size(Vec3(0.8, 1.5, 2.4));
        return compound(cowParts(s.x, s.y, s.z));
    }
    if (kind == "man") {
        return compound(manParts(positive(p, "heightM", 1.75)));
    }
    if (kind == "cylinder" || kind == "windTurbine") {
        const double height = positive(p, "heightM", kind == "windTurbine" ? 100.0 : 8.0);
        return Shape::cylinder(positive(p, "radiusM", 1.0), 0.0, height);
    }
    const Vec3 s = size(Vec3(2.0));
    Shape box = Shape::box(0.5 * s, Vec3(0.0, 0.5 * s.y, 0.0));
    if (kind == "building") {
        box.material = Material::Masonry;
    }
    return box;
}

}  // namespace

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
        case ItemType::Ramp:
        case ItemType::Bridge: {
            // `sizeM` [width, height, length] or [width, length] with `heightM`.
            const bool ramp = type == ItemType::Ramp;
            Vec3 s = size3(p).value_or(ramp ? Vec3(1.0, 0.25, 1.5) : Vec3(1.0, 0.5, 3.0));
            if (std::isnan(s.y)) {
                s.y = positive(p, "heightM", ramp ? 0.25 : 0.5);
            }
            if (ramp) {
                return compound({wedgePart(Vec3(0.0), s.x, s.y, s.z, false, Material::Wood)});
            }
            const double slope = kBridgeRampSlopeDeg * std::numbers::pi / 180.0;
            const double rampLength = positive(p, "rampLengthM", s.y / std::tan(slope));
            const bool kerbs = !(p.is_object() && p.contains("kerbs") && p["kerbs"].is_boolean() &&
                                 !p["kerbs"].get<bool>());
            return compound(bridgeParts(s.x, s.y, s.z, rampLength, kerbs));
        }
        case ItemType::Obstacle: {
            Shape shape = obstacleShape(obstacleKind(p), p);
            if (const double e = num(p, "restitution", -1.0); e >= 0.0) {
                shape.restitution = std::min(e, 1.0);
            }
            if (const double mu = num(p, "friction", -1.0); mu >= 0.0) {
                shape.friction = mu;
            }
            return shape;
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

// Contact of @p q inside the vertical cylinder standing on @p base.
std::optional<SurfaceContact> cylinderContact(const Vec3& q, const Vec3& base, double radius,
                                              double height) {
    const Vec3 d = q - base;
    const double rho = std::hypot(d.x, d.z);
    if (d.y < 0.0 || d.y > height || rho > radius) {
        return std::nullopt;
    }
    SurfaceContact c;
    c.depthM = radius - rho;
    c.normal = rho > 1e-9 ? Vec3(d.x / rho, 0.0, d.z / rho) : Vec3(1.0, 0.0, 0.0);
    if (height - d.y < c.depthM) {
        c.normal = Vec3(0.0, 1.0, 0.0);
        c.depthM = height - d.y;
    }
    if (d.y < c.depthM) {
        c.normal = Vec3(0.0, -1.0, 0.0);
        c.depthM = d.y;
    }
    return c;
}

std::optional<SurfaceContact> sphereContact(const Vec3& q, const Vec3& centre, double radius) {
    const Vec3 d = q - centre;
    const double dist = glm::length(d);
    if (dist > radius) {
        return std::nullopt;
    }
    SurfaceContact c;
    c.depthM = radius - dist;
    c.normal = dist > 1e-9 ? d / dist : Vec3(0.0, 1.0, 0.0);
    return c;
}

// Gable roof: triangular prism on the base centred on @p base, ridge along z.
std::optional<SurfaceContact> gableContact(const Vec3& q, const Vec3& base, double halfWidth,
                                           double rise, double halfLength) {
    const Vec3 d = q - base;
    if (d.y < 0.0 || d.y > rise || std::abs(d.z) > halfLength ||
        std::abs(d.x) > halfWidth * (1.0 - d.y / rise)) {
        return std::nullopt;
    }
    // Slope plane: rise·|x| + halfWidth·y = halfWidth·rise.
    const double len = std::hypot(rise, halfWidth);
    const double side = d.x >= 0.0 ? 1.0 : -1.0;
    SurfaceContact c;
    c.depthM = (halfWidth * rise - rise * std::abs(d.x) - halfWidth * d.y) / len;
    c.normal = Vec3(side * rise / len, halfWidth / len, 0.0);
    if (d.y < c.depthM) {
        c.normal = Vec3(0.0, -1.0, 0.0);
        c.depthM = d.y;
    }
    if (const double end = halfLength - std::abs(d.z); end < c.depthM) {
        c.normal = Vec3(0.0, 0.0, d.z >= 0.0 ? 1.0 : -1.0);
        c.depthM = end;
    }
    return c;
}

// Wedge: base centred on @p base, top rising by @p rise towards −z (+z when flipped).
std::optional<SurfaceContact> wedgeContact(const Vec3& q, const SolidPart& part) {
    const Vec3 d = q - part.centreM;
    const double hx = part.halfExtentsM.x;
    const double hz = part.halfExtentsM.z;
    const double rise = part.heightM;
    const double sign = part.flipped ? -1.0 : 1.0;
    const double top = rise * (hz - sign * d.z) / (2.0 * hz);
    if (d.y < 0.0 || d.y > top || std::abs(d.x) > hx || std::abs(d.z) > hz) {
        return std::nullopt;
    }
    // Slope plane: y = top(z); its upward normal leans towards the low end.
    const Vec3 gradient(0.0, 1.0, sign * rise / (2.0 * hz));
    const double len = glm::length(gradient);
    SurfaceContact c;
    c.depthM = (top - d.y) / len;
    c.normal = gradient / len;
    if (d.y < c.depthM) {
        c.normal = Vec3(0.0, -1.0, 0.0);
        c.depthM = d.y;
    }
    if (const double side = hx - std::abs(d.x); side < c.depthM) {
        c.normal = Vec3(d.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
        c.depthM = side;
    }
    if (const double end = hz + sign * d.z; end < c.depthM) {  // The vertical high end.
        c.normal = Vec3(0.0, 0.0, -sign);
        c.depthM = end;
    }
    return c;
}

std::optional<SurfaceContact> partContact(const Vec3& q, const SolidPart& part) {
    switch (part.kind) {
        case SolidPart::Kind::Box: return boxContact(q, part.centreM, part.halfExtentsM);
        case SolidPart::Kind::Cylinder:
            return cylinderContact(q, part.centreM, part.radiusM, part.heightM);
        case SolidPart::Kind::Sphere: return sphereContact(q, part.centreM, part.radiusM);
        case SolidPart::Kind::Gable:
            return gableContact(q, part.centreM, part.halfExtentsM.x, part.heightM,
                                part.halfExtentsM.z);
        case SolidPart::Kind::Wedge: return wedgeContact(q, part);
    }
    return std::nullopt;
}

// Deepest contact with the parts of a compound obstacle (inside its bounding box).
std::optional<SurfaceContact> compoundContact(const Vec3& q, const Shape& s) {
    if (std::abs(q.x - s.offsetM.x) > s.halfExtentsM.x ||
        std::abs(q.y - s.offsetM.y) > s.halfExtentsM.y ||
        std::abs(q.z - s.offsetM.z) > s.halfExtentsM.z) {
        return std::nullopt;
    }
    std::optional<SurfaceContact> deepest;
    for (const SolidPart& part : s.parts) {
        if (auto c = partContact(q, part); c && (!deepest || c->depthM > deepest->depthM)) {
            c->material = part.material;
            deepest = c;
        }
    }
    return deepest;
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
        case Shape::Kind::Box:
            // Compound obstacles: a box bounds their parts.
            local =
                s.parts.empty() ? boxContact(q, s.offsetM, s.halfExtentsM) : compoundContact(q, s);
            break;
        case Shape::Kind::Cylinder:
            local = cylinderContact(q, Vec3(0.0, s.bottomM, 0.0), s.radiusM, s.topM - s.bottomM);
            break;
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
    SurfaceContact c;
    c.material = s.parts.empty() ? s.material : local->material;
    c.surface = surfaceOf(c.material);
    c.surface.restitution = s.restitution.value_or(c.surface.restitution);
    c.surface.friction = s.friction.value_or(c.surface.friction);
    // Back to the world frame (scaled shapes: depth along the scaled normal).
    const Vec3 n = item.transform.rotation * (local->normal * item.transform.scale);
    const double len = glm::length(n);
    c.normal = n / len;
    c.depthM = local->depthM * len;
    return c;
}

std::optional<DriveSurface> driveSurfaceAt(const Item& item, double x, double z,
                                           double maxHeightM) {
    if (!item.enabled || !isDrivableType(item.type)) {
        return std::nullopt;
    }
    const Transform& t = item.transform;
    const Vec3 q = t.toLocal(Vec3(x, t.position.y, z));
    std::optional<DriveSurface> best;
    for (const SolidPart& part : item.shape.parts) {
        const Vec3 d = q - part.centreM;
        const double hx = part.halfExtentsM.x;
        const double hz = part.halfExtentsM.z;
        if (std::abs(d.x) > hx || std::abs(d.z) > hz) {
            continue;
        }
        double top = 0.0;
        Vec3 normal(0.0, 1.0, 0.0);
        if (part.kind == SolidPart::Kind::Box) {
            top = part.centreM.y + part.halfExtentsM.y;
        } else if (part.kind == SolidPart::Kind::Wedge) {
            const double sign = part.flipped ? -1.0 : 1.0;
            top = part.centreM.y + part.heightM * (hz - sign * d.z) / (2.0 * hz);
            normal = Vec3(0.0, 1.0, sign * part.heightM / (2.0 * hz));
        } else {
            continue;
        }
        const double height = t.toWorld(Vec3(q.x, top, q.z)).y;
        if (height > maxHeightM || (best && height <= best->heightM)) {
            continue;
        }
        best = DriveSurface{.heightM = height,
                            .normal = glm::normalize(t.rotation * (normal / t.scale)),
                            .material = part.material};
    }
    return best;
}

// --- Mass ----------------------------------------------------------------------------------------

double typicalMassKg(ItemType type, const common::Json& properties, const Shape& shape) {
    if (type != ItemType::Obstacle) {
        return std::numeric_limits<double>::infinity();
    }
    const std::string kind = obstacleKind(properties);
    Vec3 lo;
    Vec3 hi;
    shape.localBounds(lo, hi);
    const Vec3 size = hi - lo;
    const double boxVolume = size.x * size.y * size.z;
    const double cylinderVolume =
        std::numbers::pi * shape.radiusM * shape.radiusM * (shape.topM - shape.bottomM);
    // Bulk densities of the figures' bounding volumes, kg/m³.
    if (kind == "house" || kind == "building") {
        return 250.0 * boxVolume;  // An 8 × 7 × 10 m house: 140 t.
    }
    if (kind == "tree") {
        // Trunk wood (700 kg/m³) and a crown of branches and leaves (10 kg/m³).
        double mass = 0.0;
        for (const SolidPart& part : shape.parts) {
            mass += part.volumeM3() * (part.material == Material::Wood ? 700.0 : 10.0);
        }
        return mass;
    }
    if (kind == "cow") {
        // 650 kg for the default 0.8 × 1.5 × 2.4 m cow.
        static const double kDefaultVolume = [] {
            const Shape cow = compound(cowParts(0.8, 1.5, 2.4));
            return 8.0 * cow.halfExtentsM.x * cow.halfExtentsM.y * cow.halfExtentsM.z;
        }();
        return 650.0 * boxVolume / kDefaultVolume;
    }
    if (kind == "man") {
        return 75.0 * std::pow(size.y / 1.75, 3.0);
    }
    if (kind == "windTurbine") {
        return 3000.0 * shape.topM;  // About 300 t for a 100 m tower.
    }
    if (kind == "powerLine") {
        return 50.0 * boxVolume;
    }
    // Solid concrete.
    return 2400.0 * (shape.kind == Shape::Kind::Cylinder ? cylinderVolume : boxVolume);
}

double massKg(const Item& item) {
    const double mass = num(item.properties, "massKg", 0.0);
    return mass > 0.0 ? mass : typicalMassKg(item.type, item.properties, item.shape);
}

}  // namespace ds::items
