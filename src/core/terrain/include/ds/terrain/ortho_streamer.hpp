// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "ds/common/worker_pool.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/orthophoto.hpp"

/// @file
/// @brief Builds the orthophoto tiles of the drawn terrain chunks in the background
/// (FR-TER-I06).
/// @ingroup terrain

namespace ds::terrain {

/// @brief Streamer tuning.
struct OrthoStreamerSettings {
    /// In-memory tile cache (CPU) limit; tiles still wanted by the drawn chunks are kept.
    std::size_t cacheBytes = std::size_t{384} << 20U;
    std::size_t maxInFlight = 0;  ///< Concurrent tile jobs; 0 ⇒ 2 × worker threads.
};

/// @brief Counters for the UI and tests.
struct OrthoStreamerStats {
    std::size_t cachedTiles = 0;  ///< Tiles with imagery in the cache.
    std::size_t cachedBytes = 0;  ///< Their memory use.
    std::size_t emptyTiles = 0;   ///< Tiles known to hold no imagery.
    std::size_t inFlight = 0;     ///< Jobs queued or running.
    std::size_t built = 0;        ///< Tiles built since creation.
};

/// @brief Levels of ancestors requested with each tile: they stand in while finer tiles are
/// built, and when the finer ones do not fit in the GPU budget.
inline constexpr int kOrthoAncestorLevels = 3;

/// @brief State of a tile in the streamer.
enum class OrthoTileState {
    Unknown,  ///< Not requested (or evicted).
    Pending,  ///< Being built.
    Empty,    ///< No imagery there: the terrain materials show.
    Ready,    ///< Image available.
};

/// @brief The tiles of the chunks drawn each frame, built on a WorkerPool.
/// @details Call update() once per frame on the render thread with the chunks being drawn.
/// Each chunk wants its kOrthoTilesPerChunk² tiles of the same level, and their ancestors up to
/// kOrthoAncestorLevels above, so a coarser image can stand in while the finer one is built. Tiles
/// nearer the camera (relative to their size) and coarser tiles are built first.
/// Least-recently-wanted tiles beyond the cache limit are dropped. The destructor abandons queued
/// jobs.
class OrthoStreamer {
public:
    /// @brief Streams @p source for chunks of @p layout on @p pool (which must outlive it).
    OrthoStreamer(std::shared_ptr<const OrthophotoSource> source, const ChunkLayout& layout,
                  common::WorkerPool& pool, const OrthoStreamerSettings& settings = {});
    ~OrthoStreamer();

    OrthoStreamer(const OrthoStreamer&) = delete;
    OrthoStreamer& operator=(const OrthoStreamer&) = delete;
    OrthoStreamer(OrthoStreamer&&) = delete;
    OrthoStreamer& operator=(OrthoStreamer&&) = delete;

    /// @brief Collects finished tiles and queues the missing tiles of @p chunks for a camera
    /// at (@p x, @p z).
    void update(std::span<const std::shared_ptr<const ChunkMesh>> chunks, double x, double z);

    /// @brief State of tile @p key; with Ready, @p tile receives the image.
    [[nodiscard]] OrthoTileState find(const OrthoTileKey& key,
                                      std::shared_ptr<const OrthoTile>* tile = nullptr) const;

    /// @brief The kOrthoTilesPerChunk² tiles of chunk @p id, row by row (z, then x).
    [[nodiscard]] static std::array<OrthoTileKey, kOrthoTilesPerChunk * kOrthoTilesPerChunk>
    tilesOf(const ChunkId& id) noexcept;

    /// @brief The imagery.
    [[nodiscard]] const std::shared_ptr<const OrthophotoSource>& source() const noexcept {
        return source_;
    }
    /// @brief Unique id of this streamer in the process (GPU caches key on it).
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    /// @brief Current counters.
    [[nodiscard]] OrthoStreamerStats stats() const;

private:
    struct Shared;
    struct Entry {
        std::shared_ptr<const OrthoTile> tile;  // nullptr ⇒ empty.
        std::uint64_t lastUsed = 0;
    };

    void collect();
    void evict();
    void want(const OrthoTileKey& key, double priority);

    std::shared_ptr<const OrthophotoSource> source_;
    ChunkLayout layout_;
    common::WorkerPool* pool_;
    OrthoStreamerSettings settings_;
    std::shared_ptr<Shared> shared_;
    std::unordered_map<OrthoTileKey, Entry, OrthoTileKeyHash> cache_;
    std::unordered_set<OrthoTileKey, OrthoTileKeyHash> inFlight_;
    std::unordered_map<OrthoTileKey, double, OrthoTileKeyHash> wanted_;  // This frame.
    std::size_t cachedBytes_ = 0;
    std::size_t built_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t id_ = 0;
};

}  // namespace ds::terrain
