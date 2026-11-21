// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Elevation-file terrain in the application (M4 step 3): the scene configuration, missions on
// elevation files, edge treatment, file preparation and the terrain-edge warning.

#include <QJsonArray>
#include <QJsonObject>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ds/app/mission_setup.hpp"
#include "ds/app/scene_config.hpp"
#include "ds/sim/hud.hpp"
#include "ds/terrain/raster_source.hpp"
#include "ds/terrain/terrain_files.hpp"

#include "ortho_fixtures.hpp"
#include "raster_fixtures.hpp"

namespace {

using namespace ds;
using test::TempDir;
namespace fs = std::filesystem;

app::TerrainConfig fileConfig(const fs::path& file) {
    app::TerrainConfig c;
    c.kind = app::TerrainKind::File;
    c.file.files = {file};
    c.file.attribution = "Test DEM, CC BY 4.0";
    return c;
}

TEST(FileTerrain, ConfigJsonAndPresetRoundTrip) {
    app::TerrainConfig c = fileConfig("/data/dem/a.tif");
    c.file.files.emplace_back("/data/dem/b.tif");
    c.file.band = 2;
    c.file.crs = "EPSG:25830";
    c.file.verticalDatum = "EGM2008";
    c.file.origin = geo::LatLon{40.41, -3.70};
    c.file.originAltitudeM = 655.0;
    c.file.edge = terrain::EdgeMode::Fade;
    c.extent = terrain::Extent{1500.0, 1200.0};
    const auto back = app::terrainFromJson(app::terrainToJson(c));
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, c);

    const auto record = app::toPresetRecord(c, "user.dem", "DEM");
    EXPECT_EQ(record.sourceType, "file");
    const auto preset = app::fromPresetRecord(record);
    ASSERT_TRUE(preset.has_value()) << preset.error().message;
    EXPECT_EQ(preset->kind, app::TerrainKind::File);
    EXPECT_EQ(preset->file, c.file);

    // Point-cloud settings.
    app::TerrainConfig pc = fileConfig("/data/pc/a.LAZ");
    EXPECT_TRUE(app::isPointCloud(pc.file));
    EXPECT_FALSE(app::isPointCloud(c.file));
    pc.file.resolutionM = 0.5;
    pc.file.interpolation = terrain::LidarInterpolation::Idw;
    pc.file.classifyGround = true;
    auto pcBack = app::terrainFromJson(app::terrainToJson(pc));
    ASSERT_TRUE(pcBack.has_value()) << pcBack.error().message;
    EXPECT_EQ(*pcBack, pc);
    QJsonObject bad = app::terrainToJson(pc);
    QJsonObject badFile = bad.value(QStringLiteral("file")).toObject();
    badFile.insert(QStringLiteral("resolutionM"), 0.1);
    bad.insert(QStringLiteral("file"), badFile);
    EXPECT_FALSE(app::terrainFromJson(bad).has_value());
    badFile.insert(QStringLiteral("resolutionM"), 1.0);
    badFile.insert(QStringLiteral("interpolation"), QStringLiteral("max"));
    bad.insert(QStringLiteral("file"), badFile);
    EXPECT_FALSE(app::terrainFromJson(bad).has_value());

    // No files, or a water edge, are not file terrain.
    QJsonObject json = app::terrainToJson(c);
    QJsonObject file = json.value(QStringLiteral("file")).toObject();
    file.insert(QStringLiteral("edge"), QStringLiteral("water"));
    json.insert(QStringLiteral("file"), file);
    EXPECT_FALSE(app::terrainFromJson(json).has_value());
    file.insert(QStringLiteral("edge"), QStringLiteral("fade"));
    file.insert(QStringLiteral("files"), QJsonArray());
    json.insert(QStringLiteral("file"), file);
    EXPECT_FALSE(app::terrainFromJson(json).has_value());
}

TEST(FileTerrain, BuildTerrainOpensTheFilesAndTakesTheOriginFromTheData) {
    TempDir dir("file_terrain");
    test::writeRaster(dir / "dem.tif", test::planarUtmSpec());
    auto built = app::buildTerrain(fileConfig(dir / "dem.tif"));
    ASSERT_TRUE(built.has_value()) << built.error().message;
    ASSERT_TRUE(built->metadata.has_value());
    EXPECT_EQ(built->metadata->crsCode, "EPSG:32630");
    EXPECT_EQ(built->attribution, "Test DEM, CC BY 4.0");
    // Centre of the tile, at ground level.
    EXPECT_NEAR(built->origin.heightM, test::planeUtm(test::kE0 + 1000.0, test::kN0 - 1000.0),
                1e-3);
    EXPECT_NEAR(built->source->height(0.0, 0.0), 0.0, 1e-3);
    EXPECT_TRUE(built->source->extent().has_value());

    app::TerrainConfig missing = fileConfig(dir / "missing.tif");
    EXPECT_FALSE(app::buildTerrain(missing).has_value());
}

