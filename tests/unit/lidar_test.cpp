// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// LAS / LAZ → DTM (FR-TER-L01 … L06; SPECIFICATION §8 "terrain — LAS/LAZ"): fixtures are
// written in-test with LASzip from a known surface.

#include "ds/terrain/lidar.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

#include "ds/terrain/raster_source.hpp"

#include "lidar_fixtures.hpp"
#include "raster_fixtures.hpp"
#include <gdal.h>

namespace ds::terrain {
namespace {

namespace fs = std::filesystem;

constexpr double kX0 = 400000.0;
constexpr double kY0 = 4600000.0;

// Height of the DTM at (x, y) in its CRS, nullopt for NoData.
std::optional<double> dtmAt(const fs::path& cog, double x, double y) {
    GDALDatasetH ds = GDALOpen(cog.c_str(), GA_ReadOnly);
    if (ds == nullptr) {
        ADD_FAILURE() << "cannot open " << cog;
        return std::nullopt;
    }
    std::array<double, 6> gt{};
    GDALGetGeoTransform(ds, gt.data());
    const int col = static_cast<int>(std::floor((x - gt[0]) / gt[1]));
    const int row = static_cast<int>(std::floor((y - gt[3]) / gt[5]));
    float v = 0.0F;
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
    const CPLErr e = GDALRasterIO(band, GF_Read, col, row, 1, 1, &v, 1, 1, GDT_Float32, 0, 0);
    GDALClose(ds);
    EXPECT_EQ(e, CE_None);
    if (hasNoData != 0 && v == static_cast<float>(noData)) {
        return std::nullopt;
    }
    return v;
}

struct Fixture {
    test::TempDir dir{"lidar"};

