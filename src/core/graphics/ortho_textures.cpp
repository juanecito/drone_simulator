// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ortho_textures.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ds/common/log.hpp"

namespace ds::graphics::detail {
namespace {

// Not in the GL 4.5 core header: EXT/ARB_texture_filter_anisotropic (core in 4.6), which
// every desktop driver offers. Set only if querying the limit raises no error.
constexpr GLenum kMaxAnisotropy = 0x84FF;
constexpr GLenum kTextureMaxAnisotropy = 0x84FE;
constexpr int kFirstCapacity = 32;

}  // namespace

void OrthoTextures::setBudget(std::size_t bytes) {
    if (bytes != budget_) {
        budget_ = bytes;
        clear();
    }
}

void OrthoTextures::clear() {
    texture_ = gl::Texture();
    resident_.clear();
    free_.clear();
    capacity_ = 0;
    maxLayers_ = 0;
    tileSizePx_ = 0;
}

void OrthoTextures::beginFrame(const terrain::OrthoStreamer* streamer, std::uint64_t frame) {
    const std::uint64_t id = streamer != nullptr ? streamer->id() : 0;
    if (id != streamerId_) {
        // New terrain or imagery: the old tiles are of no use (the memory is kept).
        resident_.clear();
        free_.clear();
        for (int layer = capacity_ - 1; layer >= 0; --layer) {
            free_.push_back(layer);
        }
        streamerId_ = id;
    }
    streamer_ = streamer;
    frame_ = frame;
    uploads_ = 0;
}

int OrthoTextures::residentLayer(const terrain::OrthoTileKey& key) {
    const auto it = resident_.find(key);
    if (it == resident_.end()) {
        return -1;
    }
    it->second.lastUsed = frame_;
    return it->second.layer;
}

int OrthoTextures::layerLimit(int tileSizePx) {
    if (maxLayers_ == 0 || tileSizePx != tileSizePx_) {
        GLint limit = 256;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &limit);
        const std::size_t perLayer = terrain::OrthoTile::bytesFor(tileSizePx);
        maxLayers_ = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(std::max(limit, 1)), budget_ / perLayer));
    }
    return maxLayers_;
}

void OrthoTextures::prepare(std::span<const terrain::ChunkId> chunks) {
    bias_ = 0;
    if (streamer_ == nullptr || !streamer_->source()) {
        return;
    }
    // Distinct tiles with (possible) imagery at each raise of the level, until they fit with
    // room to spare for the tiles of the next frames.
    const int limit = layerLimit(streamer_->source()->options().tilePx);
    std::vector<terrain::OrthoTileKey> keys;
    for (const terrain::ChunkId& id : chunks) {
        for (const terrain::OrthoTileKey& key : terrain::OrthoStreamer::tilesOf(id)) {
            if (streamer_->find(key) != terrain::OrthoTileState::Empty) {
                keys.push_back(key);
            }
        }
    }
    for (; bias_ < kMaxFallbackLevels; ++bias_) {
        std::ranges::sort(keys);
        const auto [first, last] = std::ranges::unique(keys);
        keys.erase(first, last);
        if (4 * keys.size() <= 3 * static_cast<std::size_t>(limit)) {
            break;
        }
        for (terrain::OrthoTileKey& key : keys) {
            key = key.parent();
        }
    }
}

bool OrthoTextures::grow(int tileSizePx) {
    layerLimit(tileSizePx);
    tileSizePx_ = tileSizePx;
    if (capacity_ >= maxLayers_) {
        return false;
    }
    const int capacity = std::min(capacity_ == 0 ? kFirstCapacity : 2 * capacity_, maxLayers_);
    int levels = 1;
    for (int s = tileSizePx; s > 1; s >>= 1) {
        ++levels;
    }
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &id);
    gl::Texture texture(id);
    glTextureStorage3D(id, levels, GL_SRGB8_ALPHA8, tileSizePx, tileSizePx, capacity);
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (glGetError() != GL_NO_ERROR) {
    }
    GLfloat anisotropy = 1.0F;
    glGetFloatv(kMaxAnisotropy, &anisotropy);
    if (glGetError() == GL_NO_ERROR && anisotropy > 1.0F) {
        glTextureParameterf(id, kTextureMaxAnisotropy, std::min(anisotropy, 8.0F));
    }
    if (capacity_ > 0) {
        for (int level = 0; level < levels; ++level) {
            const int s = std::max(tileSizePx >> level, 1);
            glCopyImageSubData(texture_.id(), GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, id,
                               GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, s, s, capacity_);
        }
    }
    for (int layer = capacity - 1; layer >= capacity_; --layer) {
        free_.push_back(layer);
    }
    texture_ = std::move(texture);
    log::debug("Orthophoto texture array: {} layers ({:.0f} MB)", capacity,
               static_cast<double>(terrain::OrthoTile::bytesFor(tileSizePx)) * capacity / 1.0e6);
    capacity_ = capacity;
    return true;
}

