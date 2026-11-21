// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/orthophoto.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <utility>

#include "ds/common/log.hpp"
#include "ds/common/sha256.hpp"

#include "gdal_support.hpp"
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>
#include <unistd.h>

namespace ds::terrain {
namespace fs = std::filesystem;
namespace {

constexpr int kLattice = 17;        // Exact transformations per tile side.
constexpr int kFeatherPx = 2;       // Alpha ramps up over this many texels inside an edge.
constexpr int kBorderSamples = 64;  // Points per image edge for the coverage box.
constexpr std::array<const char*, 2> kVrtDriver{"VRT", nullptr};

struct TransformDeleter {
    void operator()(OGRCoordinateTransformation* t) const noexcept {
        OGRCoordinateTransformation::DestroyCT(t);
    }
};
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, TransformDeleter>;

// Side-car files GDAL may need next to an image: world files (PNG / JPEG / JPEG 2000 without
// GeoJP2), external overviews and masks. Only those that exist are named.
std::vector<std::string> siblingsOf(const fs::path& file) {
    std::vector<std::string> out{file.filename().string()};
    const std::string stem = file.stem().string();
    const std::string name = file.filename().string();
    std::vector<std::string> candidates{name + ".ovr", name + ".msk", stem + ".wld"};
    for (const char* world :
         {".pgw", ".pngw", ".jgw", ".jpgw", ".jpegw", ".j2w", ".tfw", ".tifw", ".wpw"}) {
        candidates.push_back(stem + world);
    }
    const auto upper = [](std::string text) {
        std::ranges::transform(text, text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text;
    };
    for (const std::string& c : candidates) {
        for (const std::string& variant : {c, stem + upper(c.substr(stem.size()))}) {
            std::error_code ec;
            if (std::ranges::find(out, variant) == out.end() &&
                fs::is_regular_file(file.parent_path() / variant, ec)) {
                out.push_back(variant);
            }
        }
    }
    return out;
}

gdal::DatasetPtr openImage(const fs::path& file) {
    if (file.string().starts_with("/vsimem/")) {
        return gdal::openRaster(file.string(), gdal::kOrthoDrivers);
    }
    const std::vector<std::string> siblings = siblingsOf(file);
    return gdal::openRaster(file.string(), gdal::kOrthoDrivers, siblings);
}

std::string fileList(std::span<const fs::path> files) {
    if (files.size() == 1) {
        return files.front().filename().string();
    }
    return std::format("{} files", files.size());
}

// Band layout of one image: colour bands (1 or 3) and the alpha band (0 if none).
struct BandLayout {
    std::vector<int> colour;
    int alpha = 0;
    std::string name;
    bool operator==(const BandLayout&) const = default;
};

TerrainResult<BandLayout> bandLayout(GDALDatasetH h, const std::vector<int>& asked,
                                     const std::string& name) {
    const int count = GDALGetRasterCount(h);
    auto interp = [&](int b) { return GDALGetRasterColorInterpretation(GDALGetRasterBand(h, b)); };
    BandLayout l;
    if (!asked.empty()) {
        if (asked.size() != 3 && asked.size() != 4) {
            return terrainError(TerrainErrorCode::InvalidSpec,
                                "orthophotoBands must list 3 (RGB) or 4 (RGBA) bands");
        }
        for (const int b : asked) {
            if (b < 1 || b > count) {
                return terrainError(
                    TerrainErrorCode::UnsupportedFormat,
                    std::format("\"{}\" has {} band(s), band {} was asked for", name, count, b));
            }
        }
        l.colour.assign(asked.begin(), asked.begin() + 3);
        l.alpha = asked.size() == 4 ? asked[3] : 0;
        l.name = "bands";
        for (std::size_t i = 0; i < asked.size(); ++i) {
            l.name += std::format("{}{}", i == 0 ? " " : ", ", asked[i]);
        }
        return l;
    }
    if (count >= 1 && GDALGetRasterColorTable(GDALGetRasterBand(h, 1)) != nullptr) {
        return terrainError(TerrainErrorCode::UnsupportedFormat,
                            std::format("\"{}\" is a paletted image; expand it to RGB first "
                                        "(gdal_translate -expand rgb)",
                                        name));
    }
    switch (count) {
        case 1:
            l.colour = {1, 1, 1};
            l.name = "grey";
            return l;
        case 2:
            if (interp(2) == GCI_AlphaBand) {
                l.colour = {1, 1, 1};
                l.alpha = 2;
                l.name = "grey + alpha";
                return l;
            }
            break;
        case 3:
            l.colour = {1, 2, 3};
            l.name = "RGB";
            return l;
        case 4:
            if (interp(4) == GCI_AlphaBand) {
                l.colour = {1, 2, 3};
                l.alpha = 4;
                l.name = "RGBA";
                return l;
            }
            break;
        default: break;
    }
    return terrainError(TerrainErrorCode::UnsupportedFormat,
                        std::format("\"{}\" has {} bands; say which are red, green, blue (and "
                                    "alpha) with \"orthophotoBands\", e.g. [1, 2, 3]",
                                    name, count));
}

// Colour lookup table of the adjustments.
std::array<std::uint8_t, 256> colourTable(const OrthoColor& c) {
    std::array<std::uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        v = (v - 0.5) * c.contrast + 0.5 + c.brightness;
        v = std::clamp(v, 0.0, 1.0);
        v = std::pow(v, 1.0 / c.gamma);
        lut[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(v * 255.0));
    }
    return lut;
}

struct CopyProgress {
    std::stop_token stop;
    const OrthoProgress* progress;
};

int CPL_STDCALL onCopyProgress(double done, const char* /*message*/, void* arg) {
    const auto* p = static_cast<const CopyProgress*>(arg);
    if (p->progress != nullptr && *p->progress) {
        (*p->progress)(done);
    }
    return p->stop.stop_requested() ? FALSE : TRUE;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------------------------

std::size_t OrthoTileKeyHash::operator()(const OrthoTileKey& key) const noexcept {
    std::size_t h = std::hash<std::int64_t>{}(key.ix);
    h ^= std::hash<std::int64_t>{}(key.iz) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    h ^= std::hash<int>{}(key.level) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    return h;
}

double orthoTileSize(int level) noexcept { return std::ldexp(kOrthoTileSizeM, level); }

std::string validateOrthoColor(const OrthoColor& c) {
    if (!(c.brightness >= -1.0 && c.brightness <= 1.0)) {
        return "brightness must be between −1 and 1";
    }
    if (!(c.contrast >= 0.0 && c.contrast <= 4.0)) {
        return "contrast must be between 0 and 4";
    }
    if (!(c.gamma >= 0.1 && c.gamma <= 10.0)) {
        return "gamma must be between 0.1 and 10";
    }
    return {};
}

int OrthoTile::mipCount() const noexcept {
    int n = 1;
    for (int s = sizePx; s > 1; s >>= 1) {
        ++n;
    }
    return n;
}

std::size_t OrthoTile::mipOffset(int mip) const noexcept {
    std::size_t offset = 0;
    for (int m = 0; m < mip; ++m) {
        const auto s = static_cast<std::size_t>(std::max(sizePx >> m, 1));
        offset += s * s * 4U;
    }
    return offset;
}

std::size_t OrthoTile::bytesFor(int sizePx) noexcept {
    OrthoTile t;
    t.sizePx = sizePx;
    return t.mipOffset(t.mipCount());
}

// ---------------------------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------------------------

struct OrthophotoSource::Layout {
    std::string datasetPath;  // File, or the in-memory VRT of a mosaic.
    std::string vrtPath;      // In-memory VRT to delete ("" if none).
    std::vector<std::string> siblings;
    bool vrt = false;
    BandLayout bands;
    std::array<double, 6> inverse{};  // Georeferenced → pixel.
    int width = 0;
    int height = 0;
    std::optional<double> noData;  // Of the colour bands (all equal ⇒ transparent).
    std::string srsWkt;
    std::optional<geo::LocalFrame> frame;
    double pixelSizeM = 1.0;
    std::array<std::uint8_t, 256> lut{};
    bool adjust = false;
};

struct OrthophotoSource::Reader {
    gdal::DatasetPtr dataset;
    TransformPtr toImage;  // Longitude/latitude (WGS84) → image CRS.
    std::vector<double> lon, lat;
    std::vector<int> ok;
    std::vector<std::uint8_t> colour, mask;
};

OrthophotoSource::OrthophotoSource() : layout_(std::make_unique<Layout>()) {}

OrthophotoSource::~OrthophotoSource() {
    readers_.clear();
    if (!layout_->vrtPath.empty()) {
        VSIUnlink(layout_->vrtPath.c_str());
    }
}

TerrainResult<std::unique_ptr<OrthophotoSource::Reader>> OrthophotoSource::makeReader() const {
    auto reader = std::make_unique<Reader>();
    reader->dataset = layout_->vrt ? gdal::openRaster(layout_->datasetPath, kVrtDriver)
                                   : gdal::openRaster(layout_->datasetPath, gdal::kOrthoDrivers,
                                                      layout_->siblings);
    if (!reader->dataset) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            "cannot reopen the orthophotos: " + gdal::lastError());
    }
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target;
    if (target.importFromWkt(layout_->srsWkt.c_str()) != OGRERR_NONE) {
        return terrainError(TerrainErrorCode::MissingCrs, "unusable CRS");
    }
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    reader->toImage.reset(OGRCreateCoordinateTransformation(&wgs84, &target));
    if (!reader->toImage) {
        return terrainError(
            TerrainErrorCode::MissingCrs,
            "no transformation from WGS84 to the orthophotos' CRS: " + gdal::lastError());
    }
    return reader;
}

std::unique_ptr<OrthophotoSource::Reader> OrthophotoSource::acquire() const {
    {
        const std::scoped_lock lock(readersMutex_);
        if (!readers_.empty()) {
            auto reader = std::move(readers_.back());
            readers_.pop_back();
            return reader;
        }
    }
    auto reader = makeReader();
    if (!reader) {
        ++readErrors_;
        log::warn("Orthophotos: {}", reader.error().message);
        return nullptr;
    }
    return std::move(*reader);
}

void OrthophotoSource::release(std::unique_ptr<Reader> reader) const {
    if (reader) {
        const std::scoped_lock lock(readersMutex_);
        readers_.push_back(std::move(reader));
    }
}

bool OrthophotoSource::mayCover(const OrthoTileKey& key) const noexcept {
    const double s = std::ldexp(options_.tileSizeM, key.level);
    const double x0 = static_cast<double>(key.ix) * s;
    const double z0 = static_cast<double>(key.iz) * s;
    const double margin = 2.0 * s / options_.tilePx;
    return metadata_.overlaps(x0 - margin, z0 - margin, x0 + s + margin, z0 + s + margin);
}

// ---------------------------------------------------------------------------------------------
// Tile building
// ---------------------------------------------------------------------------------------------

std::shared_ptr<const OrthoTile> OrthophotoSource::tile(const OrthoTileKey& key) const {
    if (!mayCover(key)) {
        return nullptr;
    }
    const Layout& l = *layout_;
    const int n = options_.tilePx;
    const int ext = n + 2 * kFeatherPx;  // Texels with the feather border.
    const double side = std::ldexp(options_.tileSizeM, key.level);
    const double texel = side / n;
    const double x0 = static_cast<double>(key.ix) * side;
    const double z0 = static_cast<double>(key.iz) * side;

    std::unique_ptr<Reader> reader = acquire();
    if (!reader) {
        return nullptr;
    }
    struct Release {
        const OrthophotoSource* self;
        std::unique_ptr<Reader>* reader;
        ~Release() { self->release(std::move(*reader)); }
    } release{this, &reader};
    Reader& r = *reader;

    // Image pixel coordinates of a lattice of texel centres (exact), then of every texel
    // (bilinear in between).
    const std::size_t lat2 = static_cast<std::size_t>(kLattice) * kLattice;
    r.lon.resize(lat2);
    r.lat.resize(lat2);
    r.ok.assign(lat2, 0);
    const double step = static_cast<double>(ext - 1) / (kLattice - 1);
    for (int j = 0; j < kLattice; ++j) {
        for (int i = 0; i < kLattice; ++i) {
            const double x = x0 + (i * step - kFeatherPx + 0.5) * texel;
            const double z = z0 + (j * step - kFeatherPx + 0.5) * texel;
            const geo::Geodetic g = l.frame->toGeodetic(geo::Enu{x, -z, 0.0});
            const std::size_t k =
                static_cast<std::size_t>(j) * kLattice + static_cast<std::size_t>(i);
            r.lon[k] = g.longitudeDeg;
            r.lat[k] = g.latitudeDeg;
        }
    }
    r.toImage->Transform(lat2, r.lon.data(), r.lat.data(), nullptr, r.ok.data());
    std::vector<double> lu(lat2);
    std::vector<double> lv(lat2);
    for (std::size_t k = 0; k < lat2; ++k) {
        if (r.ok[k] == 0 || !std::isfinite(r.lon[k]) || !std::isfinite(r.lat[k])) {
            return nullptr;  // Outside the CRS's domain.
        }
        const double X = r.lon[k];
        const double Y = r.lat[k];
        lu[k] = l.inverse[0] + l.inverse[1] * X + l.inverse[2] * Y;
        lv[k] = l.inverse[3] + l.inverse[4] * X + l.inverse[5] * Y;
    }
    const auto [uMin, uMax] = std::ranges::minmax(lu);
    const auto [vMin, vMax] = std::ranges::minmax(lv);
    if (uMax < 0.0 || vMax < 0.0 || uMin > l.width || vMin > l.height) {
        return nullptr;
    }

    // Window and read resolution: overviews (or averages) close to the texel size.
    int d = 1;
    while (2.0 * d * l.pixelSizeM <= texel && l.width / (2 * d) >= 1 && l.height / (2 * d) >= 1) {
        d *= 2;
    }
    const int wx0 = std::clamp(static_cast<int>(std::floor(uMin)) - 2 * d, 0, l.width);
    const int wy0 = std::clamp(static_cast<int>(std::floor(vMin)) - 2 * d, 0, l.height);
    const int wx1 = std::clamp(static_cast<int>(std::ceil(uMax)) + 2 * d, 0, l.width);
    const int wy1 = std::clamp(static_cast<int>(std::ceil(vMax)) + 2 * d, 0, l.height);
    if (wx1 <= wx0 || wy1 <= wy0) {
        return nullptr;
    }
    // A skewed mapping could ask for a huge window: read it coarser.
    while ((wx1 - wx0) / d > 4 * ext || (wy1 - wy0) / d > 4 * ext) {
        d *= 2;
    }
    const int bw = std::max(1, (wx1 - wx0 + d - 1) / d);
    const int bh = std::max(1, (wy1 - wy0 + d - 1) / d);
    const double sx = static_cast<double>(wx1 - wx0) / bw;  // Image pixels per buffer pixel.
    const double sy = static_cast<double>(wy1 - wy0) / bh;
    const std::size_t pixels = static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh);
    auto* h = static_cast<GDALDatasetH>(r.dataset.get());
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = d > 1 ? GRIORA_Average : GRIORA_NearestNeighbour;
    r.colour.resize(pixels * 3U);
    std::array<int, 3> bandMap{l.bands.colour[0], l.bands.colour[1], l.bands.colour[2]};
    if (GDALDatasetRasterIOEx(h, GF_Read, wx0, wy0, wx1 - wx0, wy1 - wy0, r.colour.data(), bw, bh,
                              GDT_Byte, 3, bandMap.data(), 3, static_cast<GSpacing>(bw) * 3, 1,
                              &extra) != CE_None) {
        ++readErrors_;
        return nullptr;
    }
    r.mask.assign(pixels, 255);
    GDALRasterBandH maskBand = nullptr;
    if (l.bands.alpha > 0) {
        maskBand = GDALGetRasterBand(h, l.bands.alpha);
    } else {
        GDALRasterBandH first = GDALGetRasterBand(h, l.bands.colour[0]);
        if ((GDALGetMaskFlags(first) & GMF_ALL_VALID) == 0) {
            maskBand = GDALGetMaskBand(first);
        }
    }
    if (maskBand != nullptr) {
        GDALRasterIOExtraArg maskExtra;
        INIT_RASTERIO_EXTRA_ARG(maskExtra);
        maskExtra.eResampleAlg = d > 1 ? GRIORA_Average : GRIORA_NearestNeighbour;
        if (GDALRasterIOEx(maskBand, GF_Read, wx0, wy0, wx1 - wx0, wy1 - wy0, r.mask.data(), bw, bh,
                           GDT_Byte, 0, 0, &maskExtra) != CE_None) {
            ++readErrors_;
            return nullptr;
        }
    }
    if (l.noData) {
        const auto nd = static_cast<std::uint8_t>(std::clamp(*l.noData, 0.0, 255.0));
        for (std::size_t p = 0; p < pixels; ++p) {
            if (r.colour[3 * p] == nd && r.colour[3 * p + 1] == nd && r.colour[3 * p + 2] == nd) {
                r.mask[p] = 0;
            }
        }
    }

