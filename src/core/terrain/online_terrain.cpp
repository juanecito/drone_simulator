// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/online_terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <utility>

#include "ds/common/log.hpp"
#include "ds/common/sha256.hpp"

namespace ds::terrain {
namespace {

// Plausible elevations on Earth (FR-TER-O15): anything outside is a broken payload.
constexpr double kLowestPlausibleM = -12'000.0;
constexpr double kHighestPlausibleM = 9'000.0;
// Pixels fetched beyond a block or area border.
constexpr double kMarginPixels = 3.0;
// focus() recomputes the wanted blocks after this move or time.
constexpr double kRefocusM = 250.0;
constexpr auto kRefocusTime = std::chrono::seconds(1);

std::string label(const net::ProviderConfig& provider, const net::DatasetConfig& dataset) {
    return std::format("{} {}", provider.name, dataset.id);
}

TerrainResult<void> checkHeights(const SourceMetadata& meta, std::string_view what) {
    if (meta.minHeightM < kLowestPlausibleM || meta.maxHeightM > kHighestPlausibleM) {
        return terrainError(TerrainErrorCode::CorruptData,
                            std::format("{}: elevations {:.0f} … {:.0f} m are not plausible", what,
                                        meta.minHeightM, meta.maxHeightM));
    }
    return {};
}

std::string attributionOf(const net::ProviderConfig& provider, const net::DatasetConfig& dataset) {
    if (dataset.attribution.empty()) {
        return provider.attribution;
    }
    if (provider.attribution.empty()) {
        return dataset.attribution;
    }
    return dataset.attribution + " · " + provider.attribution;
}

OnlineMetadata metadataOf(const OnlineTerrainRequest& request, const net::DatasetConfig& dataset) {
    OnlineMetadata m;
    m.providerKey = request.provider.key;
    m.providerName = request.provider.name;
    m.protocol = request.provider.protocol;
    m.datasetId = dataset.id;
    m.datasetName = dataset.name;
    m.resolutionM = dataset.resolutionM;
    m.verticalDatum = dataset.verticalDatum;
    m.ellipsoidal = dataset.ellipsoidal;
    m.coverage = dataset.coverage;
    m.attribution = attributionOf(request.provider, dataset);
    m.streaming = !request.extent.has_value();
    return m;
}

RasterOptions rasterOptions(const OnlineTerrainRequest& request,
                            const net::DatasetConfig& dataset) {
    RasterOptions o;
    o.origin = request.origin;
    o.originAltitudeM = request.originAltitudeM;
    o.extent = request.extent;
    o.edge = request.edge;
    o.verticalDatum = dataset.verticalDatum;
    o.processing = request.processing;
    return o;
}

}  // namespace

// --- Areas -------------------------------------------------------------------------------------

TerrainResult<const net::DatasetConfig*> onlineDataset(const OnlineTerrainRequest& request) {
    const net::ProviderConfig& p = request.provider;
    if (p.kind != "elevation" || p.protocol != "rest_bbox") {
        return terrainError(TerrainErrorCode::ProviderError,
                            std::format("{} is not an elevation provider with bounding-box "
                                        "requests",
                                        p.name));
    }
    const std::string& id = request.dataset.empty() ? p.defaultDataset : request.dataset;
    const net::DatasetConfig* dataset = p.dataset(id);
    if (dataset == nullptr) {
        return terrainError(TerrainErrorCode::ProviderError,
                            std::format("{} has no dataset \"{}\"", p.name, id));
    }
    return dataset;
}

net::GeoRect blockFetchRect(const net::BlockKey& key, const net::ProviderConfig& provider,
                            const net::DatasetConfig& dataset,
                            const HeightProcessing& processing) noexcept {
    // Filters look this far, plus a filter cell and the cubic support (filters always reach
    // at least one cell).
    const double filterM =
        processing.filtered() ? 1.25 * processing.reachM() + 2.0 * dataset.resolutionM : 0.0;
    return net::blockRect(key, provider.blockSizeKm)
        .expandedKm((kMarginPixels * dataset.resolutionM + filterM) / 1000.0);
}

TerrainResult<net::GeoRect> onlineRequiredArea(const OnlineTerrainRequest& request) {
    auto dataset = onlineDataset(request);
    if (!dataset) {
        return std::unexpected(dataset.error());
    }
    const net::DatasetConfig& d = **dataset;
    const geo::LatLon& o = request.origin;
    if (!(std::abs(o.latitudeDeg) < 89.0) || !(std::abs(o.longitudeDeg) <= 180.0)) {
        return terrainError(TerrainErrorCode::InvalidSpec, "the origin is not a valid position");
    }
    net::GeoRect area;
    if (request.extent) {
        const double marginKm = (kOnlineMarginM + kMarginPixels * d.resolutionM) / 1000.0;
        area = net::GeoRect::around(o.latitudeDeg, o.longitudeDeg,
                                    request.extent->widthM / 1000.0 + 2.0 * marginKm,
                                    request.extent->lengthM / 1000.0 + 2.0 * marginKm);
        if (d.coverage && !d.coverage->contains(area)) {
            return terrainError(TerrainErrorCode::NotCovered,
                                std::format("the terrain area is outside the coverage of {} ({})",
                                            d.name, request.provider.name));
        }
    } else {
        const net::BlockKey key =
            net::blockAt(o.latitudeDeg, o.longitudeDeg, request.provider.blockSizeKm);
        area = blockFetchRect(key, request.provider, d, request.processing);
        const net::GeoRect point{o.longitudeDeg - 1e-6, o.latitudeDeg - 1e-6, o.longitudeDeg + 1e-6,
                                 o.latitudeDeg + 1e-6};
        if (d.coverage && !d.coverage->contains(point)) {
            return terrainError(TerrainErrorCode::NotCovered,
                                std::format("the origin is outside the coverage of {} ({})", d.name,
                                            request.provider.name));
        }
    }
    if (!area.valid()) {
        return terrainError(TerrainErrorCode::InvalidSpec,
                            "the terrain area crosses a pole or the antimeridian");
    }
    return area;
}

// --- Bounded and streaming terrain -------------------------------------------------------------

TerrainResult<OpenedOnlineTerrain> openOnlineTerrain(net::DataClient& client,
                                                     const OnlineTerrainRequest& request,
                                                     const OnlineStreamOptions& streamOptions) {
    auto dataset = onlineDataset(request);
    if (!dataset) {
        return std::unexpected(dataset.error());
    }
    const net::DatasetConfig& d = **dataset;
    auto area = onlineRequiredArea(request);
    if (!area) {
        return std::unexpected(area.error());
    }
    OpenedOnlineTerrain out;
    out.online = metadataOf(request, d);
    out.online.area = *area;

    if (!request.extent) {
        auto stream = OnlineStreamSource::open(client, request, streamOptions);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        out.data = (*stream)->originMetadata();
        const OnlineStreamStatus status = (*stream)->status();
        out.online.pieces = status.loadedBlocks;
        out.online.bytes = status.bytes;
        out.source = *stream;
        out.stream = std::move(*stream);
        return out;
    }

    auto plan = net::planExtentRequests(*area, request.provider, d);
    if (!plan) {
        return terrainError(TerrainErrorCode::ProviderError, plan.error().message);
    }
    std::vector<std::string> bodies;
    std::string hashes = request.provider.key + '\n' + d.id + '\n';
    int missing = 0;
    for (const net::GeoRect& piece : *plan) {
        auto cached = client.cached(request.provider, net::BBoxRequest{d.id, piece});
        if (!cached) {
            return terrainError(TerrainErrorCode::ProviderError,
                                "elevation cache: " + cached.error().message);
        }
        if (!*cached) {
            ++missing;
            continue;
        }
        hashes += (*cached)->contentHash + '\n';
        out.online.bytes += (*cached)->body.size();
        bodies.push_back(std::move((*cached)->body));
    }
    if (missing > 0) {
        return terrainError(
            TerrainErrorCode::NotCached,
            std::format("the {:.1f} × {:.1f} km area of {} {} is not downloaded yet ({} of {} "
                        "request(s) missing)",
                        request.extent->widthM / 1000.0, request.extent->lengthM / 1000.0,
                        request.provider.name, d.id, missing, plan->size()));
    }
    auto raster =
        RasterSource::openMemory(bodies, rasterOptions(request, d), label(request.provider, d));
    if (!raster) {
        return std::unexpected(raster.error());
    }
    if (auto ok = checkHeights((*raster)->metadata(), label(request.provider, d)); !ok) {
        return std::unexpected(ok.error());
    }
    out.data = (*raster)->metadata();
    out.online.pieces = static_cast<int>(plan->size());
    if (request.processing != HeightProcessing{}) {
        hashes += "processing " + describeProcessing(request.processing);  // Other heights.
    }
    out.dataHash = common::sha256Hex(hashes);
    out.source = std::move(*raster);
    return out;
}

// --- OnlineStreamSource ------------------------------------------------------------------------

OnlineStreamSource::OnlineStreamSource(Private, net::DataClient& client,
                                       OnlineTerrainRequest request,
                                       const net::DatasetConfig& dataset,
                                       const OnlineStreamOptions& options)
    : client_(&client), request_(std::move(request)), dataset_(dataset), options_(options) {}

TerrainResult<std::shared_ptr<OnlineStreamSource>> OnlineStreamSource::open(
    net::DataClient& client, const OnlineTerrainRequest& request,
    const OnlineStreamOptions& options) {
    auto dataset = onlineDataset(request);
    if (!dataset) {
        return std::unexpected(dataset.error());
    }
    OnlineTerrainRequest originBlock = request;
    originBlock.extent.reset();
    auto area = onlineRequiredArea(originBlock);
    if (!area) {
        return std::unexpected(area.error());
    }
    const net::DatasetConfig& d = **dataset;
    auto cached = client.cached(request.provider, net::BBoxRequest{d.id, *area});
    if (!cached) {
        return terrainError(TerrainErrorCode::ProviderError,
                            "elevation cache: " + cached.error().message);
    }
    if (!*cached) {
        return terrainError(TerrainErrorCode::NotCached,
                            std::format("the {} {} data around the origin is not downloaded yet",
                                        request.provider.name, d.id));
    }
    RasterOptions o = rasterOptions(request, d);
    o.extent.reset();
    o.unbounded = true;
    const std::array<std::string, 1> body{std::move((*cached)->body)};
    auto raster = RasterSource::openMemory(body, o, label(request.provider, d));
    if (!raster) {
        return std::unexpected(raster.error());
    }
    if (auto ok = checkHeights((*raster)->metadata(), label(request.provider, d)); !ok) {
        return std::unexpected(ok.error());
    }
    auto source = std::make_shared<OnlineStreamSource>(Private{}, client, request, d, options);
    const SourceMetadata& meta = (*raster)->metadata();
    source->originMeta_ = meta;
    source->originAltitude_ = meta.origin.heightM;
    source->frame_ = geo::LocalFrame(meta.origin);
    source->fallback_ = meta.minHeightM - meta.origin.heightM;
    source->request_.originAltitudeM = meta.origin.heightM;  // Same for every block.
    const net::BlockKey key = net::blockAt(request.origin.latitudeDeg, request.origin.longitudeDeg,
                                           request.provider.blockSizeKm);
    source->blocks_.emplace(key, Loaded{std::move(*raster), body[0].size()});
    return source;
}

OnlineStreamSource::~OnlineStreamSource() {
    std::vector<net::RequestId> ids;
    {
        const std::unique_lock lock(mutex_);
        for (const auto& [key, id] : pending_) {
            if (id != 0) {
                ids.push_back(id);
            }
        }
    }
    for (const net::RequestId id : ids) {
        client_->cancel(id);
    }
}

net::BlockKey OnlineStreamSource::keyAt(double x, double z) const noexcept {
    const geo::Geodetic g = frame_.toGeodetic(geo::Enu{x, -z, 0.0});
    return net::blockAt(g.latitudeDeg, g.longitudeDeg, request_.provider.blockSizeKm);
}

const RasterSource* OnlineStreamSource::blockAt(const net::BlockKey& key) const {
    // Blocks are never removed, and unordered_map elements do not move, so the pointer stays
    // valid after the lock is released.
    const std::shared_lock lock(mutex_);
    const auto it = blocks_.find(key);
    return it == blocks_.end() ? nullptr : it->second.raster.get();
}

double OnlineStreamSource::height(double x, double z) const {
    const RasterSource* raster = blockAt(keyAt(x, z));
    return raster != nullptr ? raster->height(x, z) : fallback_;
}

void OnlineStreamSource::sampleGrid(double x0, double z0, double step, int cols, int rows,
                                    std::span<double> out) const {
    const std::size_t n = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    const double x1 = x0 + (cols - 1) * step;
    const double z1 = z0 + (rows - 1) * step;
    const net::BlockKey key = keyAt(x0, z0);
    if (keyAt(x1, z0) == key && keyAt(x0, z1) == key && keyAt(x1, z1) == key) {
        // The whole grid lies in one block: one windowed read (and reduced resolution for
        // coarse grids).
        if (const RasterSource* raster = blockAt(key)) {
            raster->sampleGrid(x0, z0, step, cols, rows, out);
        } else {
            std::ranges::fill(out.first(n), fallback_);
        }
        return;
    }
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            out[(static_cast<std::size_t>(row) * static_cast<std::size_t>(cols)) +
                static_cast<std::size_t>(col)] = height(x0 + (col * step), z0 + (row * step));
        }
    }
}

