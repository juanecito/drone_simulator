// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "ds/common/json.hpp"

/// @file
/// @brief World items other than terrain (FR-ITM-02, FR-ITM-03): types, identifiers, shapes
/// and the item record.
/// @details Positions use the engine world frame (`x` = east, `y` = up, `z` = south, metres
/// from the terrain origin). An item's local frame is rotated by its heading: local `−z` is the
/// item's forward direction (north at heading 0), local `+x` its right, local `+y` up.
/// @ingroup items

namespace ds::items {

/// @brief 3-vector in double precision (world frame).
using Vec3 = glm::dvec3;
/// @brief 2-vector in double precision (horizontal x, z).
using Vec2 = glm::dvec2;
/// @brief Rotation quaternion in double precision.
using Quat = glm::dquat;

/// @brief Built-in item types (FR-ITM-02, FR-ITM-13).
enum class ItemType {
    Drone,             ///< A vehicle (aerial or ground).
    Spawn,             ///< Home point / spawn.
    Obstacle,          ///< Box, cylinder, tree, building, power line or wind turbine.
    Gate,              ///< Rectangular gate to fly or drive through, direction-aware.
    Waypoint,          ///< Sphere to reach.
    LandingPad,        ///< Flat pad to land on.
    Payload,           ///< Cargo to pick up.
    PickupZone,        ///< Where cargo is picked up.
    DropZone,          ///< Where cargo is dropped.
    InspectionTarget,  ///< Object to keep in the camera view.
    SurveyArea,        ///< Polygon to cover.
    NoFlyZone,         ///< Keep-out volume (polygon prism or cylinder).
    Geofence,          ///< Keep-in volume (polygon prism or cylinder).
    PilotPosition,     ///< Pilot's eye position (LOS camera, VLOS checks).
    ParkingZone,       ///< Target box with heading (ground drones).
    Ramp,              ///< Inclined driving surface (ground drones).
    Bridge,            ///< Elevated driving surface (ground drones).
};

/// @brief Name used in JSON files ("landingPad", "noFlyZone", …).
[[nodiscard]] std::string_view itemTypeName(ItemType type) noexcept;
/// @brief Type for a JSON name, or nullopt.
[[nodiscard]] std::optional<ItemType> parseItemType(std::string_view name) noexcept;
/// @brief Every type, in declaration order.
[[nodiscard]] const std::vector<ItemType>& allItemTypes();
/// @brief True for types that define a volume the vehicle can be inside (zones, pads,
/// waypoints, gates excluded).
[[nodiscard]] bool isZoneType(ItemType type) noexcept;

/// @brief Stable 64-bit generational handle (FR-ITM-01): slot index + generation, so a handle
/// to a removed item never refers to a newer item in the same slot.
struct ItemId {
    std::uint32_t index = 0;       ///< Slot.
    std::uint32_t generation = 0;  ///< 0 ⇒ invalid handle.

    /// @brief Packed value (`generation << 32 | index`); 0 for an invalid handle.
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32U) | index;
    }
    /// @brief Unpacks value().
    [[nodiscard]] static constexpr ItemId fromValue(std::uint64_t v) noexcept {
        return ItemId{static_cast<std::uint32_t>(v & 0xFFFF'FFFFU),
                      static_cast<std::uint32_t>(v >> 32U)};
    }
    /// @brief True for a handle that was issued by a registry.
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
    /// @brief Equality.
    constexpr bool operator==(const ItemId&) const = default;
};

/// @brief Reference of an altitude.
enum class AltitudeRef {
    Agl,   ///< Above the terrain under the item: follows the terrain (FR-ITM-07).
    Amsl,  ///< Above mean sea level: fixed.
};

/// @brief Collision / containment shape, in the item's local frame (scaled by the transform).
struct Shape {
    /// @brief Kinds of shape.
    enum class Kind {
        Point,     ///< No volume.
        Sphere,    ///< `radiusM` around `offsetM`.
        Box,       ///< `halfExtentsM` around `offsetM`.
        Cylinder,  ///< Vertical: `radiusM`, from `bottomM` to `topM` (relative to the origin).
        Prism,     ///< Vertical prism: `polygon` (x, z offsets), from `bottomM` to `topM`.
        Gate,      ///< Rectangular opening `halfExtentsM.x` × `halfExtentsM.y` in the x–y plane.
    };
    Kind kind = Kind::Point;      ///< Kind.
    double radiusM = 0.0;         ///< Sphere / cylinder radius.
    Vec3 halfExtentsM{0.0};       ///< Box half sizes; gate half width/height.
    Vec3 offsetM{0.0};            ///< Centre of a sphere or box relative to the origin.
    double bottomM = 0.0;         ///< Cylinder / prism bottom (−∞ allowed).
    double topM = 0.0;            ///< Cylinder / prism top (+∞ allowed).
    std::vector<Vec2> polygon;    ///< Prism outline (x, z), at least 3 vertices.

