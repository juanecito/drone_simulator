// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ds/common/worker_pool.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/chunk_selection.hpp"
#include "ds/terrain/height_source.hpp"

/// @file
/// @brief Asynchronous chunk generation and caching around the camera (FR-TER-04/05).
/// @ingroup terrain

namespace ds::terrain {

/// @brief Streamer tuning.
struct StreamerSettings {
    SelectionSettings selection;             ///< LOD selection.
    std::size_t cacheBytes = 512ULL << 20U;  ///< In-memory chunk cache limit (NFR-PERF-04).
    std::size_t maxInFlight = 0;             ///< Concurrent jobs; 0 ⇒ 2 × worker threads.
};

/// @brief Counters for the UI and tests.
struct StreamerStats {
    std::size_t cachedChunks = 0;  ///< Meshes in the cache.
    std::size_t cachedBytes = 0;   ///< Their memory use.
    std::size_t inFlight = 0;      ///< Jobs queued or running.
    std::size_t generated = 0;     ///< Meshes built since creation.
    std::size_t stale = 0;         ///< Cached meshes waiting to be rebuilt (changed data).
};

/// @brief Keeps the chunks around the camera generated, using a WorkerPool.
/// @details Call update() once per frame from one thread (the render thread). Chunk meshes are
/// built on the pool and become visible on a later update(); the render and simulation
/// threads never wait for them (FR-TER-05). Least-recently-used meshes beyond the cache limit
/// are dropped. The destructor abandons queued jobs; jobs that are already running finish on
/// their own and their results are discarded.
///
/// When the source's data changes (HeightSource::revision()), cached meshes that overlap a
/// changed rectangle (or sample it through their normal apron) are marked stale: they are
/// still drawn, and rebuilt in the background with the new data.
class TerrainStreamer {
public:
    /// @brief Streams @p source with @p layout on @p pool (which must outlive the streamer).
    TerrainStreamer(std::shared_ptr<const HeightSource> source, const ChunkLayout& layout,
                    common::WorkerPool& pool, const StreamerSettings& settings = {});
    ~TerrainStreamer();

    TerrainStreamer(const TerrainStreamer&) = delete;
    TerrainStreamer& operator=(const TerrainStreamer&) = delete;
    TerrainStreamer(TerrainStreamer&&) = delete;
    TerrainStreamer& operator=(TerrainStreamer&&) = delete;

    /// @brief Collects finished chunks, selects the chunks for a camera at (@p x, @p y, @p z),
    /// queues missing ones and returns the meshes to draw.
    [[nodiscard]] std::vector<std::shared_ptr<const ChunkMesh>> update(double x, double y,
                                                                       double z);

    /// @brief Changes the LOD selection (e.g. view distance) for the next update().
    void setSelection(const SelectionSettings& selection) noexcept {
        settings_.selection = selection;
    }

    /// @brief The source being streamed.
    [[nodiscard]] const std::shared_ptr<const HeightSource>& source() const noexcept {
        return source_;
    }
    /// @brief The chunk layout.
    [[nodiscard]] const ChunkLayout& layout() const noexcept { return layout_; }
    /// @brief Current counters.
    [[nodiscard]] StreamerStats stats() const;

private:
    struct Shared;
    struct CacheEntry {
        std::shared_ptr<const ChunkMesh> mesh;
        std::uint64_t lastUsed = 0;
        std::uint64_t revision = 0;  // Source revision the mesh was built from.
        bool stale = false;          // Overlaps data that changed since.
    };

    void collect();
    void evict();
    void applyChanges();
    void submit(const ChunkId& id, double priority);
    [[nodiscard]] bool touches(const ChunkId& id, const ChangedArea& change) const noexcept;

    std::shared_ptr<const HeightSource> source_;
    ChunkLayout layout_;
    common::WorkerPool* pool_;
    StreamerSettings settings_;
    std::shared_ptr<Shared> shared_;
    std::unordered_map<ChunkId, CacheEntry, ChunkIdHash> cache_;
    std::unordered_set<ChunkId, ChunkIdHash> inFlight_;
    std::size_t cachedBytes_ = 0;
    std::size_t generated_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t seenRevision_ = 0;
    std::vector<ChangedArea> changes_;  // Recent changes, oldest first.
};

}  // namespace ds::terrain
