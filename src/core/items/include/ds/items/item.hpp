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

/// @brief What a solid part is made of: decides how a vehicle bounces off it and slides along
/// it, and how it is drawn.
enum class Material {
    Concrete,   ///< Plain obstacles, masts, gate frames.
    Masonry,    ///< House walls and chimneys.
    RoofTiles,  ///< House roofs.
    Wood,       ///< Tree trunks.
    Foliage,    ///< Tree crowns: branches and leaves.
    Hide,       ///< Animals.
    Skin,       ///< A person's head and hands.
    Clothing,   ///< A person's body.
};

/// @brief Collision behaviour of a material.
struct SurfaceProperties {
    double restitution = 0.2;  ///< Fraction of the normal speed kept when bouncing off (0–1).
    double friction = 0.5;     ///< Coulomb friction coefficient against the vehicle.
};

/// @brief Collision behaviour of @p material (concrete 0.2/0.5, foliage 0.05/0.9, …).
[[nodiscard]] SurfaceProperties surfaceOf(Material material) noexcept;

/// @brief One solid primitive of a compound obstacle (house, tree, cow, person), in the item's
/// local frame. Several parts may overlap.
struct SolidPart {
    /// @brief Kinds of primitive.
    enum class Kind {
        Box,       ///< `halfExtentsM` around `centreM`.
        Cylinder,  ///< Vertical: `radiusM`, from `centreM` (bottom centre) up `heightM`.
        Sphere,    ///< `radiusM` around `centreM`.
        Gable,     ///< Gable roof: base `2·halfExtentsM.x` × `2·halfExtentsM.z` centred on
                   ///< `centreM`, rising `heightM` to a ridge along the local z axis.
        Wedge,     ///< Ramp: base `2·halfExtentsM.x` × `2·halfExtentsM.z` centred on `centreM`,
                   ///< its top rising linearly by `heightM` towards local −z (towards +z
                   ///< when `flipped`), with a vertical face at the high end.
    };
    Kind kind = Kind::Box;                   ///< Kind.
    Vec3 centreM{0.0};                       ///< See @ref Kind.
    Vec3 halfExtentsM{0.0};                  ///< Box half sizes; gable half width and length.
    double radiusM = 0.0;                    ///< Cylinder / sphere radius.
    double heightM = 0.0;                    ///< Cylinder height; gable rise.
    Material material = Material::Concrete;  ///< What it is made of.
    bool flipped = false;                    ///< Wedge rising towards local +z.

