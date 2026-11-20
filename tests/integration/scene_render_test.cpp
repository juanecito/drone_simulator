// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Scene rendering on a real OpenGL 4.5 core context: every shader compiles, and noon, sunset,
// full-moon and moonless scenes have the expected image properties. Property checks replace
// golden images, which differ between drivers (documented in docs/TESTING.md).

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ds/app/opengl_check.hpp"
#include "ds/astro/stars.hpp"
#include "ds/astro/time.hpp"
#include "ds/common/worker_pool.hpp"
#include "ds/graphics/camera.hpp"
#include "ds/graphics/camera_rig.hpp"
#include "ds/graphics/drone_model.hpp"
#include "ds/graphics/renderer.hpp"
#include "ds/graphics/sky_model.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/terrain/streamer.hpp"
#include "ds/weather/weather.hpp"

namespace {

using namespace ds;

graphics::GlProcAddress resolve(void* user, const char* name) {
    return static_cast<QOpenGLContext*>(user)->getProcAddress(name);
}

const std::filesystem::path kAssets{DS_TEST_ASSETS_DIR};
constexpr int kWidth = 320;
constexpr int kHeight = 200;

struct Luma {
    double mean = 0.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

// Mean colour (0–255) of a rectangle given in fractions of the image (y from the top).
Luma average(const QImage& image, double x0, double y0, double x1, double y1) {
    Luma l;
    int n = 0;
    for (int y = static_cast<int>(y0 * image.height()); y < static_cast<int>(y1 * image.height());
         ++y) {
        for (int x = static_cast<int>(x0 * image.width()); x < static_cast<int>(x1 * image.width());
             ++x) {
            const QColor c = image.pixelColor(x, y);
            l.r += c.red();
            l.g += c.green();
            l.b += c.blue();
            ++n;
        }
    }
    l.r /= n;
    l.g /= n;
    l.b /= n;
    l.mean = (l.r + l.g + l.b) / 3.0;
    return l;
}

class SceneRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (const auto check = app::checkOpenGlContext(); !check) {
            GTEST_SKIP() << "OpenGL 4.5 core not available: " << check.error().message;
        }
        context_ = std::make_unique<QOpenGLContext>();
        context_->setFormat(QSurfaceFormat::defaultFormat());
        ASSERT_TRUE(context_->create());
        surface_ = std::make_unique<QOffscreenSurface>();
        surface_->setFormat(context_->format());
        surface_->create();
        ASSERT_TRUE(context_->makeCurrent(surface_.get()));
        renderer_ = std::make_unique<graphics::Renderer>();
        ASSERT_TRUE(renderer_->initialize(&resolve, context_.get()).has_value());
        const auto loaded =
            renderer_->loadResources({.shaderDirectory = kAssets / "shaders",
                                      .moonTexture = kAssets / "sky" / "moon_albedo.pgm"});
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
        fbo_ = std::make_unique<QOpenGLFramebufferObject>(kWidth, kHeight);
        ASSERT_TRUE(fbo_->isValid());

        auto catalog = astro::loadStarCatalog(kAssets / "sky" / "bsc5.csv");
        ASSERT_TRUE(catalog.has_value());
        sky_.setCatalog(std::move(*catalog));

        terrain::ProceduralParams params;
        params.seed = 42;
        params.waterLevelM = -40.0;
        auto source = terrain::ProceduralSource::create(params);
        ASSERT_TRUE(source.has_value());
        source_ = *source;
        streamer_ = std::make_unique<terrain::TerrainStreamer>(*source, layout_, pool_);
    }

    void TearDown() override {
        if (context_ != nullptr) {
            context_->makeCurrent(surface_.get());
            fbo_.reset();
            renderer_.reset();
            context_->doneCurrent();
        }
    }

    // Renders with the camera at 300 m looking at the given azimuth/pitch; converges the
    // exposure over several seconds of simulated time.
    QImage render(const char* utc, double yawDeg, double pitchDeg, double fovDeg = 60.0) {
        graphics::Camera camera;
        camera.position = {0.0, 300.0, 0.0};
        camera.yawRad = yawDeg * 3.14159265358979 / 180.0;
        camera.pitchRad = pitchDeg * 3.14159265358979 / 180.0;
        camera.fovYRad = fovDeg * 3.14159265358979 / 180.0;
        return renderScene(utc, camera, {});
    }

