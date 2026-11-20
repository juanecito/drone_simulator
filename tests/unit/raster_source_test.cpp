// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/raster_source.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ds/geo/coordinates.hpp"
#include "ds/geo/utm.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/query.hpp"

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>
#include <unistd.h>

namespace {

using namespace ds::terrain;
namespace fs = std::filesystem;

// UTM zone 30N tile near Madrid, 10 m pixels.
constexpr double kE0 = 440'000.0;
constexpr double kN0 = 4'476'000.0;
constexpr int kUtmEpsg = 32630;

struct RasterSpec {
    int width = 200;
    int height = 200;
    std::array<double, 6> geoTransform{kE0, 10.0, 0.0, kN0, 0.0, -10.0};
    int epsg = kUtmEpsg;  // 0 = no CRS.
    GDALDataType type = GDT_Float32;
    std::optional<double> noData;
    double scale = 1.0;
    double offset = 0.0;
    std::string unit;
    std::string driver = "GTiff";
    std::vector<std::string> creationOptions;
    bool overviews = false;
    // Raw pixel value at column i, row j.
    std::function<double(int i, int j)> value;
};

class TempDir {
public:
    TempDir() : path_(fs::temp_directory_path() / std::format("ds_raster_{}", ::getpid())) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() { fs::remove_all(path_); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] fs::path operator/(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

void writeRaster(const fs::path& file, const RasterSpec& spec) {
    GDALAllRegister();
    GDALDriverH mem = GDALGetDriverByName("MEM");
    GDALDatasetH ds = GDALCreate(mem, "", spec.width, spec.height, 1, spec.type, nullptr);
    ASSERT_NE(ds, nullptr);
    auto gt = spec.geoTransform;
    GDALSetGeoTransform(ds, gt.data());
    if (spec.epsg != 0) {
        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRImportFromEPSG(srs, spec.epsg);
        GDALSetSpatialRef(ds, srs);
        OSRDestroySpatialReference(srs);
    }
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    if (spec.noData) {
        GDALSetRasterNoDataValue(band, *spec.noData);
    }
    if (spec.scale != 1.0 || spec.offset != 0.0) {
        GDALSetRasterScale(band, spec.scale);
        GDALSetRasterOffset(band, spec.offset);
    }
    if (!spec.unit.empty()) {
        GDALSetRasterUnitType(band, spec.unit.c_str());
    }
    std::vector<double> row(static_cast<std::size_t>(spec.width));
    for (int j = 0; j < spec.height; ++j) {
        for (int i = 0; i < spec.width; ++i) {
            row[static_cast<std::size_t>(i)] = spec.value(i, j);
        }
        ASSERT_EQ(GDALRasterIO(band, GF_Write, 0, j, spec.width, 1, row.data(), spec.width, 1,
                               GDT_Float64, 0, 0),
                  CE_None);
    }
    if (spec.overviews) {
        const std::array<int, 3> levels{2, 4, 8};
        ASSERT_EQ(GDALBuildOverviews(ds, "AVERAGE", 3, levels.data(), 0, nullptr, nullptr, nullptr),
                  CE_None);
    }
    std::vector<const char*> options;
    for (const auto& o : spec.creationOptions) {
        options.push_back(o.c_str());
    }
    options.push_back(nullptr);
    GDALDatasetH out =
        GDALCreateCopy(GDALGetDriverByName(spec.driver.c_str()), file.string().c_str(), ds, FALSE,
                       const_cast<char**>(options.data()), nullptr, nullptr);
    ASSERT_NE(out, nullptr) << CPLGetLastErrorMsg();
    GDALClose(out);
    GDALClose(ds);
}

// Planar surface in UTM: height at easting / northing.
double planeUtm(double e, double n) { return 600.0 + 0.01 * (e - kE0) + 0.02 * (n - kN0); }

RasterSpec planarUtmSpec() {
    RasterSpec s;
    s.value = [](int i, int j) { return planeUtm(kE0 + (i + 0.5) * 10.0, kN0 - (j + 0.5) * 10.0); };
    return s;
}

// The planar height expected at world (x, z), through the project's own UTM code.
double expectedPlanar(const RasterSource& source, double x, double z) {
    const ds::geo::LocalFrame frame(source.metadata().origin);
    const ds::geo::Geodetic g = frame.toGeodetic(ds::geo::Enu{x, -z, 0.0});
    const auto utm = ds::geo::toUtmInZone(g.latitudeDeg, g.longitudeDeg, 30);
    EXPECT_TRUE(utm.has_value());
    return planeUtm(utm->easting, utm->northing);
}

std::shared_ptr<RasterSource> openOne(const fs::path& file, const RasterOptions& options = {}) {
    const std::array<fs::path, 1> files{file};
    auto source = RasterSource::open(files, options);
    EXPECT_TRUE(source.has_value()) << (source ? "" : source.error().message);
    return source ? *source : nullptr;
}

TEST(RasterSource, ReprojectsAUtmRasterIntoTheLocalFrame) {
    TempDir dir;
    writeRaster(dir / "dem.tif", planarUtmSpec());
    auto source = openOne(dir / "dem.tif");
    ASSERT_TRUE(source);
    const SourceMetadata& m = source->metadata();
    EXPECT_EQ(m.format, "GTiff");
    EXPECT_EQ(m.crsCode, "EPSG:32630");
    EXPECT_EQ(m.widthPx, 200);
    EXPECT_NEAR(m.resolutionM, 10.0, 1e-9);
    // The default origin is the centre of the data, and an AGL origin puts the ground there
    // at world height 0.
    const auto centre = ds::geo::toUtmInZone(m.origin.latitudeDeg, m.origin.longitudeDeg, 30);
    ASSERT_TRUE(centre.has_value());
    EXPECT_NEAR(centre->easting, kE0 + 1000.0, 0.01);
    EXPECT_NEAR(centre->northing, kN0 - 1000.0, 0.01);
    EXPECT_NEAR(m.origin.heightM, planeUtm(kE0 + 1000.0, kN0 - 1000.0), 1e-3);
    EXPECT_NEAR(source->height(0.0, 0.0), 0.0, 1e-3);
    // The largest origin-centred rectangle inside the 2 km tile, which UTM grid convergence
    // (about 0.45° here) rotates against true north.
    EXPECT_GT(m.extent.widthM, 1900.0);
    EXPECT_LE(m.extent.widthM, 2000.0);
    EXPECT_GT(m.extent.lengthM, 1900.0);
    EXPECT_NEAR(m.minHeightM, planeUtm(kE0, kN0 - 2000.0), 1.0);
    EXPECT_NEAR(m.maxHeightM, planeUtm(kE0 + 2000.0, kN0), 1.0);
    EXPECT_EQ(m.noDataPercent, 0.0);

    // Bilinear interpolation reproduces a plane, so every height matches the analytic value
    // through an independent UTM implementation (FR-TER-I05-like alignment, < 1 mm).
    for (double x = -900.0; x <= 900.0; x += 137.0) {
        for (double z = -900.0; z <= 900.0; z += 151.0) {
            EXPECT_NEAR(source->heightAmsl(x, z), expectedPlanar(*source, x, z), 1e-3)
                << x << ", " << z;
        }
    }
}

TEST(RasterSource, ReadsAGeographicRaster) {
    TempDir dir;
    RasterSpec s;
    s.epsg = 4326;
    s.geoTransform = {-3.72, 0.0002, 0.0, 40.43, 0.0, -0.0002};  // About 17 × 22 m pixels.
    s.value = [](int i, int j) {
        const double lon = -3.72 + (i + 0.5) * 0.0002;
        const double lat = 40.43 - (j + 0.5) * 0.0002;
        return 500.0 + 1000.0 * (lon + 3.72) + 2000.0 * (40.43 - lat);
    };
    writeRaster(dir / "geo.tif", s);
    RasterOptions options;
    options.origin = ds::geo::LatLon{40.41, -3.70};
    options.originAltitudeM = 0.0;
    auto source = openOne(dir / "geo.tif", options);
    ASSERT_TRUE(source);
    EXPECT_NEAR(source->metadata().resolutionM,
                0.5 * (0.0002 * 111'320.0 * std::cos(0.7054) + 0.0002 * 110'574.0), 0.5);
    const ds::geo::LocalFrame frame(source->metadata().origin);
    for (double x = -1000.0; x <= 1000.0; x += 250.0) {
        for (double z = -1000.0; z <= 1000.0; z += 250.0) {
            const ds::geo::Geodetic g = frame.toGeodetic(ds::geo::Enu{x, -z, 0.0});
            const double expected =
                500.0 + 1000.0 * (g.longitudeDeg + 3.72) + 2000.0 * (40.43 - g.latitudeDeg);
            EXPECT_NEAR(source->height(x, z), expected, 1e-4);  // Float32 pixels.
        }
    }
}

TEST(RasterSource, SampleGridEqualsHeightAtFullDetailAndAveragesCoarseGrids) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.overviews = true;
    writeRaster(dir / "dem.tif", s);
    auto source = openOne(dir / "dem.tif");
    ASSERT_TRUE(source);
    std::vector<double> grid(9 * 7);
    source->sampleGrid(-100.0, -80.0, 2.0, 9, 7, grid);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 9; ++col) {
            EXPECT_EQ(grid[static_cast<std::size_t>(row * 9 + col)],
                      source->height(-100.0 + col * 2.0, -80.0 + row * 2.0));
        }
    }
    // An 80 m grid reads data averaged over 4 × 4 pixels: the plane survives averaging.
    source->sampleGrid(-640.0, -640.0, 80.0, 17, 17, grid = std::vector<double>(17 * 17));
    for (int row = 0; row < 17; ++row) {
        for (int col = 0; col < 17; ++col) {
            const double x = -640.0 + col * 80.0;
            const double z = -640.0 + row * 80.0;
            EXPECT_NEAR(
                grid[static_cast<std::size_t>(row * 17 + col)] + source->metadata().origin.heightM,
                expectedPlanar(*source, x, z), 1e-3);
        }
    }
}

