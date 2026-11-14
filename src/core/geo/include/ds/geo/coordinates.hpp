// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>

/// @file
/// @brief WGS84 geodetic ⇄ ECEF ⇄ local ENU ⇄ engine world frame (FR-GEO-01, FR-GEO-02).
/// @ingroup geo

namespace ds::geo {

/// @brief WGS84 geodetic position. Angles in degrees, height above the ellipsoid in metres.
struct Geodetic {
    double latitudeDeg = 0.0;   ///< Latitude, −90…90°, positive north.
    double longitudeDeg = 0.0;  ///< Longitude, −180…180°, positive east.
    double heightM = 0.0;       ///< Height above the WGS84 ellipsoid.
};

/// @brief Latitude/longitude in degrees (no height).
struct LatLon {
    double latitudeDeg = 0.0;   ///< Latitude in degrees.
    double longitudeDeg = 0.0;  ///< Longitude in degrees, −180…180.
};

/// @brief Earth-centred, Earth-fixed Cartesian position in metres.
struct Ecef {
    double x = 0.0;  ///< Towards (0° N, 0° E).
    double y = 0.0;  ///< Towards (0° N, 90° E).
    double z = 0.0;  ///< Towards the north pole.
};

/// @brief Local East-North-Up position in metres, relative to a LocalFrame origin.
struct Enu {
    double east = 0.0;   ///< Metres east.
    double north = 0.0;  ///< Metres north.
    double up = 0.0;     ///< Metres up (along the ellipsoid normal at the origin).
};

/// @brief Engine world frame: right-handed, Y-up, `x = East`, `y = Up`, `z = −North` (FR-X-04).
struct WorldPosition {
    double x = 0.0;  ///< East.
    double y = 0.0;  ///< Up.
    double z = 0.0;  ///< South (−North).
};

/// @brief Geodetic → ECEF (exact).
[[nodiscard]] Ecef toEcef(const Geodetic& position) noexcept;

/// @brief ECEF → geodetic. Accurate to well below 1 µm for any point outside the Earth's core.
[[nodiscard]] Geodetic toGeodetic(const Ecef& position) noexcept;

/// @brief ENU → engine world frame.
[[nodiscard]] constexpr WorldPosition toWorld(const Enu& enu) noexcept {
    return {enu.east, enu.up, -enu.north};
}

/// @brief Engine world frame → ENU.
[[nodiscard]] constexpr Enu toEnu(const WorldPosition& world) noexcept {
    return {world.x, -world.z, world.y};
}

/// @brief A local tangent-plane frame anchored at a geodetic origin (scene origin).
/// @details Conversions are done in `double` and are exact up to rounding: round trips stay
/// far below 1 mm within 50 km of the origin (NFR-REAL-06).
class LocalFrame {
public:
    /// @brief Creates the frame with its origin at @p origin.
    explicit LocalFrame(const Geodetic& origin) noexcept;

    /// @brief The geodetic origin of the frame.
    [[nodiscard]] const Geodetic& origin() const noexcept { return origin_; }

    /// @brief ECEF → ENU.
    [[nodiscard]] Enu toEnu(const Ecef& position) const noexcept;
    /// @brief ENU → ECEF.
    [[nodiscard]] Ecef toEcef(const Enu& position) const noexcept;
    /// @brief Geodetic → ENU.
    [[nodiscard]] Enu toEnu(const Geodetic& position) const noexcept;
    /// @brief ENU → geodetic.
    [[nodiscard]] Geodetic toGeodetic(const Enu& position) const noexcept;

private:
    Geodetic origin_;
    Ecef originEcef_;
    // Rows: unit vectors east, north, up expressed in ECEF.
    std::array<std::array<double, 3>, 3> rotation_{};
};

}  // namespace ds::geo
