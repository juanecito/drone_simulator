// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/raster_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

#include "raster_fixtures.hpp"
#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_srs_api.h>
#include <unistd.h>

namespace {

using namespace ds::terrain;
namespace fs = std::filesystem;

using ds::test::kE0;
using ds::test::kN0;
using ds::test::RasterSpec;
using ds::test::TempDir;
using ds::test::writeRaster;

using ds::test::planeUtm;

RasterSpec planarUtmSpec() { return ds::test::planarUtmSpec(); }

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

// --- Interpolation and filters (FR-TER-C03) ------------------------------------------------

RasterOptions processed(HeightInterpolation interpolation, double despikeM = 0.0,
                        double smoothM = 0.0) {
    RasterOptions o;
    o.processing = {
        .interpolation = interpolation, .despikeRadiusM = despikeM, .smoothRadiusM = smoothM};
    return o;
}

TEST(RasterSource, EveryInterpolationAndFilterKeepsAPlane) {
    // Cubic interpolations, a symmetric Gaussian and the median of a symmetric window all
    // reproduce a linear surface, so the heights match the analytic plane away from the
    // raster's border.
    TempDir dir;
    writeRaster(dir / "dem.tif", planarUtmSpec());
    for (const auto& options :
         {processed(HeightInterpolation::Bicubic), processed(HeightInterpolation::BSpline),
          processed(HeightInterpolation::Bilinear, 10.0, 0.0),
          processed(HeightInterpolation::Bilinear, 0.0, 60.0),
          processed(HeightInterpolation::Bicubic, 5.0, 100.0)}) {
        auto source = openOne(dir / "dem.tif", options);
        ASSERT_TRUE(source);
        for (double x = -700.0; x <= 700.0; x += 137.0) {
            for (double z = -700.0; z <= 700.0; z += 151.0) {
                EXPECT_NEAR(source->heightAmsl(x, z), expectedPlanar(*source, x, z), 1e-3)
                    << describeProcessing(options.processing) << " at " << x << ", " << z;
            }
        }
    }
}

TEST(RasterSource, NearestShowsTheRawCellsAndCubicsAreSmooth) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.value = [](int i, int j) { return 500.0 + 30.0 * std::sin(0.3 * i) * std::cos(0.2 * j); };
    writeRaster(dir / "dem.tif", s);
    RasterOptions agl;
    agl.originAltitudeM = 0.0;
    auto nearestOptions = processed(HeightInterpolation::Nearest);
    nearestOptions.originAltitudeM = 0.0;
    auto nearest = openOne(dir / "dem.tif", nearestOptions);
    auto bilinear = openOne(dir / "dem.tif", agl);
    ASSERT_TRUE(nearest && bilinear);
    // Pixel (100, 100) has its centre about (5, 5) m from the origin: inside it, the nearest
    // height is that pixel's value, flat.
    const double pixel = s.value(100, 100);
    EXPECT_NEAR(nearest->height(3.0, 3.5), pixel, 1e-4);
    EXPECT_NEAR(nearest->height(7.0, 6.5), pixel, 1e-4);
    EXPECT_NE(bilinear->height(3.0, 3.5), bilinear->height(7.0, 6.5));

    // The slope of a bilinear surface jumps at a cell border, that of a cubic one does not.
    auto slopeJump = [](const RasterSource& r, double x) {
        const double e = 0.05;
        const double left = (r.height(x, 2.0) - r.height(x - e, 2.0)) / e;
        const double right = (r.height(x + e, 2.0) - r.height(x, 2.0)) / e;
        return std::abs(right - left);
    };
    for (const HeightInterpolation how :
         {HeightInterpolation::Bicubic, HeightInterpolation::BSpline}) {
        auto o = processed(how);
        o.originAltitudeM = 0.0;
        auto cubic = openOne(dir / "dem.tif", o);
        ASSERT_TRUE(cubic);
        // Bilinear cells join at pixel centres, near x = 5 + 10 k m (UTM convergence moves
        // them a centimetre here).
        double bilinearJump = 0.0;
        double cubicJump = 0.0;
        for (const double x : {-15.0, -5.0, 5.0, 15.0, 25.0}) {
            bilinearJump = std::max(bilinearJump, slopeJump(*bilinear, x));
            cubicJump = std::max(cubicJump, slopeJump(*cubic, x));
        }
        EXPECT_GT(bilinearJump, 0.05) << interpolationName(how);
        EXPECT_LT(cubicJump, 0.25 * bilinearJump) << interpolationName(how);
        // Catmull-Rom passes through the cells; the B-spline approximates them.
        const double atCell = cubic->height(5.0, 5.0);
        if (how == HeightInterpolation::Bicubic) {
            EXPECT_NEAR(atCell, bilinear->height(5.0, 5.0), 0.05);
        }
    }
}

