// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/chunk_selection.hpp"

#include <algorithm>
#include <cmath>

namespace ds::terrain {
namespace {

struct Context {
    const ChunkLayout& layout;
    const std::optional<Extent>& extent;
    double x;
    double y;
    double z;
    const SelectionSettings& settings;
    const ChunkReadiness& ready;
    ChunkSelection& out;
};

double horizontalDistance(const ChunkBounds& b, double x, double z) noexcept {
    const double dx = std::max({b.minX - x, 0.0, x - b.maxX});
    const double dz = std::max({b.minZ - z, 0.0, z - b.maxZ});
    return std::hypot(dx, dz);
}

double distanceTo(const Context& c, const ChunkId& id,
                  std::optional<std::pair<float, float>> range) {
    const double horizontal = horizontalDistance(chunkBounds(c.layout, id), c.x, c.z);
    double vertical = 0.0;
    if (range) {
        vertical = std::max({static_cast<double>(range->first) - c.y, 0.0,
                             c.y - static_cast<double>(range->second)});
    } else {
        vertical = std::abs(c.y);  // Unknown heights: assume terrain near height 0.
    }
    return std::hypot(horizontal, vertical);
}

bool inRange(const Context& c, const ChunkId& id) {
    return chunkInExtent(c.layout, id, c.extent) &&
           horizontalDistance(chunkBounds(c.layout, id), c.x, c.z) <= c.settings.viewRadiusM;
}

void visit(const Context& c, const ChunkId& id) {
    const auto range = c.ready(id);
    const double distance = distanceTo(c, id, range);
    const double size = c.layout.size(id.lod);
    if (!range) {
        c.out.wanted.emplace_back(id, distance / size + id.lod * 0.01);
        return;
    }
    const bool split = id.lod > 0 && distance < c.settings.splitFactor * size;
    if (!split) {
        c.out.render.push_back(id);
        return;
    }
    bool allReady = true;
    std::vector<ChunkId> children;
    for (int i = 0; i < 4; ++i) {
        const ChunkId child = id.child(i);
        if (!inRange(c, child)) {
            continue;
        }
        children.push_back(child);
        if (!c.ready(child)) {
            allReady = false;
            c.out.wanted.emplace_back(
                child, distanceTo(c, child, std::nullopt) / c.layout.size(child.lod));
        }
    }
    if (!allReady || children.empty()) {
        c.out.render.push_back(id);
        return;
    }
    for (const ChunkId& child : children) {
        visit(c, child);
    }
}

}  // namespace

ChunkSelection selectChunks(const ChunkLayout& layout, const std::optional<Extent>& extent,
                            double x, double y, double z, const SelectionSettings& settings,
                            const ChunkReadiness& ready) {
    ChunkSelection out;
    const Context c{layout, extent, x, y, z, settings, ready, out};
    const int top = layout.lodCount - 1;
    const double rootSize = layout.size(top);
    const double r = settings.viewRadiusM;
    const auto first = [rootSize](double v) {
        return static_cast<std::int64_t>(std::floor(v / rootSize));
    };
    for (std::int64_t iz = first(z - r); iz <= first(z + r); ++iz) {
        for (std::int64_t ix = first(x - r); ix <= first(x + r); ++ix) {
            const ChunkId root{top, ix, iz};
            if (inRange(c, root)) {
                visit(c, root);
            }
        }
    }
    std::ranges::stable_sort(out.wanted, {}, &std::pair<ChunkId, double>::second);
    return out;
}

}  // namespace ds::terrain