TEST(RasterSource, CoarseReadsAgreeWhateverTheWindow) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.value = [](int i, int j) { return 700.0 + 40.0 * std::sin(0.11 * i) * std::cos(0.07 * j); };
    writeRaster(dir / "dem.tif", s);
    auto source = openOne(dir / "dem.tif");
    ASSERT_TRUE(source);
    for (const double step : {2.0, 41.0, 160.0}) {
        std::vector<double> whole(12 * 12);
        source->sampleGrid(-800.0, -800.0, step, 12, 12, whole);
        for (int row = 0; row < 12; row += 3) {
            for (int col = 0; col < 12; col += 5) {
                std::array<double, 1> one{};
                source->sampleGrid(-800.0 + col * step, -800.0 + row * step, step, 1, 1, one);
                // Full detail is bit-identical; GDAL's averaged reads agree to float rounding.
                const double w = whole[static_cast<std::size_t>(row * 12 + col)];
                if (step <= 2.0) {
                    EXPECT_EQ(one[0], w);
                } else {
                    EXPECT_NEAR(one[0], w, 1e-3) << step;
                }
            }
        }
    }
}

TEST(RasterSource, MosaicOfTilesEqualsTheSingleFile) {
    TempDir dir;
    const RasterSpec whole = planarUtmSpec();
    writeRaster(dir / "whole.tif", whole);
    RasterSpec west = whole;
    west.width = 100;
    RasterSpec east = whole;
    east.width = 100;
    east.geoTransform[0] = kE0 + 1000.0;
    east.value = [&](int i, int j) { return whole.value(i + 100, j); };
    writeRaster(dir / "west.tif", west);
    writeRaster(dir / "east.tif", east);
    auto single = openOne(dir / "whole.tif");
    const std::array<fs::path, 2> tiles{dir / "west.tif", dir / "east.tif"};
    auto mosaic = RasterSource::open(tiles);
    ASSERT_TRUE(mosaic.has_value()) << mosaic.error().message;
    EXPECT_EQ((*mosaic)->metadata().fileCount, 2);
    EXPECT_EQ((*mosaic)->metadata().widthPx, 200);
    for (double x = -900.0; x <= 900.0; x += 50.0) {
        EXPECT_NEAR((*mosaic)->height(x, 37.0), single->height(x, 37.0), 1e-9) << x;
    }
}

