// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/query.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ds::terrain {

TerrainQuery::TerrainQuery(std::shared_ptr<const HeightSource> source, const ChunkLayout& layout,
                           double originAltitudeM)
    : source_(std::move(source)), spacing_(layout.spacing(0)), originAltitude_(originAltitudeM) {}

TerrainQuery::Cell TerrainQuery::cell(double x, double z) const {
    if (const auto extent = source_->extent()) {
        x = std::clamp(x, -0.5 * extent->widthM, 0.5 * extent->widthM);
        z = std::clamp(z, -0.5 * extent->lengthM, 0.5 * extent->lengthM);
    }
    const double cx = std::floor(x / spacing_);
    const double cz = std::floor(z / spacing_);
    const double x0 = cx * spacing_;
    const double z0 = cz * spacing_;
    // Same sampling path as the mesh builder (sampleGrid at level-0 spacing), so the corner
    // heights are bit-identical to the mesh vertices before float rounding.
    std::array<double, 4> h{};
    source_->sampleGrid(x0, z0, spacing_, 2, 2, h);
    return {h[0],
            h[1],
            h[2],
            h[3],
            std::clamp(x / spacing_ - cx, 0.0, 1.0),
            std::clamp(z / spacing_ - cz, 0.0, 1.0)};
}

double TerrainQuery::height(double x, double z) const {
    const Cell c = cell(x, z);
    // The mesh splits each cell along (0,0)–(1,1): triangle (00, 01, 11) where fz ≥ fx and
    // triangle (00, 11, 10) where fx > fz.
    if (c.fz >= c.fx) {
        return c.h00 + c.fz * (c.h01 - c.h00) + c.fx * (c.h11 - c.h01);
    }
    return c.h00 + c.fx * (c.h10 - c.h00) + c.fz * (c.h11 - c.h10);
}

Normal TerrainQuery::normal(double x, double z) const {
    const Cell c = cell(x, z);
    double gx = 0.0;  // ∂h/∂x
    double gz = 0.0;  // ∂h/∂z
    if (c.fz >= c.fx) {
        gx = (c.h11 - c.h01) / spacing_;
        gz = (c.h01 - c.h00) / spacing_;
    } else {
        gx = (c.h10 - c.h00) / spacing_;
        gz = (c.h11 - c.h10) / spacing_;
    }
    const double len = std::sqrt(gx * gx + 1.0 + gz * gz);
    return {-gx / len, 1.0 / len, -gz / len};
}

std::vector<float> sampleHeightMap(const HeightSource& source, double centerX, double centerZ,
                                   double sizeM, int width, int height) {
    const double step = sizeM / std::max(width, height);
    const double x0 = centerX - 0.5 * step * (width - 1);
    const double z0 = centerZ - 0.5 * step * (height - 1);
    std::vector<double> values(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    source.sampleGrid(x0, z0, step, width, height, values);
    return {values.begin(), values.end()};
}

}  // namespace ds::terrain
