// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string_view>

#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Earth's main magnetic field from the World Magnetic Model WMM2025 (FR-GEO-05):
/// declination for the compass (FR-GFX-14b) and the field vector for the magnetometer.
/// @ingroup geo

namespace ds::geo {

/// @brief Main field at a point, geodetic (ellipsoidal) components.
struct MagneticField {
    double northNt = 0.0;         ///< X: northward component, nT.
    double eastNt = 0.0;          ///< Y: eastward component, nT.
    double downNt = 0.0;          ///< Z: downward component, nT.
    double horizontalNt = 0.0;    ///< H = √(X² + Y²), nT.
    double totalNt = 0.0;         ///< F, total intensity, nT.
    double declinationDeg = 0.0;  ///< D: magnetic north relative to true north, + east.
    double inclinationDeg = 0.0;  ///< I: dip below the horizontal, + down.
    /// True if the date lies in the model's validity period. Outside it the field is evaluated
    /// at the nearest end of the period and is only indicative.
    bool dateValid = true;
};

/// @brief Compass quality from the horizontal field strength (WMM "blackout zones").
enum class CompassQuality {
    Good,        ///< H ≥ 6000 nT.
    Caution,     ///< 2000 nT ≤ H < 6000 nT: the compass is unreliable.
    Unreliable,  ///< H < 2000 nT: near the magnetic poles; the compass cannot be used.
};

/// @brief Name of the model, e.g. "WMM-2025".
[[nodiscard]] std::string_view magneticModelName() noexcept;
/// @brief First decimal year of the validity period (2025.0).
[[nodiscard]] double magneticModelStartYear() noexcept;
/// @brief End of the validity period, exclusive (2030.0).
[[nodiscard]] double magneticModelEndYear() noexcept;

/// @brief Evaluates the WMM at @p position (height above the WGS84 ellipsoid) and
/// @p decimalYear (e.g. 2026.5).
/// @details Spherical-harmonic synthesis to degree 12 in geocentric coordinates, rotated to
/// the ellipsoidal frame (WMM2025 Technical Report, eqs. 7–19). At the geographic poles the
/// latitude is limited to ±89.9999° so that the declination stays defined.
[[nodiscard]] MagneticField magneticField(const Geodetic& position, double decimalYear) noexcept;

/// @brief Compass quality for @p field.
[[nodiscard]] CompassQuality compassQuality(const MagneticField& field) noexcept;

/// @brief Decimal year of @p time: year + (seconds since 1 January) / (seconds in the year).
[[nodiscard]] double decimalYear(std::chrono::sys_time<std::chrono::milliseconds> time) noexcept;

}  // namespace ds::geo
