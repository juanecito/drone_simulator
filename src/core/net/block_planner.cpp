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

ResponseSize responseSizePx(const GeoRect& area, const DatasetConfig& dataset) noexcept {
    const double resKm = dataset.resolutionM / 1000.0;
    return {static_cast<long>(std::ceil(area.widthKm() / resKm)),
            static_cast<long>(std::ceil(area.heightKm() / resKm))};
}

std::uint64_t estimateResponseBytes(const GeoRect& area, const DatasetConfig& dataset) noexcept {
    const ResponseSize size = responseSizePx(area, dataset);
    return static_cast<std::uint64_t>(static_cast<double>(size.width) *
                                      static_cast<double>(size.height) * dataset.bytesPerSample);
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
    const long maxPx = provider.maxSizePx.value_or(std::numeric_limits<int>::max());
    auto fits = [&](const GeoRect& r) {
        const ResponseSize size = responseSizePx(r, dataset);
        return r.areaKm2() <= maxArea && size.width <= maxPx && size.height <= maxPx &&
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

namespace {

// Number of blocks in the latitude band @p row of the grid.
struct Band {
    double south = 0.0;
    double north = 0.0;
    double mid = 0.0;
    int cols = 1;
    double dLon = 360.0;
};

Band band(std::int32_t row, double blockSizeKm) noexcept {
    const double dLat = blockSizeKm / kKmPerDegree;
    Band b;
    b.south = std::max(-90.0, row * dLat);
    b.north = std::min(90.0, (row + 1) * dLat);
    b.mid = 0.5 * (b.south + b.north);
    const double bandWidthKm = 360.0 * kKmPerDegree * std::cos(b.mid * kDegToRad);
    b.cols = std::max(1, static_cast<int>(std::lround(bandWidthKm / blockSizeKm)));
    b.dLon = 360.0 / b.cols;
    return b;
}

GeoRect blockRect(const Band& b, std::int32_t col) noexcept {
    const double west = -180.0 + (col * b.dLon);
    return GeoRect{west, b.south, col + 1 == b.cols ? 180.0 : west + b.dLon, b.north};
}

std::int32_t rowOf(double latDeg, double blockSizeKm) noexcept {
    const double dLat = blockSizeKm / kKmPerDegree;
    return static_cast<std::int32_t>(std::floor(std::clamp(latDeg, -90.0, 90.0 - 1e-9) / dLat));
}

}  // namespace

std::vector<Block> blocksAround(double latDeg, double lonDeg, double radiusKm, double blockSizeKm) {
    std::vector<Block> out;
    if (!(blockSizeKm > 0.0) || !(radiusKm >= 0.0)) {
        return out;
    }
    const double reach = radiusKm + blockSizeKm;  // Centre within radius ⇒ block within this.
    const std::int32_t rowMin = rowOf(latDeg - (reach / kKmPerDegree), blockSizeKm);
    const std::int32_t rowMax = rowOf(latDeg + (reach / kKmPerDegree), blockSizeKm);
    for (std::int32_t row = rowMin; row <= rowMax; ++row) {
        const Band b = band(row, blockSizeKm);
        const double reachLon =
            std::min(180.0, reach / (kKmPerDegree * std::max(0.01, std::cos(b.mid * kDegToRad))));
        const auto colMin =
            static_cast<std::int32_t>(std::floor((lonDeg - reachLon + 180.0) / b.dLon));
        const auto colMax =
            static_cast<std::int32_t>(std::floor((lonDeg + reachLon + 180.0) / b.dLon));
        for (std::int32_t c = colMin; c <= colMax; ++c) {
            const std::int32_t col = ((c % b.cols) + b.cols) % b.cols;  // Wrap at ±180°.
            const GeoRect rect = blockRect(b, col);
            const double d = greatCircleKm(latDeg, lonDeg, b.mid, rect.west + (0.5 * b.dLon));
            if (d <= radiusKm && std::ranges::none_of(out, [&](const Block& blk) {
                    return blk.key == BlockKey{row, col};
                })) {
                out.push_back(Block{BlockKey{row, col}, rect, d});
            }
        }
    }
    std::ranges::sort(out, {}, &Block::distanceKm);
    return out;
}

BlockKey blockAt(double latDeg, double lonDeg, double blockSizeKm) noexcept {
    const std::int32_t row = rowOf(latDeg, blockSizeKm);
    const Band b = band(row, blockSizeKm);
    auto col = static_cast<std::int32_t>(std::floor((lonDeg + 180.0) / b.dLon));
    col = std::clamp(col, 0, b.cols - 1);
    return BlockKey{row, col};
}

GeoRect blockRect(const BlockKey& key, double blockSizeKm) noexcept {
    const Band b = band(key.row, blockSizeKm);
    return blockRect(b, key.col);
}

}  // namespace ds::net
