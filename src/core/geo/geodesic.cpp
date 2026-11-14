// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Inverse geodesic after T. Vincenty, "Direct and inverse solutions of geodesics on the
// ellipsoid with application of nested equations", Survey Review 23 (1975) 88–93.

#include "ds/geo/geodesic.hpp"

#include <algorithm>
#include <cmath>

#include "ds/common/math.hpp"
#include "ds/geo/ellipsoid.hpp"

namespace ds::geo {
namespace {

using common::degToRad;
using common::normalizeDegrees;
using common::radToDeg;

// Mean Earth radius (IUGG R1) for the spherical fallback.
constexpr double kMeanRadius = 6371008.8;

GeodesicInverse spherical(double lat1, double lon1, double lat2, double lon2) {
    const double dLon = lon2 - lon1;
    const double sinDLat = std::sin((lat2 - lat1) / 2.0);
    const double sinDLon = std::sin(dLon / 2.0);
    const double h = sinDLat * sinDLat + std::cos(lat1) * std::cos(lat2) * sinDLon * sinDLon;
    const double central = 2.0 * std::asin(std::min(1.0, std::sqrt(h)));
    const double y1 = std::sin(dLon) * std::cos(lat2);
    const double x1 =
        std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dLon);
    const double y2 = std::sin(dLon) * std::cos(lat1);
    const double x2 =
        -std::sin(lat1) * std::cos(lat2) + std::cos(lat1) * std::sin(lat2) * std::cos(dLon);
    return {kMeanRadius * central, normalizeDegrees(radToDeg(std::atan2(y1, x1))),
            normalizeDegrees(radToDeg(std::atan2(y2, x2))), true};
}

}  // namespace

GeodesicInverse inverseGeodesic(const LatLon& from, const LatLon& to) noexcept {
    const double a = kWgs84.a;
    const double f = kWgs84.f;
    const double b = kWgs84.b();
    const double lat1 = degToRad(from.latitudeDeg);
    const double lat2 = degToRad(to.latitudeDeg);
    const double lon1 = degToRad(from.longitudeDeg);
    const double lon2 = degToRad(to.longitudeDeg);
    const double bigL = common::wrapRadiansPi(lon2 - lon1);

    const double u1 = std::atan((1.0 - f) * std::tan(lat1));
    const double u2 = std::atan((1.0 - f) * std::tan(lat2));
    const double sinU1 = std::sin(u1);
    const double cosU1 = std::cos(u1);
    const double sinU2 = std::sin(u2);
    const double cosU2 = std::cos(u2);

    double lambda = bigL;
    double sinSigma = 0.0;
    double cosSigma = 1.0;
    double sigma = 0.0;
    double cos2Alpha = 1.0;
    double cos2SigmaM = 0.0;
    double sinLambda = 0.0;
    double cosLambda = 1.0;
    bool converged = false;
    for (int i = 0; i < 200; ++i) {
        sinLambda = std::sin(lambda);
        cosLambda = std::cos(lambda);
        const double t1 = cosU2 * sinLambda;
        const double t2 = cosU1 * sinU2 - sinU1 * cosU2 * cosLambda;
        sinSigma = std::sqrt(t1 * t1 + t2 * t2);
        if (sinSigma == 0.0) {
            return {};  // Coincident points.
        }
        cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosLambda;
        sigma = std::atan2(sinSigma, cosSigma);
        const double sinAlpha = cosU1 * cosU2 * sinLambda / sinSigma;
        cos2Alpha = 1.0 - sinAlpha * sinAlpha;
        cos2SigmaM = cos2Alpha != 0.0 ? cosSigma - 2.0 * sinU1 * sinU2 / cos2Alpha : 0.0;
        const double c = f / 16.0 * cos2Alpha * (4.0 + f * (4.0 - 3.0 * cos2Alpha));
        const double previous = lambda;
        lambda =
            bigL +
            (1.0 - c) * f * sinAlpha *
                (sigma + c * sinSigma *
                             (cos2SigmaM + c * cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)));
        if (std::abs(lambda - previous) < 1e-13) {
            converged = true;
            break;
        }
    }
    if (!converged || !std::isfinite(lambda)) {
        return spherical(lat1, lon1, lat2, lon2);
    }

    const double uSq = cos2Alpha * (a * a - b * b) / (b * b);
    const double bigA =
        1.0 + uSq / 16384.0 * (4096.0 + uSq * (-768.0 + uSq * (320.0 - 175.0 * uSq)));
    const double bigB = uSq / 1024.0 * (256.0 + uSq * (-128.0 + uSq * (74.0 - 47.0 * uSq)));
    const double deltaSigma =
        bigB * sinSigma *
        (cos2SigmaM + bigB / 4.0 *
                          (cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM) -
                           bigB / 6.0 * cos2SigmaM * (-3.0 + 4.0 * sinSigma * sinSigma) *
                               (-3.0 + 4.0 * cos2SigmaM * cos2SigmaM)));
    GeodesicInverse result;
    result.distanceM = b * bigA * (sigma - deltaSigma);
    result.initialBearingDeg = normalizeDegrees(
        radToDeg(std::atan2(cosU2 * sinLambda, cosU1 * sinU2 - sinU1 * cosU2 * cosLambda)));
    result.finalBearingDeg = normalizeDegrees(
        radToDeg(std::atan2(cosU1 * sinLambda, -sinU1 * cosU2 + cosU1 * sinU2 * cosLambda)));
    return result;
}

LocalBearing localBearing(const Enu& from, const Enu& to) noexcept {
    const double dEast = to.east - from.east;
    const double dNorth = to.north - from.north;
    return {std::hypot(dEast, dNorth), normalizeDegrees(radToDeg(std::atan2(dEast, dNorth)))};
}

}  // namespace ds::geo