TEST(RasterSource, SpikeFilterRemovesSpikesAndSmoothingRemovesNoise) {
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.value = [](int i, int j) {
        if (i == 100 && j == 100) {
            return 550.0;  // A spike of 50 m, one pixel.
        }
        if (i == 120 && j == 80) {
            return 470.0;  // A pit.
        }
        return 500.0 + ((i + j) % 2 == 0 ? 0.5 : -0.5);  // ±0.5 m checkerboard noise.
    };
    writeRaster(dir / "dem.tif", s);
    auto o = [](double despike, double smooth) {
        auto options = processed(HeightInterpolation::Bilinear, despike, smooth);
        options.originAltitudeM = 0.0;
        return options;
    };
    auto raw = openOne(dir / "dem.tif", o(0.0, 0.0));
    auto despiked = openOne(dir / "dem.tif", o(10.0, 0.0));
    auto smoothed = openOne(dir / "dem.tif", o(0.0, 30.0));
    auto both = openOne(dir / "dem.tif", o(10.0, 30.0));
    ASSERT_TRUE(raw && despiked && smoothed && both);
    // The spike pixel's centre is about (5, 5) m from the origin, the pit's (205, -195).
    EXPECT_GT(raw->height(5.0, 5.0), 540.0);
    EXPECT_LT(raw->height(205.0, -195.0), 480.0);
    EXPECT_NEAR(despiked->height(5.0, 5.0), 500.0, 0.6);
    EXPECT_NEAR(despiked->height(205.0, -195.0), 500.0, 0.6);
    // A Gaussian spreads the spike instead of removing it, and flattens the noise.
    EXPECT_LT(smoothed->height(5.0, 5.0), 510.0);
    EXPECT_GT(smoothed->height(5.0, 5.0), 501.0);
    double lowest = 1e9;
    double highest = -1e9;
    for (double x = -300.0; x <= 300.0; x += 3.0) {
        lowest = std::min(lowest, raw->height(x, 405.0));
        highest = std::max(highest, raw->height(x, 405.0));
        EXPECT_NEAR(smoothed->height(x, 405.0), 500.0, 0.05) << x;
        EXPECT_NEAR(both->height(x, 405.0), 500.0, 0.05) << x;
    }
    EXPECT_GT(highest - lowest, 0.5);
    EXPECT_NEAR(both->height(5.0, 5.0), 500.0, 0.05);
}

TEST(RasterSource, FilteredHeightsDoNotDependOnTheQuery) {
    // Heights come from 128-pixel tiles: grids across a tile border, single points, a second
    // source (computing its own tiles) and other threads must all see the same values.
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.value = [](int i, int j) {
        return 700.0 + 40.0 * std::sin(0.11 * i) * std::cos(0.07 * j) + ((i * 7 + j * 3) % 5);
    };
    writeRaster(dir / "dem.tif", s);
    const auto options = processed(HeightInterpolation::Bicubic, 10.0, 40.0);
    auto source = openOne(dir / "dem.tif", options);
    auto other = openOne(dir / "dem.tif", options);
    ASSERT_TRUE(source && other);
    // Pixel column 128 starts about 280 m east of the origin.
    constexpr int kN = 41;
    std::vector<double> grid(kN * kN);
    source->sampleGrid(240.0, 230.0, 2.0, kN, kN, grid);
    std::vector<double> shifted(kN * kN);
    source->sampleGrid(250.0, 240.0, 2.0, kN, kN, shifted);
    for (int row = 0; row < kN; ++row) {
        for (int col = 0; col < kN; ++col) {
            const double x = 240.0 + col * 2.0;
            const double z = 230.0 + row * 2.0;
            const double h = grid[static_cast<std::size_t>(row * kN + col)];
            ASSERT_EQ(h, source->height(x, z)) << x << ", " << z;
            ASSERT_EQ(h, other->height(x, z)) << x << ", " << z;
            if (row >= 5 && col >= 5) {
                ASSERT_EQ(h, shifted[static_cast<std::size_t>((row - 5) * kN + col - 5)]);
            }
        }
    }
    std::vector<std::vector<double>> results(6, std::vector<double>(kN * kN));
    {
        auto fresh = openOne(dir / "dem.tif", options);
        ASSERT_TRUE(fresh);
        std::vector<std::jthread> threads;
        for (auto& r : results) {
            threads.emplace_back([&fresh, &r] { fresh->sampleGrid(240.0, 230.0, 2.0, kN, kN, r); });
        }
    }
    for (const auto& r : results) {
        EXPECT_EQ(r, grid);
    }
}

