// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/ortho_streamer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

#include "ds/common/log.hpp"

namespace ds::terrain {

// State shared with the jobs; outlives the streamer while jobs still hold it.
struct OrthoStreamer::Shared {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<Finished> finished;
};

namespace {

// Empty-tile markers kept beyond the ones in use (they cost almost nothing).
constexpr std::size_t kMaxEmptyTiles = 50'000;

std::atomic<std::uint64_t> nextId{1};

}  // namespace

OrthoStreamer::OrthoStreamer(std::shared_ptr<const OrthoImagery> source, const ChunkLayout& layout,
                             common::WorkerPool& pool, const OrthoStreamerSettings& settings)
    : source_(std::move(source)),
      layout_(layout),
      pool_(&pool),
      settings_(settings),
      shared_(std::make_shared<Shared>()),
      id_(nextId.fetch_add(1)) {
    if (settings_.maxInFlight == 0) {
        settings_.maxInFlight = 2 * pool.threadCount();
    }
    if (source_ &&
        std::abs(layout_.chunkSizeM - kOrthoTilesPerChunk * source_->options().tileSizeM) > 1e-9) {
        log::warn("Orthophoto tiles do not match the terrain chunks; imagery disabled");
        source_.reset();
    }
}

OrthoStreamer::~OrthoStreamer() { shared_->cancelled = true; }

std::array<OrthoTileKey, kOrthoTilesPerChunk * kOrthoTilesPerChunk> OrthoStreamer::tilesOf(
    const ChunkId& id) noexcept {
    std::array<OrthoTileKey, kOrthoTilesPerChunk * kOrthoTilesPerChunk> keys{};
    for (int b = 0; b < kOrthoTilesPerChunk; ++b) {
        for (int a = 0; a < kOrthoTilesPerChunk; ++a) {
            keys[static_cast<std::size_t>(b * kOrthoTilesPerChunk + a)] = OrthoTileKey{
                id.lod, id.ix * kOrthoTilesPerChunk + a, id.iz * kOrthoTilesPerChunk + b};
        }
    }
    return keys;
}

void OrthoStreamer::collect() {
    std::vector<Finished> finished;
    {
        const std::scoped_lock lock(shared_->mutex);
        finished.swap(shared_->finished);
    }
    const auto now = std::chrono::steady_clock::now();
    for (Finished& f : finished) {
        inFlight_.erase(f.key);
        ++built_;
        Entry& entry = cache_[f.key];
        if (entry.tile) {
            cachedBytes_ -= entry.tile->byteSize();
        }
        // An incomplete rebuild that came back without an image keeps the previous one.
        if (f.build.tile || f.build.complete) {
            entry.tile = std::move(f.build.tile);
        }
        if (entry.tile) {
            cachedBytes_ += entry.tile->byteSize();
        }
        entry.lastUsed = frame_;
        entry.complete = f.build.complete;
        entry.revision = f.revision;
        entry.builtAt = now;
    }
}

void OrthoStreamer::want(const OrthoTileKey& key, double priority) {
    if (!source_->mayCover(key)) {
        return;  // Known empty without a job (find() reports it).
    }
    const auto [it, inserted] = wanted_.try_emplace(key, priority);
    if (!inserted) {
        it->second = std::min(it->second, priority);
    }
}

void OrthoStreamer::update(std::span<const std::shared_ptr<const ChunkMesh>> chunks, double x,
                           double z) {
    ++frame_;
    if (!source_) {
        return;
    }
    collect();
    wanted_.clear();
    for (const auto& mesh : chunks) {
        if (mesh == nullptr) {
            continue;
        }
        for (const OrthoTileKey& key : tilesOf(mesh->id)) {
            const double side = std::ldexp(source_->options().tileSizeM, key.level);
            const double cx = (static_cast<double>(key.ix) + 0.5) * side;
            const double cz = (static_cast<double>(key.iz) + 0.5) * side;
            // Same scale as the chunk jobs (distance / chunk size), queued just after them.
            const double priority = 0.5 + std::hypot(cx - x, cz - z) / (kOrthoTilesPerChunk * side);
            want(key, priority);
            // Coarser stand-ins, queued first (few: many tiles share them).
            OrthoTileKey up = key;
            for (int m = 1; m <= kOrthoAncestorLevels; ++m) {
                up = up.parent();
                want(up, priority - 0.25 * m);
            }
        }
    }
    std::vector<std::pair<double, OrthoTileKey>> missing;
    const std::uint64_t revision = source_->revision();
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [key, priority] : wanted_) {
        if (inFlight_.contains(key)) {
            continue;
        }
        if (const auto it = cache_.find(key); it != cache_.end()) {
            Entry& e = it->second;
            e.lastUsed = frame_;
            // Online data arrived (or a failed download may be retried): build it again.
            if (!e.complete &&
                (e.revision != revision || now - e.builtAt >= settings_.retryIncomplete)) {
                missing.emplace_back(priority, key);
            }
        } else {
            missing.emplace_back(priority, key);
        }
    }
    std::ranges::sort(missing, {}, &std::pair<double, OrthoTileKey>::first);
    for (const auto& [priority, key] : missing) {
        if (inFlight_.size() >= settings_.maxInFlight) {
            break;
        }
        inFlight_.insert(key);
        pool_->submit(priority, [source = source_, key, priority, shared = shared_] {
            if (shared->cancelled) {
                return;
            }
            // The revision before building: data arriving meanwhile triggers another build.
            const std::uint64_t before = source->revision();
            OrthoBuild build = source->build(key, priority);
            const std::scoped_lock lock(shared->mutex);
            shared->finished.push_back(Finished{key, std::move(build), before});
        });
    }
    evict();
}