TEST(RasterSource, AppliesScaleOffsetAndFeet) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.type = GDT_Int16;
    s.scale = 0.5;
    s.offset = 100.0;
    s.unit = "ft";
    s.value = [](int, int) { return 1000.0; };  // (1000 · 0.5 + 100) ft = 182.88 m.
    writeRaster(dir / "feet.tif", s);
    auto source = openOne(dir / "feet.tif");
    ASSERT_TRUE(source);
    EXPECT_EQ(source->metadata().verticalUnit, "ft");
    EXPECT_NEAR(source->heightAmsl(120.0, -40.0), 182.88, 1e-9);
}

TEST(RasterSource, FillsSmallVoidsAndFallsBackInLargeOnes) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.noData = -9999.0;
    const RasterSpec plane = planarUtmSpec();
    s.value = [&](int i, int j) {
        const bool smallHole = i >= 99 && i <= 101 && j >= 99 && j <= 101;  // Centre.
        const bool largeHole = i >= 20 && i < 60 && j >= 20 && j < 60;      // North-west.
        return smallHole || largeHole ? -9999.0 : plane.value(i, j);
    };
    writeRaster(dir / "voids.tif", s);
    RasterOptions options;
    options.originAltitudeM = 0.0;
    auto source = openOne(dir / "voids.tif", options);
    ASSERT_TRUE(source);
    EXPECT_GT(source->metadata().noDataPercent, 3.0);
    // Inside the small hole: interpolated from the neighbours, close to the plane.
    EXPECT_NEAR(source->height(0.0, 0.0), expectedPlanar(*source, 0.0, 0.0), 0.5);
    // In the middle of the 400 m hole no valid pixel is within 8 pixels: the lowest
    // elevation of the data is used.
    const double x = -1000.0 + 400.0;
    const double z = -1000.0 + 400.0;
    EXPECT_NEAR(source->height(x, z), source->metadata().minHeightM, 1e-9);
}