TEST(RasterSource, WideFiltersOnFineDataReadCoarserCells) {
    // 1 m cells: a 10 m spike filter would need 21 × 21 cells, so it runs on 2 m cells; the
    // plane survives and full-detail grids still equal single queries.
    TempDir dir;
    RasterSpec s = planarUtmSpec();
    s.width = 400;
    s.height = 400;
    s.geoTransform = {kE0, 1.0, 0.0, kN0, 0.0, -1.0};
    s.value = [](int i, int j) { return 300.0 + 0.2 * i - 0.1 * j; };
    writeRaster(dir / "fine.tif", s);
    auto source = openOne(dir / "fine.tif", processed(HeightInterpolation::Bilinear, 10.0, 80.0));
    ASSERT_TRUE(source);
    const ds::geo::LocalFrame frame(source->metadata().origin);
    for (double x = -60.0; x <= 60.0; x += 17.0) {
        const ds::geo::Geodetic g = frame.toGeodetic(ds::geo::Enu{x, -x, 0.0});
        const auto utm = ds::geo::toUtmInZone(g.latitudeDeg, g.longitudeDeg, 30);
        ASSERT_TRUE(utm.has_value());
        const double i = utm->easting - kE0 - 0.5;
        const double j = kN0 - utm->northing - 0.5;
        EXPECT_NEAR(source->heightAmsl(x, x), 300.0 + 0.2 * i - 0.1 * j, 1e-3) << x;
    }
    std::vector<double> grid(9 * 9);
    source->sampleGrid(-8.0, -8.0, 2.0, 9, 9, grid);
    for (int k = 0; k < 81; ++k) {
        EXPECT_EQ(grid[static_cast<std::size_t>(k)],
                  source->height(-8.0 + (k % 9) * 2.0, -8.0 + (k / 9) * 2.0));
    }
}

TEST(RasterSource, RejectsFilterRadiiOutOfRange) {
    TempDir dir;
    writeRaster(dir / "dem.tif", planarUtmSpec());
    const std::array<fs::path, 1> files{dir / "dem.tif"};
    for (const auto& options :
         {processed(HeightInterpolation::Bilinear, -1.0),
          processed(HeightInterpolation::Bilinear, kMaxDespikeRadiusM + 1.0),
          processed(HeightInterpolation::Bilinear, 0.0, kMaxSmoothRadiusM + 1.0)}) {
        auto source = RasterSource::open(files, options);
        ASSERT_FALSE(source.has_value());
        EXPECT_EQ(source.error().code, TerrainErrorCode::InvalidSpec);
    }
    EXPECT_EQ(interpolationFromName("bspline"), HeightInterpolation::BSpline);
    EXPECT_EQ(interpolationFromName("cubic"), std::nullopt);
    EXPECT_EQ(describeProcessing({}), "bilinear");
    EXPECT_EQ(describeProcessing({.interpolation = HeightInterpolation::Bicubic,
                                  .despikeRadiusM = 2.0,
                                  .smoothRadiusM = 10.0}),
              "bicubic, spikes removed within 2 m, smoothed over 10 m");
}

}  // namespace