    // Renders from @p camera with @p objects; converges the exposure over several seconds of
    // simulated time.
    QImage renderScene(const char* utc, const graphics::Camera& camera,
                       std::span<const graphics::DrawItem> objects,
                       std::span<const graphics::SpotLight> lights = {}) {
        const auto time = astro::parseUtc(utc);
        EXPECT_TRUE(time.has_value());
        const geo::Geodetic observer{40.4, -3.7, 650.0};  // Madrid.
        sky_.update(*time, observer);

        graphics::FrameInput input;
        input.camera = camera;
        input.originAltitudeM = 650.0;
        input.sky = sky_.state();
        input.stars = &sky_.stars();
        input.starsVersion = sky_.starsVersion();
        input.terrain.gridSize = layout_.gridSize;
        input.terrain.waterLevelM = -40.0;
        input.exposure = exposure_;
        input.weather = weather_;
        input.lensWetness = lensWetness_;
        input.objects = objects;
        input.lights = lights;

        std::vector<std::shared_ptr<const terrain::ChunkMesh>> chunks;
        for (int i = 0; i < 200; ++i) {
            chunks = streamer_->update(camera.position.x, camera.position.y, camera.position.z);
            if (streamer_->stats().inFlight == 0 && i > 0) {
                break;
            }
            pool_.waitIdle();
        }
        input.terrain.chunks = chunks;
        for (int frame = 0; frame < 12; ++frame) {
            input.resetExposure = frame == 0;
            input.dtSeconds = 1.0;
            renderer_->renderFrame(
                {.framebuffer = fbo_->handle(), .width = kWidth, .height = kHeight}, input);
        }
        return fbo_->toImage().convertToFormat(QImage::Format_RGB32);
    }

    // The weather of a preset at its planned values (no fluctuations).
    graphics::WeatherView weatherPreset(const char* key, const weather::GroundCover& ground = {}) {
        const auto table =
            weather::WeatherPresetTable::load(kAssets / "environment" / "weather_presets.json");
        EXPECT_TRUE(table.has_value());
        weather::WeatherSettings settings;
        settings.seed = 7;
        settings.preset = key;
        const weather::WeatherGenerator generator(settings, *table);
        return graphics::makeWeatherView(generator.plannedAt(0.0), {}, ground,
                                         source_->height(0.0, 0.0), 3.0, 10.0);
    }

    void save(const QImage& image, const char* name) {
        if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
            image.save(QString::fromUtf8(dir) + QLatin1Char('/') + QLatin1StringView(name));
        }
    }

    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<QOffscreenSurface> surface_;
    std::unique_ptr<graphics::Renderer> renderer_;
    std::unique_ptr<QOpenGLFramebufferObject> fbo_;
    graphics::SkyModel sky_;
    graphics::ExposureSettings exposure_;
    graphics::WeatherView weather_;
    double lensWetness_ = 0.0;
    terrain::ChunkLayout layout_;
    common::WorkerPool pool_{4};
    std::shared_ptr<const terrain::ProceduralSource> source_;
    std::unique_ptr<terrain::TerrainStreamer> streamer_;
};

TEST_F(SceneRenderTest, NoonHasBlueSkyAndLitTerrain) {
    const QImage image = render("2026-06-21T12:00:00Z", 0.0, 5.0);
    save(image, "noon.png");
    const Luma sky = average(image, 0.0, 0.0, 1.0, 0.15);
    const Luma ground = average(image, 0.0, 0.85, 1.0, 1.0);
    EXPECT_GT(sky.b, sky.r + 10) << "sky should be blue";
    EXPECT_GT(sky.mean, 60.0);
    EXPECT_GT(ground.mean, 20.0) << "terrain should be lit";
    EXPECT_LT(ground.mean, 220.0);
    EXPECT_GT(renderer_->lastStats().chunksDrawn, 0);
    const auto exposure = renderer_->readExposure();
    EXPECT_GT(exposure.ev100, 10.0);
    EXPECT_LT(exposure.ev100, 17.0);
}