    // Sample every texel (extended by the feather border): colour and validity.
    const auto extN = static_cast<std::size_t>(ext);
    std::vector<float> alpha(extN * extN, 0.0F);
    std::vector<std::array<float, 3>> colour(extN * extN);
    bool any = false;
    for (int j = 0; j < ext; ++j) {
        const double lj = j / step;
        const int cj = std::min(static_cast<int>(lj), kLattice - 2);
        const double fj = lj - cj;
        for (int i = 0; i < ext; ++i) {
            const double li = i / step;
            const int ci = std::min(static_cast<int>(li), kLattice - 2);
            const double fi = li - ci;
            const std::size_t k00 =
                static_cast<std::size_t>(cj) * kLattice + static_cast<std::size_t>(ci);
            const std::size_t k10 = k00 + 1;
            const std::size_t k01 = k00 + kLattice;
            const std::size_t k11 = k01 + 1;
            const double u = (1 - fj) * ((1 - fi) * lu[k00] + fi * lu[k10]) +
                             fj * ((1 - fi) * lu[k01] + fi * lu[k11]);
            const double v = (1 - fj) * ((1 - fi) * lv[k00] + fi * lv[k10]) +
                             fj * ((1 - fi) * lv[k01] + fi * lv[k11]);
            if (u < 0.0 || v < 0.0 || u > l.width || v > l.height) {
                continue;
            }
            // Buffer coordinates, integer at buffer pixel centres.
            const double bu = std::clamp((u - wx0) / sx - 0.5, 0.0, static_cast<double>(bw - 1));
            const double bv = std::clamp((v - wy0) / sy - 0.5, 0.0, static_cast<double>(bh - 1));
            const int pu = std::min(static_cast<int>(bu), std::max(bw - 2, 0));
            const int pv = std::min(static_cast<int>(bv), std::max(bh - 2, 0));
            const int qu = std::min(pu + 1, bw - 1);
            const int qv = std::min(pv + 1, bh - 1);
            const double fu = std::clamp(bu - pu, 0.0, 1.0);
            const double fv = std::clamp(bv - pv, 0.0, 1.0);
            const std::array<std::size_t, 4> px{
                static_cast<std::size_t>(pv) * static_cast<std::size_t>(bw) +
                    static_cast<std::size_t>(pu),
                static_cast<std::size_t>(pv) * static_cast<std::size_t>(bw) +
                    static_cast<std::size_t>(qu),
                static_cast<std::size_t>(qv) * static_cast<std::size_t>(bw) +
                    static_cast<std::size_t>(pu),
                static_cast<std::size_t>(qv) * static_cast<std::size_t>(bw) +
                    static_cast<std::size_t>(qu)};
            const std::array<double, 4> w{(1 - fu) * (1 - fv), fu * (1 - fv), (1 - fu) * fv,
                                          fu * fv};
            // Mask-weighted colour, so invalid pixels never darken the edge.
            double m = 0.0;
            std::array<double, 3> c{};
            for (std::size_t q = 0; q < 4; ++q) {
                const double wm = w[q] * r.mask[px[q]] / 255.0;
                m += w[q] * r.mask[px[q]] / 255.0;
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    c[ch] += wm * r.colour[3 * px[q] + ch];
                }
            }
            if (m < 0.5) {
                continue;  // Mostly NoData / transparent.
            }
            const std::size_t t = static_cast<std::size_t>(j) * extN + static_cast<std::size_t>(i);
            alpha[t] = 1.0F;
            for (std::size_t ch = 0; ch < 3; ++ch) {
                colour[t][ch] = static_cast<float>(c[ch] / m);
            }
            any = true;
        }
    }
    if (!any) {
        return nullptr;
    }

    // Soft edges: inside the imagery, alpha ramps up over kFeatherPx texels (box filter of
    // the validity); outside it stays 0.
    auto out = std::make_shared<OrthoTile>();
    out->key = key;
    out->sizePx = n;
    out->rgba.resize(OrthoTile::bytesFor(n));
    const int box = 2 * kFeatherPx + 1;
    bool visible = false;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const std::size_t t = static_cast<std::size_t>(j + kFeatherPx) * extN +
                                  static_cast<std::size_t>(i + kFeatherPx);
            std::uint8_t* o =
                &out->rgba[(static_cast<std::size_t>(j) * static_cast<std::size_t>(n) +
                            static_cast<std::size_t>(i)) *
                           4U];
            if (alpha[t] == 0.0F) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            float sum = 0.0F;
            for (int b = 0; b < box; ++b) {
                for (int a = 0; a < box; ++a) {
                    sum += alpha[static_cast<std::size_t>(j + b) * extN +
                                 static_cast<std::size_t>(i + a)];
                }
            }
            // Share of valid texels around: 1 inside, ≈ 0.6 on an edge ⇒ 1/3, 2/3, 1.
            const float share = sum / static_cast<float>(box * box);
            const float soft = std::clamp((share - 0.4F) / 0.6F, 0.0F, 1.0F);
            for (std::size_t ch = 0; ch < 3; ++ch) {
                auto c8 =
                    static_cast<std::uint8_t>(std::lround(std::clamp(colour[t][ch], 0.0F, 255.0F)));
                if (l.adjust) {
                    c8 = l.lut[c8];
                }
                o[ch] = static_cast<std::uint8_t>(std::lround(static_cast<float>(c8) * soft));
            }
            o[3] = static_cast<std::uint8_t>(std::lround(soft * 255.0F));
            visible = visible || o[3] > 0;
        }
    }
    if (!visible) {
        return nullptr;
    }
    // Mip levels: 2 × 2 box filter of the premultiplied colours.
    for (int m = 1; m < out->mipCount(); ++m) {
        const int src = std::max(n >> (m - 1), 1);
        const int dst = std::max(n >> m, 1);
        const std::uint8_t* s = out->rgba.data() + out->mipOffset(m - 1);
        std::uint8_t* t = out->rgba.data() + out->mipOffset(m);
        for (int j = 0; j < dst; ++j) {
            for (int i = 0; i < dst; ++i) {
                for (int ch = 0; ch < 4; ++ch) {
                    int sum = 0;
                    for (int b = 0; b < 2; ++b) {
                        for (int a = 0; a < 2; ++a) {
                            const int si = std::min(2 * i + a, src - 1);
                            const int sj = std::min(2 * j + b, src - 1);
                            sum += s[(static_cast<std::size_t>(sj) * static_cast<std::size_t>(src) +
                                      static_cast<std::size_t>(si)) *
                                         4U +
                                     static_cast<std::size_t>(ch)];
                        }
                    }
                    t[(static_cast<std::size_t>(j) * static_cast<std::size_t>(dst) +
                       static_cast<std::size_t>(i)) *
                          4U +
                      static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------------

TerrainResult<std::shared_ptr<OrthophotoSource>> OrthophotoSource::open(
    std::span<const fs::path> files, const OrthoOptions& options, const geo::Geodetic& origin) {
    if (files.empty()) {
        return terrainError(TerrainErrorCode::InvalidSpec, "no orthophoto files");
    }
    if (options.tilePx < 16 || options.tilePx > 1024 ||
        (options.tilePx & (options.tilePx - 1)) != 0 || !(options.tileSizeM > 0.0)) {
        return terrainError(TerrainErrorCode::InvalidSpec, "invalid orthophoto tile size");
    }
    if (const std::string bad = validateOrthoColor(options.color); !bad.empty()) {
        return terrainError(TerrainErrorCode::InvalidSpec, "orthophoto " + bad);
    }
    gdal::ensureInitialised();
    std::shared_ptr<OrthophotoSource> source(new OrthophotoSource());
    source->options_ = options;
    Layout& l = *source->layout_;
    OrthoMetadata& meta = source->metadata_;
    meta.fileCount = static_cast<int>(files.size());

    OGRSpatialReference override;
    if (!options.crs.empty() && override.SetFromUserInput(options.crs.c_str()) != OGRERR_NONE) {
        return terrainError(TerrainErrorCode::InvalidSpec,
                            std::format("unknown CRS \"{}\"", options.crs));
    }

    std::optional<OGRSpatialReference> crs;
    std::optional<BandLayout> layout;
    int bandCount = 0;
    std::vector<std::string> drivers;
    bool allOverviews = true;
    bool fileWithoutCrs = false;
    for (const fs::path& file : files) {
        const std::string name = file.filename().string();
        std::error_code ec;
        if (!file.string().starts_with("/vsimem/") && !fs::is_regular_file(file, ec)) {
            return terrainError(TerrainErrorCode::FileNotFound,
                                std::format("orthophoto \"{}\" not found", name));
        }
        gdal::DatasetPtr ds = openImage(file);
        if (!ds) {
            return terrainError(TerrainErrorCode::UnsupportedFormat,
                                std::format("\"{}\" is not a readable orthophoto (COG, GeoTIFF, "
                                            "JPEG 2000, PNG or JPEG with a world file): {}",
                                            name, gdal::lastError()));
        }
        auto* h = static_cast<GDALDatasetH>(ds.get());
        const std::string driver = GDALGetDriverShortName(GDALGetDatasetDriver(h));
        if (std::ranges::find(drivers, driver) == drivers.end()) {
            drivers.push_back(driver);
        }
        auto bands = bandLayout(h, options.bands, name);
        if (!bands) {
            return std::unexpected(bands.error());
        }
        std::vector<int> used = bands->colour;
        if (bands->alpha > 0) {
            used.push_back(bands->alpha);
        }
        for (const int b : used) {
            const GDALDataType type = GDALGetRasterDataType(GDALGetRasterBand(h, b));
            if (type != GDT_Byte) {
                return terrainError(
                    TerrainErrorCode::UnsupportedFormat,
                    std::format("\"{}\": {} imagery is not supported, only 8-bit; convert it "
                                "with gdal_translate -ot Byte -scale",
                                name, GDALGetDataTypeName(type)));
            }
        }
        if (!layout) {
            layout = *bands;
            bandCount = GDALGetRasterCount(h);
        } else if (!(*layout == *bands) || GDALGetRasterCount(h) != bandCount) {
            return terrainError(TerrainErrorCode::UnsupportedFormat,
                                std::format("\"{}\" has a different band layout from \"{}\"; all "
                                            "orthophotos of a mission must match",
                                            name, files.front().filename().string()));
        }
        const int w = GDALGetRasterXSize(h);
        const int ht = GDALGetRasterYSize(h);
        if (w < 1 || ht < 1) {
            return terrainError(TerrainErrorCode::CorruptData,
                                std::format("\"{}\" holds no pixels", name));
        }
        if (w > options.maxDimensionPx || ht > options.maxDimensionPx) {
            return terrainError(
                TerrainErrorCode::TooLarge,
                std::format("\"{}\" is {} × {} pixels, over the limit of {} × {}", name, w, ht,
                            options.maxDimensionPx, options.maxDimensionPx));
        }
        if (GDALGetOverviewCount(GDALGetRasterBand(h, bands->colour[0])) == 0 &&
            std::max(w, ht) > 2 * kOrthoTilePx) {
            allOverviews = false;
        }
        std::array<double, 6> gt{};
        if (GDALGetGeoTransform(h, gt.data()) != CE_None) {
            return terrainError(TerrainErrorCode::CorruptData,
                                std::format("\"{}\" has no georeferencing (PNG and JPEG need a "
                                            "world file such as .pgw or .jgw)",
                                            name));
        }
        const OGRSpatialReference* fileCrs = OGRSpatialReference::FromHandle(GDALGetSpatialRef(h));
        OGRSpatialReference thisCrs;
        if (fileCrs != nullptr && !fileCrs->IsEmpty()) {
            thisCrs = *fileCrs;
        } else if (!options.crs.empty()) {
            thisCrs = override;
            fileWithoutCrs = true;
        } else {
            return terrainError(TerrainErrorCode::MissingCrs,
                                std::format("\"{}\" has no CRS; give \"orthophotoCrs\" in the "
                                            "mission (e.g. \"EPSG:25830\")",
                                            name));
        }
        if (!crs) {
            crs = thisCrs;
        } else if (!crs->IsSame(&thisCrs)) {
            return terrainError(TerrainErrorCode::MixedCrs,
                                std::format("\"{}\" is in a different CRS from \"{}\"; all "
                                            "orthophotos of a mission must share one CRS",
                                            name, files.front().filename().string()));
        }
    }
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        meta.format += (i > 0 ? ", " : "") + drivers[i];
    }
    meta.bands = layout->name;
    meta.hasOverviews = allOverviews;

    // The dataset that is read: the file itself, or an in-memory VRT mosaic holding the
    // colour bands and an alpha band (0 where no file covers the mosaic).
    if (files.size() == 1) {
        l.datasetPath = files.front().string();
        l.siblings = files.front().string().starts_with("/vsimem/") ? std::vector<std::string>{}
                                                                    : siblingsOf(files.front());
        l.bands = *layout;
    } else {
        l.vrtPath = gdal::uniqueMemPath("ortho_mosaic", ".vrt");
        std::vector<std::string> names;
        for (const auto& f : files) {
            names.push_back(f.string());
        }
        std::vector<const char*> namePtrs;
        for (const auto& n : names) {
            namePtrs.push_back(n.c_str());
        }
        CPLStringList args;
        args.AddString("-resolution");
        args.AddString("highest");
        const bool grey = layout->colour[0] == layout->colour[1];
        std::vector<int> picked = grey ? std::vector<int>{layout->colour[0]} : layout->colour;
        if (layout->alpha > 0) {
            picked.push_back(layout->alpha);
        }
        for (const int b : picked) {
            args.AddString("-b");
            args.AddString(std::to_string(b).c_str());
        }
        if (layout->alpha == 0) {
            args.AddString("-addalpha");
        }
        if (fileWithoutCrs) {
            args.AddString("-a_srs");
            args.AddString(options.crs.c_str());
        }
        GDALBuildVRTOptions* vrtOptions = GDALBuildVRTOptionsNew(args.List(), nullptr);
        int usageError = 0;
        GDALDatasetH vrt = GDALBuildVRT(l.vrtPath.c_str(), static_cast<int>(namePtrs.size()),
                                        nullptr, namePtrs.data(), vrtOptions, &usageError);
        GDALBuildVRTOptionsFree(vrtOptions);
        if (vrt == nullptr) {
            return terrainError(TerrainErrorCode::CorruptData,
                                "cannot mosaic the orthophotos: " + gdal::lastError());
        }
        GDALClose(vrt);
        l.datasetPath = l.vrtPath;
        l.vrt = true;
        const int count = static_cast<int>(picked.size()) + (layout->alpha == 0 ? 1 : 0);
        l.bands.colour = grey ? std::vector<int>{1, 1, 1} : std::vector<int>{1, 2, 3};
        l.bands.alpha = count;
        l.bands.name = layout->name;
    }

    gdal::DatasetPtr main = l.vrt
                                ? gdal::openRaster(l.datasetPath, kVrtDriver)
                                : gdal::openRaster(l.datasetPath, gdal::kOrthoDrivers, l.siblings);
    if (!main) {
        return terrainError(TerrainErrorCode::CorruptData,
                            "cannot open the orthophotos: " + gdal::lastError());
    }
    auto* mh = static_cast<GDALDatasetH>(main.get());
    l.width = GDALGetRasterXSize(mh);
    l.height = GDALGetRasterYSize(mh);
    meta.widthPx = l.width;
    meta.heightPx = l.height;
    if (l.width > options.maxDimensionPx || l.height > options.maxDimensionPx) {
        return terrainError(
            TerrainErrorCode::TooLarge,
            std::format("the orthophoto mosaic is {} × {} pixels, over the limit of {} × {}",
                        l.width, l.height, options.maxDimensionPx, options.maxDimensionPx));
    }
    std::array<double, 6> gt{};
    GDALGetGeoTransform(mh, gt.data());
    if (GDALInvGeoTransform(gt.data(), l.inverse.data()) == 0) {
        return terrainError(TerrainErrorCode::CorruptData, "degenerate georeferencing");
    }
    {
        int has = 0;
        const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(mh, l.bands.colour[0]), &has);
        if (has != 0) {
            l.noData = nd;
        }
    }

    OGRSpatialReference srs = *crs;
    if (srs.IsCompound()) {
        srs.StripVertical();
    }
    srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    meta.crs = srs.GetName() != nullptr ? srs.GetName() : "unnamed CRS";
    if (const char* auth = srs.GetAuthorityName(nullptr); auth != nullptr) {
        if (const char* code = srs.GetAuthorityCode(nullptr); code != nullptr) {
            meta.crsCode = std::format("{}:{}", auth, code);
        }
    }
    if (fileWithoutCrs) {
        meta.crsCode = options.crs;
    }
    {
        char* wkt = nullptr;
        const char* const wktOptions[] = {"FORMAT=WKT2_2019", nullptr};
        srs.exportToWkt(&wkt, wktOptions);
        l.srsWkt = wkt != nullptr ? wkt : "";
        CPLFree(wkt);
    }
    l.frame.emplace(origin);
    l.lut = colourTable(options.color);
    l.adjust = !options.color.isIdentity();

    // Pixel size on the ground and the coverage box in the world frame.
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr toWgs84(OGRCreateCoordinateTransformation(&srs, &wgs84));
    if (!toWgs84) {
        return terrainError(
            TerrainErrorCode::MissingCrs,
            std::format("no transformation from {} to WGS84: {}", meta.crs, gdal::lastError()));
    }
    if (srs.IsGeographic()) {
        double cy = gt[3] + gt[4] * 0.5 * l.width + gt[5] * 0.5 * l.height;
        const double cosLat = std::cos(cy * std::numbers::pi / 180.0);
        l.pixelSizeM = 0.5 * (std::hypot(gt[1], gt[4]) * 111'320.0 * cosLat +
                              std::hypot(gt[2], gt[5]) * 110'574.0);
    } else {
        l.pixelSizeM =
            0.5 * (std::hypot(gt[1], gt[4]) + std::hypot(gt[2], gt[5])) * srs.GetLinearUnits();
    }
    meta.resolutionM = l.pixelSizeM;
    meta.minX = meta.minZ = std::numeric_limits<double>::infinity();
    meta.maxX = meta.maxZ = -std::numeric_limits<double>::infinity();
    for (int edge = 0; edge < 4; ++edge) {
        for (int i = 0; i <= kBorderSamples; ++i) {
            const double t = static_cast<double>(i) / kBorderSamples;
            double pu = 0.0;
            double pv = 0.0;
            switch (edge) {
                case 0: pu = t * l.width; break;
                case 1:
                    pu = t * l.width;
                    pv = l.height;
                    break;
                case 2: pv = t * l.height; break;
                default:
                    pu = l.width;
                    pv = t * l.height;
                    break;
            }
            double X = gt[0] + gt[1] * pu + gt[2] * pv;
            double Y = gt[3] + gt[4] * pu + gt[5] * pv;
            if (!toWgs84->Transform(1, &X, &Y)) {
                continue;
            }
            const geo::Enu e = l.frame->toEnu(geo::Geodetic{Y, X, origin.heightM});
            meta.minX = std::min(meta.minX, e.east);
            meta.maxX = std::max(meta.maxX, e.east);
            meta.minZ = std::min(meta.minZ, -e.north);
            meta.maxZ = std::max(meta.maxZ, -e.north);
        }
    }
    if (!(meta.minX <= meta.maxX)) {
        return terrainError(TerrainErrorCode::CorruptData,
                            "the orthophotos have no geographic position");
    }
    // Curvature between border samples and the frame's tangent plane.
    const double pad = 2.0 + 1e-4 * std::max(meta.maxX - meta.minX, meta.maxZ - meta.minZ);
    meta.minX -= pad;
    meta.minZ -= pad;
    meta.maxX += pad;
    meta.maxZ += pad;

    auto first = source->makeReader();
    if (!first) {
        return std::unexpected(first.error());
    }
    source->release(std::move(*first));

    if (!meta.hasOverviews) {
        meta.notes.emplace_back(
            "no overviews: distant imagery reads slowly (a COG with overviews is made and "
            "cached for large files)");
    }
    source->description_ =
        std::format("orthophoto {} ({}, {}, {}, {:.2g} m/px, {} × {} px)", fileList(files),
                    meta.format, meta.crsCode.empty() ? meta.crs : meta.crsCode, meta.bands,
                    meta.resolutionM, l.width, l.height);
    log::info("Opened {}; coverage x {:.0f} … {:.0f} m, z {:.0f} … {:.0f} m", source->description_,
              meta.minX, meta.maxX, meta.minZ, meta.maxZ);
    return source;
}

// ---------------------------------------------------------------------------------------------
// Cached COG (FR-TER-I06)
// ---------------------------------------------------------------------------------------------

bool orthoNeedsCache(const fs::path& file, int directMaxPx) {
    gdal::ensureInitialised();
    gdal::DatasetPtr ds = openImage(file);
    if (!ds) {
        return false;
    }
    auto* h = static_cast<GDALDatasetH>(ds.get());
    if (GDALGetRasterCount(h) < 1) {
        return false;
    }
    const int w = GDALGetRasterXSize(h);
    const int ht = GDALGetRasterYSize(h);
    if (std::max(w, ht) <= directMaxPx) {
        return false;
    }
    const std::string driver = GDALGetDriverShortName(GDALGetDatasetDriver(h));
    if (driver == "PNG" || driver == "JPEG" || driver == "WEBP") {
        return true;  // Decoded from the top for every window.
    }
    return GDALGetOverviewCount(GDALGetRasterBand(h, 1)) == 0;
}

TerrainResult<std::string> orthoCacheKey(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    const auto time = fs::last_write_time(file, ec);
    if (ec) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            std::format("{}: not found", file.string()));
    }
    // Bump the version when the conversion changes, so older copies are rebuilt.
    return common::sha256Hex(std::format("ds-ortho-cog 1\n{}\t{}\t{}\n",
                                         fs::weakly_canonical(file, ec).string(), size,
                                         time.time_since_epoch().count()));
}

TerrainResult<void> buildOrthoCog(const fs::path& file, const fs::path& output,
                                  std::stop_token stop, const OrthoProgress& progress) {
    gdal::ensureInitialised();
    gdal::DatasetPtr ds = openImage(file);
    if (!ds) {
        return terrainError(TerrainErrorCode::UnsupportedFormat,
                            std::format("\"{}\" is not a readable orthophoto: {}",
                                        file.filename().string(), gdal::lastError()));
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    const fs::path partial = output.parent_path() /
                             std::format(".{}.{}.partial", output.filename().string(), ::getpid());
    char** cog = nullptr;
    cog = CSLSetNameValue(cog, "COMPRESS", "DEFLATE");
    cog = CSLSetNameValue(cog, "PREDICTOR", "YES");
    cog = CSLSetNameValue(cog, "BLOCKSIZE", "512");
    cog = CSLSetNameValue(cog, "OVERVIEWS", "AUTO");
    cog = CSLSetNameValue(cog, "RESAMPLING", "AVERAGE");
    cog = CSLSetNameValue(cog, "BIGTIFF", "IF_SAFER");
    cog = CSLSetNameValue(cog, "NUM_THREADS", "ALL_CPUS");
    CopyProgress cp{std::move(stop), &progress};
    GDALDatasetH out =
        GDALCreateCopy(GDALGetDriverByName("COG"), partial.c_str(),
                       static_cast<GDALDatasetH>(ds.get()), FALSE, cog, onCopyProgress, &cp);
    CSLDestroy(cog);
    const bool ok = out != nullptr;
    if (out != nullptr) {
        GDALClose(out);
    }
    if (!ok || cp.stop.stop_requested()) {
        fs::remove(partial, ec);
        if (cp.stop.stop_requested()) {
            return terrainError(TerrainErrorCode::Cancelled, "cancelled");
        }
        return terrainError(TerrainErrorCode::CorruptData,
                            std::format("cannot convert \"{}\" to COG: {}",
                                        file.filename().string(), gdal::lastError()));
    }
    fs::rename(partial, output, ec);
    if (ec) {
        fs::remove(partial, ec);
        return terrainError(TerrainErrorCode::CorruptData,
                            std::format("cannot store the orthophoto copy: {}", ec.message()));
    }
    if (progress) {
        progress(1.0);
    }
    log::info("Orthophoto {} converted to a cached COG", file.filename().string());
    return {};
}

}  // namespace ds::terrain
