// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/streamer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

namespace ds::terrain {

// State shared with the jobs; outlives the streamer while jobs still hold it.
struct TerrainStreamer::Shared {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::pair<std::shared_ptr<const ChunkMesh>, std::uint64_t>> finished;
};

namespace {

// Changes kept to check meshes that were being built when they arrived.
constexpr std::size_t kMaxChanges = 512;

}  // namespace

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

bool TerrainStreamer::touches(const ChunkId& id, const ChangedArea& change) const noexcept {
    const ChunkBounds b = chunkBounds(layout_, id);
    const double apron = layout_.spacing(id.lod);  // Normals sample one step outside.
    return b.minX - apron <= change.maxX && b.maxX + apron >= change.minX &&
           b.minZ - apron <= change.maxZ && b.maxZ + apron >= change.minZ;
}

void TerrainStreamer::collect() {
    std::vector<std::pair<std::shared_ptr<const ChunkMesh>, std::uint64_t>> finished;
    {
        const std::scoped_lock lock(shared_->mutex);
        finished.swap(shared_->finished);
    }
    for (auto& [mesh, revision] : finished) {
        const ChunkId id = mesh->id;  // Read before the move below.
        inFlight_.erase(id);
        ++generated_;
        // Data that arrived while the mesh was being built makes it stale at once.
        const bool stale = std::ranges::any_of(changes_, [&](const ChangedArea& c) {
            return c.revision > revision && touches(id, c);
        });
        if (const auto old = cache_.find(id); old != cache_.end() && old->second.mesh) {
            cachedBytes_ -= old->second.mesh->byteSize();  // A rebuilt mesh replaces it.
        }
        cachedBytes_ += mesh->byteSize();
        cache_[id] = CacheEntry{std::move(mesh), frame_, revision, stale};
    }
}

void TerrainStreamer::applyChanges() {
    const std::uint64_t revision = source_->revision();
    if (revision == seenRevision_) {
        return;
    }
    for (const ChangedArea& change : source_->changesSince(seenRevision_)) {
        for (auto& [id, entry] : cache_) {
            if (!entry.stale && entry.revision < change.revision && touches(id, change)) {
                entry.stale = true;
            }
        }
        changes_.push_back(change);
    }
    if (changes_.size() > kMaxChanges) {
        changes_.erase(changes_.begin(), changes_.begin() + static_cast<std::ptrdiff_t>(
                                                                changes_.size() - kMaxChanges));
    }
    seenRevision_ = revision;
}

void TerrainStreamer::submit(const ChunkId& id, double priority) {
    pool_->submit(priority, [source = source_, layout = layout_, id, shared = shared_] {
        if (shared->cancelled) {
            return;
        }
        const std::uint64_t revision = source->revision();  // Before reading any height.
        auto mesh = std::make_shared<const ChunkMesh>(buildChunkMesh(*source, layout, id));
        const std::scoped_lock lock(shared->mutex);
        shared->finished.emplace_back(std::move(mesh), revision);
    });
}

std::vector<std::shared_ptr<const ChunkMesh>> TerrainStreamer::update(double x, double y,
                                                                      double z) {
    ++frame_;
    collect();
    applyChanges();

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
        submit(id, priority);
    }

    std::vector<std::shared_ptr<const ChunkMesh>> meshes;
    meshes.reserve(selection.render.size());
    for (const ChunkId& id : selection.render) {
        meshes.push_back(cache_.at(id).mesh);
    }
    // Stale meshes used this frame (drawn, or split parents whose height range guides the
    // selection) stay in use until their rebuild arrives, queued after the missing chunks.
    if (std::ranges::any_of(cache_, [](const auto& e) { return e.second.stale; })) {
        for (const auto& [id, entry] : cache_) {
            if (inFlight_.size() >= settings_.maxInFlight) {
                break;
            }
            if (!entry.stale || entry.lastUsed != frame_ || inFlight_.contains(id)) {
                continue;
            }
            inFlight_.insert(id);
            const ChunkBounds b = chunkBounds(layout_, id);
            const double dx = 0.5 * (b.minX + b.maxX) - x;
            const double dz = 0.5 * (b.minZ + b.maxZ) - z;
            submit(id, 1.0 + std::sqrt(dx * dx + dz * dz) / layout_.size(id.lod) + id.lod * 0.01);
        }
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
    const auto stale = static_cast<std::size_t>(
        std::ranges::count_if(cache_, [](const auto& e) { return e.second.stale; }));
    return {cache_.size(), cachedBytes_, inFlight_.size(), generated_, stale};
}

}  // namespace ds::terrain
