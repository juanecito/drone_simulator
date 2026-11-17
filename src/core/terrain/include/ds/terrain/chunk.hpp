// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ds/terrain/height_source.hpp"

/// @file
/// @brief Chunk grid, level of detail and chunk meshes (FR-TER-04, FR-TER-06, FR-TER-08).
/// @ingroup terrain

namespace ds::terrain {

/// @brief Geometry of the chunk quadtree.
/// @details A chunk of level @c lod covers a square of side `chunkSizeM · 2^lod` metres and
/// has `gridSize × gridSize` vertices, so every level has the same vertex count and coarser
/// levels have a wider spacing. All levels share one lattice: vertices of level L are a
/// subset of those of level L−1.
struct ChunkLayout {
    double chunkSizeM = 256.0;  ///< Side of a level-0 chunk (default 256 m, FR-TER-04).
    int gridSize = 129;         ///< Vertices per side, 2^k + 1 (default 129).
    int lodCount = 8;           ///< Number of levels (≥ 5, FR-TER-06).

    /// @brief Side of a chunk of level @p lod, metres.
    [[nodiscard]] double size(int lod) const noexcept {
        return chunkSizeM * static_cast<double>(1LL << lod);
    }
    /// @brief Vertex spacing of level @p lod, metres.
    [[nodiscard]] double spacing(int lod) const noexcept { return size(lod) / (gridSize - 1); }
    /// @brief True if the values are usable (power-of-two grid, 5–16 levels, …).
    [[nodiscard]] bool isValid() const noexcept;
    /// @brief Equality.
    bool operator==(const ChunkLayout&) const = default;
};

/// @brief Address of a chunk: level and integer position on that level's grid.
/// @details Chunk (lod, ix, iz) spans x ∈ [ix·S, (ix+1)·S], z ∈ [iz·S, (iz+1)·S] with
/// S = ChunkLayout::size(lod).
struct ChunkId {
    int lod = 0;          ///< Level of detail (0 = finest).
    std::int64_t ix = 0;  ///< Column (east).
    std::int64_t iz = 0;  ///< Row (south).

    /// @brief Ordering (for maps and stable output).
    auto operator<=>(const ChunkId&) const = default;

    /// @brief The chunk one level up that contains this one.
    [[nodiscard]] ChunkId parent() const noexcept {
        return {lod + 1, ix >> 1, iz >> 1};  // Arithmetic shift = floor division by 2.
    }
    /// @brief Child @p index (0–3: bit 0 = east half, bit 1 = south half). Requires lod > 0.
    [[nodiscard]] ChunkId child(int index) const noexcept {
        return {lod - 1, 2 * ix + (index & 1), 2 * iz + ((index >> 1) & 1)};
    }
};

/// @brief Hash for ChunkId (unordered containers).
struct ChunkIdHash {
    /// @brief Hash value.
    std::size_t operator()(const ChunkId& id) const noexcept;
};

/// @brief Terrain surface classes (FR-TER-08). Water is not a vertex class: it is the water
/// surface drawn at the source's water level.
enum class Material : std::uint8_t { Grass = 0, Rock = 1, Sand = 2, Snow = 3 };

/// @brief One vertex of a chunk mesh (16 bytes, uploaded to the GPU as is).
/// @details The horizontal position is the grid index; the shader computes
/// `x = originX + u·spacing`, `z = originZ + v·spacing`, clamped to the terrain extent.
struct TerrainVertex {
    float height = 0.0F;         ///< World height, metres.
    std::uint16_t u = 0;         ///< Grid column, 0 … gridSize−1.
    std::uint16_t v = 0;         ///< Grid row, 0 … gridSize−1.
    std::uint32_t normal = 0;    ///< Unit normal, octahedral encoding, 2 × snorm16.
    std::uint32_t material = 0;  ///< Weights of grass, rock, sand, snow (unorm8 each, sum 255).
};
static_assert(sizeof(TerrainVertex) == 16);

/// @brief CPU mesh of one chunk: `gridSize²` grid vertices, then 4 × gridSize skirt vertices.
struct ChunkMesh {
    ChunkId id;                           ///< Address.
    double originX = 0.0;                 ///< World x of grid vertex (0, 0).
    double originZ = 0.0;                 ///< World z of grid vertex (0, 0).
    double spacingM = 0.0;                ///< Vertex spacing.
    float minHeight = 0.0F;               ///< Lowest vertex height (skirts excluded).
    float maxHeight = 0.0F;               ///< Highest vertex height.
    std::vector<TerrainVertex> vertices;  ///< Vertex data.

    /// @brief Approximate memory use, bytes.
    [[nodiscard]] std::size_t byteSize() const noexcept {
        return sizeof(ChunkMesh) + vertices.size() * sizeof(TerrainVertex);
    }
};

/// @brief Triangle indices shared by every chunk of a layout (grid + skirts), counter-
/// clockwise seen from outside. Each grid cell is split along its (u, v)–(u+1, v+1)
/// diagonal; TerrainQuery uses the same split.
[[nodiscard]] std::vector<std::uint16_t> buildChunkIndices(int gridSize);

/// @brief World rectangle of chunk @p id (before clamping to an extent).
struct ChunkBounds {
    double minX = 0.0;  ///< West edge.
    double minZ = 0.0;  ///< North edge (smaller z).
    double maxX = 0.0;  ///< East edge.
    double maxZ = 0.0;  ///< South edge.
};

/// @brief Rectangle covered by @p id.
[[nodiscard]] ChunkBounds chunkBounds(const ChunkLayout& layout, const ChunkId& id) noexcept;

/// @brief True if chunk @p id overlaps @p extent (always true without extent).
[[nodiscard]] bool chunkInExtent(const ChunkLayout& layout, const ChunkId& id,
                                 const std::optional<Extent>& extent) noexcept;

/// @brief Builds the mesh of chunk @p id from @p source (runs on a worker thread).
/// @details Heights come from HeightSource::sampleGrid with a one-sample apron for normals.
/// Grid positions outside the source's extent are clamped onto its border. Skirts hang below
/// every edge to hide cracks between levels (FR-TER-06); at the border of a bounded terrain
/// they reach far
/// enough down (200 m below the chunk) to close the terrain with a wall.
[[nodiscard]] ChunkMesh buildChunkMesh(const HeightSource& source, const ChunkLayout& layout,
                                       const ChunkId& id);

/// @brief Material weights (grass, rock, sand, snow; sum 255) for a vertex at @p height with
/// unit-normal y component @p normalY (FR-TER-08).
[[nodiscard]] std::uint32_t classifyMaterial(double height, double normalY,
                                             std::optional<double> waterLevel, double snowLine);

/// @brief The material with the largest weight in classifyMaterial() (tyre grip of ground
/// drones, FR-GND-04).
[[nodiscard]] Material dominantMaterial(double height, double normalY,
                                        std::optional<double> waterLevel, double snowLine);

/// @brief Octahedral encoding of a unit vector into 2 × snorm16.
[[nodiscard]] std::uint32_t encodeNormal(double x, double y, double z) noexcept;

/// @brief Decodes encodeNormal(); returns a unit vector.
void decodeNormal(std::uint32_t packed, double& x, double& y, double& z) noexcept;

}  // namespace ds::terrain
