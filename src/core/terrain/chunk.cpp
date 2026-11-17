// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/chunk.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "ds/common/random.hpp"

namespace ds::terrain {
namespace {

double smoothstep(double edge0, double edge1, double x) noexcept {
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

std::int16_t toSnorm16(double v) noexcept {
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
}

}  // namespace

bool ChunkLayout::isValid() const noexcept {
    const int cells = gridSize - 1;
    return chunkSizeM >= 16.0 && chunkSizeM <= 4096.0 && cells >= 4 && cells <= 256 &&
           std::has_single_bit(static_cast<unsigned>(cells)) && lodCount >= 5 && lodCount <= 16;
}

std::size_t ChunkIdHash::operator()(const ChunkId& id) const noexcept {
    std::uint64_t h = common::mix64(static_cast<std::uint64_t>(id.ix));
    h = common::mix64(h ^ static_cast<std::uint64_t>(id.iz));
    return static_cast<std::size_t>(common::mix64(h ^ static_cast<std::uint64_t>(id.lod)));
}

ChunkBounds chunkBounds(const ChunkLayout& layout, const ChunkId& id) noexcept {
    const double size = layout.size(id.lod);
    const double minX = static_cast<double>(id.ix) * size;
    const double minZ = static_cast<double>(id.iz) * size;
    return {minX, minZ, minX + size, minZ + size};
}

bool chunkInExtent(const ChunkLayout& layout, const ChunkId& id,
                   const std::optional<Extent>& extent) noexcept {
    if (!extent) {
        return true;
    }
    const ChunkBounds b = chunkBounds(layout, id);
    return b.maxX > -0.5 * extent->widthM && b.minX < 0.5 * extent->widthM &&
           b.maxZ > -0.5 * extent->lengthM && b.minZ < 0.5 * extent->lengthM;
}

std::uint32_t encodeNormal(double x, double y, double z) noexcept {
    const double inv = 1.0 / (std::abs(x) + std::abs(y) + std::abs(z));
    double ox = x * inv;
    double oz = z * inv;
    if (y < 0.0) {  // Fold the lower hemisphere.
        const double fx = (1.0 - std::abs(oz)) * (ox >= 0.0 ? 1.0 : -1.0);
        const double fz = (1.0 - std::abs(ox)) * (oz >= 0.0 ? 1.0 : -1.0);
        ox = fx;
        oz = fz;
    }
    const auto a = static_cast<std::uint16_t>(toSnorm16(ox));
    const auto b = static_cast<std::uint16_t>(toSnorm16(oz));
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 16U);
}

void decodeNormal(std::uint32_t packed, double& x, double& y, double& z) noexcept {
    const auto a = static_cast<std::int16_t>(packed & 0xFFFFU);
    const auto b = static_cast<std::int16_t>(packed >> 16U);
    x = std::max(static_cast<double>(a) / 32767.0, -1.0);
    z = std::max(static_cast<double>(b) / 32767.0, -1.0);
    y = 1.0 - std::abs(x) - std::abs(z);
    if (y < 0.0) {
        const double fx = (1.0 - std::abs(z)) * (x >= 0.0 ? 1.0 : -1.0);
        const double fz = (1.0 - std::abs(x)) * (z >= 0.0 ? 1.0 : -1.0);
        x = fx;
        z = fz;
    }
    const double len = std::sqrt(x * x + y * y + z * z);
    x /= len;
    y /= len;
    z /= len;
}

std::uint32_t classifyMaterial(double height, double normalY, std::optional<double> waterLevel,
                               double snowLine) {
    const double slope = 1.0 - std::clamp(normalY, 0.0, 1.0);  // 0 flat … 1 vertical
    const double rock = smoothstep(0.18, 0.32, slope);         // ≈ 35°–47°
    double sand = 0.0;
    if (waterLevel) {
        sand = smoothstep(*waterLevel + 3.0, *waterLevel + 0.5, height) * (1.0 - rock);
    }
    const double snowSlope = 1.0 - smoothstep(0.30, 0.45, slope);  // Snow slides off cliffs.
    const double snow =
        smoothstep(snowLine - 60.0, snowLine + 60.0, height) * snowSlope * (1.0 - sand);
    const double rest = std::max(0.0, 1.0 - std::max(rock, snow) - sand);
    std::array<double, 4> w{rest, rock * (1.0 - snow), sand, snow};
    double sum = w[0] + w[1] + w[2] + w[3];
    if (sum <= 0.0) {
        w = {1.0, 0.0, 0.0, 0.0};
        sum = 1.0;
    }
    std::array<int, 4> q{};
    int total = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        q[i] = static_cast<int>(std::floor(w[i] / sum * 255.0));
        total += q[i];
    }
    q[static_cast<std::size_t>(std::distance(w.begin(), std::ranges::max_element(w)))] +=
        255 - total;  // Exact sum of 255.
    return static_cast<std::uint32_t>(q[0]) | (static_cast<std::uint32_t>(q[1]) << 8U) |
           (static_cast<std::uint32_t>(q[2]) << 16U) | (static_cast<std::uint32_t>(q[3]) << 24U);
}