std::vector<ChangedArea> OnlineStreamSource::changesSince(std::uint64_t revision) const {
    const std::shared_lock lock(mutex_);
    std::vector<ChangedArea> out;
    for (const ChangedArea& c : changes_) {
        if (c.revision > revision) {
            out.push_back(c);
        }
    }
    return out;
}

std::string OnlineStreamSource::describe() const {
    return std::format("{} (blocks of {:.0f} km streamed around the drone)",
                       label(request_.provider, dataset_), request_.provider.blockSizeKm);
}

ChangedArea OnlineStreamSource::worldRect(const net::BlockKey& key) const noexcept {
    const net::GeoRect r = net::blockRect(key, request_.provider.blockSizeKm);
    ChangedArea a;
    a.minX = a.minZ = std::numeric_limits<double>::infinity();
    a.maxX = a.maxZ = -std::numeric_limits<double>::infinity();
    // Corners and edge midpoints: the block is slightly curved in the local frame.
    for (const double lat : {r.south, 0.5 * (r.south + r.north), r.north}) {
        for (const double lon : {r.west, 0.5 * (r.west + r.east), r.east}) {
            const geo::Enu e = frame_.toEnu(geo::Geodetic{lat, lon, originAltitude_});
            a.minX = std::min(a.minX, e.east);
            a.maxX = std::max(a.maxX, e.east);
            a.minZ = std::min(a.minZ, -e.north);
            a.maxZ = std::max(a.maxZ, -e.north);
        }
    }
    const double pad = kMarginPixels * dataset_.resolutionM;
    a.minX -= pad;
    a.minZ -= pad;
    a.maxX += pad;
    a.maxZ += pad;
    return a;
}

