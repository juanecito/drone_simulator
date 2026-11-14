// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

/// @file
/// @brief Geographic bounding box (WGS84 degrees) used by bounding-box providers.
/// @ingroup net

namespace ds::net {

/// @brief Mean Earth radius used for areas and block sizes (km).
inline constexpr double kEarthRadiusKm = 6371.0088;

/// @brief A latitude/longitude rectangle in degrees. Does not cross the antimeridian.
struct GeoRect {
    double west = 0.0;   ///< Minimum longitude.
    double south = 0.0;  ///< Minimum latitude.
    double east = 0.0;   ///< Maximum longitude.
    double north = 0.0;  ///< Maximum latitude.

    /// @brief True if west < east, south < north and all values are within WGS84 limits.
    [[nodiscard]] bool valid() const noexcept;
    /// @brief Area on the mean sphere, in km².
    [[nodiscard]] double areaKm2() const noexcept;
    /// @brief East–west size along the middle latitude, in km.
    [[nodiscard]] double widthKm() const noexcept;
    /// @brief North–south size, in km.
    [[nodiscard]] double heightKm() const noexcept;
    /// @brief True if @p other lies completely inside this rectangle.
    [[nodiscard]] bool contains(const GeoRect& other) const noexcept;
    /// @brief True if the rectangles overlap (touching edges do not count).
    [[nodiscard]] bool intersects(const GeoRect& other) const noexcept;
    /// @brief This rectangle grown by @p marginKm on every side, clamped to ±90°/±180°.
    [[nodiscard]] GeoRect expandedKm(double marginKm) const noexcept;
    /// @brief Rectangle of @p widthKm × @p lengthKm centred on (@p latDeg, @p lonDeg).
    [[nodiscard]] static GeoRect around(double latDeg, double lonDeg, double widthKm,
                                        double lengthKm) noexcept;
    /// @brief "w,s,e,n" with 7 decimals (≈ 1 cm), stable for cache keys.
    [[nodiscard]] std::string canonical() const;

    /// @brief Equality.
    bool operator==(const GeoRect&) const = default;
};

}  // namespace ds::net