Material dominantMaterial(double height, double normalY, std::optional<double> waterLevel,
                          double snowLine) {
    const std::uint32_t packed = classifyMaterial(height, normalY, waterLevel, snowLine);
    std::uint32_t best = 0;
    std::uint32_t bestWeight = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t weight = (packed >> (8U * i)) & 0xFFU;
        if (weight > bestWeight) {
            best = i;
            bestWeight = weight;
        }
    }
    return static_cast<Material>(best);
}

std::vector<std::uint16_t> buildChunkIndices(int gridSize) {
    const int n = gridSize;
    std::vector<std::uint16_t> indices;
    indices.reserve(static_cast<std::size_t>((n - 1) * (n - 1) * 6 + 4 * (n - 1) * 6));
    const auto grid = [n](int col, int row) { return static_cast<std::uint16_t>(row * n + col); };
    for (int row = 0; row + 1 < n; ++row) {
        for (int col = 0; col + 1 < n; ++col) {
            const auto a = grid(col, row);
            const auto b = grid(col + 1, row);
            const auto c = grid(col, row + 1);
            const auto d = grid(col + 1, row + 1);
            // Diagonal a–d, counter-clockwise seen from +y (x east, z south).
            indices.insert(indices.end(), {a, c, d, a, d, b});
        }
    }

    // Skirts: edge e has n vertices starting at n² + e·n, hanging below grid vertices.
    // Edge 0: row 0 (north, outward −z); 1: row n−1 (south, +z); 2: col 0 (west, −x);
    // 3: col n−1 (east, +x).
    struct Edge {
        int dx;  // Outward direction.
        int dz;
    };
    constexpr std::array<Edge, 4> kEdges{{{0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
    for (int e = 0; e < 4; ++e) {
        for (int k = 0; k + 1 < n; ++k) {
            const auto top = [&](int i) {
                switch (e) {
                    case 0: return grid(i, 0);
                    case 1: return grid(i, n - 1);
                    case 2: return grid(0, i);
                    default: return grid(n - 1, i);
                }
            };
            const auto bottom = [&](int i) {
                return static_cast<std::uint16_t>(n * n + e * n + i);
            };
            const std::uint16_t t0 = top(k);
            const std::uint16_t t1 = top(k + 1);
            const std::uint16_t b0 = bottom(k);
            const std::uint16_t b1 = bottom(k + 1);
            // (t0, b0, b1) has normal down × along: along +x ⇒ (−y) × x = +z; along +z ⇒
            // (−y) × z = −x. Keep that order when it points outward, else flip it.
            const bool alongX = e < 2;
            const double crossX = alongX ? 0.0 : -1.0;
            const double crossZ = alongX ? 1.0 : 0.0;
            const bool ccw = crossX * kEdges[static_cast<std::size_t>(e)].dx +
                                 crossZ * kEdges[static_cast<std::size_t>(e)].dz >
                             0.0;
            if (ccw) {
                indices.insert(indices.end(), {t0, b0, b1, t0, b1, t1});
            } else {
                indices.insert(indices.end(), {t0, b1, b0, t0, t1, b1});
            }
        }
    }
    return indices;
}

ChunkMesh buildChunkMesh(const HeightSource& source, const ChunkLayout& layout, const ChunkId& id) {
    const int n = layout.gridSize;
    const int apron = n + 2;
    const double spacing = layout.spacing(id.lod);
    const ChunkBounds bounds = chunkBounds(layout, id);
    const std::optional<Extent> extent = source.extent();

    ChunkMesh mesh;
    mesh.id = id;
    mesh.originX = bounds.minX;
    mesh.originZ = bounds.minZ;
    mesh.spacingM = spacing;

    std::vector<double> heights(static_cast<std::size_t>(apron) * static_cast<std::size_t>(apron));
    source.sampleGrid(bounds.minX - spacing, bounds.minZ - spacing, spacing, apron, apron, heights);
    const auto h = [&](int col, int row) {  // Grid coordinates, −1 … n.
        return heights[static_cast<std::size_t>(row + 1) * static_cast<std::size_t>(apron) +
                       static_cast<std::size_t>(col + 1)];
    };

    // Grid columns/rows whose world position lies outside the extent are clamped onto the
    // border; their heights must then be the border heights too. The source clamps its
    // input, so sampling at the unclamped position already returns the border height.
    const auto worldX = [&](int col) {
        const double x = bounds.minX + col * spacing;
        return extent ? std::clamp(x, -0.5 * extent->widthM, 0.5 * extent->widthM) : x;
    };
    const auto worldZ = [&](int row) {
        const double z = bounds.minZ + row * spacing;
        return extent ? std::clamp(z, -0.5 * extent->lengthM, 0.5 * extent->lengthM) : z;
    };

    const std::optional<double> water = source.waterLevel();
    const double snowLine = source.snowLine();
    mesh.vertices.resize(static_cast<std::size_t>(n * n + 4 * n));
    float minH = std::numeric_limits<float>::max();
    float maxH = std::numeric_limits<float>::lowest();
    for (int row = 0; row < n; ++row) {
        for (int col = 0; col < n; ++col) {
            const double height = h(col, row);
            // Central differences over the true neighbour distance (shorter where clamped).
            const double dxw = std::max(worldX(col + 1) - worldX(col - 1), 1e-6);
            const double dzw = std::max(worldZ(row + 1) - worldZ(row - 1), 1e-6);
            const double gx = (h(col + 1, row) - h(col - 1, row)) / dxw;
            const double gz = (h(col, row + 1) - h(col, row - 1)) / dzw;
            const double len = std::sqrt(gx * gx + 1.0 + gz * gz);
            const double nx = -gx / len;
            const double ny = 1.0 / len;
            const double nz = -gz / len;

            TerrainVertex& v = mesh.vertices[static_cast<std::size_t>(row * n + col)];
            v.height = static_cast<float>(height);
            v.u = static_cast<std::uint16_t>(col);
            v.v = static_cast<std::uint16_t>(row);
            v.normal = encodeNormal(nx, ny, nz);
            v.material = classifyMaterial(height, ny, water, snowLine);
            minH = std::min(minH, v.height);
            maxH = std::max(maxH, v.height);
        }
    }
    mesh.minHeight = minH;
    mesh.maxHeight = maxH;

    // Skirt depth: enough to cover the largest possible crack to a coarser neighbour.
    const float skirt =
        static_cast<float>(spacing * 1.5 + 0.05 * static_cast<double>(maxH - minH) + 2.0);
    const bool bounded = extent.has_value();
    const auto onBorder = [&](int e) {
        if (!extent) {
            return false;
        }
        switch (e) {
            case 0: return bounds.minZ <= -0.5 * extent->lengthM;
            case 1: return bounds.maxZ >= 0.5 * extent->lengthM;
            case 2: return bounds.minX <= -0.5 * extent->widthM;
            default: return bounds.maxX >= 0.5 * extent->widthM;
        }
    };
    for (int e = 0; e < 4; ++e) {
        // The extent border may run through the chunk; clamped vertices then lie on it, and
        // the skirt of that edge becomes the terrain's outer wall.
        const bool wall = bounded && onBorder(e);
        for (int i = 0; i < n; ++i) {
            int col = 0;
            int row = 0;
            switch (e) {
                case 0:
                    col = i;
                    row = 0;
                    break;
                case 1:
                    col = i;
                    row = n - 1;
                    break;
                case 2:
                    col = 0;
                    row = i;
                    break;
                default:
                    col = n - 1;
                    row = i;
                    break;
            }
            TerrainVertex v = mesh.vertices[static_cast<std::size_t>(row * n + col)];
            v.height = wall ? std::min(v.height - skirt, minH - 200.0F) : v.height - skirt;
            mesh.vertices[static_cast<std::size_t>(n * n + e * n + i)] = v;
        }
    }
    return mesh;
}

}  // namespace ds::terrain