    /// @brief Volume in m³.
    [[nodiscard]] double volumeM3() const noexcept;
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
    /// @brief Solid parts of a compound obstacle. When not empty they, not the shape, stop
    /// vehicles; the shape is then their bounding volume.
    std::vector<SolidPart> parts;
    /// @brief Material of a plain (not compound) obstacle.
    Material material = Material::Concrete;
    /// @brief `restitution` item property: replaces the materials' value, if set.
    std::optional<double> restitution;
    /// @brief `friction` item property: replaces the materials' value, if set.
    std::optional<double> friction;

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

/// @brief A point inside an obstacle: the way out, how deep it is, and what was hit.
struct SurfaceContact {
    Vec3 normal{0.0, 1.0, 0.0};              ///< Unit world direction out of the obstacle.
    double depthM = 0.0;                     ///< Distance to the surface along @ref normal.
    Material material = Material::Concrete;  ///< Material of the part hit.
    SurfaceProperties surface;               ///< Its bounce and friction.
};

/// @brief Thickness of a gate's frame bars for a gate of half width @p halfWidthM and half
/// height @p halfHeightM (drawn and solid).
[[nodiscard]] double gateFrameThicknessM(double halfWidthM, double halfHeightM) noexcept;

/// @brief True if @p item stops the vehicle: solid items and gate frames.
[[nodiscard]] bool blocksVehicle(const Item& item) noexcept;

/// @brief Contact of the world point @p p with @p item's solid volume (boxes, cylinders, the
/// parts of compound obstacles — houses, trees, cows, people — and gate frames with their posts
/// down to the ground), or nullopt if @p p is outside it or the item does not block vehicles.
/// @details Inside overlapping parts, the deepest contact wins.
[[nodiscard]] std::optional<SurfaceContact> obstacleContact(const Item& item, const Vec3& p);

/// @brief Default shape for an item of @p type built from its JSON @p properties (`radiusM`,
/// `sizeM`, `widthM`, `heightM`, `polygon`, `bottomM`/`topM`, `shape`, `restitution`,
/// `friction`, …).
/// @details Missing values get defaults: waypoint radius 2 m, landing pad radius 1 m, zones
/// radius 3 m and 10 m high, gates 2 × 2 m, parking zones 2 × 3 m. Obstacle figures (with
/// their origin on the ground, facing local −z):
/// - `house`: `sizeM` 8 × 7 × 10 m (width × height × length); masonry walls up to 62 % of
///   the height, a tiled gable roof with 0.3 m eaves along the length, and a chimney;
/// - `tree`: `radiusM` 2 m crown, `heightM` 8 m; wooden trunk and two foliage spheres;
/// - `cow`: `sizeM` 0.8 × 1.5 × 2.4 m; body, head and four legs;
/// - `man`: `heightM` 1.75 m (the rest scales with it); legs, torso, arms, neck and head.
///
/// Ground-drone items (FR-ITM-13), heading = the direction of travel up or across them:
/// - `ramp`: `sizeM` 1 × 0.25 × 1.5 m (width × height × length), a wooden wedge rising
///   towards the heading, with its vertical face at the far end;
/// - `bridge`: `sizeM` 1 × 0.5 × 3 m: a 0.3 m thick concrete deck whose top is `height` above
///   the origin over `length`, with approach ramps of `rampLengthM` (default: a 12° slope) at
///   both ends and, unless `kerbs` is false, kerbs along both sides of the deck. Below a deck
///   higher than its thickness there is room to pass.
///
/// Polygon vertices are offsets from the item position. Invalid properties are ignored (the
/// mission validator reports them).
[[nodiscard]] Shape defaultShape(ItemType type, const common::Json& properties);
/// @brief True for types that block movement and sight by default (obstacles, ramps, bridges).
[[nodiscard]] bool isSolidType(ItemType type) noexcept;
/// @brief True for types a ground drone can drive on (ramps and bridges, FR-ITM-13).
[[nodiscard]] bool isDrivableType(ItemType type) noexcept;

/// @brief Default slope of a bridge's approach ramps, degrees.
inline constexpr double kBridgeRampSlopeDeg = 12.0;

/// @brief The top of a drivable item at a horizontal position.
struct DriveSurface {
    double heightM = 0.0;                    ///< World height of the top.
    Vec3 normal{0.0, 1.0, 0.0};              ///< Upward unit normal, world.
    Material material = Material::Concrete;  ///< What the top is made of.
};

/// @brief Highest top of @p item's drivable parts (boxes and wedges of ramps and bridges)
/// over the world position (@p x, @p z) that is not higher than @p maxHeightM, or nullopt.
/// @details A wheel or a body point at height `h` stands on the top found with
/// `maxHeightM = h`: tops above it are walls or decks overhead, which the obstacle contacts
/// handle. Assumes the item is rotated about the vertical only (its heading).
[[nodiscard]] std::optional<DriveSurface> driveSurfaceAt(const Item& item, double x, double z,
                                                         double maxHeightM);

/// @brief Typical mass of an obstacle figure (`shape`) of the size given by @p shape, kg:
/// its volume times the bulk density of the figure (house 250 kg/m³ of its bounding box, a
/// 1.75 m man 75 kg, a 0.8 × 1.5 × 2.4 m cow 650 kg, …). Infinite for items that are not
/// obstacles.
[[nodiscard]] double typicalMassKg(ItemType type, const common::Json& properties,
                                   const Shape& shape);
/// @brief Mass of @p item, kg: its `massKg` property, or typicalMassKg(). Obstacles stay where
/// they are; the mass decides how much of a vehicle's momentum they take in a collision (a
/// heavy drone bounces less off a person than off a wall).
[[nodiscard]] double massKg(const Item& item);

}  // namespace ds::items
