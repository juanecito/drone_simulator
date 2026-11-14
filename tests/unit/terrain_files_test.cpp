// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/terrain_files.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <stop_token>
#include <string>

#include "ds/common/json.hpp"
#include "ds/common/sha256.hpp"

#include <unistd.h>

namespace {

using namespace ds::terrain;
using ds::common::Json;
namespace fs = std::filesystem;

Json rasterTerrain() {
    return Json::parse(R"({
        "source": "file", "format": "raster",
        "elevationFiles": [ { "path": "dem/valley.tif" } ],
        "verticalDatum": "EGM2008",
        "attribution": "Contains modified Copernicus DEM data",
        "origin": { "geo": [40.4170, -3.7036, 0], "altRef": "AGL" },
        "extent": { "widthM": 4000, "lengthM": 4000 }
    })");
}

Json lazTerrain() {
    return Json::parse(R"({
        "source": "file", "format": "laz",
        "elevationFiles": [ { "path": "dem/a.laz" }, { "path": "library:b.laz" } ],
        "crs": "EPSG:25830", "verticalDatum": "EGM08-REDNAP", "surface": "dtm",
        "resolutionM": 1.0, "interpolation": "tin", "classifyGround": false,
        "attribution": "LiDAR data, CC BY 4.0"
    })");
}

struct Folders {
    fs::path root;
    TerrainFileRoots roots;
    Folders() {
        root = fs::temp_directory_path() / std::format("ds_terrain_files_{}", ::getpid());
        fs::remove_all(root);
        roots.missionDir = root / "missions" / "valley";
        roots.libraryDir = root / "library";
        fs::create_directories(roots.missionDir / "dem");
        fs::create_directories(roots.missionDir / "ortho");
        fs::create_directories(roots.libraryDir);
    }
    ~Folders() { fs::remove_all(root); }
    Folders(const Folders&) = delete;
    Folders& operator=(const Folders&) = delete;
    static void write(const fs::path& file, std::string_view content) {
        std::ofstream(file, std::ios::binary) << content;
    }
};

// --- Parsing (FR-TER-F02, FR-TER-L02 … L04, FR-TER-I02) -----------------------------------------

TEST(FileTerrainSpec, ParsesSpecificationExamples) {
    auto raster = parseFileTerrain(rasterTerrain());
    ASSERT_TRUE(raster) << raster.error().message;
    EXPECT_EQ(raster->format, ElevationFormat::Raster);
    ASSERT_EQ(raster->elevationFiles.size(), 1U);
    EXPECT_EQ(raster->elevationFiles[0].jsonPath, "/terrain/elevationFiles/0");
    EXPECT_DOUBLE_EQ(*raster->originLatDeg, 40.4170);
    EXPECT_EQ(raster->extent, (Extent{4000, 4000}));

    auto laz = parseFileTerrain(lazTerrain());
    ASSERT_TRUE(laz) << laz.error().message;
    EXPECT_EQ(laz->format, ElevationFormat::PointCloud);
    EXPECT_EQ(laz->crs, "EPSG:25830");
    EXPECT_EQ(laz->resolutionM, 1.0);
    EXPECT_EQ(laz->interpolation, LidarInterpolation::Tin);
}

TEST(FileTerrainSpec, RejectsDsmWithClearMessage) {
    Json j = lazTerrain();
    j["surface"] = "dsm";
    auto r = parseFileTerrain(j);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, TerrainErrorCode::NotSupported);
    EXPECT_NE(r.error().message.find("not supported in v1.0"), std::string::npos);
    j["surface"] = "dtm";
    j["interpolation"] = "max";
    EXPECT_EQ(parseFileTerrain(j).error().code, TerrainErrorCode::NotSupported);
}