TEST(RasterSource, RejectsBadInputWithClearErrors) {
    TempDir dir;
    const std::array<fs::path, 1> missing{dir / "missing.tif"};
    EXPECT_EQ(RasterSource::open(missing).error().code, TerrainErrorCode::FileNotFound);

    {
        std::ofstream(dir / "garbage.tif") << "this is not a GeoTIFF";
        const std::array<fs::path, 1> files{dir / "garbage.tif"};
        EXPECT_EQ(RasterSource::open(files).error().code, TerrainErrorCode::UnsupportedFormat);
    }
    {
        RasterSpec s = planarUtmSpec();
        s.type = GDT_Byte;
        s.value = [](int, int) { return 10.0; };
        writeRaster(dir / "byte.tif", s);
        const std::array<fs::path, 1> files{dir / "byte.tif"};
        auto r = RasterSource::open(files);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, TerrainErrorCode::UnsupportedFormat);
        EXPECT_NE(r.error().message.find("Byte"), std::string::npos);
    }
    {
        RasterSpec s = planarUtmSpec();
        s.epsg = 0;
        writeRaster(dir / "nocrs.tif", s);
        const std::array<fs::path, 1> files{dir / "nocrs.tif"};
        EXPECT_EQ(RasterSource::open(files).error().code, TerrainErrorCode::MissingCrs);
        RasterOptions options;
        options.crs = "EPSG:32630";
        auto r = RasterSource::open(files, options);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_EQ((*r)->metadata().crsCode, "EPSG:32630");
        options.crs = "EPSG:999999";
        EXPECT_EQ(RasterSource::open(files, options).error().code, TerrainErrorCode::InvalidSpec);
    }
    {
        writeRaster(dir / "utm.tif", planarUtmSpec());
        RasterSpec other = planarUtmSpec();
        other.epsg = 25830;  // ETRS89 / UTM 30N: same grid, different CRS.
        writeRaster(dir / "etrs.tif", other);
        const std::array<fs::path, 2> files{dir / "utm.tif", dir / "etrs.tif"};
        EXPECT_EQ(RasterSource::open(files).error().code, TerrainErrorCode::MixedCrs);
    }
    {
        const std::array<fs::path, 1> files{dir / "utm.tif"};
        RasterOptions options;
        options.maxDimensionPx = 100;
        EXPECT_EQ(RasterSource::open(files, options).error().code, TerrainErrorCode::TooLarge);
        options = {};
        options.origin = ds::geo::LatLon{41.0, -3.0};
        EXPECT_EQ(RasterSource::open(files, options).error().code, TerrainErrorCode::NotCovered);
        options = {};
        options.extent = Extent{3000.0, 1000.0};
        EXPECT_EQ(RasterSource::open(files, options).error().code, TerrainErrorCode::NotCovered);
        options.extent = Extent{1500.0, 1000.0};
        auto r = RasterSource::open(files, options);
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->extent(), (Extent{1500.0, 1000.0}));
        options.band = 2;
        options.extent.reset();
        EXPECT_EQ(RasterSource::open(files, options).error().code,
                  TerrainErrorCode::UnsupportedFormat);
    }
}

