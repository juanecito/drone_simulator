// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Reference ellipsoid parameters (WGS84).
/// @ingroup geo

namespace ds::geo {

/// @brief A rotational ellipsoid given by semi-major axis and flattening.
struct Ellipsoid {
    double a;  ///< Semi-major axis in metres.
    double f;  ///< Flattening.

    /// @brief Semi-minor axis in metres.
    [[nodiscard]] constexpr double b() const noexcept { return a * (1.0 - f); }
    /// @brief First eccentricity squared.
    [[nodiscard]] constexpr double e2() const noexcept { return f * (2.0 - f); }
    /// @brief Second eccentricity squared.
    [[nodiscard]] constexpr double ep2() const noexcept { return e2() / (1.0 - e2()); }
    /// @brief Third flattening n = f / (2 − f).
    [[nodiscard]] constexpr double n() const noexcept { return f / (2.0 - f); }
};

/// @brief WGS84 ellipsoid (NIMA TR8350.2).
inline constexpr Ellipsoid kWgs84{6378137.0, 1.0 / 298.257223563};

}  // namespace ds::geo
