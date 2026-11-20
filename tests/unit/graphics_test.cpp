// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>

#include "ds/astro/sky_coordinates.hpp"
#include "ds/astro/stars.hpp"
#include "ds/common/math.hpp"
#include "ds/graphics/camera.hpp"
#include "ds/graphics/camera_rig.hpp"
#include "ds/graphics/drone_model.hpp"
#include "ds/graphics/exposure.hpp"
#include "ds/graphics/image.hpp"
#include "ds/graphics/light.hpp"
#include "ds/graphics/mesh.hpp"
#include "ds/graphics/rover_model.hpp"
#include "ds/graphics/shader_source.hpp"
#include "ds/graphics/shadow_cascades.hpp"
#include "ds/graphics/sky_model.hpp"
#include "ds/graphics/weather_view.hpp"
#include "ds/weather/weather.hpp"

namespace {

using namespace ds::graphics;
namespace fs = std::filesystem;

ds::astro::UtcTime utc(std::string_view iso) { return *ds::astro::parseUtc(iso); }

double angleBetweenDeg(const glm::dvec3& a, const glm::dvec3& b) {
    return ds::common::radToDeg(
        std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)));
}

// --- Camera (FR-GFX-05/06) ------------------------------------------------------------------

TEST(Camera, AxesFollowHeadingAndPitch) {
    Camera camera;
    EXPECT_NEAR(glm::distance(camera.forward(), glm::dvec3(0, 0, -1)), 0.0, 1e-12);  // North.
    camera.yawRad = ds::common::kPi / 2.0;
    EXPECT_NEAR(glm::distance(camera.forward(), glm::dvec3(1, 0, 0)), 0.0, 1e-12);  // East.
    EXPECT_NEAR(glm::distance(camera.right(), glm::dvec3(0, 0, 1)), 0.0, 1e-12);    // South.
    camera.pitchRad = 0.3;
    EXPECT_NEAR(glm::dot(camera.forward(), camera.up()), 0.0, 1e-12);
    EXPECT_GT(camera.up().y, 0.0);
}

TEST(Camera, ReversedInfiniteProjection) {
    Camera camera{.nearM = 0.5};
    const glm::mat4 vp = camera.viewProjection();
    const auto depth = [&](double distance) {
        const glm::vec4 clip = vp * glm::vec4(glm::vec3(camera.forward() * distance), 1.0F);
        return clip.z / clip.w;
    };
    EXPECT_NEAR(depth(0.5), 1.0, 1e-6);
    EXPECT_NEAR(depth(1.0), 0.5, 1e-6);
    EXPECT_GT(depth(100.0), depth(1000.0));
    EXPECT_GT(depth(1.0e7), 0.0);
    EXPECT_LT(depth(1.0e7), 1e-6);
    // A point straight ahead projects to the image centre.
    const glm::vec4 c = vp * glm::vec4(glm::vec3(camera.forward() * 50.0), 1.0F);
    EXPECT_NEAR(c.x / c.w, 0.0, 1e-6);
    EXPECT_NEAR(c.y / c.w, 0.0, 1e-6);
    // Up in the world is up on screen.
    const glm::vec4 u =
        vp * glm::vec4(glm::vec3(camera.forward() * 50.0 + glm::dvec3(0, 5, 0)), 1.0F);
    EXPECT_GT(u.y / u.w, 0.0);
}

TEST(Camera, FrustumCulling) {
    const Camera camera;  // Looking north (−z), 60° vertical FOV.
    const Frustum frustum(camera.viewProjection());
    EXPECT_TRUE(frustum.intersects({{-1, -1, -101}, {1, 1, -99}}));       // Ahead.
    EXPECT_FALSE(frustum.intersects({{-1, -1, 99}, {1, 1, 101}}));        // Behind.
    EXPECT_FALSE(frustum.intersects({{500, -1, -101}, {502, 1, -99}}));   // Far right.
    EXPECT_TRUE(frustum.intersects({{-1e5, -10, -1e5}, {1e5, 0, 1e5}}));  // Huge ground box.
    EXPECT_TRUE(frustum.intersects({{-1, -1, -1e7}, {1, 1, -1e7 + 2}}));  // No far plane.
}

TEST(FreeFly, MovesTurnsAndStaysAboveGround) {
    Camera camera;
    camera.position = {0, 100, 0};
    FreeFlyController fly;
    fly.setBaseSpeed(10.0);
    fly.update(camera, {.forward = 1.0}, 1.0);
    EXPECT_LT(camera.position.z, -9.9);  // Moved north by at least the base speed.
    fly.update(camera, {.yawDeltaRad = ds::common::kPi / 2}, 0.016);
    EXPECT_NEAR(camera.yawRad, ds::common::kPi / 2, 1e-12);
    fly.update(camera, {.pitchDeltaRad = 10.0}, 0.016);
    EXPECT_LT(camera.pitchRad, ds::common::kPi / 2);  // Clamped below 90°.

    fly.setGround([](double, double) { return std::optional<double>(95.0); });
    camera.pitchRad = 0.0;
    fly.update(camera, {.up = -1.0, .boost = true}, 10.0);
    EXPECT_DOUBLE_EQ(camera.position.y, 95.0 + FreeFlyController::kClearanceM);
}

// --- Shader sources (FR-GFX-04) ----------------------------------------------------------------

class ShaderSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("ds_shader_test_" + std::to_string(::getpid()));
        fs::create_directories(dir_ / "lib");
    }
    void TearDown() override { fs::remove_all(dir_); }
    void write(const fs::path& name, std::string_view text) { std::ofstream(dir_ / name) << text; }
    fs::path dir_;
};

TEST_F(ShaderSourceTest, IncludesDefinesAndLineDirectives) {
    write("main.frag",
          "// header\n#version 450 core\n#include \"lib/common.glsl\"\nvoid main() {}\n");
    write("lib/common.glsl",
          "#include \"lib/math.glsl\"\nfloat common_value;\n#include \"lib/math.glsl\"\n");
    write("lib/math.glsl", "const float PI = 3.14159;\n");
    const auto source = loadShaderSource(dir_, "main.frag", {"MSAA 4"});
    ASSERT_TRUE(source.has_value()) << source.error().message;
    ASSERT_EQ(source->files.size(), 3U);
    EXPECT_EQ(source->files[1], fs::path("lib/common.glsl"));
    const std::string& t = source->text;
    EXPECT_LT(t.find("#version 450 core"), t.find("#define MSAA 4"));
    EXPECT_NE(t.find("#line 3 0"), std::string::npos);
    EXPECT_NE(t.find("#line 1 1"), std::string::npos);
    EXPECT_NE(t.find("const float PI"), std::string::npos);
    EXPECT_EQ(t.find("const float PI"), t.rfind("const float PI"));  // Included once.
    EXPECT_NE(t.find("#line 4 0\nvoid main"), std::string::npos);
}

TEST_F(ShaderSourceTest, Errors) {
    write("noversion.vert", "void main() {}\n");
    EXPECT_FALSE(loadShaderSource(dir_, "noversion.vert").has_value());
    write("missing.vert", "#version 450\n#include \"nope.glsl\"\n");
    const auto missing = loadShaderSource(dir_, "missing.vert");
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message.find("missing.vert:2"), std::string::npos);
    write("escape.vert", "#version 450\n#include \"../../etc/passwd\"\n");
    EXPECT_FALSE(loadShaderSource(dir_, "escape.vert").has_value());
    EXPECT_FALSE(loadShaderSource(dir_, "absent.vert").has_value());
}

