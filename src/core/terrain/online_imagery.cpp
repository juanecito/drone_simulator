// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/online_imagery.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "ds/common/log.hpp"

#include "gdal_support.hpp"
#include "ortho_tile_builder.hpp"
#include <cpl_vsi.h>
#include <gdal.h>

namespace ds::terrain {
namespace {

using detail::kFeatherPx;
using detail::kLattice;

constexpr std::array<const char*, 4> kTileDrivers{"JPEG", "PNG", "WEBP", nullptr};
constexpr int kBorderSamples = 64;     // Points per coverage edge for the world box.
constexpr double kUnbounded = 1.0e12;  // Coverage box of a worldwide dataset, metres.

// Web-Mercator position in "world units" (0 … 1 across, y growing south).
void toMercatorUnit(double latDeg, double lonDeg, double& u, double& v) noexcept {
    const double lat =
        std::clamp(latDeg, -geo::kWebMercatorMaxLatitudeDeg, geo::kWebMercatorMaxLatitudeDeg) *
        std::numbers::pi / 180.0;
    u = (lonDeg + 180.0) / 360.0;
    v = 0.5 - std::asinh(std::tan(lat)) / (2.0 * std::numbers::pi);
}

std::string protocolLabel(std::string_view protocol) {
    if (protocol == "xyz") {
        return "XYZ tiles";
    }
    if (protocol == "tms") {
        return "TMS tiles";
    }
    if (protocol == "wmts") {
        return "WMTS";
    }
    if (protocol == "wms") {
        return "WMS";
    }
    return std::string(protocol);
}

}  // namespace

std::size_t TileIdHash::operator()(const geo::TileId& t) const noexcept {
    std::size_t h = std::hash<std::int64_t>{}(t.x);
    h ^= std::hash<std::int64_t>{}(t.y) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    h ^= std::hash<int>{}(t.z) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    return h;
}

OnlineImagerySource::OnlineImagerySource(Private, net::DataClient& client,
                                         net::ProviderConfig provider, net::DatasetConfig dataset,
                                         const OnlineImageryOptions& options)
    : client_(&client),
      provider_(std::move(provider)),
      dataset_(std::move(dataset)),
      imageryOptions_(options) {}

OnlineImagerySource::~OnlineImagerySource() {
    std::vector<net::RequestId> ids;
    {
        const std::scoped_lock lock(mutex_);
        for (const auto& [tile, id] : pending_) {
            if (id != 0) {
                ids.push_back(id);
            }
        }
    }
    for (const net::RequestId id : ids) {
        client_->cancel(id);
    }
}

TerrainResult<std::shared_ptr<OnlineImagerySource>> OnlineImagerySource::open(
    net::DataClient& client, const OnlineImageryRequest& request, const geo::Geodetic& origin,
    const OnlineImageryOptions& options, const OrthoOptions& tiles) {
    const net::ProviderConfig& p = request.provider;
    if (!p.isImagery() || !net::isTileProtocol(p.protocol)) {
        return terrainError(TerrainErrorCode::ProviderError,
                            std::format("{} is not an imagery tile provider", p.name));
    }
    const std::string& id = request.dataset.empty() ? p.defaultDataset : request.dataset;
    const net::DatasetConfig* dataset = p.dataset(id);
    if (dataset == nullptr) {
        return terrainError(TerrainErrorCode::ProviderError,
                            std::format("{} has no imagery layer \"{}\"", p.name, id));
    }
    if (tiles.tilePx < 16 || tiles.tilePx > 1024 || (tiles.tilePx & (tiles.tilePx - 1)) != 0 ||
        !(tiles.tileSizeM > 0.0)) {
        return terrainError(TerrainErrorCode::InvalidSpec, "invalid orthophoto tile size");
    }
    if (const std::string bad = validateOrthoColor(request.color); !bad.empty()) {
        return terrainError(TerrainErrorCode::InvalidSpec, "imagery " + bad);
    }
    if (!(std::abs(origin.latitudeDeg) < geo::kWebMercatorMaxLatitudeDeg)) {
        return terrainError(TerrainErrorCode::InvalidSpec,
                            "online imagery needs an origin within ±85° latitude");
    }
    gdal::ensureInitialised();
    auto source = std::make_shared<OnlineImagerySource>(Private{}, client, p, *dataset, options);
    OnlineImagerySource& s = *source;
    s.options_ = tiles;
    s.options_.color = request.color;
    s.options_.crs = "EPSG:3857";
    s.frame_ = geo::LocalFrame(origin);
    s.adjust_ = !request.color.isIdentity();
    s.lut_ = detail::colourTable(request.color);

    // Zoom per level: service pixels just finer than the texels, within the dataset's range.
    const double cosLat = std::cos(origin.latitudeDeg * std::numbers::pi / 180.0);
    const double world = 2.0 * std::numbers::pi * geo::kWebMercatorRadius * cosLat;
    const int maxZoom = dataset->maxZoom.value_or(net::kMaxImageryZoom);
    for (std::size_t level = 0; level < s.zoom_.size(); ++level) {
        const double texel = std::ldexp(tiles.tileSizeM, static_cast<int>(level)) / tiles.tilePx;
        const double z = std::ceil(std::log2(world / (p.tileSizePx * texel)) - 1e-9);
        s.zoom_[level] = std::clamp(static_cast<int>(z), dataset->minZoom, maxZoom);
    }

    OrthoMetadata& m = s.metadata_;
    m.format = protocolLabel(p.protocol);
    m.crs = "WGS 84 / Pseudo-Mercator";
    m.crsCode = "EPSG:3857";
    m.bands = "RGB";
    m.resolutionM = world / (p.tileSizePx * std::ldexp(1.0, maxZoom));
    m.hasOverviews = true;
    m.minX = m.minZ = -kUnbounded;
    m.maxX = m.maxZ = kUnbounded;
    if (const auto& c = dataset->coverage) {
        // Box of the coverage edges in the world frame. A coverage too large for a box on the
        // tangent plane (or one around the origin spanning a continent) stays unbounded.
        const bool huge = (c->east - c->west) > 60.0 || (c->north - c->south) > 60.0;
        if (!huge) {
            m.minX = m.minZ = kUnbounded;
            m.maxX = m.maxZ = -kUnbounded;
            for (int e = 0; e < 4; ++e) {
                for (int k = 0; k <= kBorderSamples; ++k) {
                    const double f = static_cast<double>(k) / kBorderSamples;
                    const double lat = e == 0   ? c->south
                                       : e == 1 ? c->north
                                                : c->south + f * (c->north - c->south);
                    const double lon = e == 2   ? c->west
                                       : e == 3 ? c->east
                                                : c->west + f * (c->east - c->west);
                    const geo::Enu enu = s.frame_.toEnu(geo::Geodetic{lat, lon, 0.0});
                    m.minX = std::min(m.minX, enu.east);
                    m.maxX = std::max(m.maxX, enu.east);
                    m.minZ = std::min(m.minZ, -enu.north);
                    m.maxZ = std::max(m.maxZ, -enu.north);
                }
            }
        }
    }
    m.notes.push_back(
        std::format("Zoom levels {} … {} ({} px tiles)", dataset->minZoom, maxZoom, p.tileSizePx));
    s.description_ = std::format("online imagery {} · {} ({}, zoom {} … {}, {:.2g} m/px)", p.name,
                                 dataset->name, m.format, dataset->minZoom, maxZoom, m.resolutionM);
    log::info("Opened {}", s.description_);
    return source;
}

int OnlineImagerySource::zoomFor(int level) const noexcept {
    return zoom_[static_cast<std::size_t>(
        std::clamp(level, 0, static_cast<int>(zoom_.size()) - 1))];
}

std::uint64_t OnlineImagerySource::revision() const noexcept { return revision_.load(); }

OnlineImageryStatus OnlineImagerySource::status() const {
    const std::scoped_lock lock(mutex_);
    OnlineImageryStatus s = counters_;
    s.memoryTiles = memory_.size();
    s.pending = pending_.size();
    return s;
}

// ---------------------------------------------------------------------------------------------
// Service tiles
// ---------------------------------------------------------------------------------------------

std::shared_ptr<const OnlineImagerySource::Image> OnlineImagerySource::decode(
    const std::string& body, std::string& error) const {
    return decodeTile(body, provider_.tileSizePx, error);
}

TerrainResult<void> checkImageryTile(const std::string& body, int tileSizePx) {
    gdal::ensureInitialised();
    std::string error;
    if (!OnlineImagerySource::decodeTile(body, tileSizePx, error)) {
        return terrainError(TerrainErrorCode::CorruptData, error);
    }
    return {};
}

std::shared_ptr<const OnlineImagerySource::Image> OnlineImagerySource::decodeTile(
    const std::string& body, int tileSizePx, std::string& error) {
    const std::string name = gdal::uniqueMemPath("imagery", "");
    VSILFILE* f =
        VSIFileFromMemBuffer(name.c_str(), reinterpret_cast<GByte*>(const_cast<char*>(body.data())),
                             static_cast<vsi_l_offset>(body.size()), FALSE);
    if (f == nullptr) {
        error = "cannot read the tile in memory";
        return nullptr;
    }
    VSIFCloseL(f);
    struct Unlink {
        const std::string& path;
        ~Unlink() { VSIUnlink(path.c_str()); }
    } unlink{name};
    gdal::DatasetPtr ds = gdal::openRaster(name, kTileDrivers);
    if (!ds) {
        error = "undecodable tile: " + gdal::lastError();
        return nullptr;
    }
    auto* h = static_cast<GDALDatasetH>(ds.get());
    const int size = tileSizePx;
    const int w = GDALGetRasterXSize(h);
    const int ht = GDALGetRasterYSize(h);
    if (w != size || ht != size) {
        error = std::format("tile is {} × {} px, expected {} × {}", w, ht, size, size);
        return nullptr;
    }
    const int bands = GDALGetRasterCount(h);
    if (bands < 1 || bands > 4 || GDALGetRasterDataType(GDALGetRasterBand(h, 1)) != GDT_Byte) {
        error = "tile is not an 8-bit grey, RGB or RGBA image";
        return nullptr;
    }
    auto image = std::make_shared<Image>();
    image->size = size;
    const auto pixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    image->rgba.assign(pixels * 4U, 255);
    GDALRasterBandH first = GDALGetRasterBand(h, 1);
    const int line = size * 4;
    if (bands == 1 && GDALGetRasterColorInterpretation(first) == GCI_PaletteIndex) {
        GDALColorTableH table = GDALGetRasterColorTable(first);
        std::vector<std::uint8_t> index(pixels);
        if (table == nullptr || GDALRasterIO(first, GF_Read, 0, 0, size, size, index.data(), size,
                                             size, GDT_Byte, 0, 0) != CE_None) {
            error = "unreadable palette tile";
            return nullptr;
        }
        std::array<std::array<std::uint8_t, 4>, 256> colours{};
        const int entries = std::min(GDALGetColorEntryCount(table), 256);
        for (int i = 0; i < entries; ++i) {
            const GDALColorEntry* e = GDALGetColorEntry(table, i);
            colours[static_cast<std::size_t>(i)] = {
                static_cast<std::uint8_t>(std::clamp<short>(e->c1, 0, 255)),
                static_cast<std::uint8_t>(std::clamp<short>(e->c2, 0, 255)),
                static_cast<std::uint8_t>(std::clamp<short>(e->c3, 0, 255)),
                static_cast<std::uint8_t>(std::clamp<short>(e->c4, 0, 255))};
        }
        for (std::size_t p = 0; p < pixels; ++p) {
            std::ranges::copy(colours[index[p]], image->rgba.begin() + static_cast<long>(4 * p));
        }
        return image;
    }
    // Grey (+ alpha) is spread over red, green and blue; RGB(A) is read as it is.
    const bool grey = bands <= 2;
    std::array<int, 4> map{1, grey ? 1 : 2, grey ? 1 : 3, grey ? 2 : 4};
    const int read = grey ? (bands == 2 ? 4 : 3) : bands;
    if (GDALDatasetRasterIO(h, GF_Read, 0, 0, size, size, image->rgba.data(), size, size, GDT_Byte,
                            read, map.data(), 4, line, 1) != CE_None) {
        error = "unreadable tile: " + gdal::lastError();
        return nullptr;
    }
    return image;
}

void OnlineImagerySource::remember(const geo::TileId& tile,
                                   std::shared_ptr<const Image> image) const {
    const std::scoped_lock lock(mutex_);
    if (!image || memory_.contains(tile)) {
        return;
    }
    memoryBytes_ += image->rgba.size();
    lru_.push_front(tile);
    memory_.emplace(tile, Cached{std::move(image), lru_.begin()});
    while (memoryBytes_ > imageryOptions_.memoryBytes && lru_.size() > 1) {
        const auto it = memory_.find(lru_.back());
        lru_.pop_back();
        if (it == memory_.end()) {
            continue;  // Not reached: every LRU entry is in the map.
        }
        if (it->second.image) {
            memoryBytes_ -= it->second.image->rgba.size();
        }
        memory_.erase(it);
    }
}

OnlineImagerySource::Lookup OnlineImagerySource::acquire(const geo::TileId& tile,
                                                         double priority) const {
    {
        const std::scoped_lock lock(mutex_);
        if (const auto it = memory_.find(tile); it != memory_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.position);
            return {TileState::Ready, it->second.image};
        }
        if (missing_.contains(tile)) {
            return {TileState::Missing, nullptr};
        }
        if (pending_.contains(tile)) {
            return {TileState::Waiting, nullptr};
        }
        if (const auto it = failed_.find(tile); it != failed_.end()) {
            if (Clock::now() - it->second < imageryOptions_.retryAfter) {
                return {TileState::Waiting, nullptr};
            }
            failed_.erase(it);
        }
    }
    const net::TileRequest request{dataset_.id, tile, false};
    // The disk cache, fresh or stale: imagery never needs revalidating before it is shown.
    if (auto cached = client_->cached(provider_, request); cached && *cached) {
        std::string error;
        if (auto image = decode((*cached)->body, error)) {
            remember(tile, image);
            const std::scoped_lock lock(mutex_);
            ++counters_.fromCache;
            return {TileState::Ready, std::move(image)};
        }
        const std::scoped_lock lock(mutex_);
        missing_.emplace(tile, true);
        ++counters_.missing;
        counters_.lastError = error;
        return {TileState::Missing, nullptr};
    }
    {
        const std::scoped_lock lock(mutex_);
        if (pending_.contains(tile) ||
            static_cast<int>(pending_.size()) >= imageryOptions_.maxPending) {
            return {TileState::Waiting, nullptr};
        }
        pending_.emplace(tile, 0);
    }
    const net::RequestId id =
        client_->fetch(provider_, request, priority,
                       [self = weak_from_this(), tile](net::NetResult<net::Payload> result) {
                           if (const auto source = self.lock()) {
                               source->arrived(tile, std::move(result));
                           }
                       });
    const std::scoped_lock lock(mutex_);
    if (const auto it = pending_.find(tile); it != pending_.end()) {
        it->second = id;
    }
    return {TileState::Waiting, nullptr};
}