TEST(FileTerrainSpec, ErrorsNameTheJsonPath) {
    Json j = rasterTerrain();
    j["elevationFiles"][0]["sha256"] = "xyz";
    j["resolutionM"] = 1.0;  // LAS/LAZ only.
    j["bogus"] = true;
    auto r = parseFileTerrain(j);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, TerrainErrorCode::InvalidSpec);
    EXPECT_NE(r.error().message.find("/terrain/elevationFiles/0/sha256"), std::string::npos);
    EXPECT_NE(r.error().message.find("/terrain/resolutionM"), std::string::npos);
    EXPECT_NE(r.error().message.find("/terrain/bogus: unknown property"), std::string::npos);

    Json empty = rasterTerrain();
    empty["elevationFiles"] = Json::array();
    EXPECT_FALSE(parseFileTerrain(empty));
}

TEST(FileTerrainSpec, FileKindsMustMatchFormat) {
    Json mixed = rasterTerrain();
    mixed["elevationFiles"].push_back({{"path", "dem/b.laz"}});
    EXPECT_EQ(parseFileTerrain(mixed).error().code, TerrainErrorCode::MixedFormats);
    Json unknown = rasterTerrain();
    unknown["elevationFiles"][0]["path"] = "dem/valley.xyz";
    EXPECT_EQ(parseFileTerrain(unknown).error().code, TerrainErrorCode::UnsupportedFormat);
    Json mixedRasters = rasterTerrain();  // Different raster formats may be mixed (FR-TER-F04).
    mixedRasters["elevationFiles"].push_back({{"path", "dem/N40W004.hgt"}});
    mixedRasters["elevationFiles"].push_back({{"path", "library:grid.ASC"}});
    EXPECT_TRUE(parseFileTerrain(mixedRasters));
}

// --- Path rules (FR-TER-F03) --------------------------------------------------------------------

TEST(TerrainPaths, RelativeAndLibraryAccepted) {
    Folders f;
    Folders::write(f.roots.missionDir / "dem/valley.tif", "x");
    Folders::write(f.roots.libraryDir / "big.tif", "y");
    auto rel = resolveTerrainPath("dem/valley.tif", f.roots);
    ASSERT_TRUE(rel) << rel.error().message;
    EXPECT_EQ(*rel, fs::canonical(f.roots.missionDir / "dem/valley.tif"));
    auto lib = resolveTerrainPath("library:big.tif", f.roots);
    ASSERT_TRUE(lib) << lib.error().message;
    EXPECT_EQ(*lib, fs::canonical(f.roots.libraryDir / "big.tif"));
}

TEST(TerrainPaths, EscapesRejected) {
    Folders f;
    Folders::write(f.root / "secret.tif", "s");
    Folders::write(f.roots.missionDir / "dem/ok.tif", "x");
    const fs::path absolute = f.roots.missionDir / "dem/ok.tif";
    EXPECT_EQ(resolveTerrainPath(absolute.string(), f.roots).error().code,
              TerrainErrorCode::PathRejected);
    EXPECT_EQ(resolveTerrainPath("../../secret.tif", f.roots).error().code,
              TerrainErrorCode::PathRejected);
    EXPECT_EQ(resolveTerrainPath("dem/../dem/ok.tif", f.roots).error().code,
              TerrainErrorCode::PathRejected);
    EXPECT_EQ(resolveTerrainPath("library:../secret.tif", f.roots).error().code,
              TerrainErrorCode::PathRejected);
    EXPECT_EQ(resolveTerrainPath("dem\\ok.tif", f.roots).error().code,
              TerrainErrorCode::PathRejected);
    EXPECT_EQ(resolveTerrainPath("C:/x.tif", f.roots).error().code, TerrainErrorCode::PathRejected);
    // A symlink inside the mission folder that points outside it.
    fs::create_symlink(f.root / "secret.tif", f.roots.missionDir / "dem/link.tif");
    EXPECT_EQ(resolveTerrainPath("dem/link.tif", f.roots).error().code,
              TerrainErrorCode::PathRejected);
    // A symlink that stays inside is fine.
    fs::create_symlink(f.roots.missionDir / "dem/ok.tif", f.roots.missionDir / "dem/alias.tif");
    EXPECT_TRUE(resolveTerrainPath("dem/alias.tif", f.roots));
    EXPECT_EQ(resolveTerrainPath("dem/missing.tif", f.roots).error().code,
              TerrainErrorCode::FileNotFound);
    EXPECT_EQ(resolveTerrainPath("dem", f.roots).error().code, TerrainErrorCode::FileNotFound);
}

