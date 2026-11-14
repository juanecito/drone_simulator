// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/astro/sky_coordinates.hpp"

#include <algorithm>
#include <cmath>

#include "ds/common/math.hpp"
#include "ds/geo/ellipsoid.hpp"

namespace ds::astro {

using common::degToRad;
using common::normalizeDegrees;
using common::radToDeg;

Equatorial eclipticToEquatorial(const Ecliptic& ecliptic, double obliquityDeg) noexcept {
    const double lambda = degToRad(ecliptic.longitudeDeg);
    const double beta = degToRad(ecliptic.latitudeDeg);
    const double eps = degToRad(obliquityDeg);
    const double ra = std::atan2(std::sin(lambda) * std::cos(eps) - std::tan(beta) * std::sin(eps),
                                 std::cos(lambda));
    const double dec = std::asin(std::sin(beta) * std::cos(eps) +
                                 std::cos(beta) * std::sin(eps) * std::sin(lambda));
    return {normalizeDegrees(radToDeg(ra)), radToDeg(dec)};
}

Horizontal equatorialToHorizontal(const Equatorial& equatorial, double localSiderealDeg,
                                  double latitudeDeg) noexcept {
    const double hourAngle = degToRad(localSiderealDeg - equatorial.rightAscensionDeg);
    const double dec = degToRad(equatorial.declinationDeg);
    const double lat = degToRad(latitudeDeg);
    const double sinEl =
        std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(hourAngle);
    const double azimuth = std::atan2(
        -std::cos(dec) * std::sin(hourAngle),
        std::sin(dec) * std::cos(lat) - std::cos(dec) * std::sin(lat) * std::cos(hourAngle));
    return {normalizeDegrees(radToDeg(azimuth)), radToDeg(std::asin(std::clamp(sinEl, -1.0, 1.0)))};
}

double refractionDeg(double airlessElevationDeg, const Atmosphere& atmosphere) noexcept {
    if (airlessElevationDeg > 89.9) {
        return 0.0;
    }
    const double h = std::max(airlessElevationDeg, -1.0);
    // Sæmundsson (1986): R [arcmin] = 1.02 / tan(h + 10.3 / (h + 5.11)), h in degrees.
    const double arcmin = 1.02 / std::tan(degToRad(h + 10.3 / (h + 5.11)));
    const double scale =
        (atmosphere.pressureHpa / 1010.0) * (283.0 / (273.0 + atmosphere.temperatureC));
    return std::max(0.0, arcmin * scale / 60.0);
}

Topocentric toTopocentric(const Equatorial& geocentric, double distanceKm,
                          const geo::Geodetic& observer, double localSiderealDeg) noexcept {
    // Observer position in the true equatorial frame of date (km): geocentric latitude and
    // distance from the ellipsoid, rotated by the local sidereal time.
    const geo::Ecef ecef = geo::toEcef(observer);
    const double rho = std::sqrt(ecef.x * ecef.x + ecef.y * ecef.y + ecef.z * ecef.z) / 1000.0;
    const double geocentricLat = std::atan2(ecef.z, std::hypot(ecef.x, ecef.y));
    const double theta = degToRad(localSiderealDeg);
    const double ox = rho * std::cos(geocentricLat) * std::cos(theta);
    const double oy = rho * std::cos(geocentricLat) * std::sin(theta);
    const double oz = rho * std::sin(geocentricLat);

    const double ra = degToRad(geocentric.rightAscensionDeg);
    const double dec = degToRad(geocentric.declinationDeg);
    const double x = distanceKm * std::cos(dec) * std::cos(ra) - ox;
    const double y = distanceKm * std::cos(dec) * std::sin(ra) - oy;
    const double z = distanceKm * std::sin(dec) - oz;
    const double distance = std::sqrt(x * x + y * y + z * z);
    return {{normalizeDegrees(radToDeg(std::atan2(y, x))), radToDeg(std::asin(z / distance))},
            distance};
}

double parallacticAngleDeg(double hourAngleDeg, double declinationDeg,
                           double latitudeDeg) noexcept {
    const double h = degToRad(hourAngleDeg);
    const double dec = degToRad(declinationDeg);
    const double lat = degToRad(latitudeDeg);
    return radToDeg(
        std::atan2(std::sin(h), std::tan(lat) * std::cos(dec) - std::sin(dec) * std::cos(h)));
}

}  // namespace ds::astro