std::shared_ptr<const RasterSource> OnlineStreamSource::decode(const net::BlockKey& key,
                                                               const std::string& body,
                                                               std::string& error) const {
    RasterOptions o = rasterOptions(request_, dataset_);
    o.extent.reset();
    o.unbounded = true;
    o.originAltitudeM = originAltitude_;
    const std::array<std::string, 1> payload{body};
    auto raster = RasterSource::openMemory(
        payload, o,
        std::format("{} block {}/{}", label(request_.provider, dataset_), key.row, key.col));
    if (!raster) {
        error = raster.error().message;
        return nullptr;
    }
    if (auto ok = checkHeights((*raster)->metadata(), label(request_.provider, dataset_)); !ok) {
        error = ok.error().message;
        return nullptr;
    }
    return std::move(*raster);
}

void OnlineStreamSource::arrived(const net::BlockKey& key, net::NetResult<net::Payload> result) {
    std::string error;
    std::shared_ptr<const RasterSource> raster;
    std::uint64_t bytes = 0;
    if (result) {
        bytes = result->body.size();
        raster = decode(key, result->body, error);
    } else if (result.error().code == net::NetErrorCode::Cancelled) {
        const std::unique_lock lock(mutex_);
        pending_.erase(key);
        return;
    } else {
        error = result.error().message;
    }
    const std::unique_lock lock(mutex_);
    pending_.erase(key);
    if (!raster) {
        failed_[key] = Clock::now();
        lastError_ = error;
        log::warn("Online terrain: block {}/{}: {}", key.row, key.col, error);
        return;
    }
    blocks_.emplace(key, Loaded{std::move(raster), bytes});
    ChangedArea change = worldRect(key);
    change.revision = revision_.load() + 1;
    changes_.push_back(change);
    lastError_.clear();
    revision_.store(change.revision);  // After the block is visible (see TerrainStreamer).
}