void OrthoStreamer::evict() {
    std::size_t empties = 0;
    for (const auto& [key, entry] : cache_) {
        empties += entry.tile ? 0U : 1U;
    }
    if (cachedBytes_ <= settings_.cacheBytes && empties <= kMaxEmptyTiles) {
        return;
    }
    std::vector<std::pair<std::uint64_t, OrthoTileKey>> old;
    for (const auto& [key, entry] : cache_) {
        if (entry.lastUsed < frame_) {
            old.emplace_back(entry.lastUsed, key);
        }
    }
    std::ranges::sort(old, {}, &std::pair<std::uint64_t, OrthoTileKey>::first);
    for (const auto& [used, key] : old) {
        if (cachedBytes_ <= settings_.cacheBytes && empties <= kMaxEmptyTiles) {
            break;
        }
        const auto it = cache_.find(key);
        if (it == cache_.end()) {
            continue;  // Not reached: the keys come from the cache.
        }
        if (it->second.tile) {
            cachedBytes_ -= it->second.tile->byteSize();
        } else {
            --empties;
        }
        cache_.erase(it);
    }
}

OrthoTileState OrthoStreamer::find(const OrthoTileKey& key,
                                   std::shared_ptr<const OrthoTile>* tile) const {
    if (!source_ || !source_->mayCover(key)) {
        return OrthoTileState::Empty;
    }
    if (const auto it = cache_.find(key); it != cache_.end()) {
        if (!it->second.tile) {
            return it->second.complete ? OrthoTileState::Empty : OrthoTileState::Pending;
        }
        if (tile != nullptr) {
            *tile = it->second.tile;
        }
        return OrthoTileState::Ready;
    }
    return inFlight_.contains(key) ? OrthoTileState::Pending : OrthoTileState::Unknown;
}

OrthoStreamerStats OrthoStreamer::stats() const {
    OrthoStreamerStats s;
    for (const auto& [key, entry] : cache_) {
        if (entry.tile) {
            ++s.cachedTiles;
        } else if (entry.complete) {
            ++s.emptyTiles;
        }
        if (!entry.complete) {
            ++s.waitingTiles;
        }
    }
    s.cachedBytes = cachedBytes_;
    s.inFlight = inFlight_.size();
    s.built = built_;
    return s;
}

}  // namespace ds::terrain