TEST(RasterSource, OpensSrtmHgtAndAsciiGrid) {
    TempDir dir;
    {
        // SRTM3 tile N40W004: 1201 × 1201 Int16, implicitly WGS84.
        RasterSpec s;
        s.width = 1201;
        s.height = 1201;
        s.epsg = 4326;
        s.type = GDT_Int16;
        constexpr double px = 1.0 / 1200.0;
        s.geoTransform = {-4.0 - 0.5 * px, px, 0.0, 41.0 + 0.5 * px, 0.0, -px};
        s.driver = "SRTMHGT";
        s.value = [](int i, int j) { return 600.0 + (i / 100) + (j / 100); };
        writeRaster(dir / "N40W004.hgt", s);
        RasterOptions options;
        options.extent = Extent{4000.0, 4000.0};
        auto source = openOne(dir / "N40W004.hgt", options);
        ASSERT_TRUE(source);
        EXPECT_EQ(source->metadata().format, "SRTMHGT");
        EXPECT_NEAR(source->metadata().origin.latitudeDeg, 40.5, 1e-6);
        EXPECT_NEAR(source->metadata().origin.longitudeDeg, -3.5, 1e-6);
        EXPECT_NEAR(source->metadata().origin.heightM, 612.0, 1.0);
    }
    {
        RasterSpec s = planarUtmSpec();
        s.driver = "AAIGrid";
        writeRaster(dir / "dem.asc", s);  // Writes dem.asc and dem.prj.
        auto source = openOne(dir / "dem.asc");
        ASSERT_TRUE(source);
        EXPECT_EQ(source->metadata().format, "AAIGrid");
        EXPECT_NEAR(source->heightAmsl(100.0, 100.0), expectedPlanar(*source, 100.0, 100.0), 1e-3);
    }
}

TEST(RasterSource, IsThreadSafeAndDeterministic) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.value = [](int i, int j) { return 700.0 + 40.0 * std::sin(0.11 * i) * std::cos(0.07 * j); };
    writeRaster(dir / "dem.tif", s);
    auto source = openOne(dir / "dem.tif");
    ASSERT_TRUE(source);
    constexpr int kN = 33;
    std::vector<double> serial(kN * kN);
    source->sampleGrid(-500.0, -500.0, 2.0, kN, kN, serial);
    std::vector<std::vector<double>> results(8, std::vector<double>(kN * kN));
    {
        std::vector<std::jthread> threads;
        for (auto& r : results) {
            threads.emplace_back([&source, &r] {
                for (int k = 0; k < 20; ++k) {
                    source->sampleGrid(-500.0, -500.0, 2.0, kN, kN, r);
                }
            });
        }
    }
    for (const auto& r : results) {
        EXPECT_EQ(r, serial);
    }
}

TEST(RasterSource, QueriesMatchTheLevelZeroMesh) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.value = [](int i, int j) { return 700.0 + 40.0 * std::sin(0.11 * i) * std::cos(0.07 * j); };
    writeRaster(dir / "dem.tif", s);
    auto source = openOne(dir / "dem.tif");
    ASSERT_TRUE(source);
    const ChunkLayout layout;
    const TerrainQuery query(source, layout, source->metadata().origin.heightM);
    const ChunkMesh mesh = buildChunkMesh(*source, layout, ChunkId{0, -1, 0});
    for (int k = 0; k < 129 * 129; k += 97) {
        const TerrainVertex& v = mesh.vertices[static_cast<std::size_t>(k)];
        const double x = mesh.originX + v.u * mesh.spacingM;
        const double z = mesh.originZ + v.v * mesh.spacingM;
        EXPECT_NEAR(query.height(x, z), v.height, 1e-3);
    }
}

}  // namespace