TEST_F(SceneRenderTest, DroneCastsAShadowAtNoon) {
    // FR-GFX-10: a drone hovering above the ground darkens the ground where its shadow falls.
    const auto noon = astro::parseUtc("2026-06-21T12:00:00Z");
    ASSERT_TRUE(noon.has_value());
    sky_.update(*noon, {40.4, -3.7, 650.0});
    const glm::dvec3 sun(sky_.state().sun.direction);
    ASSERT_GT(sun.y, 0.8);
    const double ground = source_->height(0.0, 0.0);
    // The drone (scaled up 4×) hovers 2 m above the point under the camera, offset towards the
    // sun so its shadow falls in the middle of the image.
    const std::array<glm::dvec3, 4> motors{glm::dvec3(0.08, 0.08, 0), glm::dvec3(-0.08, 0.08, 0),
                                           glm::dvec3(-0.08, -0.08, 0), glm::dvec3(0.08, -0.08, 0)};
    const graphics::DroneModel model(graphics::droneShapeFor(motors, 0.127, 3, 0.03, 30.0));
    graphics::DroneVisualState state;
    state.position = glm::dvec3(0.0, ground, 0.0) + sun * (2.0 / sun.y);
    // Level, nose north: body x, y, z (forward, right, down) → world −z, +x, −y.
    state.orientation =
        glm::quat_cast(glm::dmat3(glm::dvec3(0, 0, -1), glm::dvec3(1, 0, 0), glm::dvec3(0, -1, 0)));
    std::vector<graphics::DrawItem> items;
    model.appendDrawItems(state, items);
    for (graphics::DrawItem& item : items) {
        item.position = state.position + 4.0 * (item.position - state.position);
        item.scale = glm::vec3(4.0F);
    }
    // Straight down, north at the top of the image: the shadow falls at the centre.
    const graphics::Camera camera =
        graphics::cameraFromAxes({0.0, ground + 9.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}, 0.9);

    const QImage without = renderScene("2026-06-21T12:00:00Z", camera, {});
    const QImage with = renderScene("2026-06-21T12:00:00Z", camera, items);
    save(with, "drone_shadow.png");
    EXPECT_GT(renderer_->lastStats().objectsDrawn, 5);
    EXPECT_EQ(renderer_->lastStats().shadowCascades, 3);
    const Luma lit = average(without, 0.485, 0.485, 0.515, 0.515);
    const Luma shaded = average(with, 0.485, 0.485, 0.515, 0.515);
    EXPECT_LT(shaded.mean, 0.7 * lit.mean) << "lit " << lit.mean << " shaded " << shaded.mean;
    // Away from the drone and its shadow the ground is unchanged.
    const Luma farWithout = average(without, 0.0, 0.0, 0.15, 0.15);
    const Luma farWith = average(with, 0.0, 0.0, 0.15, 0.15);
    EXPECT_NEAR(farWith.mean, farWithout.mean, 3.0);
}

TEST_F(SceneRenderTest, InsetDrawsIntoItsRectangleWithTheMainExposure) {
    // Main view: the sky (looking up). Inset in the top-left quarter: the ground, straight down.
    const QImage main = render("2026-06-21T12:00:00Z", 0.0, 40.0);
    const auto before = renderer_->readExposure();
    const int mainDraws = renderer_->lastStats().drawCalls;

    graphics::FrameInput input;
    input.camera = graphics::cameraFromAxes({0.0, source_->height(0.0, 0.0) + 30.0, 0.0},
                                            {0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}, 0.9);
    input.originAltitudeM = 650.0;
    input.sky = sky_.state();
    input.stars = &sky_.stars();
    input.starsVersion = sky_.starsVersion();
    input.terrain.gridSize = layout_.gridSize;
    input.terrain.waterLevelM = -40.0;
    input.exposure = exposure_;
    const auto chunks = streamer_->update(0.0, 330.0, 0.0);
    input.terrain.chunks = chunks;
    renderer_->renderInset({.framebuffer = fbo_->handle(), .width = kWidth, .height = kHeight},
                           {.x = 0, .y = 0, .width = kWidth / 2, .height = kHeight / 2}, input);
    const QImage image = fbo_->toImage().convertToFormat(QImage::Format_RGB32);
    save(image, "inset.png");

    // Inside the inset: ground (not blue sky). Outside: the main view, unchanged.
    const Luma inset = average(image, 0.05, 0.05, 0.45, 0.45);
    const Luma skyBefore = average(main, 0.05, 0.05, 0.45, 0.45);
    EXPECT_GT(skyBefore.b, skyBefore.r + 10);
    EXPECT_LT(inset.b, inset.r + 10) << "the inset should show the ground";
    EXPECT_GT(inset.mean, 20.0);
    for (const auto& [x0, y0] :
         {std::pair{0.55, 0.05}, std::pair{0.05, 0.55}, std::pair{0.55, 0.55}}) {
        EXPECT_NEAR(average(image, x0, y0, x0 + 0.4, y0 + 0.4).mean,
                    average(main, x0, y0, x0 + 0.4, y0 + 0.4).mean, 0.5);
    }
    // Not metered: the exposure stays that of the main view; the counters add up.
    EXPECT_DOUBLE_EQ(renderer_->readExposure().ev100, before.ev100);
    EXPECT_GT(renderer_->lastStats().drawCalls, mainDraws);
    EXPECT_GT(renderer_->lastStats().chunksDrawn, 0);
}

TEST_F(SceneRenderTest, SunsetIsRedderTowardsTheSun) {
    // 2026-06-21 sunset in Madrid ≈ 19:48 UTC, azimuth ≈ 302°.
    const QImage image = render("2026-06-21T19:45:00Z", 302.0, 4.0);
    save(image, "sunset.png");
    const Luma horizon = average(image, 0.3, 0.30, 0.7, 0.45);
    const Luma zenithSide = average(image, 0.0, 0.0, 1.0, 0.08);
    EXPECT_GT(horizon.r / std::max(horizon.b, 1.0), zenithSide.r / std::max(zenithSide.b, 1.0))
        << "the sky near the setting sun should be warmer";
}