    fs::path write(const std::string& name, const std::vector<test::LidarPoint>& points,
                   const test::LidarFileOptions& o = {}) {
        const fs::path f = dir / name;
        std::string why;
        EXPECT_TRUE(test::writeLidar(f, points, o, &why)) << why;
        return f;
    }
};

TEST(Lidar, HeaderGivesTheCrsBoundsAndCount) {
    Fixture fx;
    const auto pts = test::lidarPoints(kX0, kY0, 50, 40, 2.0);
    const fs::path las = fx.write("a.las", pts);
    auto h = readLidarHeader(las);
    ASSERT_TRUE(h.has_value()) << h.error().message;
    EXPECT_EQ(h->version, "1.2");
    EXPECT_EQ(h->pointFormat, 1);
    EXPECT_FALSE(h->compressed);
    EXPECT_EQ(h->points, pts.size());
    EXPECT_EQ(h->crs, "EPSG:25831");
    EXPECT_NEAR(h->minX, kX0, 0.7);
    EXPECT_NEAR(h->maxY, kY0 + 40, 0.7);

    const fs::path laz = fx.write("b.laz", pts,
                                  {.versionMinor = 4,
                                   .compressed = true,
                                   .epsg = 0,
                                   .wkt = "PROJCS[\"ETRS89 / UTM zone 31N\",GEOGCS[\"ETRS89\","
                                          "DATUM[\"ETRS_1989\",SPHEROID[\"GRS 1980\",6378137,"
                                          "298.257222101]],PRIMEM[\"Greenwich\",0],UNIT["
                                          "\"degree\",0.0174532925199433]],PROJECTION["
                                          "\"Transverse_Mercator\"],PARAMETER[\"latitude_of_"
                                          "origin\",0],PARAMETER[\"central_meridian\",3],"
                                          "PARAMETER[\"scale_factor\",0.9996],PARAMETER["
                                          "\"false_easting\",500000],PARAMETER[\"false_"
                                          "northing\",0],UNIT[\"metre\",1]]"});
    h = readLidarHeader(laz);
    ASSERT_TRUE(h.has_value()) << h.error().message;
    EXPECT_EQ(h->version, "1.4");
    EXPECT_EQ(h->pointFormat, 6);
    EXPECT_TRUE(h->compressed);
    EXPECT_TRUE(h->crs.starts_with("PROJCS")) << h->crs;

    EXPECT_EQ(readLidarHeader(fx.dir / "missing.las").error().code, TerrainErrorCode::FileNotFound);
    std::ofstream(fx.dir / "junk.las") << "not a point cloud";
    EXPECT_EQ(readLidarHeader(fx.dir / "junk.las").error().code, TerrainErrorCode::CorruptData);
    EXPECT_EQ(readLidarHeader(fx.dir / "x.tif").error().code, TerrainErrorCode::UnsupportedFormat);
}

TEST(Lidar, LasAndLazGiveTheSameDtmOfTheSurface) {
    Fixture fx;
    const auto pts = test::lidarPoints(kX0, kY0, 120, 100, 0.8);
    const fs::path las = fx.write("a.las", pts);
    const fs::path laz = fx.write("a.laz", pts, {.compressed = true});
    std::vector<double> progress;
    auto a = rasterizeLidar(std::array{las}, {.resolutionM = 1.0}, fx.dir / "a.tif", {},
                            [&](double f) { progress.push_back(f); });
    ASSERT_TRUE(a.has_value()) << a.error().message;
    auto b = rasterizeLidar(std::array{laz}, {.resolutionM = 1.0}, fx.dir / "b.tif");
    ASSERT_TRUE(b.has_value()) << b.error().message;
    EXPECT_EQ(a->points, pts.size());
    EXPECT_EQ(a->groundPoints, b->groundPoints);
    EXPECT_LT(a->groundPoints, pts.size());  // Vegetation is not part of the DTM.
    EXPECT_EQ(a->resolutionM, 1.0);
    EXPECT_EQ(a->crs, "EPSG:25831");
    ASSERT_FALSE(progress.empty());
    EXPECT_DOUBLE_EQ(progress.back(), 1.0);
    EXPECT_TRUE(std::ranges::is_sorted(progress));

    double worst = 0.0;
    for (double y = kY0 + 5.5; y < kY0 + 95; y += 7.3) {
        for (double x = kX0 + 5.5; x < kX0 + 115; x += 6.1) {
            const auto ha = dtmAt(fx.dir / "a.tif", x, y);
            const auto hb = dtmAt(fx.dir / "b.tif", x, y);
            ASSERT_TRUE(ha && hb);
            EXPECT_EQ(*ha, *hb);
            // The cell's mean against the surface at the point: within the slope over a cell.
            worst = std::max(worst, std::abs(*ha - test::lidarSurface(x, y)));
        }
    }
    EXPECT_LT(worst, 0.25);

    auto summary = readLidarSummary(fx.dir / "a.tif");
    ASSERT_TRUE(summary.has_value()) << summary.error().message;
    EXPECT_EQ(summary->points, a->points);
    EXPECT_EQ(summary->groundPoints, a->groundPoints);
    EXPECT_EQ(summary->interpolation, "tin");
    EXPECT_EQ(summary->widthPx, a->widthPx);
}

TEST(Lidar, Las14DropsNoiseAndWithheldPoints) {
    Fixture fx;
    auto pts = test::lidarPoints(kX0, kY0, 60, 60, 1.0);
    // Spikes that must not reach the DTM: low and high noise, and withheld points.
    for (double x = kX0 + 2; x < kX0 + 58; x += 3) {
        pts.push_back({x, kY0 + 30.2, test::lidarSurface(x, kY0 + 30.2) - 40.0, 7});
        pts.push_back({x, kY0 + 20.2, test::lidarSurface(x, kY0 + 20.2) + 60.0, 18});
        pts.push_back({x, kY0 + 10.2, test::lidarSurface(x, kY0 + 10.2) + 90.0, 2, true});
    }
    const fs::path f = fx.write("n.laz", pts, {.versionMinor = 4, .compressed = true});
    auto s = rasterizeLidar(std::array{f}, {.resolutionM = 1.0}, fx.dir / "n.tif");
    ASSERT_TRUE(s.has_value()) << s.error().message;
    EXPECT_EQ(s->dropped, 3U * 19U);
    for (double x = kX0 + 2; x < kX0 + 58; x += 3) {
        for (double y : {kY0 + 30.2, kY0 + 20.2, kY0 + 10.2}) {
            const auto h = dtmAt(fx.dir / "n.tif", x, y);
            ASSERT_TRUE(h);
            EXPECT_NEAR(*h, test::lidarSurface(x, y), 0.3) << x << ", " << y;
        }
    }
}

TEST(Lidar, SmallGapsAreFilledByTinOrIdwLargeOnesStayNoData) {
    Fixture fx;
    // A 12 m gap (filled) and a 70 m gap (more than 50 m: NoData).
    const auto pts = test::lidarPoints(kX0, kY0, 200, 120, 0.9,
                                       {{{kX0 + 30, kY0 + 40, 12}}, {{kX0 + 110, kY0 + 25, 70}}});
    const fs::path f = fx.write("g.las", pts);
    for (const LidarInterpolation how : {LidarInterpolation::Tin, LidarInterpolation::Idw}) {
        const fs::path out = fx.dir / (how == LidarInterpolation::Tin ? "tin.tif" : "idw.tif");
        auto s = rasterizeLidar(std::array{f}, {.resolutionM = 1.0, .interpolation = how}, out);
        ASSERT_TRUE(s.has_value()) << s.error().message;
        EXPECT_GE(s->filledCells, 100U);
        EXPECT_GE(s->noDataCells, 60U * 60U);
        // Inside the small gap: interpolated from its rim (a plane with small waves).
        const double x = kX0 + 36.0;
        const double y = kY0 + 46.0;
        const auto h = dtmAt(out, x, y);
        ASSERT_TRUE(h);
        EXPECT_NEAR(*h, test::lidarSurface(x, y), how == LidarInterpolation::Tin ? 0.5 : 1.0);
        EXPECT_FALSE(dtmAt(out, kX0 + 145.0, kY0 + 60.0).has_value());  // Large gap.
    }
}

TEST(Lidar, TilesMakeOneSeamlessSurface) {
    Fixture fx;
    const auto all = test::lidarPoints(kX0, kY0, 160, 80, 1.0);
    std::vector<test::LidarPoint> west;
    std::vector<test::LidarPoint> east;
    for (const auto& p : all) {
        (p.x < kX0 + 80.0 ? west : east).push_back(p);
    }
    const fs::path one = fx.write("all.las", all);
    const fs::path w = fx.write("w.laz", west, {.compressed = true});
    const fs::path e = fx.write("e.laz", east, {.compressed = true});
    ASSERT_TRUE(rasterizeLidar(std::array{one}, {.resolutionM = 2.0}, fx.dir / "one.tif"));
    auto tiles = rasterizeLidar(std::array{w, e}, {.resolutionM = 2.0}, fx.dir / "tiles.tif");
    ASSERT_TRUE(tiles.has_value()) << tiles.error().message;
    for (double x = kX0 + 70.5; x < kX0 + 90.0; x += 1.0) {
        EXPECT_EQ(dtmAt(fx.dir / "one.tif", x, kY0 + 40.5),
                  dtmAt(fx.dir / "tiles.tif", x, kY0 + 40.5))
            << x;
    }
}

TEST(Lidar, AutomaticResolutionAndVerticalFeet) {
    Fixture fx;
    const fs::path f = fx.write("auto.las", test::lidarPoints(kX0, kY0, 100, 100, 0.7));
    auto s = rasterizeLidar(std::array{f}, {}, fx.dir / "auto.tif");
    ASSERT_TRUE(s.has_value()) << s.error().message;
    EXPECT_GE(s->resolutionM, 0.5);
    EXPECT_LE(s->resolutionM, 2.0);
    EXPECT_EQ(std::fmod(s->resolutionM * 4.0, 1.0), 0.0);  // Multiples of 0.25 m.

    // A coarser grid when the automatic one would be too large.
    s = rasterizeLidar(std::array{f}, {.maxCells = 400}, fx.dir / "coarse.tif");
    ASSERT_TRUE(s.has_value());
    EXPECT_LE(static_cast<std::uint64_t>(s->widthPx) * static_cast<std::uint64_t>(s->heightPx),
              400U);

    // Larger than one 256-pixel block: a COG with overviews (FR-TER-L05).
    const fs::path big = fx.write("big.las", test::lidarPoints(kX0, kY0, 300, 300, 1.5));
    ASSERT_TRUE(rasterizeLidar(std::array{big}, {.resolutionM = 1.0}, fx.dir / "big.tif"));
    GDALDatasetH ds = GDALOpen((fx.dir / "big.tif").c_str(), GA_ReadOnly);
    ASSERT_NE(ds, nullptr);
    EXPECT_GT(GDALGetOverviewCount(GDALGetRasterBand(ds, 1)), 0);
    EXPECT_STREQ(GDALGetMetadataItem(ds, "LAYOUT", "IMAGE_STRUCTURE"), "COG");
    GDALClose(ds);
    // It opens as an elevation raster in metres.
    auto source = RasterSource::open(std::array{fx.dir / "big.tif"}, {});
    ASSERT_TRUE(source.has_value()) << source.error().message;
    EXPECT_NEAR((*source)->metadata().resolutionM, 1.0, 0.01);

    // Heights stored in feet come out in metres.
    const fs::path feet =
        fx.write("feet.las", test::lidarPoints(kX0, kY0, 40, 40, 1.0), {.verticalFeet = true});
    ASSERT_TRUE(rasterizeLidar(std::array{feet}, {.resolutionM = 1.0}, fx.dir / "feet.tif"));
    const auto h = dtmAt(fx.dir / "feet.tif", kX0 + 20.5, kY0 + 20.5);
    ASSERT_TRUE(h);
    EXPECT_NEAR(*h, test::lidarSurface(kX0 + 20.5, kY0 + 20.5), 0.3);
}

TEST(Lidar, ClearErrorsForUnusableClouds) {
    Fixture fx;
    const auto pts = test::lidarPoints(kX0, kY0, 40, 40, 1.0);
    // No CRS: needs crs.
    const fs::path bare = fx.write("bare.las", pts, {.epsg = 0});
    auto r = rasterizeLidar(std::array{bare}, {.resolutionM = 1.0}, fx.dir / "x.tif");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, TerrainErrorCode::MissingCrs);
    EXPECT_NE(r.error().message.find("crs"), std::string::npos);
    EXPECT_TRUE(rasterizeLidar(std::array{bare}, {.resolutionM = 1.0, .crs = "EPSG:25831"},
                               fx.dir / "x.tif"));

