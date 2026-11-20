// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/raster_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <utility>

#include "ds/common/log.hpp"

#include "gdal_support.hpp"
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_utils.h>
#include <ogr_spatialref.h>

namespace ds::terrain {
namespace {

constexpr int kFillRadiusPx = 8;      // NoData voids up to this distance are interpolated.
constexpr int kStatsSizePx = 512;     // Longest side of the reduced read for min/max/NoData.
constexpr int kCoverageSamples = 64;  // Points per side when checking a rectangle is covered.
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::array<const char*, 2> kVrtDriver{"VRT", nullptr};

struct TransformDeleter {
    void operator()(OGRCoordinateTransformation* t) const noexcept {
        OGRCoordinateTransformation::DestroyCT(t);
    }
};
using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, TransformDeleter>;

bool isAllowedType(GDALDataType type) {
    return type == GDT_Int16 || type == GDT_UInt16 || type == GDT_Int32 || type == GDT_Float32 ||
           type == GDT_Float64;
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Metres per unit of a band unit name; nullopt if unknown.
std::optional<std::pair<double, std::string>> verticalUnit(const std::string& name) {
    const std::string n = lower(name);
    if (n.empty() || n == "m" || n == "metre" || n == "meter" || n == "metres" || n == "meters") {
        return std::pair{1.0, std::string("m")};
    }
    if (n == "ft" || n == "foot" || n == "feet" || n == "international foot") {
        return std::pair{0.3048, std::string("ft")};
    }
    if (n == "us survey foot" || n == "us-ft" || n == "ftus" || n == "foot_us" ||
        n == "us_survey_foot") {
        return std::pair{1200.0 / 3937.0, std::string("US ft")};
    }
    return std::nullopt;
}

std::string fileList(std::span<const std::filesystem::path> files) {
    if (files.size() == 1) {
        return files.front().filename().string();
    }
    return std::format("{} files", files.size());
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------------------------

struct RasterSource::Layout {
    std::string datasetPath;  // File, or the in-memory VRT of a mosaic.
    std::string vrtPath;      // In-memory VRT to delete ("" if none).
    std::span<const char* const> drivers;
    int band = 1;
    std::array<double, 6> inverse{};  // Georeferenced → pixel.
    int width = 0;
    int height = 0;
    double scale = 1.0;  // Raw → file unit.
    double offset = 0.0;
    double unitFactor = 1.0;  // File unit → metres.
    std::optional<double> noData;
    std::string srsWkt;  // Horizontal CRS of the raster.
    std::optional<geo::LocalFrame> frame;
    double pixelSizeM = 1.0;
    double fullDetailStepM = 2.0;
};

struct RasterSource::Reader {
    gdal::DatasetPtr dataset;
    GDALRasterBandH band = nullptr;
    TransformPtr toRaster;  // Longitude/latitude (WGS84) → raster CRS.
    std::vector<double> lon, lat, u, v, window;
};

class RasterSource::Lease {
public:
    Lease(const RasterSource& owner, std::unique_ptr<Reader> reader)
        : owner_(&owner), reader_(std::move(reader)) {}
    ~Lease() {
        if (reader_) {
            owner_->release(std::move(reader_));
        }
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept = default;
    Lease& operator=(Lease&&) = delete;
    [[nodiscard]] Reader* get() const noexcept { return reader_.get(); }

private:
    const RasterSource* owner_;
    std::unique_ptr<Reader> reader_;
};

RasterSource::RasterSource() : layout_(std::make_unique<Layout>()) {}

RasterSource::~RasterSource() {
    readers_.clear();  // Close every handle before the VRT goes.
    if (!layout_->vrtPath.empty()) {
        VSIUnlink(layout_->vrtPath.c_str());
    }
}

TerrainResult<std::unique_ptr<RasterSource::Reader>> RasterSource::makeReader() const {
    auto reader = std::make_unique<Reader>();
    reader->dataset = gdal::openRaster(layout_->datasetPath, layout_->drivers);
    if (!reader->dataset) {
        return terrainError(TerrainErrorCode::FileNotFound,
                            "cannot reopen the elevation data: " + gdal::lastError());
    }
    reader->band =
        GDALGetRasterBand(static_cast<GDALDatasetH>(reader->dataset.get()), layout_->band);
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference target;
    if (target.importFromWkt(layout_->srsWkt.c_str()) != OGRERR_NONE) {
        return terrainError(TerrainErrorCode::MissingCrs, "unusable CRS");
    }
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    reader->toRaster.reset(OGRCreateCoordinateTransformation(&wgs84, &target));
    if (!reader->toRaster) {
        return terrainError(TerrainErrorCode::MissingCrs,
                            "no transformation from WGS84 to the data's CRS: " + gdal::lastError());
    }
    return reader;
}

RasterSource::Lease RasterSource::acquire() const {
    {
        const std::scoped_lock lock(readersMutex_);
        if (!readers_.empty()) {
            auto reader = std::move(readers_.back());
            readers_.pop_back();
            return Lease(*this, std::move(reader));
        }
    }
    auto reader = makeReader();
    if (!reader) {
        // The file opened before, so this only happens if it vanished meanwhile.
        ++readErrors_;
        log::warn("Elevation data: {}", reader.error().message);
        return Lease(*this, nullptr);
    }
    return Lease(*this, std::move(*reader));
}

void RasterSource::release(std::unique_ptr<Reader> reader) const {
    const std::scoped_lock lock(readersMutex_);
    readers_.push_back(std::move(reader));
}

int RasterSource::decimationFor(double step) const noexcept {
    const Layout& l = *layout_;
    if (step <= l.fullDetailStepM * (1.0 + 1e-9)) {
        return 1;
    }
    int d = 1;
    while (2.0 * d * l.pixelSizeM <= 0.5 * step && l.width / (2 * d) >= 2 &&
           l.height / (2 * d) >= 2) {
        d *= 2;
    }
    return d;
}

// ---------------------------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------------------------

void RasterSource::sampleAmsl(Reader& reader, std::span<const double> xs,
                              std::span<const double> zs, int d, std::span<double> out) const {
    const Layout& l = *layout_;
    const std::size_t n = xs.size();
    reader.lon.resize(n);
    reader.lat.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const geo::Geodetic g = l.frame->toGeodetic(geo::Enu{xs[i], -zs[i], 0.0});
        reader.lon[i] = g.longitudeDeg;
        reader.lat[i] = g.latitudeDeg;
    }
    std::vector<int> ok(n, 0);
    reader.toRaster->Transform(n, reader.lon.data(), reader.lat.data(), nullptr, ok.data());

    // Continuous coordinates on the decimated grid, integer at pixel centres.
    const int wd = l.width / d;
    const int hd = l.height / d;
    reader.u.resize(n);
    reader.v.resize(n);
    int i0 = std::numeric_limits<int>::max();
    int j0 = i0;
    int i1 = std::numeric_limits<int>::min();
    int j1 = i1;
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (ok[i] == 0 || !std::isfinite(reader.lon[i]) || !std::isfinite(reader.lat[i])) {
            reader.u[i] = kNaN;
            continue;
        }
        const double X = reader.lon[i];
        const double Y = reader.lat[i];
        const double pu = l.inverse[0] + l.inverse[1] * X + l.inverse[2] * Y;
        const double pv = l.inverse[3] + l.inverse[4] * X + l.inverse[5] * Y;
        const double U = std::clamp(pu / d - 0.5, 0.0, static_cast<double>(wd - 1));
        const double V = std::clamp(pv / d - 0.5, 0.0, static_cast<double>(hd - 1));
        reader.u[i] = U;
        reader.v[i] = V;
        const int ci = std::min(static_cast<int>(U), wd - 2);
        const int cj = std::min(static_cast<int>(V), hd - 2);
        i0 = std::min(i0, ci);
        j0 = std::min(j0, cj);
        i1 = std::max(i1, ci + 2);
        j1 = std::max(j1, cj + 2);
        any = true;
    }
    if (!any) {
        std::ranges::fill(out, fallbackAmsl_);
        return;
    }

    // Reads decimated pixels [a0, a1) × [b0, b1) into reader.window as metres AMSL (NaN =
    // NoData). Windows are aligned to the decimation, so every pixel has the same value
    // whichever window it is read in.
    int wx0 = 0;
    int wy0 = 0;
    int wnx = 0;
    int wny = 0;
    auto readWindow = [&](int a0, int b0, int a1, int b1) {
        wx0 = a0;
        wy0 = b0;
        wnx = a1 - a0;
        wny = b1 - b0;
        reader.window.resize(static_cast<std::size_t>(wnx) * static_cast<std::size_t>(wny));
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = d > 1 ? GRIORA_Average : GRIORA_NearestNeighbour;
        const CPLErr err =
            GDALRasterIOEx(reader.band, GF_Read, a0 * d, b0 * d, wnx * d, wny * d,
                           reader.window.data(), wnx, wny, GDT_Float64, 0, 0, &extra);
        if (err != CE_None) {
            ++readErrors_;
            std::ranges::fill(reader.window, kNaN);
            return;
        }
        for (double& h : reader.window) {
            if (std::isnan(h) || (l.noData && h == *l.noData)) {
                h = kNaN;
            } else {
                h = (h * l.scale + l.offset) * l.unitFactor;
            }
        }
    };
    auto at = [&](int i, int j) {
        return reader.window[static_cast<std::size_t>(j - wy0) * static_cast<std::size_t>(wnx) +
                             static_cast<std::size_t>(i - wx0)];
    };

    readWindow(i0, j0, i1, j1);
    bool voids = false;
    for (std::size_t i = 0; i < n && !voids; ++i) {
        if (std::isnan(reader.u[i])) {
            continue;
        }
        const int ci = std::min(static_cast<int>(reader.u[i]), wd - 2);
        const int cj = std::min(static_cast<int>(reader.v[i]), hd - 2);
        voids = std::isnan(at(ci, cj)) || std::isnan(at(ci + 1, cj)) ||
                std::isnan(at(ci, cj + 1)) || std::isnan(at(ci + 1, cj + 1));
    }
    if (voids) {
        // Read again with a margin, so every void pixel sees all its neighbours within the
        // fill radius (clamped only by the raster border, which is the same for every read).
        readWindow(std::max(0, i0 - kFillRadiusPx), std::max(0, j0 - kFillRadiusPx),
                   std::min(wd, i1 + kFillRadiusPx), std::min(hd, j1 + kFillRadiusPx));
    }
    auto value = [&](int i, int j) {
        const double h = at(i, j);
        if (!std::isnan(h)) {
            return h;
        }
        double sum = 0.0;
        double weights = 0.0;
        for (int b = std::max(0, j - kFillRadiusPx); b <= std::min(hd - 1, j + kFillRadiusPx);
             ++b) {
            for (int a = std::max(0, i - kFillRadiusPx); a <= std::min(wd - 1, i + kFillRadiusPx);
                 ++a) {
                const int r2 = (a - i) * (a - i) + (b - j) * (b - j);
                if (r2 > kFillRadiusPx * kFillRadiusPx) {
                    continue;
                }
                const double s = at(a, b);
                if (!std::isnan(s)) {
                    const double w = 1.0 / static_cast<double>(r2);
                    sum += w * s;
                    weights += w;
                }
            }
        }
        return weights > 0.0 ? sum / weights : fallbackAmsl_;
    };

    for (std::size_t i = 0; i < n; ++i) {
        if (std::isnan(reader.u[i])) {
            out[i] = fallbackAmsl_;
            continue;
        }
        const double U = reader.u[i];
        const double V = reader.v[i];
        const int ci = std::min(static_cast<int>(U), wd - 2);
        const int cj = std::min(static_cast<int>(V), hd - 2);
        const double fx = U - ci;
        const double fy = V - cj;
        const double h00 = value(ci, cj);
        const double h10 = value(ci + 1, cj);
        const double h01 = value(ci, cj + 1);
        const double h11 = value(ci + 1, cj + 1);
        out[i] = (h00 * (1.0 - fx) + h10 * fx) * (1.0 - fy) + (h01 * (1.0 - fx) + h11 * fx) * fy;
    }
}

double RasterSource::heightAmsl(double x, double z) const {
    const Lease lease = acquire();
    if (lease.get() == nullptr) {
        return fallbackAmsl_;
    }
    std::array<double, 1> out{};
    sampleAmsl(*lease.get(), std::span(&x, 1), std::span(&z, 1), 1, out);
    return out[0];
}

double RasterSource::height(double x, double z) const { return heightAmsl(x, z) - originAltitude_; }

void RasterSource::sampleGrid(double x0, double z0, double step, int cols, int rows,
                              std::span<double> out) const {
    const std::size_t n = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    std::vector<double> xs(n);
    std::vector<double> zs(n);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const std::size_t i = static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
                                  static_cast<std::size_t>(col);
            xs[i] = x0 + col * step;
            zs[i] = z0 + row * step;
        }
    }
    const Lease lease = acquire();
    if (lease.get() == nullptr) {
        std::ranges::fill(out.first(n), fallbackAmsl_ - originAltitude_);
        return;
    }
    sampleAmsl(*lease.get(), xs, zs, decimationFor(step), out.first(n));
    for (double& h : out.first(n)) {
        h -= originAltitude_;
    }
}

std::string RasterSource::describe() const { return description_; }

// ---------------------------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------------------------

TerrainResult<std::shared_ptr<RasterSource>> RasterSource::open(
    std::span<const std::filesystem::path> files, const RasterOptions& options) {
    if (files.empty()) {
        return terrainError(TerrainErrorCode::InvalidSpec, "no elevation files");
    }
    if (options.band < 1) {
        return terrainError(TerrainErrorCode::InvalidSpec, "band must be 1 or more");
    }
    gdal::ensureInitialised();
    std::shared_ptr<RasterSource> source(new RasterSource());
    Layout& l = *source->layout_;
    l.band = options.band;
    l.fullDetailStepM = options.fullDetailStepM;
    SourceMetadata& meta = source->metadata_;
    meta.fileCount = static_cast<int>(files.size());

    OGRSpatialReference override;
    if (!options.crs.empty() && override.SetFromUserInput(options.crs.c_str()) != OGRERR_NONE) {
        return terrainError(TerrainErrorCode::InvalidSpec,
                            std::format("unknown CRS \"{}\"", options.crs));
    }

    // Every file on its own: format, band, data type, georeferencing, CRS.
    std::optional<OGRSpatialReference> crs;
    std::vector<std::string> drivers;
    bool fileWithoutCrs = false;
    for (const std::filesystem::path& file : files) {
        const std::string name = file.filename().string();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return terrainError(TerrainErrorCode::FileNotFound,
                                std::format("elevation file \"{}\" not found", name));
        }
        gdal::DatasetPtr ds = gdal::openRaster(file.string(), gdal::kElevationDrivers);
        if (!ds) {
            return terrainError(TerrainErrorCode::UnsupportedFormat,
                                std::format("\"{}\" is not a readable elevation raster "
                                            "(COG, GeoTIFF, .hgt, .asc, .img): {}",
                                            name, gdal::lastError()));
        }
        auto* h = static_cast<GDALDatasetH>(ds.get());
        const std::string driver = GDALGetDriverShortName(GDALGetDatasetDriver(h));
        if (std::ranges::find(drivers, driver) == drivers.end()) {
            drivers.push_back(driver);
        }
        if (GDALGetRasterCount(h) < options.band) {
            return terrainError(TerrainErrorCode::UnsupportedFormat,
                                std::format("\"{}\" has {} band(s), band {} was asked for", name,
                                            GDALGetRasterCount(h), options.band));
        }
        GDALRasterBandH band = GDALGetRasterBand(h, options.band);
        if (!isAllowedType(GDALGetRasterDataType(band))) {
            return terrainError(
                TerrainErrorCode::UnsupportedFormat,
                std::format("\"{}\": elevation data type {} is not supported (Int16, UInt16, "
                            "Int32, Float32, Float64)",
                            name, GDALGetDataTypeName(GDALGetRasterDataType(band))));
        }
        if (GDALGetRasterXSize(h) < 2 || GDALGetRasterYSize(h) < 2) {
            return terrainError(TerrainErrorCode::CorruptData,
                                std::format("\"{}\" is smaller than 2 × 2 pixels", name));
        }
        if (GDALGetRasterXSize(h) > options.maxDimensionPx ||
            GDALGetRasterYSize(h) > options.maxDimensionPx) {
            return terrainError(TerrainErrorCode::TooLarge,
                                std::format("\"{}\" is {} × {} pixels, over the limit of {} × {}",
                                            name, GDALGetRasterXSize(h), GDALGetRasterYSize(h),
                                            options.maxDimensionPx, options.maxDimensionPx));
        }
        std::array<double, 6> gt{};
        if (GDALGetGeoTransform(h, gt.data()) != CE_None) {
            return terrainError(TerrainErrorCode::CorruptData,
                                std::format("\"{}\" has no georeferencing", name));
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
                                std::format("\"{}\" has no CRS; give \"crs\" in the mission "
                                            "(e.g. \"EPSG:25830\")",
                                            name));
        }
        if (!crs) {
            crs = thisCrs;
        } else if (!crs->IsSame(&thisCrs)) {
            return terrainError(TerrainErrorCode::MixedCrs,
                                std::format("\"{}\" is in a different CRS from \"{}\"; all files "
                                            "of a terrain must share one CRS",
                                            name, files.front().filename().string()));
        }
    }
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        meta.format += (i > 0 ? ", " : "") + drivers[i];
    }

    // The dataset that is read: the file itself, or an in-memory VRT mosaic.
    if (files.size() == 1) {
        l.datasetPath = files.front().string();
        l.drivers = gdal::kElevationDrivers;
    } else {
        l.vrtPath = gdal::uniqueMemPath("dem_mosaic", ".vrt");
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
        args.AddString("-b");
        args.AddString(std::to_string(options.band).c_str());
        GDALBuildVRTOptions* vrtOptions = GDALBuildVRTOptionsNew(args.List(), nullptr);
        int usageError = 0;
        GDALDatasetH vrt = GDALBuildVRT(l.vrtPath.c_str(), static_cast<int>(namePtrs.size()),
                                        nullptr, namePtrs.data(), vrtOptions, &usageError);
        GDALBuildVRTOptionsFree(vrtOptions);
        if (vrt == nullptr) {
            return terrainError(TerrainErrorCode::CorruptData,
                                "cannot mosaic the elevation files: " + gdal::lastError());
        }
        GDALClose(vrt);
        l.datasetPath = l.vrtPath;
        l.drivers = kVrtDriver;
        l.band = 1;  // The VRT holds only the chosen band.
    }

    gdal::DatasetPtr main = gdal::openRaster(l.datasetPath, l.drivers);
    if (!main) {
        return terrainError(TerrainErrorCode::CorruptData,
                            "cannot open the elevation data: " + gdal::lastError());
    }
    auto* mh = static_cast<GDALDatasetH>(main.get());
    l.width = GDALGetRasterXSize(mh);
    l.height = GDALGetRasterYSize(mh);
    meta.widthPx = l.width;
    meta.heightPx = l.height;
    if (l.width > options.maxDimensionPx || l.height > options.maxDimensionPx) {
        return terrainError(
            TerrainErrorCode::TooLarge,
            std::format("the mosaic is {} × {} pixels, over the limit of {} × {}", l.width,
                        l.height, options.maxDimensionPx, options.maxDimensionPx));
    }
    std::array<double, 6> gt{};
    GDALGetGeoTransform(mh, gt.data());
    if (GDALInvGeoTransform(gt.data(), l.inverse.data()) == 0) {
        return terrainError(TerrainErrorCode::CorruptData, "degenerate georeferencing");
    }
    GDALRasterBandH band = GDALGetRasterBand(mh, l.band);
    int has = 0;
    const double noData = GDALGetRasterNoDataValue(band, &has);
    if (has != 0) {
        l.noData = noData;
    }
    l.scale = GDALGetRasterScale(band, &has);
    if (has == 0 || l.scale == 0.0) {
        l.scale = 1.0;
    }
    l.offset = GDALGetRasterOffset(band, nullptr);
    meta.hasOverviews = GDALGetOverviewCount(band) > 0;

    // CRS: horizontal part for the transformation, vertical part for datum and unit.
    OGRSpatialReference srs = *crs;
    if (const char* vert = srs.GetAttrValue("VERT_CS"); vert != nullptr) {
        meta.verticalDatum = vert;
    }
    std::string unitName =
        GDALGetRasterUnitType(band) != nullptr ? GDALGetRasterUnitType(band) : "";
    std::optional<std::pair<double, std::string>> unit = verticalUnit(unitName);
    if (unitName.empty() && srs.IsCompound()) {
        const char* vertUnit = nullptr;
        const double factor = srs.GetTargetLinearUnits("VERT_CS", &vertUnit);
        if (factor > 0.0 && std::abs(factor - 1.0) > 1e-9) {
            unit = std::pair{factor, std::string(vertUnit != nullptr ? vertUnit : "?")};
        }
    }
    if (!unit) {
        return terrainError(
            TerrainErrorCode::UnsupportedFormat,
            std::format("elevation unit \"{}\" is not supported (metres or feet)", unitName));
    }
    l.unitFactor = unit->first;
    meta.verticalUnit = unit->second;
    if (!options.verticalDatum.empty()) {
        meta.verticalDatum = options.verticalDatum;
    }
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

    // Origin: given, or the centre of the data.
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr toWgs84(OGRCreateCoordinateTransformation(&srs, &wgs84));
    if (!toWgs84) {
        return terrainError(
            TerrainErrorCode::MissingCrs,
            std::format("no transformation from {} to WGS84: {}", meta.crs, gdal::lastError()));
    }
    double cx = gt[0] + gt[1] * 0.5 * l.width + gt[2] * 0.5 * l.height;
    double cy = gt[3] + gt[4] * 0.5 * l.width + gt[5] * 0.5 * l.height;
    if (!toWgs84->Transform(1, &cx, &cy)) {
        return terrainError(TerrainErrorCode::CorruptData,
                            "the centre of the data has no geographic position");
    }
    const geo::LatLon origin = options.origin.value_or(geo::LatLon{cy, cx});
    l.frame.emplace(geo::Geodetic{origin.latitudeDeg, origin.longitudeDeg, 0.0});
    if (srs.IsGeographic()) {
        const double cosLat = std::cos(cy * std::numbers::pi / 180.0);
        l.pixelSizeM = 0.5 * (std::hypot(gt[1], gt[4]) * 111'320.0 * cosLat +
                              std::hypot(gt[2], gt[5]) * 110'574.0);
    } else {
        l.pixelSizeM =
            0.5 * (std::hypot(gt[1], gt[4]) + std::hypot(gt[2], gt[5])) * srs.GetLinearUnits();
    }
    meta.resolutionM = l.pixelSizeM;

    auto first = source->makeReader();
    if (!first) {
        return std::unexpected(first.error());
    }
    Reader& reader = **first;

    // Coverage: a world point is covered if it maps inside the raster.
    auto covered = [&](double x, double z) {
        const geo::Geodetic g = l.frame->toGeodetic(geo::Enu{x, -z, 0.0});
        double X = g.longitudeDeg;
        double Y = g.latitudeDeg;
        if (!reader.toRaster->Transform(1, &X, &Y)) {
            return false;
        }
        const double u = l.inverse[0] + l.inverse[1] * X + l.inverse[2] * Y;
        const double v = l.inverse[3] + l.inverse[4] * X + l.inverse[5] * Y;
        return u >= 0.0 && v >= 0.0 && u <= l.width && v <= l.height;
    };
    if (!covered(0.0, 0.0)) {
        return terrainError(TerrainErrorCode::NotCovered,
                            std::format("the origin {:.6f}, {:.6f} is outside the elevation data",
                                        origin.latitudeDeg, origin.longitudeDeg));
    }
    // Reduced read of the whole raster: range, NoData share and the fallback height.
    {
        const int longest = std::max(l.width, l.height);
        const int stride = std::max(1, (longest + kStatsSizePx - 1) / kStatsSizePx);
        const int nx = std::max(1, l.width / stride);
        const int ny = std::max(1, l.height / stride);
        std::vector<double> values(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = stride > 1 ? GRIORA_Average : GRIORA_NearestNeighbour;
        if (GDALRasterIOEx(reader.band, GF_Read, 0, 0, nx * stride, ny * stride, values.data(), nx,
                           ny, GDT_Float64, 0, 0, &extra) != CE_None) {
            return terrainError(TerrainErrorCode::CorruptData,
                                "cannot read the elevation data: " + gdal::lastError());
        }
        double lowest = std::numeric_limits<double>::infinity();
        double highest = -lowest;
        std::size_t voids = 0;
        for (const double raw : values) {
            if (std::isnan(raw) || (l.noData && raw == *l.noData)) {
                ++voids;
                continue;
            }
            const double h = (raw * l.scale + l.offset) * l.unitFactor;
            lowest = std::min(lowest, h);
            highest = std::max(highest, h);
        }
        if (voids == values.size()) {
            return terrainError(TerrainErrorCode::CorruptData,
                                "the elevation data holds no valid heights (all NoData)");
        }
        meta.minHeightM = lowest;
        meta.maxHeightM = highest;
        meta.noDataPercent =
            100.0 * static_cast<double>(voids) / static_cast<double>(values.size());
        source->fallbackAmsl_ = lowest;
    }

    std::array<double, 1> originHeight{};
    {
        const double zero = 0.0;
        source->sampleAmsl(reader, std::span(&zero, 1), std::span(&zero, 1), 1, originHeight);
    }
    source->originAltitude_ = options.originAltitudeM.value_or(originHeight[0]);
    meta.origin = geo::Geodetic{origin.latitudeDeg, origin.longitudeDeg, source->originAltitude_};
    // The scene's local frame sits at the origin altitude (geo::LocalFrame(origin) in the
    // app), which scales horizontal distances by (R + h) / R: use exactly that frame.
    l.frame.emplace(meta.origin);

    auto rectangleCovered = [&](double halfW, double halfL) {
        for (int i = 0; i <= kCoverageSamples; ++i) {
            const double t = -1.0 + 2.0 * i / kCoverageSamples;
            if (!covered(t * halfW, -halfL) || !covered(t * halfW, halfL) ||
                !covered(-halfW, t * halfL) || !covered(halfW, t * halfL)) {
                return false;
            }
        }
        return true;
    };
    if (options.extent) {
        if (!rectangleCovered(0.5 * options.extent->widthM, 0.5 * options.extent->lengthM)) {
            return terrainError(TerrainErrorCode::NotCovered,
                                std::format("the extent {:.0f} × {:.0f} m around the origin is "
                                            "not covered by the elevation data",
                                            options.extent->widthM, options.extent->lengthM));
        }
        meta.extent = *options.extent;
    } else {
        // Reach of the data from the origin along each axis, then the largest rectangle with
        // those proportions that lies inside the data.
        auto reach = [&](double dx, double dz) {
            double lo = 0.0;
            double hi = 0.5 * kMaxExtentM;
            if (covered(hi * dx, hi * dz)) {
                return hi;
            }
            for (int i = 0; i < 40; ++i) {
                const double mid = 0.5 * (lo + hi);
                (covered(mid * dx, mid * dz) ? lo : hi) = mid;
            }
            return lo;
        };
        const double a0 = std::min(reach(1.0, 0.0), reach(-1.0, 0.0));
        const double b0 = std::min(reach(0.0, 1.0), reach(0.0, -1.0));
        double lo = 0.0;
        double hi = 1.0;
        if (!rectangleCovered(a0, b0)) {
            for (int i = 0; i < 30; ++i) {
                const double mid = 0.5 * (lo + hi);
                (rectangleCovered(mid * a0, mid * b0) ? lo : hi) = mid;
            }
        } else {
            lo = 1.0;
        }
        meta.extent = Extent{2.0 * std::floor(lo * a0), 2.0 * std::floor(lo * b0)};
        if (meta.extent.widthM < kMinExtentM || meta.extent.lengthM < kMinExtentM) {
            return terrainError(
                TerrainErrorCode::NotCovered,
                std::format("the elevation data around the origin covers only {:.0f} × {:.0f} m "
                            "(at least {:.0f} m a side is needed)",
                            meta.extent.widthM, meta.extent.lengthM, kMinExtentM));
        }
    }

    source->release(std::move(*first));

    if (!meta.hasOverviews && std::max(l.width, l.height) > 4096) {
        meta.notes.push_back(
            "large raster without overviews: converting it to COG (gdal_translate -of COG) "
            "makes distant terrain load faster");
    }
    source->description_ = std::format(
        "elevation raster {} ({}, {}, {:.2g} m/px, {} × {} px)", fileList(files), meta.format,
        meta.crsCode.empty() ? meta.crs : meta.crsCode, meta.resolutionM, l.width, l.height);
    log::info("Opened {}; extent {:.0f} × {:.0f} m, heights {:.1f} … {:.1f} m{}",
              source->description_, meta.extent.widthM, meta.extent.lengthM, meta.minHeightM,
              meta.maxHeightM,
              meta.noDataPercent > 0.0 ? std::format(", {:.1f} % NoData", meta.noDataPercent)
                                       : std::string());
    return source;
}

}  // namespace ds::terrain