TEST_F(SceneRenderTest, FullMoonNightShowsTheMoon) {
    // Full moon 2026-08-28 ≈ 04:18 UTC; around 23:00 UTC on the 27th it is low in the SE.
    sky_.update(*astro::parseUtc("2026-08-27T23:30:00Z"), {40.4, -3.7, 650.0});
    const auto& moon = sky_.state().moon;
    ASSERT_GT(moon.elevationDeg, 5.0);
    const QImage image = render("2026-08-27T23:30:00Z", moon.azimuthDeg, moon.elevationDeg);
    save(image, "full_moon.png");
    // The disc is under 2 px wide here: look for its brightest pixel near the centre.
    int brightest = 0;
    for (int y = kHeight / 2 - 4; y <= kHeight / 2 + 4; ++y) {
        for (int x = kWidth / 2 - 4; x <= kWidth / 2 + 4; ++x) {
            brightest = std::max(brightest, qGray(image.pixel(x, y)));
        }
    }
    const Luma corner = average(image, 0.0, 0.0, 0.15, 0.15);
    EXPECT_GT(brightest, 200) << "the moon disc should be bright";
    EXPECT_LT(corner.mean, 60.0) << "the night sky should stay dark";
    const auto exposure = renderer_->readExposure();
    EXPECT_LT(exposure.ev100, 4.0) << "night exposure";
}

// FR-GFX-08a: through a narrow lens the moon fills the view; the half facing the Sun is lit.
TEST_F(SceneRenderTest, MoonDiscIsLitTowardsTheSun) {
    const char* utc = "2026-11-17T17:30:00Z";  // Waxing crescent (~45 %), low in the SW.
    sky_.update(*astro::parseUtc(utc), {40.4, -3.7, 650.0});
    const graphics::SkyState state = sky_.state();
    ASSERT_GT(state.moon.elevationDeg, 5.0);
    graphics::Camera camera;
    camera.yawRad = state.moon.azimuthDeg * 3.14159265358979 / 180.0;
    camera.pitchRad = state.moon.elevationDeg * 3.14159265358979 / 180.0;
    const QImage image = render(utc, state.moon.azimuthDeg, state.moon.elevationDeg, 0.8);
    save(image, "moon_closeup.png");

    // Direction to the Sun projected onto the image (x right, y up).
    const glm::dvec3 toSun(state.moonToSun);
    const double sx = glm::dot(toSun, camera.right());
    const double sy = glm::dot(toSun, camera.up());
    double litSide = 0.0;
    double darkSide = 0.0;
    int lit = 0;
    int dark = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const double dx = x - kWidth / 2.0;
            const double dy = kHeight / 2.0 - y;
            if (dx * dx + dy * dy > 60.0 * 60.0) {
                continue;  // Inside the disc only (radius ≈ 65 px).
            }
            const double g = qGray(image.pixel(x, y));
            if (dx * sx + dy * sy > 0.0) {
                litSide += g;
                ++lit;
            } else {
                darkSide += g;
                ++dark;
            }
        }
    }
    ASSERT_GT(lit, 0);
    ASSERT_GT(dark, 0);
    EXPECT_GT(litSide / lit, 2.0 * darkSide / dark + 20.0);

    // Exposed for the moon itself (EV100 ≈ 13, "looney 11" rule), the maria show: the lit
    // part is not uniform.
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 13.0};
    const QImage exposed = render(utc, state.moon.azimuthDeg, state.moon.elevationDeg, 0.8);
    save(exposed, "moon_closeup_ev13.png");
    double sum = 0.0;
    double sum2 = 0.0;
    int n = 0;
    for (int y = kHeight / 2 - 30; y < kHeight / 2 + 30; ++y) {
        for (int x = kWidth / 2; x < kWidth / 2 + 50; ++x) {
            const double dx = x - kWidth / 2.0;
            const double dy = kHeight / 2.0 - y;
            if (dx * sx + dy * sy > 15.0) {
                const double g = qGray(exposed.pixel(x, y));
                sum += g;
                sum2 += g * g;
                ++n;
            }
        }
    }
    ASSERT_GT(n, 100);
    const double mean = sum / n;
    const double stddev = std::sqrt(std::max(sum2 / n - mean * mean, 0.0));
    EXPECT_GT(mean, 40.0);
    EXPECT_LT(mean, 250.0);
    EXPECT_GT(stddev, 8.0) << "albedo texture should be visible";
}

