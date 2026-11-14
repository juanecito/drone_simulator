// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Distance and bearing on the WGS84 ellipsoid (FR-GEO-04): HUD home arrow, mission
/// distances, GPS display.
/// @ingroup geo

namespace ds::geo {

/// @brief Result of the inverse geodesic problem between two points.
struct GeodesicInverse {
    double distanceM = 0.0;          ///< Ellipsoidal (geodesic) distance in metres.
    double initialBearingDeg = 0.0;  ///< Azimuth at the first point, 0…360°, clockwise from north.
    double finalBearingDeg = 0.0;    ///< Azimuth at the second point, 0…360°.
    bool approximate = false;  ///< True for nearly antipodal points (spherical fallback, ~0.5 %).
};

/// @brief Distance and bearings from @p from to @p to (Vincenty's inverse formula; accurate to
/// about 0.5 mm, falling back to a spherical solution when it does not converge).
[[nodiscard]] GeodesicInverse inverseGeodesic(const LatLon& from, const LatLon& to) noexcept;

/// @brief Horizontal distance and bearing in a local ENU frame (short ranges, e.g. the HUD).
struct LocalBearing {
    double distanceM = 0.0;   ///< Horizontal distance.
    double bearingDeg = 0.0;  ///< 0…360°, clockwise from north.
};

/// @brief Horizontal distance and bearing from @p from to @p to, both in the same ENU frame.
[[nodiscard]] LocalBearing localBearing(const Enu& from, const Enu& to) noexcept;

}  // namespace ds::geo