TEST(FileTerrain, OrthophotosInTheConfigAndTheBuiltTerrain) {
    TempDir dir("file_terrain_ortho");
    test::writeRaster(dir / "dem.tif", test::planarUtmSpec());
    // 1 m imagery over the middle 800 m of the 2 km DEM.
    test::ImageSpec image;
    image.width = 800;
    image.height = 800;
    image.geoTransform = {test::kE0 + 600.0, 1.0, 0.0, test::kN0 - 600.0, 0.0, -1.0};
    test::writeImage(dir / "ortho.tif", image);

    app::TerrainConfig c = fileConfig(dir / "dem.tif");
    c.ortho.files = {dir / "ortho.tif"};
    c.ortho.crs = "EPSG:32630";
    c.ortho.bands = {1, 2, 3};
    c.ortho.attribution = "Test imagery, CC0";
    c.ortho.color = {.brightness = 0.1, .contrast = 1.2, .gamma = 0.9};
    const auto back = app::terrainFromJson(app::terrainToJson(c));
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, c);
    const auto preset = app::fromPresetRecord(app::toPresetRecord(c, "user.o", "O"));
    ASSERT_TRUE(preset.has_value()) << preset.error().message;
    EXPECT_EQ(preset->ortho, c.ortho);
    // Procedural terrain has no imagery (FR-TER-I03).
    app::TerrainConfig procedural;
    procedural.ortho = c.ortho;
    EXPECT_FALSE(app::terrainToJson(procedural).contains(QStringLiteral("ortho")));
    QJsonObject bad = app::terrainToJson(c);
    QJsonObject badOrtho = bad.value(QStringLiteral("ortho")).toObject();
    badOrtho.insert(QStringLiteral("gamma"), 0.0);
    bad.insert(QStringLiteral("ortho"), badOrtho);
    EXPECT_FALSE(app::terrainFromJson(bad).has_value());

    auto built = app::buildTerrain(c);
    ASSERT_TRUE(built.has_value()) << built.error().message;
    ASSERT_NE(built->ortho, nullptr);
    EXPECT_TRUE(built->orthoWarning.empty()) << built->orthoWarning;
    EXPECT_EQ(built->orthoAttribution, "Test imagery, CC0");
    // The imagery is placed in the terrain's frame: the data centre is the origin. (The box
    // is a little larger: UTM north is turned ≈ 0.6° from true north here.)
    const terrain::OrthoMetadata& m = built->ortho->metadata();
    EXPECT_NEAR(m.minX, -404.0, 4.0);
    EXPECT_NEAR(m.maxZ, 404.0, 4.0);

    // A broken orthophoto only disables the imagery (FR-TER-I08).
    std::ofstream(dir / "broken.tif") << "not an image";
    c.ortho.files = {dir / "broken.tif"};
    built = app::buildTerrain(c);
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(built->ortho, nullptr);
    EXPECT_NE(built->orthoWarning.find("imagery disabled"), std::string::npos)
        << built->orthoWarning;
    // Imagery elsewhere: kept, with a caution.
    image.geoTransform[0] += 50'000.0;
    test::writeImage(dir / "far.tif", image);
    c.ortho.files = {dir / "far.tif"};
    built = app::buildTerrain(c);
    ASSERT_TRUE(built.has_value());
    EXPECT_NE(built->ortho, nullptr);
    EXPECT_NE(built->orthoWarning.find("do not cover"), std::string::npos);
}