    /// @brief Point.
    [[nodiscard]] static Shape point() { return {}; }
    /// @brief Sphere of radius @p r.
    [[nodiscard]] static Shape sphere(double r);
    /// @brief Box with half sizes @p half centred on @p offset.
    [[nodiscard]] static Shape box(const Vec3& half, const Vec3& offset = Vec3(0.0));
    /// @brief Vertical cylinder.
    [[nodiscard]] static Shape cylinder(double r, double bottom, double top);
    /// @brief Vertical prism.
    [[nodiscard]] static Shape prism(std::vector<Vec2> outline, double bottom, double top);
    /// @brief Gate opening of @p width × @p height, centred on the origin.
    [[nodiscard]] static Shape gate(double width, double height);

    /// @brief True if the local point @p p is inside (borders included). Gates and points
    /// have no inside.
    [[nodiscard]] bool contains(const Vec3& p) const noexcept;
    /// @brief Local axis-aligned bounds (infinite sides are clamped to ±1e7 m).
    void localBounds(Vec3& lo, Vec3& hi) const noexcept;
};

/// @brief Position, rotation and scale.
struct Transform {
    Vec3 position{0.0};                      ///< World position.
    Quat rotation{1.0, 0.0, 0.0, 0.0};       ///< Local → world rotation.
    Vec3 scale{1.0};                         ///< Per-axis scale (> 0).

    /// @brief World → local.
    [[nodiscard]] Vec3 toLocal(const Vec3& world) const noexcept;
    /// @brief Local → world.
    [[nodiscard]] Vec3 toWorld(const Vec3& local) const noexcept;
    /// @brief Local direction → world direction (rotation only).
    [[nodiscard]] Vec3 directionToWorld(const Vec3& local) const noexcept;
};

/// @brief Rotation about the vertical for a heading in degrees (clockwise from north).
[[nodiscard]] Quat headingRotation(double headingDeg) noexcept;
/// @brief Heading in degrees [0, 360) of the item's forward direction.
[[nodiscard]] double headingOf(const Quat& rotation) noexcept;

/// @brief Geographic position (WGS84) an item was authored with.
struct GeoPoint {
    double latitudeDeg = 0.0;   ///< Latitude.
    double longitudeDeg = 0.0;  ///< Longitude.
};

/// @brief One world item (FR-ITM-03).
struct Item {
    ItemId id;                            ///< Assigned by the registry.
    ItemType type = ItemType::Obstacle;   ///< Type.
    std::string key;                      ///< Mission id ("pad"); unique when not empty.
    std::string name;                     ///< Display name.
    Transform transform;                  ///< Placement.
    AltitudeRef altitudeRef = AltitudeRef::Agl;  ///< How `altitudeM` is measured.
    double altitudeM = 0.0;               ///< Height of the origin above the reference.
    std::optional<GeoPoint> geo;          ///< Authored geographic position, if any.
    Shape shape;                          ///< Collision / containment shape.
    bool enabled = true;                  ///< Takes part in triggers and queries.
    bool visible = true;                  ///< Drawn.
    bool solid = false;                   ///< Blocks movement and lines of sight.
    common::Json properties = common::Json::object();  ///< Type-specific properties.

    /// @brief True if the world point @p p is inside the shape.
    [[nodiscard]] bool contains(const Vec3& p) const noexcept {
        return shape.contains(transform.toLocal(p));
    }
    /// @brief World axis-aligned bounds of the shape.
    void worldBounds(Vec3& lo, Vec3& hi) const noexcept;
};

/// @brief Default shape for an item of @p type built from its JSON @p properties (`radiusM`,
/// `sizeM`, `widthM`, `heightM`, `polygon`, `bottomM`/`topM`, `shape`, …).
/// @details Missing values get defaults: waypoint radius 2 m, landing pad radius 1 m, zones
/// radius 3 m and 10 m high, gates 2 × 2 m, parking zones 2 × 3 m. Polygon vertices are
/// offsets from the item position. Invalid properties are ignored (the mission validator
/// reports them).
[[nodiscard]] Shape defaultShape(ItemType type, const common::Json& properties);
/// @brief True for types that block movement and sight by default (obstacles, ramps, bridges).
[[nodiscard]] bool isSolidType(ItemType type) noexcept;

}  // namespace ds::items
