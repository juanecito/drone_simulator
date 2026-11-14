// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/astro/sky_coordinates.hpp"
#include "ds/astro/time.hpp"
#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Apparent positions of the Sun (FR-AST-04) and the Moon (FR-AST-05), and the Moon's
/// phase and bright-limb orientation (FR-AST-06), for an observer at the drone's position
/// (FR-AST-03).
/// @ingroup astro

namespace ds::astro {

/// @brief Astronomical unit in kilometres.
inline constexpr double kAstronomicalUnitKm = 149597870.7;

/// @brief Position of a body as seen by the observer.
struct BodyPosition {
    Ecliptic geocentricEcliptic;         ///< Apparent geocentric λ, β (equinox of date).
    Equatorial geocentric;               ///< Apparent geocentric α, δ.
    Equatorial topocentric;              ///< Apparent topocentric α, δ.
    double distanceKm = 0.0;             ///< Geocentric distance.
    double topocentricDistanceKm = 0.0;  ///< Distance from the observer.
    double hourAngleDeg = 0.0;     ///< Topocentric local hour angle, −180…180 (west positive).
    Horizontal airless;            ///< Topocentric azimuth/elevation without refraction.
    Horizontal apparent;           ///< Topocentric azimuth/elevation with refraction.
    double semidiameterDeg = 0.0;  ///< Apparent (topocentric) angular radius.
};

/// @brief Apparent Sun position: VSOP87D (truncated) + FK5 correction, nutation, aberration.
/// Accuracy about 1″ (geocentric) for 1950–2100.
[[nodiscard]] BodyPosition sunPosition(UtcTime time, const geo::Geodetic& observer,
                                       const Atmosphere& atmosphere = {});

/// @brief Apparent Moon position: Meeus ch. 47 (truncated ELP-2000/82) + nutation and
/// topocentric parallax. Accuracy about 10″ in longitude and 4″ in latitude.
[[nodiscard]] BodyPosition moonPosition(UtcTime time, const geo::Geodetic& observer,
                                        const Atmosphere& atmosphere = {});

/// @brief Illumination and orientation of the Moon's disc for the observer (FR-AST-06).
struct MoonPhase {
    double illuminatedFraction = 0.0;  ///< k, 0 (new) … 1 (full).
    double phaseAngleDeg = 0.0;        ///< i, Sun–Moon–observer angle, 0…180.
    double elongationDeg = 0.0;        ///< Moon − Sun ecliptic longitude, 0…360 (0 new, 180 full).
    bool waxing = false;               ///< True between new and full moon.
    /// Position angle of the midpoint of the bright limb, measured from celestial north
    /// towards east (Meeus eq. 48.5), 0…360.
    double brightLimbPositionAngleDeg = 0.0;
    /// Same angle measured from the observer's zenith direction (χ − q): what the renderer
    /// needs to orient the crescent on screen, 0…360.
    double brightLimbZenithAngleDeg = 0.0;
};

/// @brief Moon phase from already computed Sun and Moon positions (same instant and observer).
[[nodiscard]] MoonPhase moonPhase(const BodyPosition& sun, const BodyPosition& moon,
                                  double latitudeDeg) noexcept;

}  // namespace ds::astro