TEST_F(SceneRenderTest, MoonlessNightIsDarkWithStars) {
    // New moon 2026-08-12; 23:30 UTC: Sun and Moon well below the horizon.
    const QImage image = render("2026-08-12T23:30:00Z", 180.0, 40.0);
    save(image, "moonless.png");
    const Luma all = average(image, 0.0, 0.0, 1.0, 1.0);
    EXPECT_LT(all.mean, 60.0) << "a moonless night must look dark";
    int stars = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) > all.mean + 40) {
                ++stars;
            }
        }
    }
    EXPECT_GT(stars, 5) << "stars should be visible";
}

TEST_F(SceneRenderTest, HeadlightLightsTheGroundAtNight) {
    // FR-GFX-08d: a spot light 3 m above the ground, aimed straight down, on a moonless night.
    const double ground = source_->height(0.0, 0.0);
    graphics::SpotLight lamp;
    lamp.position = {0.0, ground + 3.0, 0.0};
    lamp.direction = {0.0, -1.0, 0.0};
    lamp.color = graphics::headlightColor();
    lamp.intensityCd = 400.0;  // ≈ 44 lux under it.
    lamp.innerAngleRad = 10.0 * std::numbers::pi / 180.0;
    lamp.outerAngleRad = 25.0 * std::numbers::pi / 180.0;
    const std::array<graphics::SpotLight, 1> lights{lamp};
    const graphics::Camera camera =
        graphics::cameraFromAxes({0.0, ground + 6.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}, 0.9);
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 0.0};
    const QImage dark = renderScene("2026-08-12T23:30:00Z", camera, {});
    const QImage lit = renderScene("2026-08-12T23:30:00Z", camera, {}, lights);
    exposure_ = {};
    save(lit, "headlight.png");
    EXPECT_EQ(renderer_->lastStats().spotLights, 1);
    const Luma centreDark = average(dark, 0.45, 0.45, 0.55, 0.55);
    const Luma centreLit = average(lit, 0.45, 0.45, 0.55, 0.55);
    EXPECT_GT(centreLit.mean, centreDark.mean + 40.0)
        << "dark " << centreDark.mean << " lit " << centreLit.mean;
    // Outside the beam (the image corners) the night is unchanged.
    const Luma cornerDark = average(dark, 0.0, 0.0, 0.1, 0.1);
    const Luma cornerLit = average(lit, 0.0, 0.0, 0.1, 0.1);
    EXPECT_NEAR(cornerLit.mean, cornerDark.mean, 2.0);
}

// --- Weather (FR-WX-09) ----------------------------------------------------------------------

int countBrighterThan(const QImage& image, double level) {
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            n += qGray(image.pixel(x, y)) > level ? 1 : 0;
        }
    }
    return n;
}

TEST_F(SceneRenderTest, OvercastSkyIsGreyAndTheGroundDarker) {
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 14.0};
    const QImage clear = render("2026-06-21T12:00:00Z", 0.0, 5.0);
    weather_ = weatherPreset("overcast");
    const QImage overcast = render("2026-06-21T12:00:00Z", 0.0, 5.0);
    save(overcast, "overcast.png");
    const Luma skyClear = average(clear, 0.0, 0.0, 1.0, 0.15);
    const Luma sky = average(overcast, 0.0, 0.0, 1.0, 0.15);
    EXPECT_GT(skyClear.b, skyClear.r + 10);
    EXPECT_LT(std::abs(sky.b - sky.r), 6.0) << "an overcast sky is grey";
    EXPECT_LT(std::abs(sky.g - sky.r), 6.0) << "an overcast sky is grey";
    const Luma groundClear = average(clear, 0.0, 0.85, 1.0, 1.0);
    const Luma ground = average(overcast, 0.0, 0.85, 1.0, 1.0);
    EXPECT_LT(ground.mean, 0.6 * groundClear.mean)
        << "clear " << groundClear.mean << " overcast " << ground.mean;
    EXPECT_GT(ground.mean, 3.0);
}