void OnlineStreamSource::focus(double x, double z) {
    const Clock::time_point now = Clock::now();
    if (lastFocus_ && std::hypot(x - lastFocus_->first, z - lastFocus_->second) < kRefocusM &&
        now - lastFocusTime_ < kRefocusTime) {
        return;
    }
    lastFocus_ = std::pair{x, z};
    lastFocusTime_ = now;
    const geo::Geodetic g = frame_.toGeodetic(geo::Enu{x, -z, 0.0});
    const double blockKm = request_.provider.blockSizeKm;
    const double radiusKm = options_.radiusKm > 0.0 ? options_.radiusKm : blockKm;
    const std::vector<net::Block> wanted =
        net::blocksAround(g.latitudeDeg, g.longitudeDeg, radiusKm, blockKm);
    std::vector<net::Block> fetch;
    {
        const std::unique_lock lock(mutex_);
        for (const net::Block& b : wanted) {
            if (static_cast<int>(pending_.size()) >= options_.maxPending) {
                break;
            }
            if (blocks_.contains(b.key) || pending_.contains(b.key)) {
                continue;
            }
            if (const auto f = failed_.find(b.key);
                f != failed_.end() && now - f->second < options_.retryAfter) {
                continue;
            }
            if (dataset_.coverage && !dataset_.coverage->intersects(b.rect)) {
                continue;
            }
            failed_.erase(b.key);
            pending_.emplace(b.key, 0);
            fetch.push_back(b);
        }
    }
    for (const net::Block& b : fetch) {
        std::weak_ptr<OnlineStreamSource> weak = weak_from_this();
        const net::RequestId id = client_->fetch(
            request_.provider,
            net::BBoxRequest{dataset_.id, blockFetchRect(b.key, request_.provider, dataset_,
                                                         request_.processing)},
            b.distanceKm, [weak, key = b.key](net::NetResult<net::Payload> r) {
                if (auto self = weak.lock()) {
                    self->arrived(key, std::move(r));
                }
            });
        const std::unique_lock lock(mutex_);
        if (const auto it = pending_.find(b.key); it != pending_.end()) {
            it->second = id;  // Still running (not answered from the cache at once).
        }
    }
}

OnlineStreamStatus OnlineStreamSource::status() const {
    const std::shared_lock lock(mutex_);
    OnlineStreamStatus s;
    s.loadedBlocks = static_cast<int>(blocks_.size());
    s.pendingBlocks = static_cast<int>(pending_.size());
    s.failedBlocks = static_cast<int>(failed_.size());
    s.lastError = lastError_;
    s.minHeightM = std::numeric_limits<double>::infinity();
    s.maxHeightM = -s.minHeightM;
    for (const auto& [key, block] : blocks_) {
        s.bytes += block.bytes;
        s.minHeightM = std::min(s.minHeightM, block.raster->metadata().minHeightM);
        s.maxHeightM = std::max(s.maxHeightM, block.raster->metadata().maxHeightM);
    }
    return s;
}

}  // namespace ds::terrain
