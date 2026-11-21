// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/orthophoto.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <stop_token>
#include <vector>

#include "ds/common/worker_pool.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/geo/utm.hpp"
#include "ds/terrain/ortho_streamer.hpp"

#include "ortho_fixtures.hpp"

namespace {

namespace fs = std::filesystem;
using ds::terrain::OrthoOptions;
using ds::terrain::OrthophotoSource;
using ds::terrain::OrthoTile;
using ds::terrain::OrthoTileKey;
using ds::terrain::TerrainErrorCode;
using ds::test::ImageSpec;
using ds::test::kE0;
using ds::test::kN0;
using ds::test::TempDir;
using ds::test::writeImage;

// Geodetic position of a UTM 30N point, at the altitude of the scene origin.
ds::geo::Geodetic utmPoint(double e, double n, double altitude = 600.0) {
    const auto ll =
        ds::geo::fromUtm({.zone = 30, .band = 'T', .north = true, .easting = e, .northing = n});
    return {ll->latitudeDeg, ll->longitudeDeg, altitude};
}

// The default 400 × 400 image (0.5 m pixels): origin at its centre.
ds::geo::Geodetic imageCentre() { return utmPoint(kE0 + 100.0, kN0 - 100.0); }

// Colour (r, g, b, a) of texel (i, j) of mip level 0.
std::array<int, 4> texel(const OrthoTile& t, int i, int j) {
    const std::size_t at = (static_cast<std::size_t>(j) * static_cast<std::size_t>(t.sizePx) +
                            static_cast<std::size_t>(i)) *
                           4U;
    return {t.rgba[at], t.rgba[at + 1], t.rgba[at + 2], t.rgba[at + 3]};
}

// Image pixel coordinates (u, v; 0 at the top-left corner) of world (x, z) in the frame of
// @p origin, by the project's own UTM code.
std::pair<double, double> pixelOf(const ds::geo::LocalFrame& frame, double x, double z,
                                  double pixelM = 0.5, double e0 = kE0) {
    const ds::geo::Geodetic g = frame.toGeodetic(ds::geo::Enu{x, -z, 0.0});
    const auto utm = ds::geo::toUtmInZone(g.latitudeDeg, g.longitudeDeg, 30);
    return {(utm->easting - e0) / pixelM, (kN0 - utm->northing) / pixelM};
}

TEST(Orthophoto, PixelsLandAtTheirWorldPositionsWithinOnePixel) {
    TempDir dir("ortho_pos");
    writeImage(dir / "a.tif", ImageSpec{});
    const fs::path file = dir / "a.tif";
    auto source = OrthophotoSource::open(std::span(&file, 1), {}, imageCentre());
    ASSERT_TRUE(source) << source.error().message;
    const auto& meta = (*source)->metadata();
    EXPECT_EQ(meta.bands, "RGB");
    EXPECT_EQ(meta.crsCode, "EPSG:32630");
    EXPECT_NEAR(meta.resolutionM, 0.5, 1e-9);
    EXPECT_NEAR(meta.minX, -100.0, 4.0);
    EXPECT_NEAR(meta.maxX, 100.0, 4.0);
    EXPECT_NEAR(meta.minZ, -100.0, 4.0);
    EXPECT_NEAR(meta.maxZ, 100.0, 4.0);

    const ds::geo::LocalFrame frame(imageCentre());
    int checked = 0;
    double worst = 0.0;
    for (const OrthoTileKey key :
         {OrthoTileKey{0, 0, 0}, OrthoTileKey{0, -1, -2}, OrthoTileKey{0, -2, 0}}) {
        const auto tile = (*source)->tile(key);
        ASSERT_NE(tile, nullptr);
        ASSERT_EQ(tile->sizePx, 256);
        const double side = 64.0;
        const double step = side / 256.0;
        for (int j = 3; j < 253; j += 7) {
            for (int i = 3; i < 253; i += 7) {
                const double x = static_cast<double>(key.ix) * side + (i + 0.5) * step;
                const double z = static_cast<double>(key.iz) * side + (j + 0.5) * step;
                const auto [u, v] = pixelOf(frame, x, z);
                if (u < 2 || v < 2 || u > 398 || v > 398) {
                    continue;
                }
                const double eu = std::fmod(u - 0.5, 256.0);
                const double ev = std::fmod(v - 0.5, 256.0);
                if (eu < 2.0 || eu > 253.0 || ev < 2.0 || ev > 253.0) {
                    continue;  // Next to a wrap of the colour ramp.
                }
                const auto c = texel(*tile, i, j);
                ASSERT_EQ(c[3], 255);
                worst = std::max({worst, std::abs(c[0] - eu), std::abs(c[1] - ev)});
                ++checked;
            }
        }
    }
    EXPECT_GT(checked, 500);
    EXPECT_LE(worst, 1.0) << "misalignment in pixels";
}

TEST(Orthophoto, OutsideTheCoverageAndNoDataFallBackToTheMaterials) {
    TempDir dir("ortho_cover");
    ImageSpec spec;
    // A NoData square (all channels 0) in the middle.
    spec.noData = 0.0;
    spec.value = [](int i, int j, int b) {
        if (i >= 180 && i < 220 && j >= 180 && j < 220) {
            return 0;
        }
        return 100 + b;
    };
    writeImage(dir / "a.tif", spec);
    const fs::path file = dir / "a.tif";
    auto source = OrthophotoSource::open(std::span(&file, 1), {}, imageCentre());
    ASSERT_TRUE(source) << source.error().message;
    // Far away: certainly empty, without reading.
    EXPECT_FALSE((*source)->mayCover({0, 10, 10}));
    EXPECT_EQ((*source)->tile({0, 10, 10}), nullptr);
    // Tile (0, 1, 0) spans x 64 … 128: the image ends at x = 100.
    const auto edge = (*source)->tile({0, 1, 0});
    ASSERT_NE(edge, nullptr);
    const int border = static_cast<int>((100.0 - 64.0) / 0.25);  // Texel column of the edge.
    EXPECT_EQ(texel(*edge, border - 20, 100)[3], 255);
    EXPECT_EQ(texel(*edge, border - 20, 100)[0], 100);
    EXPECT_EQ(texel(*edge, border + 4, 100)[3], 0);
    // Soft edge: alpha ramps up inside the imagery.
    const int ramp = texel(*edge, border - 1, 100)[3];
    EXPECT_GT(ramp, 0);
    EXPECT_LT(ramp, 255);
    // The NoData square (world −10 … 10 m) is transparent; the tile at the origin holds it.
    const auto centre = (*source)->tile({0, 0, 0});
    ASSERT_NE(centre, nullptr);
    EXPECT_EQ(texel(*centre, 10, 10)[3], 0);      // (2.6, 2.6) m: NoData.
    EXPECT_EQ(texel(*centre, 100, 100)[3], 255);  // (25, 25) m: imagery.
    EXPECT_EQ(texel(*centre, 100, 100)[1], 101);
    // Mip levels exist and keep the premultiplied average.
    EXPECT_EQ(centre->mipCount(), 9);
    EXPECT_EQ(centre->rgba.size(), OrthoTile::bytesFor(256));
    const std::size_t last = centre->mipOffset(8);
    EXPECT_GT(centre->rgba[last + 3], 200);
}

TEST(Orthophoto, MosaicIsSeamlessAndCoversBothFiles) {
    TempDir dir("ortho_mosaic");
    // Two 200 × 400 halves of the 400 × 400 image, the colour ramp continuing across.
    ImageSpec west;
    west.width = 200;
    ImageSpec east = west;
    east.geoTransform[0] = kE0 + 100.0;
    east.value = [](int i, int j, int b) { return ImageSpec{}.value(i + 200, j, b); };
    writeImage(dir / "w.tif", west);
    writeImage(dir / "e.tif", east);
    const std::vector<fs::path> files{dir / "w.tif", dir / "e.tif"};
    auto source = OrthophotoSource::open(files, {}, imageCentre());
    ASSERT_TRUE(source) << source.error().message;
    EXPECT_EQ((*source)->metadata().widthPx, 400);
    EXPECT_EQ((*source)->metadata().fileCount, 2);
    // The seam is at x = 0: tiles (0, −1, 0) and (0, 0, 0) meet there.
    const auto left = (*source)->tile({0, -1, 0});
    const auto right = (*source)->tile({0, 0, 0});
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    const ds::geo::LocalFrame frame(imageCentre());
    for (const int j : {20, 128, 230}) {
        const auto a = texel(*left, 255, j);
        const auto b = texel(*right, 0, j);
        EXPECT_EQ(a[3], 255);
        EXPECT_EQ(b[3], 255);
        EXPECT_LE(std::abs(a[0] - b[0]), 1);  // 0.25 m apart: half a pixel.
        const auto [u, v] = pixelOf(frame, 0.125, 64.0 * 0 + (j + 0.5) * 0.25);
        EXPECT_NEAR(b[0], std::fmod(u - 0.5, 256.0), 1.0);
    }
}

TEST(Orthophoto, BandLayouts) {
    TempDir dir("ortho_bands");
    const auto origin = imageCentre();
    // RGBA: alpha 0 on the left half.
    ImageSpec rgba;
    rgba.bands = 4;
    rgba.alpha = true;
    rgba.value = [](int i, int /*j*/, int b) { return b == 3 ? (i < 200 ? 0 : 255) : 50 + b; };
    writeImage(dir / "rgba.tif", rgba);
    {
        const fs::path f = dir / "rgba.tif";
        auto s = OrthophotoSource::open(std::span(&f, 1), {}, origin);
        ASSERT_TRUE(s) << s.error().message;
        EXPECT_EQ((*s)->metadata().bands, "RGBA");
        const auto t = (*s)->tile({0, -1, 0});  // x −64 … 0: transparent.
        EXPECT_EQ(t, nullptr);
        const auto r = (*s)->tile({0, 0, 0});
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(texel(*r, 128, 128), (std::array<int, 4>{50, 51, 52, 255}));
    }
    // Four bands, none alpha: needs orthophotoBands.
    ImageSpec four;
    four.bands = 4;
    four.value = [](int /*i*/, int /*j*/, int b) { return 10 * (b + 1); };
    writeImage(dir / "four.tif", four);
    {
        const fs::path f = dir / "four.tif";
        auto s = OrthophotoSource::open(std::span(&f, 1), {}, origin);
        ASSERT_FALSE(s);
        EXPECT_EQ(s.error().code, TerrainErrorCode::UnsupportedFormat);
        EXPECT_NE(s.error().message.find("orthophotoBands"), std::string::npos);
        OrthoOptions o;
        o.bands = {3, 2, 1};
        auto picked = OrthophotoSource::open(std::span(&f, 1), o, origin);
        ASSERT_TRUE(picked) << picked.error().message;
        EXPECT_EQ((*picked)->metadata().bands, "bands 3, 2, 1");
        const auto t = (*picked)->tile({0, 0, 0});
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(texel(*t, 128, 128), (std::array<int, 4>{30, 20, 10, 255}));
        o.bands = {1, 2, 9};
        EXPECT_EQ(OrthophotoSource::open(std::span(&f, 1), o, origin).error().code,
                  TerrainErrorCode::UnsupportedFormat);
    }
    // Grey.
    ImageSpec grey;
    grey.bands = 1;
    grey.value = [](int, int, int) { return 77; };
    writeImage(dir / "grey.tif", grey);
    {
        const fs::path f = dir / "grey.tif";
        auto s = OrthophotoSource::open(std::span(&f, 1), {}, origin);
        ASSERT_TRUE(s) << s.error().message;
        const auto t = (*s)->tile({0, 0, 0});
        ASSERT_NE(t, nullptr);
        EXPECT_EQ(texel(*t, 128, 128), (std::array<int, 4>{77, 77, 77, 255}));
    }
}

TEST(Orthophoto, InvalidFilesAreClearErrors) {
    TempDir dir("ortho_invalid");
    const auto origin = imageCentre();
    auto openOne = [&](const fs::path& f, const OrthoOptions& o = {}) {
        return OrthophotoSource::open(std::span(&f, 1), o, origin);
    };
    EXPECT_EQ(openOne(dir / "missing.tif").error().code, TerrainErrorCode::FileNotFound);
    std::ofstream(dir / "text.tif") << "not an image";
    EXPECT_EQ(openOne(dir / "text.tif").error().code, TerrainErrorCode::UnsupportedFormat);

    ImageSpec wide;
    wide.type = GDT_UInt16;
    writeImage(dir / "u16.tif", wide);
    const auto u16 = openOne(dir / "u16.tif");
    ASSERT_FALSE(u16);
    EXPECT_EQ(u16.error().code, TerrainErrorCode::UnsupportedFormat);
    EXPECT_NE(u16.error().message.find("8-bit"), std::string::npos);

    ImageSpec palette;
    palette.bands = 1;
    palette.palette = true;
    writeImage(dir / "palette.tif", palette);
    EXPECT_EQ(openOne(dir / "palette.tif").error().code, TerrainErrorCode::UnsupportedFormat);

    ImageSpec noCrs;
    noCrs.epsg = 0;
    writeImage(dir / "nocrs.tif", noCrs);
    EXPECT_EQ(openOne(dir / "nocrs.tif").error().code, TerrainErrorCode::MissingCrs);
    OrthoOptions withCrs;
    withCrs.crs = "EPSG:32630";
    EXPECT_TRUE(openOne(dir / "nocrs.tif", withCrs));
    withCrs.crs = "not a crs";
    EXPECT_EQ(openOne(dir / "nocrs.tif", withCrs).error().code, TerrainErrorCode::InvalidSpec);

    writeImage(dir / "a.tif", ImageSpec{});
    ImageSpec other;
    other.epsg = 25830;  // ETRS89 / UTM 30N: another CRS.
    writeImage(dir / "b.tif", other);
    const std::vector<fs::path> mixed{dir / "a.tif", dir / "b.tif"};
    EXPECT_EQ(OrthophotoSource::open(mixed, {}, origin).error().code, TerrainErrorCode::MixedCrs);
    ImageSpec rgba;
    rgba.bands = 4;
    rgba.alpha = true;
    rgba.geoTransform[0] += 200.0;
    writeImage(dir / "c.tif", rgba);
    const std::vector<fs::path> layouts{dir / "a.tif", dir / "c.tif"};
    EXPECT_EQ(OrthophotoSource::open(layouts, {}, origin).error().code,
              TerrainErrorCode::UnsupportedFormat);

    OrthoOptions small;
    small.maxDimensionPx = 300;
    EXPECT_EQ(openOne(dir / "a.tif", small).error().code, TerrainErrorCode::TooLarge);
    OrthoOptions dark;
    dark.color.gamma = 0.0;
    EXPECT_EQ(openOne(dir / "a.tif", dark).error().code, TerrainErrorCode::InvalidSpec);
}

TEST(Orthophoto, OtherFormatsAndColourAdjustments) {
    TempDir dir("ortho_formats");
    const auto origin = imageCentre();
    const ds::geo::LocalFrame frame(origin);
    // PNG with a world file and the mission's CRS (FR-TER-I01, S).
    ImageSpec png;
    png.driver = "PNG";
    png.epsg = 0;
    png.worldFile = true;
    writeImage(dir / "a.png", png);
    ASSERT_TRUE(fs::exists(dir / "a.pgw"));
    {
        const fs::path f = dir / "a.png";
        EXPECT_EQ(OrthophotoSource::open(std::span(&f, 1), {}, origin).error().code,
                  TerrainErrorCode::MissingCrs);
        OrthoOptions o;
        o.crs = "EPSG:32630";
        auto s = OrthophotoSource::open(std::span(&f, 1), o, origin);
        ASSERT_TRUE(s) << s.error().message;
        EXPECT_EQ((*s)->metadata().format, "PNG");
        const auto t = (*s)->tile({0, 0, 0});
        ASSERT_NE(t, nullptr);
        const auto [u, v] = pixelOf(frame, 30.0 + 0.125, 30.0 + 0.125);
        EXPECT_NEAR(texel(*t, 120, 120)[0], std::fmod(u - 0.5, 256.0), 1.0);
        EXPECT_NEAR(texel(*t, 120, 120)[1], std::fmod(v - 0.5, 256.0), 1.0);
    }
    // JPEG 2000 with GeoJP2 (lossless).
    ImageSpec jp2;
    jp2.driver = "JP2OpenJPEG";
    jp2.creationOptions = {"QUALITY=100", "REVERSIBLE=YES"};
    writeImage(dir / "a.jp2", jp2);
    {
        const fs::path f = dir / "a.jp2";
        auto s = OrthophotoSource::open(std::span(&f, 1), {}, origin);
        ASSERT_TRUE(s) << s.error().message;
        const auto t = (*s)->tile({0, 0, 0});
        ASSERT_NE(t, nullptr);
        const auto [u, v] = pixelOf(frame, 30.0 + 0.125, 30.0 + 0.125);
        EXPECT_NEAR(texel(*t, 120, 120)[0], std::fmod(u - 0.5, 256.0), 1.0);
    }
    // Colour adjustments: brightness, contrast, gamma.
    ImageSpec grey;
    grey.bands = 1;
    grey.value = [](int, int, int) { return 128; };
    writeImage(dir / "grey.tif", grey);
    const fs::path f = dir / "grey.tif";
    auto valueWith = [&](ds::terrain::OrthoColor c) {
        OrthoOptions o;
        o.color = c;
        auto s = OrthophotoSource::open(std::span(&f, 1), o, origin);
        EXPECT_TRUE(s);
        return texel(*(*s)->tile({0, 0, 0}), 128, 128)[0];
    };
    EXPECT_EQ(valueWith({}), 128);
    EXPECT_EQ(valueWith({.brightness = 0.2}), 179);
    EXPECT_EQ(valueWith({.contrast = 0.0}), 128);
    EXPECT_EQ(valueWith({.gamma = 2.0}), 181);  // (128/255)^(1/2) × 255.
}

TEST(Orthophoto, LargeImagesWithoutOverviewsGetACachedCog) {
    TempDir dir("ortho_cog");
    ImageSpec spec;
    spec.width = 600;
    spec.height = 600;
    writeImage(dir / "plain.tif", spec);
    spec.overviews = true;
    writeImage(dir / "ovr.tif", spec);
    EXPECT_TRUE(ds::terrain::orthoNeedsCache(dir / "plain.tif", 512));
    EXPECT_FALSE(ds::terrain::orthoNeedsCache(dir / "plain.tif", 1024));
    EXPECT_FALSE(ds::terrain::orthoNeedsCache(dir / "ovr.tif", 512));
    EXPECT_FALSE(ds::terrain::orthoNeedsCache(dir / "missing.tif", 512));

    const auto key = ds::terrain::orthoCacheKey(dir / "plain.tif");
    ASSERT_TRUE(key);
    EXPECT_EQ(key->size(), 64U);
    EXPECT_EQ(ds::terrain::orthoCacheKey(dir / "missing.tif").error().code,
              TerrainErrorCode::FileNotFound);

    // Cancelled: nothing is left behind.
    std::stop_source cancel;
    cancel.request_stop();
    const fs::path cog = dir / "cache" / (*key + ".tif");
    auto stopped = ds::terrain::buildOrthoCog(dir / "plain.tif", cog, cancel.get_token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, TerrainErrorCode::Cancelled);
    EXPECT_FALSE(fs::exists(cog));
    EXPECT_TRUE(fs::is_empty(dir / "cache"));

    double last = 0.0;
    auto made =
        ds::terrain::buildOrthoCog(dir / "plain.tif", cog, {}, [&](double done) { last = done; });
    ASSERT_TRUE(made) << made.error().message;
    EXPECT_DOUBLE_EQ(last, 1.0);
    EXPECT_FALSE(ds::terrain::orthoNeedsCache(cog, 512));
    // The copy shows the same imagery.
    const fs::path original = dir / "plain.tif";
    auto a = OrthophotoSource::open(std::span(&original, 1), {}, imageCentre());
    auto b = OrthophotoSource::open(std::span(&cog, 1), {}, imageCentre());
    ASSERT_TRUE(a && b);
    EXPECT_TRUE((*b)->metadata().hasOverviews);
    EXPECT_EQ((*a)->tile({0, 0, 0})->rgba, (*b)->tile({0, 0, 0})->rgba);
    // Coarse tiles read the overviews (2 m texels from 0.5 m pixels).
    const auto coarse = (*b)->tile({3, 0, 0});
    ASSERT_NE(coarse, nullptr);
    EXPECT_EQ(texel(*coarse, 2, 2)[3], 255);
}

std::shared_ptr<const ds::terrain::ChunkMesh> chunkAt(int lod, int ix, int iz) {
    auto mesh = std::make_shared<ds::terrain::ChunkMesh>();
    mesh->id = {lod, ix, iz};
    return mesh;
}

TEST(OrthoStreamer, BuildsTheTilesOfTheDrawnChunks) {
    TempDir dir("ortho_stream");
    writeImage(dir / "a.tif", ImageSpec{});
    const fs::path file = dir / "a.tif";
    auto source = OrthophotoSource::open(std::span(&file, 1), {}, imageCentre());
    ASSERT_TRUE(source);
    ds::common::WorkerPool pool(2);
    ds::terrain::ChunkLayout layout;
    ds::terrain::OrthoStreamerSettings settings;
    settings.maxInFlight = 64;
    ds::terrain::OrthoStreamer streamer(*source, layout, pool, settings);
    // Chunk (0, 0, 0) spans 0 … 256 m: its tiles up to 128 m see the image (to 100 m).
    const std::vector chunks{chunkAt(0, 0, 0), chunkAt(0, 5, 5)};
    streamer.update(chunks, 0.0, 0.0);
    EXPECT_EQ(streamer.find({0, 0, 0}), ds::terrain::OrthoTileState::Pending);
    EXPECT_EQ(streamer.find({0, 20, 20}), ds::terrain::OrthoTileState::Empty);  // Outside.
    EXPECT_EQ(streamer.find({0, 3, 3}), ds::terrain::OrthoTileState::Empty);
    pool.waitIdle();
    streamer.update(chunks, 0.0, 0.0);
    std::shared_ptr<const OrthoTile> tile;
    EXPECT_EQ(streamer.find({0, 0, 0}, &tile), ds::terrain::OrthoTileState::Ready);
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(tile->key, (OrthoTileKey{0, 0, 0}));
    EXPECT_EQ(streamer.find({0, 1, 1}), ds::terrain::OrthoTileState::Ready);
    // The parents were built too, to stand in while finer tiles load.
    EXPECT_EQ(streamer.find({1, 0, 0}), ds::terrain::OrthoTileState::Ready);
    EXPECT_EQ(streamer.find({3, 0, 0}), ds::terrain::OrthoTileState::Ready);
    EXPECT_EQ(streamer.find({4, 0, 0}), ds::terrain::OrthoTileState::Unknown);
    const auto stats = streamer.stats();
    EXPECT_EQ(stats.cachedTiles, 7U);  // 4 tiles (0, 0…1, 0…1) and 3 levels of ancestors.
    EXPECT_EQ(stats.inFlight, 0U);
    EXPECT_GT(stats.cachedBytes, 5 * OrthoTile::bytesFor(256));
    EXPECT_EQ(ds::terrain::OrthoStreamer::tilesOf({2, 1, -1})[5], (OrthoTileKey{2, 5, -3}));

    // Over the cache limit, tiles no longer drawn go; the drawn ones stay.
    ds::terrain::OrthoStreamerSettings tiny = settings;
    tiny.cacheBytes = 1;
    ds::terrain::OrthoStreamer small(*source, layout, pool, tiny);
    small.update(std::vector{chunkAt(0, 0, 0)}, 0.0, 0.0);
    pool.waitIdle();
    small.update(std::vector{chunkAt(0, 0, 0)}, 0.0, 0.0);
    EXPECT_EQ(small.find({0, 0, 0}), ds::terrain::OrthoTileState::Ready);
    small.update(std::vector{chunkAt(0, -1, -1)}, 0.0, 0.0);
    EXPECT_EQ(small.find({0, 0, 0}), ds::terrain::OrthoTileState::Unknown);
    EXPECT_NE(streamer.id(), small.id());
}

}  // namespace