TEST_F(SceneRenderTest, OvercastLeavesNoDroneShadow) {
    // FR-WX-09: overcast skies diffuse the sunlight; a hovering drone casts no visible shadow.
    weather_ = weatherPreset("overcast");
    const auto noon = astro::parseUtc("2026-06-21T12:00:00Z");
    ASSERT_TRUE(noon.has_value());
    sky_.update(*noon, {40.4, -3.7, 650.0});
    const glm::dvec3 sun(sky_.state().sun.direction);
    const double ground = source_->height(0.0, 0.0);
    const std::array<glm::dvec3, 4> motors{glm::dvec3(0.08, 0.08, 0), glm::dvec3(-0.08, 0.08, 0),
                                           glm::dvec3(-0.08, -0.08, 0), glm::dvec3(0.08, -0.08, 0)};
    const graphics::DroneModel model(graphics::droneShapeFor(motors, 0.127, 3, 0.03, 30.0));
    graphics::DroneVisualState state;
    state.position = glm::dvec3(0.0, ground, 0.0) + sun * (2.0 / sun.y);
    state.orientation =
        glm::quat_cast(glm::dmat3(glm::dvec3(0, 0, -1), glm::dvec3(1, 0, 0), glm::dvec3(0, -1, 0)));
    std::vector<graphics::DrawItem> items;
    model.appendDrawItems(state, items);
    for (graphics::DrawItem& item : items) {
        item.position = state.position + 4.0 * (item.position - state.position);
        item.scale = glm::vec3(4.0F);
    }
    const graphics::Camera camera =
        graphics::cameraFromAxes({0.0, ground + 9.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}, 0.9);
    const QImage without = renderScene("2026-06-21T12:00:00Z", camera, {});
    const QImage with = renderScene("2026-06-21T12:00:00Z", camera, items);
    save(with, "overcast_drone.png");
    const Luma lit = average(without, 0.485, 0.485, 0.515, 0.515);
    const Luma shaded = average(with, 0.485, 0.485, 0.515, 0.515);
    EXPECT_GT(shaded.mean, 0.85 * lit.mean) << "lit " << lit.mean << " shaded " << shaded.mean;
}

TEST_F(SceneRenderTest, BrokenCloudsShowBlueSkyAndWhiteClouds) {
    weather_ = weatherPreset("scattered-clouds");
    const QImage image = render("2026-06-21T12:00:00Z", 0.0, 35.0, 90.0);
    save(image, "scattered.png");
    int blue = 0;
    int white = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.blue() > c.red() + 25) {
                ++blue;
            } else if (c.red() > 150 && std::abs(c.blue() - c.red()) < 25) {
                ++white;
            }
        }
    }
    const int all = image.width() * image.height();
    EXPECT_GT(blue, all / 10) << "blue sky between the clouds";
    EXPECT_GT(white, all / 20) << "white clouds";
}

TEST_F(SceneRenderTest, OvercastNightHidesTheStars) {
    const QImage clear = render("2026-08-12T23:30:00Z", 180.0, 40.0);
    weather_ = weatherPreset("overcast");
    const QImage overcast = render("2026-08-12T23:30:00Z", 180.0, 40.0);
    save(overcast, "overcast_night.png");
    const double level = average(clear, 0, 0, 1, 1).mean + 40;
    EXPECT_GT(countBrighterThan(clear, level), 5);
    EXPECT_EQ(countBrighterThan(overcast, level), 0) << "clouds hide the stars";
}

TEST_F(SceneRenderTest, FogHidesTheDistantTerrain) {
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 14.0};
    graphics::Camera camera;
    camera.position = {0.0, source_->height(0.0, 0.0) + 5.0, 0.0};
    camera.pitchRad = -0.05;
    const QImage clear = renderScene("2026-06-21T10:00:00Z", camera, {});
    weather_ = weatherPreset("fog");
    const QImage fog = renderScene("2026-06-21T10:00:00Z", camera, {});
    save(fog, "fog.png");
    // Clear: blue sky above the terrain. Fog: the distant terrain fades into the fog, which
    // looks the same above and below the horizon.
    const auto contrast = [](const QImage& image) {
        // The hills around meet the sky at y ≈ 0.47; just below, the ground is a few hundred
        // metres away.
        const Luma above = average(image, 0.0, 0.30, 1.0, 0.42);
        const Luma below = average(image, 0.0, 0.48, 1.0, 0.52);
        return std::abs(above.r - below.r) + std::abs(above.g - below.g) +
               std::abs(above.b - below.b);
    };
    EXPECT_LT(contrast(fog), 0.3 * contrast(clear))
        << "clear " << contrast(clear) << " fog " << contrast(fog);
    const Luma horizon = average(fog, 0.0, 0.4, 1.0, 0.6);
    EXPECT_LT(std::abs(horizon.b - horizon.r), 15.0) << "fog is grey";
    EXPECT_GT(horizon.mean, 60.0) << "lit fog is bright";
}