TEST(ShaderLog, MapsDriverPositionsToFiles) {
    const std::vector<fs::path> files{"terrain.frag", "atmosphere.glsl"};
    EXPECT_EQ(mapShaderLog("1(42) : error C1008: undefined variable", files),
              "atmosphere.glsl:42 : error C1008: undefined variable");
    EXPECT_EQ(mapShaderLog("0:17(5): error: syntax error", files),
              "terrain.frag:17: error: syntax error");
    EXPECT_EQ(mapShaderLog("ERROR: 0:3: 'x' : undeclared", files),
              "ERROR: terrain.frag:3: 'x' : undeclared");
    EXPECT_EQ(mapShaderLog("7(1) : unknown file", files), "7(1) : unknown file");
}

// --- Sky model (FR-GFX-08, FR-AST-09) ------------------------------------------------------------

TEST(SkyModel, PhotometryHelpers) {
    EXPECT_NEAR(magnitudeToIlluminanceLux(0.0), 2.54e-6, 0.05e-6);
    EXPECT_NEAR(magnitudeToIlluminanceLux(-12.73), 0.32, 0.03);  // Full moon ≈ 0.3 lux.
    EXPECT_NEAR(moonMagnitude(0.0, 384'400.0), -12.73, 1e-9);
    EXPECT_GT(moonMagnitude(90.0, 384'400.0), -10.5);  // Quarter ≈ 2.5 mag fainter.
    EXPECT_LT(moonMagnitude(0.0, 357'000.0), moonMagnitude(0.0, 406'000.0));
    EXPECT_NEAR(colorIndexToTemperature(0.65), 5800.0, 200.0);  // The Sun.
    EXPECT_GT(colorIndexToTemperature(-0.3), 15'000.0);
    EXPECT_LT(colorIndexToTemperature(1.6), 3700.0);
}

TEST(SkyModel, BlackbodyColours) {
    const auto luminance = [](glm::vec3 c) {
        return 0.2126F * c.r + 0.7152F * c.g + 0.0722F * c.b;
    };
    const glm::vec3 sun = blackbodyColor(5772.0);
    const glm::vec3 d65 = blackbodyColor(6504.0);
    const glm::vec3 hot = blackbodyColor(20'000.0);
    const glm::vec3 cool = blackbodyColor(3000.0);
    for (const auto& c : {sun, d65, hot, cool}) {
        EXPECT_NEAR(luminance(c), 1.0F, 1e-5F);
    }
    EXPECT_NEAR(d65.r, d65.b, 0.06F);  // ≈ white.
    EXPECT_GT(sun.r, sun.b);           // Slightly warm.
    EXPECT_GT(hot.b, hot.r);           // Blue-white.
    EXPECT_GT(cool.r, 2.0F * cool.b);  // Orange.
}

TEST(SkyModel, DirectionsMatchAstronomy) {
    const ds::geo::Geodetic madrid{40.4168, -3.7038, 667.0};
    const auto t = utc("2026-06-21T12:00:00Z");
    const SkyState s = computeSkyState(t, madrid);
    const auto sun = ds::astro::sunPosition(t, madrid);
    EXPECT_NEAR(std::asin(s.sun.direction.y), ds::common::degToRad(sun.apparent.elevationDeg),
                1e-6);
    EXPECT_GT(s.sun.elevationDeg, 70.0);  // Near the summer-solstice culmination (73°).
    EXPECT_NEAR(s.sun.illuminanceLux, 128'000.0 / (1.0163 * 1.0163), 400.0);  // Aphelion.
    EXPECT_NEAR(ds::common::radToDeg(s.sun.angularRadiusRad), 0.262, 0.002);
    EXPECT_NEAR(glm::length(s.celestialNorth), 1.0F, 1e-6F);
    EXPECT_NEAR(ds::common::radToDeg(std::asin(s.celestialNorth.y)), madrid.latitudeDeg, 1e-4);
    // The ecliptic pole is 23.44° from the celestial pole.
    EXPECT_NEAR(glm::length(s.moonNorth), 1.0F, 1e-5F);
    EXPECT_NEAR(ds::common::radToDeg(std::acos(glm::dot(s.moonNorth, s.celestialNorth))), 23.44,
                0.02);
}

TEST(SkyModel, EquatorialRotationMatchesHorizontalConversion) {
    const double last = 123.4;
    const double lat = -33.9;
    const glm::dmat3 m = equatorialToWorld(last, lat);
    for (const auto& [ra, dec] :
         {std::pair{10.0, 20.0}, {200.0, -60.0}, {123.4, -33.9}, {0.0, 89.0}}) {
        const double a = ds::common::degToRad(ra);
        const double d = ds::common::degToRad(dec);
        const glm::dvec3 world =
            m * glm::dvec3(std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d));
        const auto h = ds::astro::equatorialToHorizontal({ra, dec}, last, lat);
        EXPECT_LT(angleBetweenDeg(world, horizontalToWorld(h.azimuthDeg, h.elevationDeg)), 1e-9);
    }
}

TEST(SkyModel, MoonLightingGivesThePhase) {
    // Full moon 2026-03-03 11:38 UTC (total lunar eclipse): the moon-to-sun vector points
    // back at the observer; near first quarter it is about perpendicular.
    const ds::geo::Geodetic observer{0.0, 0.0, 0.0};
    const SkyState full = computeSkyState(utc("2026-03-03T11:38:00Z"), observer);
    EXPECT_GT(full.moonPhase.illuminatedFraction, 0.99);
    EXPECT_LT(angleBetweenDeg(full.moonToSun, -glm::dvec3(full.moon.direction)), 2.0);
    EXPECT_NEAR(full.moon.illuminanceLux, 0.3, 0.1);
    const SkyState quarter = computeSkyState(utc("2026-03-25T19:18:00Z"), observer);
    EXPECT_NEAR(angleBetweenDeg(quarter.moonToSun, -glm::dvec3(quarter.moon.direction)), 90.0, 2.0);
    EXPECT_LT(quarter.moon.illuminanceLux, 0.2F * full.moon.illuminanceLux);
    // Disc luminance: full-moon illuminance spread over the disc ≈ 2 500 cd/m²·(×2 for L–S).
    EXPECT_NEAR(full.moonDiscLuminance, 2.0 * 0.3 / (3.14159 * std::pow(0.0045, 2)), 2500.0);
}

TEST(SkyModel, StarsAndCaching) {
    SkyModel model;
    const auto catalog =
        ds::astro::loadStarCatalog(fs::path(DS_TEST_ASSETS_DIR) / "sky" / "bsc5.csv");
    ASSERT_TRUE(catalog.has_value());
    model.setCatalog(*catalog);
    const ds::geo::Geodetic observer{40.0, -3.0, 0.0};
    EXPECT_TRUE(model.update(utc("2026-01-01T22:00:00Z"), observer));
    EXPECT_EQ(model.stars().size(), catalog->size());
    const auto version = model.starsVersion();
    EXPECT_FALSE(model.update(utc("2026-01-01T22:00:00.050Z"), observer));  // < 100 ms.
    EXPECT_TRUE(model.update(utc("2026-01-01T22:00:01Z"), observer));
    EXPECT_EQ(model.starsVersion(), version);  // Same day: star places reused.
    EXPECT_TRUE(model.update(utc("2026-01-03T22:00:01Z"), observer));
    EXPECT_GT(model.starsVersion(), version);

    // Polaris (HR 424) after rotation: elevation ≈ latitude.
    const auto polaris = std::ranges::find(*catalog, 424, &ds::astro::CatalogStar::hr);
    const auto index = static_cast<std::size_t>(std::distance(catalog->begin(), polaris));
    const glm::vec3 world = model.state().equatorialToWorld * model.stars()[index].direction;
    EXPECT_NEAR(ds::common::radToDeg(std::asin(world.y)), 40.0, 0.8);
    // Sirius is brighter and bluer than Betelgeuse.
    const auto at = [&](int hr) {
        return model.stars()[static_cast<std::size_t>(std::distance(
            catalog->begin(), std::ranges::find(*catalog, hr, &ds::astro::CatalogStar::hr)))];
    };
    EXPECT_GT(at(2491).illuminanceLux, at(2061).illuminanceLux);
    EXPECT_GT(at(2491).color.b / at(2491).color.r, at(2061).color.b / at(2061).color.r);
}

// --- Exposure (FR-GFX-08c) -----------------------------------------------------------------------

TEST(Exposure, PhotographicRelations) {
    // Sunny-16: EV100 15 for an average scene of ≈ 4 100 cd/m².
    EXPECT_NEAR(ev100FromLuminance(4096.0), 15.0, 1e-9);
    EXPECT_NEAR(exposureFromEv100(0.0), 1.0 / 1.2, 1e-12);
    EXPECT_NEAR(exposureFromEv100(15.0) * 4096.0, 4096.0 / (1.2 * 32768.0), 1e-12);
    EXPECT_DOUBLE_EQ(nightBiasEv(12.0), 0.0);
    EXPECT_DOUBLE_EQ(nightBiasEv(-20.0), 6.0);
    // Night stays darker than mid-grey; day is metered normally.
    EXPECT_DOUBLE_EQ(autoExposureEv100(4096.0, 0.0), 15.0);
    EXPECT_GT(autoExposureEv100(2e-4, 0.0), ev100FromLuminance(2e-4));
    EXPECT_GE(autoExposureEv100(1e-12, 0.0), kMinEv100);
    EXPECT_LE(autoExposureEv100(1e12, 0.0), kMaxEv100);
}

TEST(Exposure, Adaptation) {
    double ev = 15.0;
    for (int i = 0; i < 60; ++i) {
        ev = adaptEv100(ev, 5.0, 1.0 / 60.0);  // Into a dark tunnel: 1 s.
    }
    EXPECT_LT(ev, 7.0);  // Brightens quickly (τ 0.6 s: 81 % of the way in 1 s).
    double ev2 = 5.0;
    for (int i = 0; i < 60; ++i) {
        ev2 = adaptEv100(ev2, 15.0, 1.0 / 60.0);
    }
    EXPECT_LT(ev2, 10.0);  // Darkens slowly (τ 2.5 s).
    EXPECT_GT(ev2, 7.0);
}

// --- PGM -----------------------------------------------------------------------------------------

TEST(Image, ReadsPgmAndRejectsBadFiles) {
    const auto moon = loadPgm(fs::path(DS_TEST_ASSETS_DIR) / "sky" / "moon_albedo.pgm");
    ASSERT_TRUE(moon.has_value()) << moon.error().message;
    EXPECT_EQ(moon->width, 1024);
    EXPECT_EQ(moon->height, 512);
    const auto dir = fs::temp_directory_path();
    std::ofstream(dir / "ds_bad.pgm", std::ios::binary) << "P5\n4 4\n255\nabc";
    EXPECT_FALSE(loadPgm(dir / "ds_bad.pgm").has_value());  // Truncated.
    std::ofstream(dir / "ds_bad.pgm", std::ios::binary) << "P6\n1 1\n255\nabc";
    EXPECT_FALSE(loadPgm(dir / "ds_bad.pgm").has_value());  // Colour PPM.
    fs::remove(dir / "ds_bad.pgm");
    EXPECT_FALSE(loadPgm("/nonexistent.pgm").has_value());
}

// --- Meshes (FR-GFX-12) ---------------------------------------------------------------------

// Every triangle is in range, front faces are counter-clockwise (agreeing with the vertex
// normals) and normals are unit length.
void expectWellFormed(const Mesh& mesh) {
    ASSERT_EQ(mesh.indices.size() % 3, 0U);
    for (const MeshVertex& v : mesh.vertices) {
        EXPECT_NEAR(glm::length(v.normal), 1.0F, 1e-4F);
    }
    int checked = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        ASSERT_LT(mesh.indices[i], mesh.vertices.size());
        ASSERT_LT(mesh.indices[i + 1], mesh.vertices.size());
        ASSERT_LT(mesh.indices[i + 2], mesh.vertices.size());
        const MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const glm::vec3 face = glm::cross(b.position - a.position, c.position - a.position);
        if (glm::length(face) < 1e-6F) {
            continue;  // Degenerate (sphere poles).
        }
        EXPECT_GT(glm::dot(face, a.normal + b.normal + c.normal), 0.0F) << "triangle " << i / 3;
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST(Mesh, BoxIsClosedWithOutwardNormals) {
    Mesh mesh;
    const glm::vec3 centre(1, 2, 3);
    const glm::quat r = glm::angleAxis(0.7F, glm::normalize(glm::vec3(1, 1, 0)));
    appendBox(mesh, centre, glm::vec3(0.5F, 1.0F, 2.0F), {glm::vec3(0.2F), 0.5F, 3.0F}, r);
    EXPECT_EQ(mesh.vertices.size(), 24U);
    EXPECT_EQ(mesh.indices.size(), 36U);
    expectWellFormed(mesh);
    for (const MeshVertex& v : mesh.vertices) {
        EXPECT_GT(glm::dot(v.normal, v.position - centre), 0.0F);  // Outward.
        EXPECT_NEAR(v.color.a, 0.5F, 1e-6F);
        EXPECT_NEAR(v.emissive, 3.0F, 1e-6F);
    }
    // A second primitive appends with offset indices.
    appendBox(mesh, centre, glm::vec3(1.0F), {});
    EXPECT_EQ(mesh.vertices.size(), 48U);
    EXPECT_EQ(*std::max_element(mesh.indices.begin(), mesh.indices.end()), 47U);
    expectWellFormed(mesh);
}

TEST(Mesh, CylinderDiscAndSphere) {
    Mesh cylinder;
    appendCylinder(cylinder, glm::vec3(0, 0, 0), glm::vec3(0, 0, -2), 0.5F, 12, {});
    expectWellFormed(cylinder);
    for (const MeshVertex& v : cylinder.vertices) {
        EXPECT_LE(std::hypot(v.position.x, v.position.y), 0.5F + 1e-5F);
        EXPECT_GE(v.position.z, -2.0F - 1e-5F);
        EXPECT_LE(v.position.z, 1e-5F);
    }

    Mesh disc;
    appendDisc(disc, glm::vec3(0, 1, 0), glm::vec3(0, 1, 0), 2.0F, 16, {});
    expectWellFormed(disc);
    int up = 0;
    int down = 0;
    for (const MeshVertex& v : disc.vertices) {
        EXPECT_NEAR(v.position.y, 1.0F, 1e-6F);
        up += v.normal.y > 0.99F ? 1 : 0;
        down += v.normal.y < -0.99F ? 1 : 0;
    }
    EXPECT_GT(up, 0);  // Both faces.
    EXPECT_EQ(up, down);

    Mesh sphere;
    const glm::vec3 c(0, 0, 5);
    appendSphere(sphere, c, 1.5F, 10, {});
    expectWellFormed(sphere);
    for (const MeshVertex& v : sphere.vertices) {
        EXPECT_NEAR(glm::distance(v.position, c), 1.5F, 1e-4F);
        EXPECT_GT(glm::dot(v.normal, v.position - c), 0.0F);
    }
}

// --- Drone model ----------------------------------------------------------------------------

// Level body (x forward, y right, z down) with the nose north (−z).
glm::dquat levelNorth() {
    return glm::quat_cast(
        glm::dmat3(glm::dvec3(0, 0, -1), glm::dvec3(1, 0, 0), glm::dvec3(0, -1, 0)));
}

DroneModel racer() {
    const double a = 0.08;
    return DroneModel(droneShapeFor(
        {glm::dvec3(a, a, 0), glm::dvec3(-a, a, 0), glm::dvec3(-a, -a, 0), glm::dvec3(a, -a, 0)},
        0.127, 3, 0.03, 30.0));
}

TEST(DroneModel, ShapeScalesWithTheFrame) {
    const double a = 0.08;
    const DroneShape small = droneShapeFor(
        {glm::dvec3(a, a, 0), glm::dvec3(-a, a, 0), glm::dvec3(-a, -a, 0), glm::dvec3(a, -a, 0)},
        0.127, 3, 0.03, 30.0);
    const DroneShape big =
        droneShapeFor({glm::dvec3(2 * a, 2 * a, 0), glm::dvec3(-2 * a, 2 * a, 0),
                       glm::dvec3(-2 * a, -2 * a, 0), glm::dvec3(2 * a, -2 * a, 0)},
                      0.254, 2, 0.06, 0.0);
    EXPECT_GT(big.bodySize.x, small.bodySize.x);
    EXPECT_GT(big.armWidthM, small.armWidthM);
    EXPECT_GT(big.motorRadiusM, small.motorRadiusM);
    EXPECT_GT(small.gearHeightM, 0.0);
    EXPECT_EQ(big.blades, 2);
}

TEST(DroneModel, ItemsPropsAndLights) {
    const DroneModel model = racer();
    DroneVisualState state;
    state.position = {10, 50, -20};
    state.orientation = levelNorth();
    std::vector<DrawItem> items;
    model.appendDrawItems(state, items);
    ASSERT_EQ(items.size(), 8U);  // Frame, 4 propellers, 3 lights.
    for (const DrawItem& item : items) {
        ASSERT_TRUE(item.mesh);
        expectWellFormed(*item.mesh);
        EXPECT_LT(glm::distance(item.position, state.position), 0.2);
    }
    // Propellers sit above the arms when level, at the motors.
    for (std::size_t i = 1; i <= 4; ++i) {
        EXPECT_GT(items[i].position.y, state.position.y);
        EXPECT_FALSE(items[i].mesh->transparent);
        EXPECT_TRUE(items[i].castsShadow);
    }
    // Front-right motor: north-east of the centre.
    EXPECT_GT(items[1].position.x, state.position.x);
    EXPECT_LT(items[1].position.z, state.position.z);
    // Red light on the left (west), green on the right (east), both at the front.
    EXPECT_LT(items[5].position.x, state.position.x);
    EXPECT_GT(items[6].position.x, state.position.x);
    EXPECT_LT(items[5].position.z, state.position.z);
    EXPECT_EQ(items[5].emissiveScale, 1.0F);
    EXPECT_GT(items[5].mesh->vertices.front().emissive, 0.0F);
    // White strobe at the rear: double flash once a second.
    EXPECT_GT(items[7].position.z, state.position.z);
    EXPECT_EQ(items[7].emissiveScale, 1.0F);  // t = 0: flash.
    state.timeS = 0.5;
    items.clear();
    model.appendDrawItems(state, items);
    EXPECT_EQ(items[7].emissiveScale, 0.0F);

    // Lights off.
    state.lights = false;
    items.clear();
    model.appendDrawItems(state, items);
    EXPECT_EQ(items[5].emissiveScale, 0.0F);
    EXPECT_EQ(items[6].emissiveScale, 0.0F);
}

TEST(DroneModel, PropellersSpinAndBlur) {
    const DroneModel model = racer();
    DroneVisualState state;
    state.orientation = levelNorth();
    std::vector<DrawItem> a;
    model.appendDrawItems(state, a);
    state.propAngleRad = {0.5, 0.5, 0.5, 0.5};
    std::vector<DrawItem> b;
    model.appendDrawItems(state, b);
    for (std::size_t i = 1; i <= 4; ++i) {
        // Rotation about the shaft (body z = world down) by the spin direction.
        const glm::dquat delta = glm::inverse(a[i].orientation) * b[i].orientation;
        EXPECT_NEAR(glm::angle(delta), 0.5, 1e-9);
        const glm::dvec3 axis = glm::axis(delta) * (glm::angle(delta) > 0 ? 1.0 : -1.0);
        EXPECT_NEAR(std::abs(axis.z), 1.0, 1e-9);
        EXPECT_NEAR(axis.z, model.shape().spin[i - 1], 1e-9);
    }
    // Fast propellers become translucent discs that cast no shadow; front ones are orange.
    state.rotorSpeedFraction = {0.9, 0.9, 0.1, 0.9};
    std::vector<DrawItem> c;
    model.appendDrawItems(state, c);
    EXPECT_TRUE(c[1].mesh->transparent);
    EXPECT_FALSE(c[1].castsShadow);
    EXPECT_GT(c[1].tint.r, c[1].tint.b);   // Front: orange.
    EXPECT_FALSE(c[3].mesh->transparent);  // Slow: blades.
    EXPECT_GT(c[1].tint.a, c[2].tint.a - 1e-6F);
}

// --- Rover model (FR-GFX-20) ------------------------------------------------------------------

RoverModel rover() {
    RoverShape s;
    for (const double y : {-0.15, 0.15}) {
        for (const double x : {0.13, -0.13}) {
            s.wheelHubs.emplace_back(x, y, 0.045);
        }
    }
    s.staticCompressionM = 0.012;
    return RoverModel(s);
}

TEST(RoverModel, WheelsTurnAndFollowTheSuspension) {
    const RoverModel model = rover();
    RoverVisualState state;
    state.orientation = levelNorth();
    state.compression = {0.012, 0.012, 0.012, 0.012};
    std::vector<DrawItem> a;
    model.appendDrawItems(state, a);
    ASSERT_EQ(a.size(), 1U + 4U + 4U);  // Body, wheels, two headlights and two tail lights.
    // At the static compression the hubs are where the profile puts them.
    EXPECT_NEAR(glm::length(model.hubAt(0, 0.012) - glm::dvec3(0.13, -0.15, 0.045)), 0.0, 1e-12);
    // More compression lifts the wheel towards the body (body z is down).
    EXPECT_NEAR(model.hubAt(0, 0.022).z, 0.035, 1e-12);
    EXPECT_NEAR(model.hubAt(0, 0.0).z, 0.057, 1e-12);  // Full droop off the ground.

    state.wheelAngle = {0.4, 0.4, 0.4, 0.4};
    std::vector<DrawItem> b;
    model.appendDrawItems(state, b);
    for (std::size_t i = 1; i <= 4; ++i) {
        // Rotation about the axle (body y).
        const glm::dquat delta = glm::inverse(a[i].orientation) * b[i].orientation;
        EXPECT_NEAR(glm::angle(delta), 0.4, 1e-9);
        EXPECT_NEAR(std::abs(glm::axis(delta).y), 1.0, 1e-9);
        // Rolling forward moves the top of the wheel forward (body +x).
        const glm::dvec3 top = b[i].orientation * glm::dvec3(0, 0, -0.065);
        const glm::dvec3 topBefore = a[i].orientation * glm::dvec3(0, 0, -0.065);
        const glm::dvec3 forward = levelNorth() * glm::dvec3(1, 0, 0);
        EXPECT_GT(glm::dot(top - topBefore, forward), 0.0);
    }
}

TEST(SpotLight, PhotometryConeAndRange) {
    SpotLight l;
    l.position = {0.0, 0.0, 0.0};
    l.direction = {0.0, 0.0, -1.0};
    l.intensityCd = 400.0;
    l.innerAngleRad = 10.0 * std::numbers::pi / 180.0;
    l.outerAngleRad = 25.0 * std::numbers::pi / 180.0;
    l.rangeM = 60.0;
    const glm::dvec3 facing(0.0, 0.0, 1.0);
    // Inverse-square law on the axis (the range window is ≈ 1 this close).
    EXPECT_NEAR(spotIlluminance(l, {0.0, 0.0, -5.0}, facing), 16.0, 0.01);
    EXPECT_NEAR(spotIlluminance(l, {0.0, 0.0, -10.0}, facing), 4.0, 0.01);
    // Lambert: a surface tilted by 60° gets half.
    const glm::dvec3 tilted(0.0, std::sin(std::numbers::pi / 3.0), 0.5);
    EXPECT_NEAR(spotIlluminance(l, {0.0, 0.0, -5.0}, tilted), 8.0, 0.01);
    // Facing away, behind the lamp, outside the cone, past the range: nothing.
    EXPECT_DOUBLE_EQ(spotIlluminance(l, {0.0, 0.0, -5.0}, -facing), 0.0);
    EXPECT_DOUBLE_EQ(spotIlluminance(l, {0.0, 0.0, 5.0}, -facing), 0.0);
    const double off = 30.0 * std::numbers::pi / 180.0;
    EXPECT_DOUBLE_EQ(spotIlluminance(l, {5.0 * std::sin(off), 0.0, -5.0 * std::cos(off)}, facing),
                     0.0);
    const double half = 17.5 * std::numbers::pi / 180.0;
    const double edge =
        spotIlluminance(l, {5.0 * std::sin(half), 0.0, -5.0 * std::cos(half)}, facing);
    EXPECT_GT(edge, 0.5);  // Soft edge between the inner and outer angles.
    EXPECT_LT(edge, 16.0 * std::cos(half));
    EXPECT_DOUBLE_EQ(spotIlluminance(l, {0.0, 0.0, -61.0}, facing), 0.0);
    EXPECT_NEAR(glm::dot(headlightColor(), glm::vec3(0.2126F, 0.7152F, 0.0722F)), 1.0F, 1e-5F);
}

TEST(RoverModel, HeadlightsAimForwardAndSlightlyDown) {
    const RoverModel model = rover();
    RoverVisualState state;
    state.orientation = levelNorth();
    state.position = {10.0, 5.0, 20.0};
    state.lights = false;
    std::vector<SpotLight> lights;
    model.appendLights(state, lights);
    EXPECT_TRUE(lights.empty());
    state.lights = true;
    model.appendLights(state, lights);
    ASSERT_EQ(lights.size(), 2U);
    const glm::dvec3 north(0.0, 0.0, -1.0);
    for (const SpotLight& l : lights) {
        EXPECT_GT(glm::dot(l.direction, north), 0.99);
        EXPECT_LT(l.direction.y, 0.0);
        EXPECT_GT(glm::dot(l.position - state.position, north), 0.19);  // At the nose.
        EXPECT_DOUBLE_EQ(l.intensityCd, RoverModel::kHeadlightCd);
    }
    EXPECT_LT(lights[0].position.x, lights[1].position.x);  // Left, then right.
}

TEST(RoverModel, LightsAndBrakeLights) {
    const RoverModel model = rover();
    RoverVisualState state;
    state.lights = false;
    std::vector<DrawItem> off;
    model.appendDrawItems(state, off);
    for (std::size_t i = 5; i < off.size(); ++i) {
        EXPECT_FLOAT_EQ(off[i].emissiveScale, 0.0F);
        EXPECT_FALSE(off[i].castsShadow);
    }
    state.braking = true;
    std::vector<DrawItem> braking;
    model.appendDrawItems(state, braking);
    // Lamps alternate headlight, tail light per side: the tail lights glow while braking.
    EXPECT_FLOAT_EQ(braking[5].emissiveScale, 0.0F);
    EXPECT_GT(braking[6].emissiveScale, 1.0F);
    state.lights = true;
    state.braking = false;
    std::vector<DrawItem> on;
    model.appendDrawItems(state, on);
    EXPECT_FLOAT_EQ(on[5].emissiveScale, 1.0F);
    EXPECT_LT(on[6].emissiveScale, braking[6].emissiveScale);
}

// --- Camera roll and rig (FR-GFX-05) --------------------------------------------------------

TEST(Camera, RollTiltsTheRightSideDown) {
    Camera camera;
    camera.rollRad = 0.3;
    EXPECT_LT(camera.right().y, 0.0);
    EXPECT_NEAR(glm::dot(camera.right(), camera.up()), 0.0, 1e-12);
    EXPECT_NEAR(glm::dot(camera.forward(), camera.right()), 0.0, 1e-12);
    EXPECT_NEAR(glm::length(camera.up()), 1.0, 1e-12);
    // Banked right, the horizon on the right rises: a point to the right appears above centre.
    const glm::vec4 p = camera.viewProjection() *
                        glm::vec4(glm::vec3(camera.forward() * 10.0 + glm::dvec3(1, 0, 0)), 1.0F);
    EXPECT_GT(p.x / p.w, 0.0F);
    EXPECT_GT(p.y / p.w, 0.0F);
}

TEST(CameraRig, ModesAndFieldOfView) {
    EXPECT_EQ(nextCameraMode(CameraMode::Fpv), CameraMode::Chase);
    EXPECT_EQ(nextCameraMode(CameraMode::Free), CameraMode::Fpv);
    EXPECT_EQ(cameraModeName(CameraMode::LineOfSight), "Line of sight");
    const double h = ds::common::degToRad(90.0);
    EXPECT_NEAR(verticalFov(h, 1.0), h, 1e-12);
    EXPECT_NEAR(std::tan(0.5 * verticalFov(h, 2.0)), 0.5, 1e-12);
}

TEST(CameraRig, CameraFromAxesReproducesTheBasis) {
    const glm::dvec3 f = glm::normalize(glm::dvec3(0.3, -0.4, -0.8));
    const glm::dvec3 side = glm::normalize(glm::cross(f, glm::dvec3(0, 1, 0)));
    const glm::dvec3 level = glm::cross(side, f);
    for (const double roll : {-2.5, -0.4, 0.0, 0.7, 3.0}) {
        const glm::dvec3 up = std::cos(roll) * level - std::sin(roll) * side;
        const Camera c = cameraFromAxes({1, 2, 3}, f, up, 1.0);
        EXPECT_NEAR(glm::distance(c.forward(), f), 0.0, 1e-9) << roll;
        EXPECT_NEAR(glm::distance(c.up(), up), 0.0, 1e-9) << roll;
        EXPECT_NEAR(c.fovYRad, 1.0, 0.0);
    }
    // Straight down with north at the top of the image.
    const Camera down = cameraFromAxes({}, {0, -1, 0}, {0, 0, -1}, 1.0);
    EXPECT_NEAR(glm::distance(down.forward(), glm::dvec3(0, -1, 0)), 0.0, 1e-9);
    EXPECT_NEAR(glm::distance(down.up(), glm::dvec3(0, 0, -1)), 0.0, 1e-9);
}

TEST(CameraRig, FpvFollowsTheBodyWithUptilt) {
    CameraRig rig;
    VehiclePose pose{.position = {0, 10, 0}, .orientation = levelNorth()};
    const FpvCameraParams fpv{
        .uptiltDeg = 30.0, .horizontalFovDeg = 120.0, .offsetBody = {0.1, 0.0, 0.0}};
    Camera c = rig.update(CameraMode::Fpv, pose, fpv, 16.0 / 9.0, 0.01);
    EXPECT_NEAR(angleBetweenDeg(c.forward(), {0, 0.5, -std::sqrt(3.0) / 2}), 0.0, 1e-6);
    EXPECT_NEAR(c.rollRad, 0.0, 1e-9);
    EXPECT_NEAR(glm::distance(c.position, glm::dvec3(0, 10, -0.1)), 0.0, 1e-9);
    EXPECT_NEAR(c.fovYRad, verticalFov(ds::common::degToRad(120.0), 16.0 / 9.0), 1e-12);

    // Rolled right by 20° (right side down): the image rolls with it.
    pose.orientation =
        levelNorth() * glm::angleAxis(ds::common::degToRad(20.0), glm::dvec3(1, 0, 0));
    c = rig.update(CameraMode::Fpv, pose, {.uptiltDeg = 0.0}, 1.0, 0.01);
    EXPECT_NEAR(ds::common::radToDeg(c.rollRad), 20.0, 1e-6);
    EXPECT_LT(c.right().y, 0.0);
}

TEST(CameraRig, ChaseIsBehindSmoothedAndAboveGround) {
    CameraRig rig;
    VehiclePose pose{.position = {0, 10, 0}, .orientation = levelNorth()};
    rig.reset(pose);
    Camera c = rig.update(CameraMode::Chase, pose, {}, 1.5, 0.01);
    // Behind the nose (south), above, looking at the vehicle, level horizon.
    EXPECT_NEAR(c.position.z, rig.chaseDistance(), 1e-9);
    EXPECT_NEAR(c.position.y, 10.0 + rig.chaseHeight(), 1e-9);
    EXPECT_LT(c.forward().z, 0.0);
    EXPECT_EQ(c.rollRad, 0.0);
    EXPECT_EQ(c.aspect, 1.5);

    // Moving east fast: the camera swings behind the travel direction, smoothly.
    pose.velocity = {20, 0, 0};
    pose.position = {2, 10, 0};
    c = rig.update(CameraMode::Chase, pose, {}, 1.5, 0.05);
    const glm::dvec3 target(2.0 - rig.chaseDistance(), 10.0 + rig.chaseHeight(), 0.0);
    EXPECT_GT(glm::distance(c.position, target), 0.5);  // Not there yet.
    for (int i = 0; i < 100; ++i) {
        c = rig.update(CameraMode::Chase, pose, {}, 1.5, 0.05);
    }
    EXPECT_NEAR(glm::distance(c.position, target), 0.0, 1e-3);

    // A smaller vehicle gets a closer camera.
    rig.setChaseDistance(1.5);
    rig.reset(pose);
    c = rig.update(CameraMode::Chase, pose, {}, 1.5, 0.05);
    EXPECT_NEAR(glm::distance(c.position, pose.position), std::hypot(1.5, 0.525), 1e-6);
    rig.setChaseDistance(0.0);
    EXPECT_EQ(rig.chaseDistance(), 0.5);

    // Never below the ground.
    rig.setGround([](double, double) { return std::optional<double>(30.0); });
    c = rig.update(CameraMode::Chase, pose, {}, 1.5, 0.05);
    EXPECT_GE(c.position.y, 30.0 + CameraRig::kClearanceM - 1e-9);
}

TEST(CameraRig, ChaseInsetsLookDownAndFromBehind) {
    CameraRig rig;
    rig.setChaseDistance(2.0);
    // Heading east (body x → world +x), climbing fast towards the north.
    const glm::dquat east =
        glm::quat_cast(glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 1), glm::dvec3(0, -1, 0)));
    const VehiclePose pose{.position = {10, 20, 30}, .orientation = east, .velocity = {0, 5, -9}};

    // Top: straight above, looking down, north at the top of the image and east on the right.
    const Camera top = rig.topView(pose, 1.6);
    EXPECT_NEAR(glm::distance(top.position, pose.position), CameraRig::kTopHeightRatio * 2.0, 1e-9);
    EXPECT_NEAR(top.position.y, 20.0 + CameraRig::kTopHeightRatio * 2.0, 1e-9);
    EXPECT_NEAR(top.forward().y, -1.0, 1e-9);
    EXPECT_NEAR(glm::dot(top.up(), glm::dvec3(0, 0, -1)), 1.0, 1e-9);
    EXPECT_NEAR(glm::dot(top.right(), glm::dvec3(1, 0, 0)), 1.0, 1e-9);
    EXPECT_NEAR(top.fovYRad, ds::common::degToRad(CameraRig::kTopFovDeg), 1e-12);
    EXPECT_EQ(top.aspect, 1.6);

    // Behind: west of the vehicle (behind its nose, not its travel), above, looking at it,
    // level horizon; no smoothing, so it is there at once.
    const Camera behind = rig.behindView(pose, 1.6);
    const double d = CameraRig::kBehindDistanceRatio * 2.0;
    EXPECT_NEAR(behind.position.x, 10.0 - d, 1e-9);
    EXPECT_NEAR(behind.position.z, 30.0, 1e-9);
    EXPECT_GT(behind.position.y, 20.0);
    EXPECT_LT(angleBetweenDeg(behind.forward(), pose.position - behind.position), 10.0);
    EXPECT_GT(behind.forward().x, 0.9);
    EXPECT_EQ(behind.rollRad, 0.0);
    EXPECT_EQ(behind.aspect, 1.6);

    // Turning the vehicle turns the behind view with it.
    const VehiclePose north{.position = pose.position, .orientation = levelNorth()};
    EXPECT_NEAR(rig.behindView(north, 1.6).position.z, 30.0 + d, 1e-9);

    // Kept above the ground.
    rig.setGround([](double, double) { return std::optional<double>(25.0); });
    EXPECT_GE(rig.behindView(pose, 1.6).position.y, 25.0 + CameraRig::kClearanceM - 1e-9);
}

TEST(CameraRig, LineOfSightLooksAtTheVehicle) {
    CameraRig rig;
    rig.setPilotEye({0, 1.7, 0});
    const VehiclePose pose{.position = {30, 20, -40}};
    const Camera c = rig.update(CameraMode::LineOfSight, pose, {}, 1.0, 0.01);
    EXPECT_NEAR(angleBetweenDeg(c.forward(), pose.position - c.position), 0.0, 1e-6);
    EXPECT_NEAR(c.fovYRad, ds::common::degToRad(CameraRig::kLosFovDeg), 1e-12);
    EXPECT_EQ(c.rollRad, 0.0);
    rig.setGround([](double, double) { return std::optional<double>(5.0); });
    EXPECT_GE(rig.update(CameraMode::LineOfSight, pose, {}, 1.0, 0.01).position.y, 5.3 - 1e-9);
}

// --- Shadow cascades (FR-GFX-10) ------------------------------------------------------------

glm::dvec3 lightSpace(const ShadowCascade& c, const glm::dvec3& rel) {
    const glm::dvec4 p = glm::dmat4(c.matrix) * glm::dvec4(rel, 1.0);
    return glm::dvec3(p) / p.w;
}

TEST(ShadowCascades, CoverTheirSliceOfTheView) {
    Camera camera;
    camera.position = {100, 400, -300};
    camera.yawRad = 0.8;
    camera.pitchRad = -0.4;
    camera.aspect = 16.0 / 9.0;
    const glm::dvec3 sun = glm::normalize(glm::dvec3(0.3, 0.6, -0.5));
    const ShadowSetup setup = computeShadowCascades(camera, sun, 2048);
    ASSERT_TRUE(setup.enabled);
    EXPECT_NEAR(glm::distance(glm::dvec3(setup.lightDirection), sun), 0.0, 1e-6);

    const double tanY = std::tan(0.5 * camera.fovYRad);
    const double tanX = tanY * camera.aspect;
    double previousFar = camera.nearM;
    for (int i = 0; i < kShadowCascades; ++i) {
        const ShadowCascade& c = setup.cascades[static_cast<std::size_t>(i)];
        EXPECT_FLOAT_EQ(c.farM,
                        static_cast<float>(kDefaultShadowSplitsM[static_cast<std::size_t>(i)]));
        EXPECT_NEAR(c.texelM, 2.0F * c.radiusM / 2048.0F, 1e-5F);
        if (i > 0) {
            EXPECT_GT(c.radiusM, setup.cascades[static_cast<std::size_t>(i - 1)].radiusM);
        }
        // Every corner of the slice lies inside the cascade.
        for (const double d : {previousFar, static_cast<double>(c.farM)}) {
            for (const double sx : {-1.0, 1.0}) {
                for (const double sy : {-1.0, 1.0}) {
                    const glm::dvec3 rel = camera.forward() * d + camera.right() * (sx * tanX * d) +
                                           camera.up() * (sy * tanY * d);
                    const glm::dvec3 l = lightSpace(c, rel);
                    EXPECT_LE(std::abs(l.x), 1.0 + 1e-6);
                    EXPECT_LE(std::abs(l.y), 1.0 + 1e-6);
                    EXPECT_GE(l.z, -1e-6);
                    EXPECT_LE(l.z, 1.0 + 1e-6);
                }
            }
        }
        previousFar = c.farM;
        // Depth grows away from the light.
        const glm::dvec3 centre = camera.forward() * (0.5 * static_cast<double>(c.farM));
        EXPECT_LT(lightSpace(c, centre + sun).z, lightSpace(c, centre).z);
    }
}

TEST(ShadowCascades, SnapToTexelsAsTheCameraMoves) {
    Camera camera;
    camera.position = {10, 200, 10};
    camera.pitchRad = -0.3;
    const glm::dvec3 sun = glm::normalize(glm::dvec3(-0.2, 0.8, 0.4));
    const ShadowSetup a = computeShadowCascades(camera, sun, 1024);
    Camera moved = camera;
    moved.position += glm::dvec3(0.37, -0.11, 0.23);
    const ShadowSetup b = computeShadowCascades(moved, sun, 1024);
    const glm::dvec3 world(40, 180, -60);
    for (std::size_t i = 0; i < kShadowCascades; ++i) {
        ASSERT_EQ(a.cascades[i].radiusM, b.cascades[i].radiusM);
        // The same world point lands on the same texel grid: whole-texel shifts only.
        const glm::dvec3 pa = lightSpace(a.cascades[i], world - camera.position);
        const glm::dvec3 pb = lightSpace(b.cascades[i], world - moved.position);
        for (const double shift : {(pa.x - pb.x) * 512.0, (pa.y - pb.y) * 512.0}) {
            EXPECT_NEAR(shift, std::round(shift), 1e-3) << "cascade " << i;
        }
    }
    // No light: disabled.
    EXPECT_FALSE(computeShadowCascades(camera, glm::dvec3(0), 1024).enabled);
}

// --- Weather view (FR-WX-09) ------------------------------------------------------------------

// Mean cloud density over a regular grid (independent of the random samples the threshold is
// computed from).
double gridCover(double threshold, double softness) {
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < 160; ++i) {
        for (int j = 0; j < 160; ++j) {
            const double f = cloudNoise({i * 1.37 - 300.0, j * 1.41 + 120.0});
            const double t = std::clamp((f - threshold) / softness, 0.0, 1.0);
            sum += t * t * (3.0 - 2.0 * t);
            ++n;
        }
    }
    return sum / n;
}

TEST(WeatherView, CloudCoverMatchesTheOktas) {
    for (const double softness : {0.05, 0.25}) {
        EXPECT_NEAR(gridCover(cloudCoverThreshold(0.0, softness), softness), 0.0, 1e-9);
        EXPECT_NEAR(gridCover(cloudCoverThreshold(1.0, softness), softness), 1.0, 1e-9);
        for (const double cover : {0.125, 0.25, 0.5, 0.75}) {
            EXPECT_NEAR(gridCover(cloudCoverThreshold(cover, softness), softness), cover, 0.04)
                << "cover " << cover << " softness " << softness;
        }
    }
    // The noise spans most of 0–1.
    double lo = 1.0;
    double hi = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double f = cloudNoise({i * 0.73, i * 0.31});
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    EXPECT_LT(lo, 0.3);
    EXPECT_GT(hi, 0.7);
}

TEST(WeatherView, LayerThresholdsComeFromATableThatMatchesTheExactOnes) {
    for (const double oktas : {1.0, 3.0, 5.0, 7.0}) {
        ds::weather::WeatherConditions c;
        c.clouds = {{.type = ds::weather::CloudType::Cumulus, .coverOktas = oktas}};
        const WeatherView v = makeWeatherView(c, {}, {}, 0.0, 0.0, 0.0);
        ASSERT_EQ(v.layerCount, 1U);
        EXPECT_NEAR(v.layers[0].threshold, cloudCoverThreshold(oktas / 8.0, v.layers[0].softness),
                    0.005);
    }
}

TEST(WeatherView, DiffuseTransmittanceAndParticles) {
    EXPECT_DOUBLE_EQ(cloudDiffuseTransmittance(0.0), 1.0);
    EXPECT_GT(cloudDiffuseTransmittance(10.0), cloudDiffuseTransmittance(40.0));
    using ds::weather::PrecipitationType;
    EXPECT_EQ(precipitationParticles(PrecipitationType::None, 5.0), 0);
    EXPECT_EQ(precipitationParticles(PrecipitationType::Rain, 0.0), 0);
    EXPECT_LT(precipitationParticles(PrecipitationType::Rain, 1.0),
              precipitationParticles(PrecipitationType::Rain, 12.0));
    EXPECT_LT(precipitationParticles(PrecipitationType::Snow, 0.5),
              precipitationParticles(PrecipitationType::Snow, 4.0));
    EXPECT_LE(precipitationParticles(PrecipitationType::Snow, 1000.0), 10000);
}

ds::weather::WeatherConditions presetConditions(const char* key) {
    ds::weather::WeatherSettings settings;
    settings.preset = key;
    return ds::weather::WeatherGenerator(settings).plannedAt(0.0);
}

TEST(WeatherView, ClearSky) {
    const WeatherView v = makeWeatherView(presetConditions("clear"), {}, {}, 100.0, 4.0, 12.0);
    EXPECT_EQ(v.layerCount, 0U);
    EXPECT_DOUBLE_EQ(v.skyFactor, 1.0);
    EXPECT_DOUBLE_EQ(v.clearFraction, 1.0);
    EXPECT_DOUBLE_EQ(v.beamDiffuse, 0.0);
    EXPECT_DOUBLE_EQ(v.fogExtinctionPerM, 0.0);
    EXPECT_DOUBLE_EQ(v.aerosolVisibilityM, 50'000.0);
    EXPECT_EQ(v.particles, 0);
    EXPECT_DOUBLE_EQ(v.timeS, 12.0);
    // Wind from 270° blows towards the east (+x).
    EXPECT_NEAR(v.windMs.x, 4.0, 1e-9);
    EXPECT_NEAR(v.windMs.z, 0.0, 1e-9);
}

TEST(WeatherView, OvercastDimsTheDaylightLikeRealOvercast) {
    const std::array<ds::weather::Drift, 3> drift{
        ds::weather::Drift{.eastM = 50.0, .northM = 20.0}};
    const WeatherView v = makeWeatherView(presetConditions("overcast"), drift, {}, 100.0, 4.0, 0.0);
    ASSERT_EQ(v.layerCount, 1U);
    const CloudLayerView& l = v.layers[0];
    EXPECT_EQ(l.type, ds::weather::CloudType::Stratus);
    EXPECT_DOUBLE_EQ(l.baseY, 700.0);  // 600 m above the ground at y = 100.
    EXPECT_DOUBLE_EQ(l.topY, 1300.0);
    EXPECT_DOUBLE_EQ(l.cover, 1.0);
    EXPECT_DOUBLE_EQ(l.driftM.x, 50.0);
    EXPECT_DOUBLE_EQ(l.driftM.y, -20.0);  // North is −z.
    // Overcast daylight is typically 10–30 % of a clear day's; no direct sunlight.
    EXPECT_GT(v.skyFactor, 0.1);
    EXPECT_LT(v.skyFactor, 0.3);
    EXPECT_LT(v.clearFraction, 1e-6);
    EXPECT_NEAR(v.beamDiffuse, v.skyFactor, 1e-6);
    // Heavy rain under a thick stratus is darker still.
    const WeatherView rain = makeWeatherView(presetConditions("heavy-rain"), {}, {}, 0.0, 4.0, 0.0);
    EXPECT_LT(rain.skyFactor, 0.5 * v.skyFactor);
}

TEST(WeatherView, LayersAreSortedAndShadeTheLowerOnes) {
    const WeatherView v = makeWeatherView(presetConditions("thunderstorm"), {}, {}, 0.0, 10.0, 0.0);
    ASSERT_EQ(v.layerCount, 2U);
    EXPECT_LT(v.layers[0].baseY, v.layers[1].baseY);
    EXPECT_EQ(v.layers[0].type, ds::weather::CloudType::Cumulonimbus);
    EXPECT_DOUBLE_EQ(v.layers[1].lightFactor, 1.0);  // Nothing above the highest.
    EXPECT_LT(v.layers[0].lightFactor, 1.0);
    EXPECT_GT(v.layers[0].threshold, v.layers[1].threshold - 1.0);  // Partly covered.
    EXPECT_GT(v.beamDiffuse, 0.0);
    EXPECT_GT(v.clearFraction, 0.0);
}

TEST(WeatherView, FogAndPrecipitationLowerTheVisibilityNearTheGround) {
    // Ground fog: a 120 m layer with the fog's visibility; clear air above.
    const WeatherView fog = makeWeatherView(presetConditions("fog"), {}, {}, 10.0, 1.0, 0.0);
    EXPECT_DOUBLE_EQ(fog.fogTopY, 130.0);
    EXPECT_DOUBLE_EQ(fog.aerosolVisibilityM, ds::weather::kAboveFogVisibilityM);
    EXPECT_NEAR(3.912 / (fog.fogExtinctionPerM + 3.912 / fog.aerosolVisibilityM), 500.0, 1e-6);
    // Heavy rain: 3 km visibility below the 500 m cloud base, 20 km above.
    const WeatherView rain = makeWeatherView(presetConditions("heavy-rain"), {}, {}, 0.0, 4.0, 0.0);
    EXPECT_DOUBLE_EQ(rain.fogTopY, 500.0);
    EXPECT_DOUBLE_EQ(rain.aerosolVisibilityM, 20'000.0);
    EXPECT_NEAR(3.912 / (rain.fogExtinctionPerM + 3.912 / rain.aerosolVisibilityM), 3000.0, 1e-6);
    EXPECT_EQ(rain.precipitation, ds::weather::PrecipitationType::Rain);
    EXPECT_GT(rain.particles, 1000);
    EXPECT_GT(rain.fallSpeedMs, 5.0);
    // Snow falls slowly; the ground cover is passed on.
    const WeatherView snow = makeWeatherView(presetConditions("heavy-snow"), {},
                                             {.wetness = 0.4, .snowDepthM = 0.12}, 0.0, 4.0, 0.0);
    EXPECT_DOUBLE_EQ(snow.fallSpeedMs, 1.0);
    EXPECT_DOUBLE_EQ(snow.wetness, 0.4);
    EXPECT_DOUBLE_EQ(snow.snowDepthM, 0.12);
}

TEST(WeatherView, LightningFlashes) {
    // About two flashes a minute: count the starts over an hour.
    int flashes = 0;
    bool inFlash = false;
    double peak = 0.0;
    for (double t = 0.0; t < 3600.0; t += 0.005) {
        const double lux = lightningLux(t);
        peak = std::max(peak, lux);
        if (!inFlash && lux > 0.2) {
            ++flashes;
        }
        inFlash = lux > 0.02;
    }
    EXPECT_GT(flashes, 60);
    EXPECT_LT(flashes, 150);
    EXPECT_GT(peak, 5.0);
    EXPECT_LT(peak, 20.0);
    EXPECT_DOUBLE_EQ(lightningLux(1234.5), lightningLux(1234.5));
    // Only thunderstorms flash.
    double maxClear = 0.0;
    double maxStorm = 0.0;
    ds::weather::WeatherConditions storm = presetConditions("thunderstorm");
    for (double t = 0.0; t < 600.0; t += 0.01) {
        maxClear =
            std::max(maxClear,
                     makeWeatherView(presetConditions("heavy-rain"), {}, {}, 0, 0, t).lightningLux);
        maxStorm = std::max(maxStorm, makeWeatherView(storm, {}, {}, 0, 0, t).lightningLux);
    }
    EXPECT_DOUBLE_EQ(maxClear, 0.0);
    EXPECT_GT(maxStorm, 0.3);
}

TEST(Exposure, DaylightWhiteBalance) {
    // The daylight illuminant becomes neutral grey of the same luminance.
    const glm::dvec3 daylight(blackbodyColor(kDaylightWhiteK));
    const glm::dvec3 balanced = glm::dmat3(whiteBalanceMatrix(daylight)) * daylight;
    EXPECT_NEAR(balanced.r, balanced.g, 1e-4);
    EXPECT_NEAR(balanced.b, balanced.g, 1e-4);
    EXPECT_NEAR(0.2126 * balanced.r + 0.7152 * balanced.g + 0.0722 * balanced.b, 1.0, 1e-3);
    // D65 needs no correction.
    const glm::dmat3 identity(whiteBalanceMatrix(glm::dvec3(1.0)));
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            EXPECT_NEAR(identity[c][r], c == r ? 1.0 : 0.0, 1e-3);
        }
    }
    // A sunset (3000 K) stays warm after the daylight balance.
    const glm::dvec3 sunset =
        glm::dmat3(whiteBalanceMatrix(daylight)) * glm::dvec3(blackbodyColor(3000.0));
    EXPECT_GT(sunset.r, 1.3 * sunset.b);
}

}  // namespace
