// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "ds/common/error.hpp"
#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Web-Mercator (EPSG:3857) and XYZ tile math for tile providers (FR-GEO-02b).
/// @ingroup geo

namespace ds::geo {

/// @brief Latitude limit of the Web-Mercator square, ±85.0511…°.
inline constexpr double kWebMercatorMaxLatitudeDeg = 85.051128779806592;
/// @brief Sphere radius used by EPSG:3857 (the WGS84 semi-major axis).
inline constexpr double kWebMercatorRadius = 6378137.0;
/// @brief Highest supported zoom level.
inline constexpr int kMaxTileZoom = 30;

/// @brief An XYZ ("slippy map") tile: x grows east, y grows south, origin at the north-west.
struct TileId {
    std::int64_t x = 0;  ///< Column, 0 … 2^z − 1.
    std::int64_t y = 0;  ///< Row, 0 … 2^z − 1.
    int z = 0;           ///< Zoom level, 0 … kMaxTileZoom.

    /// @brief Equality.
    friend bool operator==(const TileId&, const TileId&) = default;
};

/// @brief Geographic bounds of a tile in degrees.
struct TileBounds {
    double west = 0.0;   ///< Western longitude.
    double south = 0.0;  ///< Southern latitude.
    double east = 0.0;   ///< Eastern longitude.
    double north = 0.0;  ///< Northern latitude.
};

/// @brief Position in EPSG:3857 metres.
struct MercatorPoint {
    double x = 0.0;  ///< Metres east of the prime meridian.
    double y = 0.0;  ///< Metres north of the equator.
};

/// @brief Fractional tile coordinates (the integer part is the tile, the rest the position inside).
struct TilePoint {
    double x = 0.0;  ///< Fractional column.
    double y = 0.0;  ///< Fractional row.
};

/// @brief Latitude/longitude (degrees) → EPSG:3857. Latitude is clamped to the Mercator limit.
[[nodiscard]] MercatorPoint toWebMercator(double latitudeDeg, double longitudeDeg) noexcept;

/// @brief EPSG:3857 → latitude/longitude (degrees).
[[nodiscard]] LatLon fromWebMercator(const MercatorPoint& point) noexcept;

/// @brief Latitude/longitude → fractional tile coordinates at @p zoom.
[[nodiscard]] common::Result<TilePoint> toTilePoint(double latitudeDeg, double longitudeDeg,
                                                    int zoom);

/// @brief The tile containing a position at @p zoom (latitude clamped, longitude wrapped).
[[nodiscard]] common::Result<TileId> tileAt(double latitudeDeg, double longitudeDeg, int zoom);

/// @brief Geographic bounds of @p tile.
[[nodiscard]] common::Result<TileBounds> tileBounds(const TileId& tile);

/// @brief Ground resolution in metres per pixel at a latitude, for square tiles of
/// @p tileSizePx pixels (e.g. 256).
[[nodiscard]] double groundResolution(double latitudeDeg, int zoom, int tileSizePx) noexcept;

}  // namespace ds::geo