    // Unclassified (class 1): no ground points.
    auto unclassified = pts;
    for (auto& p : unclassified) {
        p.classification = 1;
    }
    const fs::path raw = fx.write("raw.las", unclassified);
    r = rasterizeLidar(std::array{raw}, {.resolutionM = 1.0}, fx.dir / "y.tif");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, TerrainErrorCode::NotSupported);
    EXPECT_NE(r.error().message.find("no ground points"), std::string::npos);
    r = rasterizeLidar(std::array{raw}, {.resolutionM = 1.0, .classifyGround = true},
                       fx.dir / "y.tif");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("classifyGround"), std::string::npos) << r.error().message;

    // Mixed CRSs, a geographic CRS, a grid that is too fine.
    const fs::path other = fx.write("other.las", pts, {.epsg = 25830});
    const fs::path a = fx.write("a.las", pts);
    EXPECT_EQ(rasterizeLidar(std::array{a, other}, {}, fx.dir / "z.tif").error().code,
              TerrainErrorCode::MixedCrs);
    const fs::path geo = fx.write("geo.las", pts, {.epsg = 0, .wkt = "EPSG:4326"});
    EXPECT_EQ(rasterizeLidar(std::array{geo}, {}, fx.dir / "z.tif").error().code,
              TerrainErrorCode::NotSupported);
    r = rasterizeLidar(std::array{a}, {.resolutionM = 0.25, .maxCells = 1000}, fx.dir / "z.tif");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, TerrainErrorCode::TooLarge);
    EXPECT_NE(r.error().message.find("resolutionM"), std::string::npos);
    EXPECT_FALSE(fs::exists(fx.dir / "y.tif"));
    EXPECT_FALSE(fs::exists(fx.dir / "z.tif"));
}

