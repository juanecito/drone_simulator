// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Private header: orthophoto tiles resident on the GPU (FR-TER-I06, FR-GFX-07), in one
/// sRGB texture array within a memory budget.
/// @ingroup graphics

#include <glad/gl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "ds/terrain/ortho_streamer.hpp"

#include "gl_objects.hpp"
#include <glm/vec4.hpp>

namespace ds::graphics::detail {

/// @brief Per-chunk lookup of the terrain shader: one entry per tile of the chunk, row by row.
/// Each is (layer or −1, scale, offset x, offset z): a texel of the tile at position p ∈
/// [0, 1]² is read at `offset + scale · p` of the layer, which may hold a coarser ancestor.
using OrthoSlots =
    std::array<glm::vec4, terrain::kOrthoTilesPerChunk * terrain::kOrthoTilesPerChunk>;

/// @brief Counters of the GPU tile cache.
struct OrthoTextureStats {
    std::size_t residentTiles = 0;  ///< Layers in use.
    std::size_t bytes = 0;          ///< Texture memory allocated (all layers, with mipmaps).
    std::size_t uploads = 0;        ///< Tiles uploaded this frame.
    std::size_t capacity = 0;       ///< Layers allocated.
};

/// @brief Keeps the orthophoto tiles of the drawn chunks in a GL_TEXTURE_2D_ARRAY.
/// @details The array grows (doubling) up to the budget and the driver's layer limit; when it
/// is full, the least recently used tile not needed this frame gives up its layer. A tile
/// that cannot be uploaded yet (upload limit per frame, full array, still being built) is
/// replaced by its nearest resident ancestor, so the imagery sharpens progressively and the
/// frame never waits. A tile rebuilt with more data (online imagery) is uploaded again into
/// its layer.
class OrthoTextures {
public:
    /// @brief Tiles uploaded per frame at most (≈ 350 kB each).
    static constexpr std::size_t kUploadsPerFrame = 16;
    /// @brief Levels above a tile searched for a stand-in (the streamer builds them).
    static constexpr int kMaxFallbackLevels = terrain::kOrthoAncestorLevels;

    OrthoTextures() = default;

    /// @brief Sets the GPU memory budget, bytes; shrinking it drops every tile.
    void setBudget(std::size_t bytes);
    /// @brief Starts frame @p frame with the tiles of @p streamer (null ⇒ no imagery). A
    /// different streamer (new terrain or imagery) drops the tiles of the previous one.
    void beginFrame(const terrain::OrthoStreamer* streamer, std::uint64_t frame);
    /// @brief Plans the frame for the visible chunks @p chunks: if their tiles do not all fit
    /// in the budget, coarser tiles (one level up, or more) are used for every chunk.
    void prepare(std::span<const terrain::ChunkId> chunks);
    /// @brief Levels the tiles are raised by this frame (0 = full detail).
    [[nodiscard]] int levelBias() const noexcept { return bias_; }
    /// @brief Fills @p slots for chunk @p id, uploading tiles as allowed.
    /// @return False if no tile of the chunk has imagery (the shader skips the lookup).
    bool slotsFor(const terrain::ChunkId& id, OrthoSlots& slots);
    /// @brief The texture array (0 before the first upload).
    [[nodiscard]] GLuint texture() const noexcept { return texture_.id(); }
    /// @brief Counters.
    [[nodiscard]] OrthoTextureStats stats() const noexcept;

private:
    struct Resident {
        int layer = 0;
        std::uint64_t lastUsed = 0;
        std::uint64_t serial = 0;  // OrthoTile::serial of the image in the layer.
    };

    int layerLimit(int tileSizePx);
    int residentLayer(const terrain::OrthoTileKey& key);
    int upload(const terrain::OrthoTileKey& key, const terrain::OrthoTile& tile);
    void write(int layer, const terrain::OrthoTile& tile);
    int freeLayer();
    bool grow(int tileSizePx);
    void clear();

    std::size_t budget_ = std::size_t{1} << 30U;
    const terrain::OrthoStreamer* streamer_ = nullptr;
    std::uint64_t streamerId_ = 0;
    std::uint64_t frame_ = 0;
    std::size_t uploads_ = 0;
    int bias_ = 0;
    gl::Texture texture_;
    int tileSizePx_ = 0;
    int capacity_ = 0;
    int maxLayers_ = 0;
    std::vector<int> free_;
    std::unordered_map<terrain::OrthoTileKey, Resident, terrain::OrthoTileKeyHash> resident_;
};

}  // namespace ds::graphics::detail
