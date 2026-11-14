// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/geo/coordinates.hpp"

#include <cmath>

#include "ds/common/math.hpp"
#include "ds/geo/ellipsoid.hpp"

namespace ds::geo {

using common::degToRad;
using common::radToDeg;

Ecef toEcef(const Geodetic& position) noexcept {
    const double lat = degToRad(position.latitudeDeg);
    const double lon = degToRad(position.longitudeDeg);
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double e2 = kWgs84.e2();
    const double primeVertical = kWgs84.a / std::sqrt(1.0 - e2 * sinLat * sinLat);
    const double h = position.heightM;
    return {(primeVertical + h) * cosLat * std::cos(lon),
            (primeVertical + h) * cosLat * std::sin(lon),
            (primeVertical * (1.0 - e2) + h) * sinLat};
}

Geodetic toGeodetic(const Ecef& position) noexcept {
    const double a = kWgs84.a;
    const double e2 = kWgs84.e2();
    const double p = std::hypot(position.x, position.y);
    const double lon = std::atan2(position.y, position.x);

    // Fixed-point iteration on latitude; well defined at the poles (p = 0) and converging by a
    // factor of about e² per step, i.e. to full double precision in a handful of iterations.
    double lat = std::atan2(position.z, p * (1.0 - e2));
    for (int i = 0; i < 16; ++i) {
        const double sinLat = std::sin(lat);
        const double primeVertical = a / std::sqrt(1.0 - e2 * sinLat * sinLat);
        const double next = std::atan2(position.z + e2 * primeVertical * sinLat, p);
        const bool converged = std::abs(next - lat) < 1e-15;
        lat = next;
        if (converged) {
            break;
        }
    }
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    // Height formula valid at every latitude (no division by cos φ).
    const double h = p * cosLat + position.z * sinLat - a * std::sqrt(1.0 - e2 * sinLat * sinLat);
    return {radToDeg(lat), radToDeg(lon), h};
}

LocalFrame::LocalFrame(const Geodetic& origin) noexcept
    : origin_(origin), originEcef_(geo::toEcef(origin)) {
    const double lat = degToRad(origin.latitudeDeg);
    const double lon = degToRad(origin.longitudeDeg);
    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double sinLon = std::sin(lon);
    const double cosLon = std::cos(lon);
    rotation_ = {{
        {-sinLon, cosLon, 0.0},                        // east
        {-sinLat * cosLon, -sinLat * sinLon, cosLat},  // north
        {cosLat * cosLon, cosLat * sinLon, sinLat},    // up
    }};
}

Enu LocalFrame::toEnu(const Ecef& position) const noexcept {
    const double dx = position.x - originEcef_.x;
    const double dy = position.y - originEcef_.y;
    const double dz = position.z - originEcef_.z;
    const auto& r = rotation_;
    return {r[0][0] * dx + r[0][1] * dy + r[0][2] * dz, r[1][0] * dx + r[1][1] * dy + r[1][2] * dz,
            r[2][0] * dx + r[2][1] * dy + r[2][2] * dz};
}

Ecef LocalFrame::toEcef(const Enu& position) const noexcept {
    const auto& r = rotation_;  // Orthonormal: inverse = transpose.
    return {
        originEcef_.x + r[0][0] * position.east + r[1][0] * position.north + r[2][0] * position.up,
        originEcef_.y + r[0][1] * position.east + r[1][1] * position.north + r[2][1] * position.up,
        originEcef_.z + r[0][2] * position.east + r[1][2] * position.north + r[2][2] * position.up};
}

Enu LocalFrame::toEnu(const Geodetic& position) const noexcept {
    return toEnu(geo::toEcef(position));
}

Geodetic LocalFrame::toGeodetic(const Enu& position) const noexcept {
    return geo::toGeodetic(toEcef(position));
}

}  // namespace ds::geo
