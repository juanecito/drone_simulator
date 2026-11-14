// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/geo/web_mercator.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "ds/common/math.hpp"

namespace ds::geo {
namespace {

using common::degToRad;
using common::kPi;
using common::radToDeg;

common::Result<double> tileCount(int zoom) {
    if (zoom < 0 || zoom > kMaxTileZoom) {
        return common::makeError(std::format("tile zoom {} is outside 0…{}", zoom, kMaxTileZoom));
    }
    return std::ldexp(1.0, zoom);
}

double tileRowToLatitudeDeg(double row, double count) {
    return radToDeg(std::atan(std::sinh(kPi * (1.0 - 2.0 * row / count))));
}

}  // namespace

MercatorPoint toWebMercator(double latitudeDeg, double longitudeDeg) noexcept {
    const double lat =
        std::clamp(latitudeDeg, -kWebMercatorMaxLatitudeDeg, kWebMercatorMaxLatitudeDeg);
    return {kWebMercatorRadius * degToRad(longitudeDeg),
            kWebMercatorRadius * std::log(std::tan(kPi / 4.0 + degToRad(lat) / 2.0))};
}

LatLon fromWebMercator(const MercatorPoint& point) noexcept {
    return {radToDeg(2.0 * std::atan(std::exp(point.y / kWebMercatorRadius)) - kPi / 2.0),
            radToDeg(point.x / kWebMercatorRadius)};
}

common::Result<TilePoint> toTilePoint(double latitudeDeg, double longitudeDeg, int zoom) {
    const auto count = tileCount(zoom);
    if (!count) {
        return std::unexpected(count.error());
    }
    const double lat =
        degToRad(std::clamp(latitudeDeg, -kWebMercatorMaxLatitudeDeg, kWebMercatorMaxLatitudeDeg));
    const double lon = common::wrapDegrees180(longitudeDeg);
    return TilePoint{(lon + 180.0) / 360.0 * *count,
                     (1.0 - std::asinh(std::tan(lat)) / kPi) / 2.0 * *count};
}

common::Result<TileId> tileAt(double latitudeDeg, double longitudeDeg, int zoom) {
    const auto point = toTilePoint(latitudeDeg, longitudeDeg, zoom);
    if (!point) {
        return std::unexpected(point.error());
    }
    const auto last = static_cast<std::int64_t>(std::ldexp(1.0, zoom)) - 1;
    return TileId{
        std::clamp(static_cast<std::int64_t>(std::floor(point->x)), std::int64_t{0}, last),
        std::clamp(static_cast<std::int64_t>(std::floor(point->y)), std::int64_t{0}, last), zoom};
}

common::Result<TileBounds> tileBounds(const TileId& tile) {
    const auto count = tileCount(tile.z);
    if (!count) {
        return std::unexpected(count.error());
    }
    const auto limit = static_cast<std::int64_t>(*count);
    if (tile.x < 0 || tile.y < 0 || tile.x >= limit || tile.y >= limit) {
        return common::makeError(
            std::format("tile {}/{}/{} does not exist", tile.z, tile.x, tile.y));
    }
    const auto x = static_cast<double>(tile.x);
    const auto y = static_cast<double>(tile.y);
    return TileBounds{x / *count * 360.0 - 180.0, tileRowToLatitudeDeg(y + 1.0, *count),
                      (x + 1.0) / *count * 360.0 - 180.0, tileRowToLatitudeDeg(y, *count)};
}

double groundResolution(double latitudeDeg, int zoom, int tileSizePx) noexcept {
    return std::cos(degToRad(latitudeDeg)) * 2.0 * kPi * kWebMercatorRadius /
           (static_cast<double>(tileSizePx) * std::ldexp(1.0, zoom));
}

}  // namespace ds::geo