void OnlineImagerySource::arrived(const geo::TileId& tile,
                                  net::NetResult<net::Payload> result) const {
    if (!result) {
        const std::scoped_lock lock(mutex_);
        pending_.erase(tile);
        const net::NetError& e = result.error();
        if (e.code == net::NetErrorCode::Cancelled) {
            return;
        }
        // The service has no such tile: show the materials there.
        if ((e.code == net::NetErrorCode::HttpStatus &&
             (e.httpStatus == 404 || e.httpStatus == 410 || e.httpStatus == 204)) ||
            e.code == net::NetErrorCode::InvalidPayload) {
            missing_.emplace(tile, true);
            ++counters_.missing;
        } else {
            failed_[tile] = Clock::now();
            ++counters_.failed;
            if (counters_.lastError != e.message) {
                log::warn("Imagery {}: {}", provider_.name, e.message);
            }
            counters_.lastError = e.message;
        }
        ++revision_;
        return;
    }
    std::string error;
    auto image = decode(result->body, error);
    const bool decoded = image != nullptr;
    if (decoded) {
        remember(tile, std::move(image));
    }
    const std::scoped_lock lock(mutex_);
    pending_.erase(tile);
    if (!decoded) {
        missing_.emplace(tile, true);
        ++counters_.missing;
        counters_.lastError = error;
    } else if (result->fromCache) {
        ++counters_.fromCache;
    } else {
        ++counters_.downloaded;
    }
    ++revision_;
}