TEST_F(SceneRenderTest, RainAndSnowAreDrawnAroundTheCamera) {
    graphics::Camera camera;
    camera.position = {0.0, source_->height(0.0, 0.0) + 20.0, 0.0};
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 13.0};
    weather_ = weatherPreset("heavy-rain");
    weather_.particles = 0;
    const QImage dry = renderScene("2026-06-21T12:00:00Z", camera, {});
    weather_ = weatherPreset("heavy-rain");
    const QImage rain = renderScene("2026-06-21T12:00:00Z", camera, {});
    save(rain, "rain.png");
    EXPECT_GT(renderer_->lastStats().precipitation, 1000);
    weather_ = weatherPreset("heavy-snow");
    const QImage snow = renderScene("2026-06-21T12:00:00Z", camera, {});
    save(snow, "snow.png");
    EXPECT_GT(renderer_->lastStats().precipitation, 1000);
    const auto difference = [](const QImage& a, const QImage& b) {
        int changed = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                changed += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y))) > 3 ? 1 : 0;
            }
        }
        return changed;
    };
    EXPECT_GT(difference(dry, rain), kWidth * kHeight / 200) << "rain streaks";
    EXPECT_GT(difference(dry, snow), kWidth * kHeight / 200) << "snowflakes";
}

TEST_F(SceneRenderTest, SnowCoverWhitensAndRainDarkensTheGround) {
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 12.0};
    const double ground = source_->height(0.0, 0.0);
    const graphics::Camera camera = graphics::cameraFromAxes(
        {0.0, ground + 40.0, 0.0}, {0.0, -1.0, 0.0}, {0.0, 0.0, -1.0}, 0.9);
    weather_ = weatherPreset("overcast");
    const double dry = average(renderScene("2026-06-21T12:00:00Z", camera, {}), 0, 0, 1, 1).mean;
    weather_ = weatherPreset("overcast", {.wetness = 1.0, .snowDepthM = 0.0});
    const double wet = average(renderScene("2026-06-21T12:00:00Z", camera, {}), 0, 0, 1, 1).mean;
    weather_ = weatherPreset("overcast", {.wetness = 0.0, .snowDepthM = 0.1});
    const QImage snowy = renderScene("2026-06-21T12:00:00Z", camera, {});
    save(snowy, "snow_cover.png");
    const double snow = average(snowy, 0, 0, 1, 1).mean;
    EXPECT_LT(wet, dry) << "wet ground is darker";
    EXPECT_GT(snow, dry + 40.0) << "snow is white";
}

TEST_F(SceneRenderTest, RaindropsOnTheLens) {
    graphics::Camera camera;
    camera.position = {0.0, source_->height(0.0, 0.0) + 20.0, 0.0};
    camera.pitchRad = 0.1;  // Sky above, ground below: the drops show both, upside down.
    weather_ = weatherPreset("light-rain");
    weather_.particles = 0;
    const QImage dry = renderScene("2026-06-21T12:00:00Z", camera, {});
    lensWetness_ = 1.0;
    const QImage wet = renderScene("2026-06-21T12:00:00Z", camera, {});
    lensWetness_ = 0.0;
    save(wet, "lens_drops.png");
    int changed = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            changed += std::abs(qGray(dry.pixel(x, y)) - qGray(wet.pixel(x, y))) > 8 ? 1 : 0;
        }
    }
    EXPECT_GT(changed, kWidth * kHeight / 50) << "drops on the lens";
    EXPECT_LT(changed, kWidth * kHeight / 2) << "the view stays mostly clear";
}

TEST_F(SceneRenderTest, LightningLightsTheStormAtNight) {
    // A moonless night under a thunderstorm: dark between flashes, lit during one.
    weather_ = weatherPreset("thunderstorm");
    double flashTime = -1.0;
    double darkTime = -1.0;
    for (double t = 0.0; t < 600.0 && (flashTime < 0.0 || darkTime < 0.0); t += 0.01) {
        const double lux = graphics::lightningLux(t);
        if (flashTime < 0.0 && lux > 4.0) {
            flashTime = t;
        }
        if (darkTime < 0.0 && t > 1.0 && lux == 0.0) {
            darkTime = t;
        }
    }
    ASSERT_GE(flashTime, 0.0);
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 3.0};
    weather_.lightningLux = graphics::lightningLux(darkTime);
    const QImage dark = render("2026-08-12T23:30:00Z", 0.0, 10.0);
    weather_.lightningLux = graphics::lightningLux(flashTime);
    const QImage flash = render("2026-08-12T23:30:00Z", 0.0, 10.0);
    save(flash, "lightning.png");
    const double before = average(dark, 0, 0, 1, 1).mean;
    const double during = average(flash, 0, 0, 1, 1).mean;
    EXPECT_LT(before, 30.0);
    EXPECT_GT(during, before + 40.0) << "dark " << before << " flash " << during;
}

TEST_F(SceneRenderTest, ManualExposureIsApplied) {
    exposure_ = {.mode = graphics::ExposureMode::Manual, .manualEv100 = 18.0};
    const QImage dark = render("2026-06-21T12:00:00Z", 0.0, 5.0);
    EXPECT_NEAR(renderer_->readExposure().ev100, 18.0, 1e-3);
    exposure_ = {};
    const QImage normal = render("2026-06-21T12:00:00Z", 0.0, 5.0);
    EXPECT_LT(average(dark, 0, 0, 1, 1).mean + 20.0, average(normal, 0, 0, 1, 1).mean);
}