TEST(FileTerrain, MissionOrthophotoColourIsParsed) {
    common::Json j = {{"source", "file"},
                      {"format", "raster"},
                      {"elevationFiles", common::Json::array({{{"path", "dem/a.tif"}}})},
                      {"orthophotoFiles", common::Json::array({{{"path", "ortho/a.tif"}}})},
                      {"orthophotoColor", {{"brightness", 0.1}, {"gamma", 1.5}}}};
    auto spec = terrain::parseFileTerrain(j);
    ASSERT_TRUE(spec.has_value()) << spec.error().message;
    EXPECT_DOUBLE_EQ(spec->orthophotoColor.brightness, 0.1);
    EXPECT_DOUBLE_EQ(spec->orthophotoColor.contrast, 1.0);
    EXPECT_DOUBLE_EQ(spec->orthophotoColor.gamma, 1.5);
    j["orthophotoColor"] = {{"gamma", 0.0}};
    EXPECT_FALSE(terrain::parseFileTerrain(j).has_value());
    j["orthophotoColor"] = {{"saturation", 1.0}};
    EXPECT_FALSE(terrain::parseFileTerrain(j).has_value());
}

TEST(FileTerrain, FadeEdgeBlendsToTheLowestHeight) {
    TempDir dir("file_terrain");
    test::writeRaster(dir / "dem.tif", test::planarUtmSpec());
    const std::array<fs::path, 1> files{dir / "dem.tif"};
    terrain::RasterOptions options;
    options.extent = terrain::Extent{1800.0, 1800.0};
    auto clamped = terrain::RasterSource::open(files, options);
    options.edge = terrain::EdgeMode::Fade;
    auto faded = terrain::RasterSource::open(files, options);
    ASSERT_TRUE(clamped.has_value() && faded.has_value());
    // 180 m band (a tenth of 1.8 km): unchanged inside it, lowest height at the border.
    EXPECT_EQ((*faded)->height(0.0, 0.0), (*clamped)->height(0.0, 0.0));
    EXPECT_EQ((*faded)->height(600.0, -300.0), (*clamped)->height(600.0, -300.0));
    const double lowest = (*faded)->metadata().minHeightM - (*faded)->metadata().origin.heightM;
    EXPECT_NEAR((*faded)->height(900.0, 0.0), lowest, 1e-9);
    EXPECT_NEAR((*faded)->height(0.0, -900.0), lowest, 1e-9);
    const double mid = (*faded)->height(810.0, 0.0);  // Halfway through the band.
    EXPECT_GT(mid, std::min(lowest, (*clamped)->height(810.0, 0.0)) - 1e-9);
    EXPECT_LT(mid, std::max(lowest, (*clamped)->height(810.0, 0.0)) + 1e-9);

    options.edge = terrain::EdgeMode::Water;
    EXPECT_EQ(terrain::RasterSource::open(files, options).error().code,
              terrain::TerrainErrorCode::NotSupported);
}

TEST(FileTerrain, PrepareReportsProgressAndHashesAFileOnce) {
    TempDir dir("file_terrain");
    fs::create_directories(dir / "mission" / "dem");
    test::writeRaster(dir / "mission" / "dem" / "a.tif", test::planarUtmSpec());
    test::writeRaster(dir / "mission" / "dem" / "b.tif", test::planarUtmSpec());
    terrain::FileTerrainSpec spec;
    spec.elevationFiles = {{"dem/a.tif", "", "/terrain/elevationFiles/0"},
                           {"dem/b.tif", "", "/terrain/elevationFiles/1"}};
    const terrain::TerrainFileRoots roots{dir / "mission", dir / "library"};
    terrain::FileHashCache cache;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reports;
    terrain::PrepareHooks hooks;
    hooks.cache = &cache;
    hooks.progress = [&](std::uint64_t done, std::uint64_t total) {
        reports.emplace_back(done, total);
    };
    auto first = terrain::prepareFileTerrain(spec, roots, {}, {}, hooks);
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_LT(reports[0].first, reports[1].first);
    EXPECT_EQ(reports[1].first, reports[1].second);
    EXPECT_TRUE(cache.find(first->elevation[0].path).has_value());

    // Same hashes from the cache; a changed file is hashed again.
    auto second = terrain::prepareFileTerrain(spec, roots, {}, {}, hooks);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->terrainDataHash, first->terrainDataHash);
    test::RasterSpec other = test::planarUtmSpec();
    other.value = [](int, int) { return 1.0; };
    test::writeRaster(dir / "mission" / "dem" / "b.tif", other);
    auto third = terrain::prepareFileTerrain(spec, roots, {}, {}, hooks);
    ASSERT_TRUE(third.has_value());
    EXPECT_NE(third->terrainDataHash, first->terrainDataHash);
}