// --- Integrity (FR-TER-F09, FR-TER-I08, FR-TER-I09) ---------------------------------------------

TEST(PrepareFileTerrain, HashesVerifiesAndDisablesBrokenOrthophotos) {
    Folders f;
    Folders::write(f.roots.missionDir / "dem/valley.tif", "elevation");
    Folders::write(f.roots.missionDir / "ortho/a.tif", "image");
    Json j = rasterTerrain();
    j["elevationFiles"][0]["sha256"] = ds::common::sha256Hex("elevation");
    j["orthophotoFiles"] = Json::array({{{"path", "ortho/a.tif"}}});
    auto spec = parseFileTerrain(j);
    ASSERT_TRUE(spec) << spec.error().message;

    auto ok = prepareFileTerrain(*spec, f.roots);
    ASSERT_TRUE(ok) << ok.error().message;
    ASSERT_EQ(ok->elevation.size(), 1U);
    EXPECT_EQ(ok->elevation[0].sha256, ds::common::sha256Hex("elevation"));
    EXPECT_EQ(ok->orthophotos.size(), 1U);
    EXPECT_TRUE(ok->warnings.empty());
    EXPECT_TRUE(ds::common::isSha256Hex(ok->terrainDataHash));

    // Orthophotos don't change the data hash (FR-TER-I09); settings do.
    FileTerrainSpec noOrtho = *spec;
    noOrtho.orthophotoFiles.clear();
    EXPECT_EQ(prepareFileTerrain(noOrtho, f.roots)->terrainDataHash, ok->terrainDataHash);
    FileTerrainSpec otherBand = *spec;
    otherBand.band = 2;
    EXPECT_NE(prepareFileTerrain(otherBand, f.roots)->terrainDataHash, ok->terrainDataHash);

    // A broken orthophoto only disables the imagery (FR-TER-I08).
    FileTerrainSpec brokenOrtho = *spec;
    brokenOrtho.orthophotoFiles[0].sha256 = ds::common::sha256Hex("other");
    auto warned = prepareFileTerrain(brokenOrtho, f.roots);
    ASSERT_TRUE(warned);
    EXPECT_TRUE(warned->orthophotos.empty());
    ASSERT_EQ(warned->warnings.size(), 1U);
    EXPECT_NE(warned->warnings[0].find("imagery disabled"), std::string::npos);

    // A changed elevation file invalidates the mission.
    Folders::write(f.roots.missionDir / "dem/valley.tif", "elevation, edited");
    auto mismatch = prepareFileTerrain(*spec, f.roots);
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().code, TerrainErrorCode::Sha256Mismatch);
    EXPECT_NE(mismatch.error().message.find("/terrain/elevationFiles/0"), std::string::npos);
}

TEST(PrepareFileTerrain, LimitsAndCancel) {
    Folders f;
    Folders::write(f.roots.missionDir / "dem/valley.tif", std::string(2000, 'z'));
    auto spec = parseFileTerrain(rasterTerrain());
    ASSERT_TRUE(spec);
    auto tooLarge = prepareFileTerrain(*spec, f.roots, TerrainFileLimits{.maxRasterBytes = 1000});
    ASSERT_FALSE(tooLarge);
    EXPECT_EQ(tooLarge.error().code, TerrainErrorCode::TooLarge);

    std::stop_source stop;
    stop.request_stop();
    auto cancelled = prepareFileTerrain(*spec, f.roots, {}, stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, TerrainErrorCode::Cancelled);
}

}  // namespace