// NFR-PERF-01: ≥ 60 FPS at 1920×1080. Asserted only on hardware renderers (software
// rasterisers such as llvmpipe are far slower); the measured time is always printed.
TEST_F(SceneRenderTest, FrameTimeAt1080p) {
    constexpr int kW = 1920;
    constexpr int kH = 1080;
    QOpenGLFramebufferObject fbo(kW, kH);
    ASSERT_TRUE(fbo.isValid());
    const auto time = astro::parseUtc("2026-06-21T18:00:00Z");
    sky_.update(*time, {40.4, -3.7, 650.0});
    graphics::FrameInput input;
    input.camera.position = {0.0, 120.0, 0.0};
    input.camera.pitchRad = -0.1;
    input.originAltitudeM = 650.0;
    input.sky = sky_.state();
    input.stars = &sky_.stars();
    input.starsVersion = sky_.starsVersion();
    input.terrain.gridSize = layout_.gridSize;
    input.terrain.waterLevelM = -40.0;
    input.dtSeconds = 1.0 / 60.0;
    std::vector<std::shared_ptr<const terrain::ChunkMesh>> chunks;
    for (int i = 0; i < 400; ++i) {
        chunks = streamer_->update(0.0, 120.0, 0.0);
        if (streamer_->stats().inFlight == 0 && i > 0) {
            break;
        }
        pool_.waitIdle();
    }
    input.terrain.chunks = chunks;
    const graphics::FrameTarget target{.framebuffer = fbo.handle(), .width = kW, .height = kH};
    const auto finish = [this] {
        // Read one pixel back: waits until the GPU has finished the frame.
        fbo_->bind();
        static_cast<void>(fbo_->toImage());
    };
    for (int i = 0; i < 5; ++i) {
        renderer_->renderFrame(target, input);  // Warm-up (uploads, shader caches).
    }
    finish();
    constexpr int kFrames = 60;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        input.camera.yawRad = 0.1 * i;
        renderer_->renderFrame(target, input);
    }
    finish();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() /
        kFrames;
    const auto info = QString::fromLatin1(
        reinterpret_cast<const char*>(context_->functions()->glGetString(GL_RENDERER)));
    std::printf("[ PERF     ] %.2f ms/frame at 1920x1080 (%d chunks drawn, %llu triangles) on %s\n",
                ms, renderer_->lastStats().chunksDrawn,
                static_cast<unsigned long long>(renderer_->lastStats().triangles),
                info.toStdString().c_str());
    const bool hardware =
        !info.contains(QLatin1String("llvmpipe")) && !info.contains(QLatin1String("softpipe"));
    if (hardware) {
        EXPECT_LT(ms, 16.7);
    }

    // The same with the weather: broken clouds over a wide sky, and heavy rain.
    input.weather = weatherPreset("broken-clouds");
    const graphics::WeatherView rain = weatherPreset("heavy-rain");
    input.weather.precipitation = rain.precipitation;
    input.weather.rateMmH = rain.rateMmH;
    input.weather.fallSpeedMs = rain.fallSpeedMs;
    input.weather.particles = rain.particles;
    input.camera.pitchRad = 0.3;
    for (int i = 0; i < 5; ++i) {
        renderer_->renderFrame(target, input);
    }
    finish();
    const auto weatherStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        input.camera.yawRad = 0.1 * i;
        renderer_->renderFrame(target, input);
    }
    finish();
    const double weatherMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - weatherStart)
            .count() /
        kFrames;
    std::printf("[ PERF     ] %.2f ms/frame at 1920x1080 with broken clouds and heavy rain\n",
                weatherMs);
    save(fbo.toImage().convertToFormat(QImage::Format_RGB32), "weather_1080p.png");
    if (hardware) {
        EXPECT_LT(weatherMs, 16.7);
    }
}

TEST_F(SceneRenderTest, ShaderErrorsNameFileAndLine) {
    const auto dir = std::filesystem::temp_directory_path() / "ds_shader_error_test";
    std::filesystem::remove_all(dir);
    std::filesystem::copy(kAssets / "shaders", dir);
    {
        std::ofstream broken(dir / "tonemap.frag", std::ios::app);
        broken << "\nthis is not glsl;\n";
    }
    graphics::Renderer other;
    ASSERT_TRUE(other.initialize(&resolve, context_.get()).has_value());
    const auto result = other.loadResources({.shaderDirectory = dir});
    std::filesystem::remove_all(dir);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("tonemap.frag:"), std::string::npos)
        << result.error().message;
}

}  // namespace
