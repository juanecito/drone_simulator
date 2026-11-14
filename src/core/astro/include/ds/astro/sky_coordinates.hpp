// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Celestial coordinate systems and transforms: ecliptic → equatorial → horizontal,
/// topocentric correction, atmospheric refraction (Meeus ch. 13, 14, 16, 40).
/// @ingroup astro

namespace ds::astro {

/// @brief Ecliptic coordinates, degrees.
struct Ecliptic {
    double longitudeDeg = 0.0;  ///< λ, 0…360.
    double latitudeDeg = 0.0;   ///< β.
};

/// @brief Equatorial coordinates, degrees.
struct Equatorial {
    double rightAscensionDeg = 0.0;  ///< α, 0…360.
    double declinationDeg = 0.0;     ///< δ.
};

/// @brief Horizontal coordinates, degrees.
struct Horizontal {
    double azimuthDeg = 0.0;    ///< 0…360, from north towards east.
    double elevationDeg = 0.0;  ///< Above the astronomical horizon.
};

/// @brief Atmosphere at the observer, for refraction (FR-AST-04).
struct Atmosphere {
    double pressureHpa = 1010.0;  ///< Station pressure.
    double temperatureC = 10.0;   ///< Air temperature.
};

/// @brief Ecliptic → equatorial for obliquity @p obliquityDeg.
[[nodiscard]] Equatorial eclipticToEquatorial(const Ecliptic& ecliptic,
                                              double obliquityDeg) noexcept;

/// @brief Equatorial → horizontal for a local sidereal time and observer latitude.
[[nodiscard]] Horizontal equatorialToHorizontal(const Equatorial& equatorial,
                                                double localSiderealDeg,
                                                double latitudeDeg) noexcept;

/// @brief Refraction in degrees to add to a geometric (airless) elevation (Sæmundsson's
/// formula scaled for pressure and temperature, Meeus eq. 16.4). Below −1° the value at −1° is
/// used; above 89.9° it is 0.
[[nodiscard]] double refractionDeg(double airlessElevationDeg,
                                   const Atmosphere& atmosphere) noexcept;

/// @brief A position seen from the observer instead of the Earth's centre.
struct Topocentric {
    Equatorial equatorial;    ///< Topocentric right ascension and declination.
    double distanceKm = 0.0;  ///< Distance from the observer.
};

/// @brief Applies diurnal parallax for an observer on the WGS84 ellipsoid (exact vector
/// method, equivalent to Meeus ch. 40).
/// @param geocentric  Apparent geocentric position, equator and equinox of date.
/// @param distanceKm  Geocentric distance.
/// @param observer    Observer position (height above the ellipsoid).
/// @param localSiderealDeg Local apparent sidereal time.
[[nodiscard]] Topocentric toTopocentric(const Equatorial& geocentric, double distanceKm,
                                        const geo::Geodetic& observer,
                                        double localSiderealDeg) noexcept;

/// @brief Parallactic angle q in degrees (Meeus eq. 14.1): angle at the object between the
/// directions to the celestial pole and to the zenith.
[[nodiscard]] double parallacticAngleDeg(double hourAngleDeg, double declinationDeg,
                                         double latitudeDeg) noexcept;

}  // namespace ds::astro
