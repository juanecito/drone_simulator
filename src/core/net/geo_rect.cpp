// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/geo_rect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace ds::net {
namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kKmPerDegree = kEarthRadiusKm * kDegToRad;

}  // namespace

bool GeoRect::valid() const noexcept {
    return std::isfinite(west) && std::isfinite(south) && std::isfinite(east) &&
           std::isfinite(north) && west >= -180.0 && east <= 180.0 && south >= -90.0 &&
           north <= 90.0 && west < east && south < north;
}

double GeoRect::areaKm2() const noexcept {
    // Exact area of a latitude/longitude cell on a sphere: R² Δλ (sin φ₂ − sin φ₁).
    return kEarthRadiusKm * kEarthRadiusKm * (east - west) * kDegToRad *
           (std::sin(north * kDegToRad) - std::sin(south * kDegToRad));
}

double GeoRect::widthKm() const noexcept {
    return (east - west) * kKmPerDegree * std::cos(0.5 * (south + north) * kDegToRad);
}

double GeoRect::heightKm() const noexcept { return (north - south) * kKmPerDegree; }

bool GeoRect::contains(const GeoRect& o) const noexcept {
    return o.west >= west && o.east <= east && o.south >= south && o.north <= north;
}

bool GeoRect::intersects(const GeoRect& o) const noexcept {
    return o.west < east && o.east > west && o.south < north && o.north > south;
}

GeoRect GeoRect::expandedKm(double marginKm) const noexcept {
    const double dLat = marginKm / kKmPerDegree;
    // Use the latitude closest to a pole so the margin is at least marginKm everywhere.
    const double lat = std::min(89.0, std::max(std::abs(south), std::abs(north)));
    const double dLon = marginKm / (kKmPerDegree * std::cos(lat * kDegToRad));
    return GeoRect{std::max(-180.0, west - dLon), std::max(-90.0, south - dLat),
                   std::min(180.0, east + dLon), std::min(90.0, north + dLat)};
}

GeoRect GeoRect::around(double latDeg, double lonDeg, double widthKm, double lengthKm) noexcept {
    const double dLat = 0.5 * lengthKm / kKmPerDegree;
    const double lat = std::min(89.0, std::abs(latDeg) + dLat);
    const double dLon = 0.5 * widthKm / (kKmPerDegree * std::cos(lat * kDegToRad));
    return GeoRect{std::max(-180.0, lonDeg - dLon), std::max(-90.0, latDeg - dLat),
                   std::min(180.0, lonDeg + dLon), std::min(90.0, latDeg + dLat)};
}

std::string GeoRect::canonical() const {
    return std::format("{:.7f},{:.7f},{:.7f},{:.7f}", west, south, east, north);
}

}  // namespace ds::net