missions::MissionDefinition fileMission(const std::vector<fs::path>& resolved) {
    missions::MissionDefinition m;
    m.id = "dem-test";
    m.terrain.source = missions::TerrainSource::File;
    terrain::FileTerrainSpec spec;
    spec.elevationFiles = {{"dem/a.tif", "", "/terrain/elevationFiles/0"}};
    spec.attribution = "Mission DEM";
    spec.verticalDatum = "EGM2008";
    spec.resolvedElevationFiles = resolved;
    m.terrain.file = spec;
    return m;
}

TEST(FileTerrain, MissionsOnElevationRastersArePlayable) {
    missions::MissionDefinition m = fileMission({});
    ASSERT_TRUE(app::unplayableReason(m).has_value());
    EXPECT_NE(app::unplayableReason(m)->find("not found"), std::string::npos);
    // A point cloud needs the data cache, where its DTM is kept (FR-TER-L05).
    m = fileMission({"/data/a.las"});
    m.terrain.file->format = terrain::ElevationFormat::PointCloud;
    m.terrain.file->resolutionM = 1.5;
    m.terrain.file->interpolation = terrain::LidarInterpolation::Idw;
    ASSERT_TRUE(app::unplayableReason(m).has_value());
    EXPECT_NE(app::unplayableReason(m)->find("data cache"), std::string::npos);
    auto lidar = app::missionTerrain(m);
    ASSERT_TRUE(lidar.has_value()) << lidar.error().message;
    EXPECT_TRUE(app::isPointCloud(lidar->file));
    EXPECT_EQ(lidar->file.resolutionM, 1.5);
    EXPECT_EQ(app::lidarOptions(lidar->file).interpolation, terrain::LidarInterpolation::Idw);
    EXPECT_FALSE(app::buildTerrain(*lidar).has_value());  // No cache to read the DTM from.

    m = fileMission({"/data/a.tif"});
    EXPECT_FALSE(app::unplayableReason(m).has_value());
    m.terrain.origin = geo::LatLon{40.4, -3.69};
    m.terrain.originAltitudeM = 700.0;
    m.terrain.originAltitudeRef = items::AltitudeRef::Amsl;
    m.terrain.extent = terrain::Extent{1000.0, 800.0};
    auto config = app::missionTerrain(m);
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->kind, app::TerrainKind::File);
    EXPECT_EQ(config->file.files, std::vector<fs::path>{"/data/a.tif"});
    EXPECT_EQ(config->file.attribution, "Mission DEM");
    EXPECT_EQ(config->file.verticalDatum, "EGM2008");
    EXPECT_EQ(config->file.origin, (geo::LatLon{40.4, -3.69}));
    EXPECT_EQ(config->file.originAltitudeM, 700.0);
    EXPECT_EQ(config->extent, (terrain::Extent{1000.0, 800.0}));

    // An AGL origin puts the ground at the origin at height 0.
    m.terrain.originAltitudeRef = items::AltitudeRef::Agl;
    EXPECT_FALSE(app::missionTerrain(m)->file.originAltitudeM.has_value());
}

TEST(FileTerrain, HudWarnsBeforeTheTerrainEdge) {
    sim::WorldSnapshot s;
    s.hasVehicle = true;
    s.telemetry.battery.cells = 4;
    s.telemetry.battery.voltage = 15.6;
    const geo::LocalFrame frame({40.4, -3.7, 650.0});
    sim::HudOptions options;
    options.terrainExtent = terrain::Extent{2000.0, 1000.0};
    auto edgeWarning = [&](double x, double z) -> std::optional<sim::HudLevel> {
        s.body.position = physics::Vec3(x, 10.0, z);
        for (const sim::HudItem& w : sim::buildHud(s, frame, options).warnings) {
            if (w.text.starts_with("TERRAIN EDGE")) {
                return w.level;
            }
        }
        return std::nullopt;
    };
    EXPECT_FALSE(edgeWarning(0.0, 0.0).has_value());
    EXPECT_FALSE(edgeWarning(650.0, 150.0).has_value());          // 350 m from the east.
    EXPECT_EQ(edgeWarning(800.0, 0.0), sim::HudLevel::Caution);   // 200 m.
    EXPECT_EQ(edgeWarning(0.0, -450.0), sim::HudLevel::Warning);  // 50 m from the north.
    EXPECT_EQ(edgeWarning(1200.0, 0.0), sim::HudLevel::Warning);  // Beyond it.
    options.terrainExtent.reset();
    EXPECT_FALSE(edgeWarning(1200.0, 0.0).has_value());
}

}  // namespace
