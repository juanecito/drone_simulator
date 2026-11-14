// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/streamer.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace ds::terrain {

// State shared with the jobs; outlives the streamer while jobs still hold it.
struct TerrainStreamer::Shared {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::shared_ptr<const ChunkMesh>> finished;
};

TerrainStreamer::TerrainStreamer(std::shared_ptr<const HeightSource> source,
                                 const ChunkLayout& layout, common::WorkerPool& pool,
                                 const StreamerSettings& settings)
    : source_(std::move(source)),
      layout_(layout),
      pool_(&pool),
      settings_(settings),
      shared_(std::make_shared<Shared>()) {
    if (settings_.maxInFlight == 0) {
        settings_.maxInFlight = 2 * pool.threadCount();
    }
}

TerrainStreamer::~TerrainStreamer() { shared_->cancelled = true; }

void TerrainStreamer::collect() {
    std::vector<std::shared_ptr<const ChunkMesh>> finished;
    {
        const std::scoped_lock lock(shared_->mutex);
        finished.swap(shared_->finished);
    }
    for (auto& mesh : finished) {
        const ChunkId id = mesh->id;  // Read before the move below.
        inFlight_.erase(id);
        cachedBytes_ += mesh->byteSize();
        ++generated_;
        cache_[id] = CacheEntry{std::move(mesh), frame_};
    }
}

std::vector<std::shared_ptr<const ChunkMesh>> TerrainStreamer::update(double x, double y,
                                                                      double z) {
    ++frame_;
    collect();

    const ChunkSelection selection = selectChunks(
        layout_, source_->extent(), x, y, z, settings_.selection,
        [this](const ChunkId& id) -> std::optional<std::pair<float, float>> {
            const auto it = cache_.find(id);
            if (it == cache_.end()) {
                return std::nullopt;
            }
            it->second.lastUsed = frame_;  // Visited chunks (incl. split parents) stay cached.
            return std::pair{it->second.mesh->minHeight, it->second.mesh->maxHeight};
        });

    for (const auto& [id, priority] : selection.wanted) {
        if (inFlight_.size() >= settings_.maxInFlight) {
            break;
        }
        if (!inFlight_.insert(id).second) {
            continue;  // Already queued.
        }
        pool_->submit(priority, [source = source_, layout = layout_, id, shared = shared_] {
            if (shared->cancelled) {
                return;
            }
            auto mesh = std::make_shared<const ChunkMesh>(buildChunkMesh(*source, layout, id));
            const std::scoped_lock lock(shared->mutex);
            shared->finished.push_back(std::move(mesh));
        });
    }

    std::vector<std::shared_ptr<const ChunkMesh>> meshes;
    meshes.reserve(selection.render.size());
    for (const ChunkId& id : selection.render) {
        meshes.push_back(cache_.at(id).mesh);
    }
    evict();
    return meshes;
}

void TerrainStreamer::evict() {
    if (cachedBytes_ <= settings_.cacheBytes) {
        return;
    }
    std::vector<std::pair<std::uint64_t, ChunkId>> candidates;
    candidates.reserve(cache_.size());
    for (const auto& [id, entry] : cache_) {
        if (entry.lastUsed != frame_) {  // Never drop chunks used this frame.
            candidates.emplace_back(entry.lastUsed, id);
        }
    }
    std::ranges::sort(candidates);
    for (const auto& [lastUsed, id] : candidates) {
        if (cachedBytes_ <= settings_.cacheBytes) {
            break;
        }
        const auto it = cache_.find(id);
        if (it == cache_.end() || !it->second.mesh) {
            continue;
        }
        cachedBytes_ -= it->second.mesh->byteSize();
        cache_.erase(it);
    }
}

StreamerStats TerrainStreamer::stats() const {
    return {cache_.size(), cachedBytes_, inFlight_.size(), generated_};
}

}  // namespace ds::terrain