int OrthoTextures::freeLayer() {
    if (free_.empty()) {
        // The least recently used tile that this frame does not need.
        auto oldest = resident_.end();
        std::uint64_t used = std::numeric_limits<std::uint64_t>::max();
        for (auto it = resident_.begin(); it != resident_.end(); ++it) {
            if (it->second.lastUsed < frame_ && it->second.lastUsed < used) {
                used = it->second.lastUsed;
                oldest = it;
            }
        }
        if (oldest == resident_.end()) {
            return -1;  // Every layer is in use this frame: ancestors stand in.
        }
        free_.push_back(oldest->second.layer);
        resident_.erase(oldest);
    }
    const int layer = free_.back();
    free_.pop_back();
    return layer;
}

int OrthoTextures::upload(const terrain::OrthoTileKey& key, const terrain::OrthoTile& tile) {
    if (uploads_ >= kUploadsPerFrame) {
        return -1;
    }
    if (tile.sizePx != tileSizePx_) {
        clear();  // Tiles of another size (never mixed in one streamer).
    }
    if (capacity_ == 0 || (free_.empty() && capacity_ < maxLayers_)) {
        static_cast<void>(grow(tile.sizePx));  // Otherwise a used layer is taken over.
    }
    const int layer = freeLayer();
    if (layer < 0) {
        return -1;
    }
    for (int mip = 0; mip < tile.mipCount(); ++mip) {
        const int s = std::max(tile.sizePx >> mip, 1);
        glTextureSubImage3D(texture_.id(), mip, 0, 0, layer, s, s, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            tile.rgba.data() + tile.mipOffset(mip));
    }
    resident_[key] = Resident{layer, frame_};
    ++uploads_;
    return layer;
}

bool OrthoTextures::slotsFor(const terrain::ChunkId& id, OrthoSlots& slots) {
    bool any = false;
    const auto keys = terrain::OrthoStreamer::tilesOf(id);
    for (std::size_t s = 0; s < keys.size(); ++s) {
        slots[s] = glm::vec4(-1.0F, 1.0F, 0.0F, 0.0F);
        if (streamer_ == nullptr) {
            continue;
        }
        const terrain::OrthoTileKey key = keys[s];
        terrain::OrthoTileKey k = key;
        for (int m = 0; m < bias_; ++m) {
            k = k.parent();  // Over the budget: coarser tiles for every chunk.
        }
        for (int m = bias_; m <= kMaxFallbackLevels; ++m, k = k.parent()) {
            std::shared_ptr<const terrain::OrthoTile> tile;
            const terrain::OrthoTileState state = streamer_->find(k, &tile);
            if (state == terrain::OrthoTileState::Empty) {
                break;  // No imagery here at all.
            }
            int layer = residentLayer(k);
            if (layer < 0 && state == terrain::OrthoTileState::Ready) {
                layer = upload(k, *tile);
            }
            if (layer < 0) {
                continue;  // Not on the GPU yet: try the parent.
            }
            const float scale = std::ldexp(1.0F, -m);
            slots[s] = glm::vec4(static_cast<float>(layer), scale,
                                 static_cast<float>(key.ix - (k.ix << m)) * scale,
                                 static_cast<float>(key.iz - (k.iz << m)) * scale);
            any = true;
            break;
        }
    }
    return any;
}

OrthoTextureStats OrthoTextures::stats() const noexcept {
    OrthoTextureStats s;
    s.residentTiles = resident_.size();
    s.capacity = static_cast<std::size_t>(capacity_);
    s.bytes = tileSizePx_ > 0
                  ? terrain::OrthoTile::bytesFor(tileSizePx_) * static_cast<std::size_t>(capacity_)
                  : 0;
    s.uploads = uploads_;
    return s;
}

}  // namespace ds::graphics::detail
