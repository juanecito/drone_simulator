// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/block_planner.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace ds::net {
namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kKmPerDegree = kEarthRadiusKm * kDegToRad;
constexpr int kMaxPieces = 4096;

GeoRect piece(const GeoRect& area, int cols, int rows, int c, int r) noexcept {
    const double dLon = (area.east - area.west) / cols;
    const double dLat = (area.north - area.south) / rows;
    // The last piece ends exactly on the edge (no rounding gap).
    return GeoRect{area.west + (c * dLon), area.south + (r * dLat),
                   c + 1 == cols ? area.east : area.west + ((c + 1) * dLon),
                   r + 1 == rows ? area.north : area.south + ((r + 1) * dLat)};
}

double greatCircleKm(double lat1, double lon1, double lat2, double lon2) noexcept {
    const double p1 = lat1 * kDegToRad;
    const double p2 = lat2 * kDegToRad;
    const double dp = p2 - p1;
    const double dl = (lon2 - lon1) * kDegToRad;
    const double a = (std::sin(dp / 2) * std::sin(dp / 2)) +
                     (std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2));
    return 2.0 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(a)));
}

}  // namespace

std::uint64_t estimateResponseBytes(const GeoRect& area, const DatasetConfig& dataset) noexcept {
    const double resKm = dataset.resolutionM / 1000.0;
    const double px = std::ceil(area.widthKm() / resKm) * std::ceil(area.heightKm() / resKm);
    return static_cast<std::uint64_t>(px * dataset.bytesPerSample);
}

NetResult<std::vector<GeoRect>> planExtentRequests(const GeoRect& area,
                                                   const ProviderConfig& provider,
                                                   const DatasetConfig& dataset) {
    if (!area.valid()) {
        return netError(NetErrorCode::InvalidRequest, "invalid area");
    }
    if (dataset.coverage && !dataset.coverage->contains(area)) {
        return netError(NetErrorCode::InvalidRequest,
                        std::format("the area is outside the coverage of {} ({})", dataset.name,
                                    provider.name));
    }
    const double maxArea = dataset.maxAreaKm2.value_or(std::numeric_limits<double>::infinity());
    // Leave 10 % headroom: the payload has headers and the estimate is approximate.
    const auto maxBytes = static_cast<double>(provider.maxResponseBytes) * 0.9;
    auto fits = [&](const GeoRect& r) {
        return r.areaKm2() <= maxArea &&
               static_cast<double>(estimateResponseBytes(r, dataset)) <= maxBytes;
    };
    for (int count = 1; count <= kMaxPieces; ++count) {
        int bestCols = 0;
        double bestShape = std::numeric_limits<double>::infinity();
        for (int cols = 1; cols <= count; ++cols) {
            if (count % cols != 0) {
                continue;
            }
            const int rows = count / cols;
            bool ok = true;
            // Pieces nearest the equator are the largest; checking each row covers all cases.
            for (int r = 0; r < rows && ok; ++r) {
                ok = fits(piece(area, cols, rows, 0, r));
            }
            if (!ok) {
                continue;
            }
            const GeoRect p = piece(area, cols, rows, 0, 0);
            const double shape = std::abs(std::log(p.widthKm() / p.heightKm()));
            if (shape < bestShape) {
                bestShape = shape;
                bestCols = cols;
            }
        }
        if (bestCols > 0) {
            const int rows = count / bestCols;
            std::vector<GeoRect> out;
            out.reserve(static_cast<std::size_t>(count));
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < bestCols; ++c) {
                    out.push_back(piece(area, bestCols, rows, c, r));
                }
            }
            return out;
        }
    }
    return netError(NetErrorCode::InvalidRequest,
                    std::format("the area needs more than {} requests", kMaxPieces));
}

std::vector<Block> blocksAround(double latDeg, double lonDeg, double radiusKm, double blockSizeKm) {
    std::vector<Block> out;
    if (!(blockSizeKm > 0.0) || !(radiusKm >= 0.0)) {
        return out;
    }
    const double dLat = blockSizeKm / kKmPerDegree;
    const auto rowOf = [&](double lat) {
        return static_cast<std::int32_t>(std::floor(lat / dLat));
    };
    const double reach = radiusKm + blockSizeKm;  // Centre within radius ⇒ block within this.
    const std::int32_t rowMin = rowOf(std::max(-90.0, latDeg - (reach / kKmPerDegree)));
    const std::int32_t rowMax = rowOf(std::min(90.0 - 1e-9, latDeg + (reach / kKmPerDegree)));
    for (std::int32_t row = rowMin; row <= rowMax; ++row) {
        const double south = std::max(-90.0, row * dLat);
        const double north = std::min(90.0, (row + 1) * dLat);
        const double mid = 0.5 * (south + north);
        const double bandWidthKm = 360.0 * kKmPerDegree * std::cos(mid * kDegToRad);
        const int cols = std::max(1, static_cast<int>(std::lround(bandWidthKm / blockSizeKm)));
        const double dLon = 360.0 / cols;
        const double reachLon =
            std::min(180.0, reach / (kKmPerDegree * std::max(0.01, std::cos(mid * kDegToRad))));
        const auto colMin =
            static_cast<std::int32_t>(std::floor((lonDeg - reachLon + 180.0) / dLon));
        const auto colMax =
            static_cast<std::int32_t>(std::floor((lonDeg + reachLon + 180.0) / dLon));
        for (std::int32_t c = colMin; c <= colMax; ++c) {
            const std::int32_t col = ((c % cols) + cols) % cols;  // Wrap around the antimeridian.
            const double west = -180.0 + (col * dLon);
            const GeoRect rect{west, south, col + 1 == cols ? 180.0 : west + dLon, north};
            const double d = greatCircleKm(latDeg, lonDeg, mid, west + (0.5 * dLon));
            if (d <= radiusKm && std::ranges::none_of(out, [&](const Block& b) {
                    return b.key == BlockKey{row, col};
                })) {
                out.push_back(Block{BlockKey{row, col}, rect, d});
            }
        }
    }
    std::ranges::sort(out, {}, &Block::distanceKm);
    return out;
}

}  // namespace ds::net