// ---------------------------------------------------------------------------------------------
// Orthophoto tiles
// ---------------------------------------------------------------------------------------------

OrthoBuild OnlineImagerySource::build(const OrthoTileKey& key, double priority) const {
    if (!mayCover(key)) {
        return {nullptr, true};
    }
    const int n = options_.tilePx;
    const int ext = n + 2 * kFeatherPx;
    const double side = std::ldexp(options_.tileSizeM, key.level);
    const double texel = side / n;
    const double x0 = static_cast<double>(key.ix) * side;
    const double z0 = static_cast<double>(key.iz) * side;
    const int zoom = zoomFor(key.level);
    const int ts = provider_.tileSizePx;
    const std::int64_t count = std::int64_t{1} << zoom;
    const double scale = static_cast<double>(count) * ts;  // Service pixels across the world.

    // Service pixel coordinates of a lattice of texel centres (exact), bilinear in between.
    constexpr std::size_t kLat2 = static_cast<std::size_t>(kLattice) * kLattice;
    std::array<double, kLat2> gu{};
    std::array<double, kLat2> gv{};
    const double step = static_cast<double>(ext - 1) / (kLattice - 1);
    for (int j = 0; j < kLattice; ++j) {
        for (int i = 0; i < kLattice; ++i) {
            const double x = x0 + (i * step - kFeatherPx + 0.5) * texel;
            const double z = z0 + (j * step - kFeatherPx + 0.5) * texel;
            const geo::Geodetic g = frame_.toGeodetic(geo::Enu{x, -z, 0.0});
            double u = 0.0;
            double v = 0.0;
            toMercatorUnit(g.latitudeDeg, g.longitudeDeg, u, v);
            const std::size_t k =
                static_cast<std::size_t>(j) * kLattice + static_cast<std::size_t>(i);
            gu[k] = u * scale;
            gv[k] = v * scale;
        }
    }
    const auto [uMin, uMax] = std::ranges::minmax(gu);
    const auto [vMin, vMax] = std::ranges::minmax(gv);
    const auto tileOf = [&](double p) {
        return std::clamp(static_cast<std::int64_t>(std::floor(p / ts)), std::int64_t{0},
                          count - 1);
    };
    const std::int64_t tx0 = tileOf(uMin - 1.0);
    const std::int64_t tx1 = tileOf(uMax + 1.0);
    const std::int64_t ty0 = tileOf(vMin - 1.0);
    const std::int64_t ty1 = tileOf(vMax + 1.0);
    const std::int64_t cols = tx1 - tx0 + 1;
    const std::int64_t rows = ty1 - ty0 + 1;
    if (cols * rows > imageryOptions_.maxTilesPerBuild) {
        return {nullptr, true};  // Far coarser than the service's coarsest zoom.
    }

    // The service tiles: nearest first in priority, all requested now.
    std::vector<std::shared_ptr<const Image>> images(static_cast<std::size_t>(cols * rows));
    bool complete = true;
    bool any = false;
    for (std::int64_t ty = ty0; ty <= ty1; ++ty) {
        for (std::int64_t tx = tx0; tx <= tx1; ++tx) {
            const Lookup got = acquire(geo::TileId{tx, ty, zoom}, priority);
            complete = complete && got.state != TileState::Waiting;
            if (got.image) {
                any = true;
                images[static_cast<std::size_t>((ty - ty0) * cols + (tx - tx0))] = got.image;
            }
        }
    }
    if (!any) {
        return {nullptr, complete};
    }
    const auto pixel = [&](std::int64_t pu, std::int64_t pv) -> const std::uint8_t* {
        const std::int64_t tx = pu >= 0 ? pu / ts : -1;
        const std::int64_t ty = pv >= 0 ? pv / ts : -1;
        if (tx < tx0 || tx > tx1 || ty < ty0 || ty > ty1) {
            return nullptr;
        }
        const auto& image = images[static_cast<std::size_t>((ty - ty0) * cols + (tx - tx0))];
        if (!image) {
            return nullptr;
        }
        const auto px = static_cast<std::size_t>(pu - tx * ts);
        const auto py = static_cast<std::size_t>(pv - ty * ts);
        return &image->rgba[(py * static_cast<std::size_t>(ts) + px) * 4U];
    };

    // Several samples per texel when the service pixels are finer than the texels.
    const double pixelsPerTexel =
        std::hypot(gu[kLattice - 1] - gu[0], gv[kLattice - 1] - gv[0]) / ((kLattice - 1) * step);
    const int sub = pixelsPerTexel > 1.25 ? 2 : 1;
    const auto extN = static_cast<std::size_t>(ext);
    std::vector<float> alpha(extN * extN, 0.0F);
    std::vector<std::array<float, 3>> colour(extN * extN);
    bool visible = false;
    for (int j = 0; j < ext; ++j) {
        for (int i = 0; i < ext; ++i) {
            double m = 0.0;
            std::array<double, 3> c{};
            for (int sj = 0; sj < sub; ++sj) {
                for (int si = 0; si < sub; ++si) {
                    const double oi = sub == 1 ? 0.0 : (si - 0.5) * 0.5;
                    const double oj = sub == 1 ? 0.0 : (sj - 0.5) * 0.5;
                    const double li = std::clamp((i + oi) / step, 0.0, kLattice - 1.0);
                    const double lj = std::clamp((j + oj) / step, 0.0, kLattice - 1.0);
                    const int ci = std::min(static_cast<int>(li), kLattice - 2);
                    const int cj = std::min(static_cast<int>(lj), kLattice - 2);
                    const double fi = li - ci;
                    const double fj = lj - cj;
                    const std::size_t k00 =
                        static_cast<std::size_t>(cj) * kLattice + static_cast<std::size_t>(ci);
                    const std::size_t k10 = k00 + 1;
                    const std::size_t k01 = k00 + kLattice;
                    const std::size_t k11 = k01 + 1;
                    const double u = (1 - fj) * ((1 - fi) * gu[k00] + fi * gu[k10]) +
                                     fj * ((1 - fi) * gu[k01] + fi * gu[k11]) - 0.5;
                    const double v = (1 - fj) * ((1 - fi) * gv[k00] + fi * gv[k10]) +
                                     fj * ((1 - fi) * gv[k01] + fi * gv[k11]) - 0.5;
                    const auto pu = static_cast<std::int64_t>(std::floor(u));
                    const auto pv = static_cast<std::int64_t>(std::floor(v));
                    const double fu = u - static_cast<double>(pu);
                    const double fv = v - static_cast<double>(pv);
                    const std::array<const std::uint8_t*, 4> px{
                        pixel(pu, pv), pixel(pu + 1, pv), pixel(pu, pv + 1), pixel(pu + 1, pv + 1)};
                    const std::array<double, 4> w{(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv,
                                                  fu * fv};
                    // Alpha-weighted colour, so transparent pixels never darken the edge.
                    for (std::size_t q = 0; q < 4; ++q) {
                        if (px[q] == nullptr) {
                            continue;
                        }
                        const double wa = w[q] * px[q][3] / 255.0;
                        m += wa;
                        for (std::size_t ch = 0; ch < 3; ++ch) {
                            c[ch] += wa * px[q][ch];
                        }
                    }
                }
            }
            if (m < 0.5 * sub * sub) {
                continue;  // Mostly outside the imagery.
            }
            const std::size_t t = static_cast<std::size_t>(j) * extN + static_cast<std::size_t>(i);
            alpha[t] = 1.0F;
            for (std::size_t ch = 0; ch < 3; ++ch) {
                colour[t][ch] = static_cast<float>(c[ch] / m);
            }
            visible = true;
        }
    }
    if (!visible) {
        return {nullptr, complete};
    }
    return {detail::finishOrthoTile(key, n, alpha, colour, adjust_ ? &lut_ : nullptr), complete};
}

}  // namespace ds::terrain