TEST(Lidar, DemKeyFollowsTheFilesAndOptions) {
    Fixture fx;
    const fs::path f = fx.write("k.las", test::lidarPoints(kX0, kY0, 20, 20, 1.0));
    const auto key = lidarDemKey(std::array{f}, {});
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->size(), 64U);
    EXPECT_EQ(*key, *lidarDemKey(std::array{f}, {}));
    EXPECT_NE(*key, *lidarDemKey(std::array{f}, {.resolutionM = 1.0}));
    EXPECT_NE(*key, *lidarDemKey(std::array{f}, {.interpolation = LidarInterpolation::Idw}));
    EXPECT_NE(*key, *lidarDemKey(std::array{f}, {.crs = "EPSG:25830"}));
    // A rewritten file is a new key.
    (void)fx.write("k.las", test::lidarPoints(kX0, kY0, 20, 21, 1.0));
    EXPECT_NE(*key, *lidarDemKey(std::array{f}, {}));
    EXPECT_EQ(lidarDemKey(std::array{fx.dir / "none.las"}, {}).error().code,
              TerrainErrorCode::FileNotFound);
}

TEST(Lidar, CancelLeavesNoFile) {
    Fixture fx;
    const fs::path f = fx.write("c.las", test::lidarPoints(kX0, kY0, 300, 300, 0.5));
    std::stop_source stop;
    std::atomic<int> calls{0};
    auto r = rasterizeLidar(std::array{f}, {.resolutionM = 0.5}, fx.dir / "c.tif", stop.get_token(),
                            [&](double) {
                                if (++calls == 2) {
                                    stop.request_stop();
                                }
                            });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, TerrainErrorCode::Cancelled);
    for (const auto& entry : fs::directory_iterator(fx.dir.path())) {
        EXPECT_NE(entry.path().extension(), ".tif") << entry.path();
        EXPECT_EQ(entry.path().string().find("partial"), std::string::npos) << entry.path();
    }
}

}  // namespace
}  // namespace ds::terrain
