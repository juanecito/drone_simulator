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
    std::vector<std::pair<OrthoTileKey, std::shared_ptr<const OrthoTile>>> finished;
};

namespace {

// Empty-tile markers kept beyond the ones in use (they cost almost nothing).
constexpr std::size_t kMaxEmptyTiles = 50'000;

std::atomic<std::uint64_t> nextId{1};

}  // namespace

OrthoStreamer::OrthoStreamer(std::shared_ptr<const OrthophotoSource> source,
                             const ChunkLayout& layout, common::WorkerPool& pool,
                             const OrthoStreamerSettings& settings)
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
    std::vector<std::pair<OrthoTileKey, std::shared_ptr<const OrthoTile>>> finished;
    {
        const std::scoped_lock lock(shared_->mutex);
        finished.swap(shared_->finished);
    }
    for (auto& [key, tile] : finished) {
        inFlight_.erase(key);
        ++built_;
        if (tile) {
            cachedBytes_ += tile->byteSize();
        }
        cache_[key] = Entry{std::move(tile), frame_};
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
    for (const auto& [key, priority] : wanted_) {
        if (const auto it = cache_.find(key); it != cache_.end()) {
            it->second.lastUsed = frame_;
        } else if (!inFlight_.contains(key)) {
            missing.emplace_back(priority, key);
        }
    }
    std::ranges::sort(missing, {}, &std::pair<double, OrthoTileKey>::first);
    for (const auto& [priority, key] : missing) {
        if (inFlight_.size() >= settings_.maxInFlight) {
            break;
        }
        inFlight_.insert(key);
        pool_->submit(priority, [source = source_, key, shared = shared_] {
            if (shared->cancelled) {
                return;
            }
            auto tile = source->tile(key);
            const std::scoped_lock lock(shared->mutex);
            shared->finished.emplace_back(key, std::move(tile));
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
            return OrthoTileState::Empty;
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
        } else {
            ++s.emptyTiles;
        }
    }
    s.cachedBytes = cachedBytes_;
    s.inFlight = inFlight_.size();
    s.built = built_;
    return s;
}

}  // namespace ds::terrain
