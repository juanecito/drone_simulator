// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSignalSpy>
#include <QStatusBar>
#include <QSurfaceFormat>
#include <QTest>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <utility>

#include "ds/app/main_window.hpp"
#include "ds/app/opengl_check.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/viewport.hpp"
#include "ds/astro/time.hpp"
#include "ds/graphics/renderer.hpp"

namespace {

using ds::graphics::Renderer;

ds::graphics::GlProcAddress resolve(void* user, const char* name) {
    return static_cast<QOpenGLContext*>(user)->getProcAddress(name);
}

// Skips the current test unless an OpenGL 4.5 core context can really be created.
#define DS_REQUIRE_OPENGL_45()                                                          \
    do {                                                                                \
        if (const auto check = ds::app::checkOpenGlContext(); !check) {                 \
            GTEST_SKIP() << "OpenGL 4.5 core not available: " << check.error().message; \
        }                                                                               \
    } while (false)

// An offscreen OpenGL 4.5 core context made current, or skips the test.
class OpenGlTest : public ::testing::Test {
protected:
    void SetUp() override {
        DS_REQUIRE_OPENGL_45();
        context_ = std::make_unique<QOpenGLContext>();
        context_->setFormat(QSurfaceFormat::defaultFormat());
        ASSERT_TRUE(context_->create());
        surface_ = std::make_unique<QOffscreenSurface>();
        surface_->setFormat(context_->format());
        surface_->create();
        ASSERT_TRUE(context_->makeCurrent(surface_.get()));
    }

    void TearDown() override {
        if (context_ != nullptr) {
            context_->doneCurrent();
        }
    }

    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<QOffscreenSurface> surface_;
};

// The start-up check (Qt side) and the renderer (Core side) must agree on every platform,
// including those where no suitable context exists.
TEST(OpenGlCheck, AgreesWithRenderer) {
    const auto check = ds::app::checkOpenGlContext();
    QOpenGLContext context;
    context.setFormat(QSurfaceFormat::defaultFormat());
    if (!context.create()) {
        EXPECT_FALSE(check.has_value());
        return;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    ASSERT_TRUE(context.makeCurrent(&surface));
    Renderer renderer;
    const auto info = renderer.initialize(&resolve, &context);
    context.doneCurrent();
    EXPECT_EQ(check.has_value(), info.has_value())
        << "check: " << (check ? "ok" : check.error().message)
        << "\nrenderer: " << (info ? "ok" : info.error().message);
}

TEST_F(OpenGlTest, RendererInitialisesAndReportsContext) {
    Renderer renderer;
    const auto info = renderer.initialize(&resolve, context_.get());
    ASSERT_TRUE(info.has_value()) << info.error().message;
    EXPECT_TRUE(renderer.isInitialized());
    EXPECT_TRUE(info->coreProfile);
    EXPECT_GE(info->major * 10 + info->minor, 45);
    EXPECT_FALSE(info->renderer.empty());
    EXPECT_FALSE(info->version.empty());
}

TEST_F(OpenGlTest, RenderFrameClearsTargetToClearColour) {
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize(&resolve, context_.get()).has_value());
    renderer.setClearColor({.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F});

    constexpr int kSize = 32;
    QOpenGLFramebufferObject fbo(kSize, kSize, QOpenGLFramebufferObject::CombinedDepthStencil);
    ASSERT_TRUE(fbo.isValid());
    renderer.renderFrame({.framebuffer = fbo.handle(), .width = kSize, .height = kSize});

    const QImage image = fbo.toImage();
    EXPECT_EQ(image.pixelColor(0, 0), QColor(255, 0, 0));
    EXPECT_EQ(image.pixelColor(kSize - 1, kSize - 1), QColor(255, 0, 0));
}

// End to end: the real main window gets a working viewport (FR-UI-01).
TEST(Viewport, MainWindowViewportBecomesReady) {
    DS_REQUIRE_OPENGL_45();

    ds::app::MainWindow window;
    QSignalSpy ready(window.viewport(), &ds::app::Viewport::glReady);
    QSignalSpy failed(window.viewport(), &ds::app::Viewport::glFailed);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    if (ready.isEmpty() && failed.isEmpty()) {
        ASSERT_TRUE(ready.wait(5000)) << "viewport did not initialise";
    }
    ASSERT_TRUE(failed.isEmpty()) << failed.first().first().toString().toStdString();
    ASSERT_EQ(ready.size(), 1);
    ASSERT_TRUE(window.viewport()->glInfo().has_value());
    EXPECT_TRUE(window.viewport()->glInfo()->coreProfile);
}

// The drone seen from the chase camera with the HUD painted over the 3D view (FR-GFX-05,
// FR-GFX-14, FR-GFX-14a). Without settings nothing is saved.
// Pixels of the compass's heading arrow (yellow) and magnetic-north marker (blue) in the
// bottom-centre area of @p image (FR-GFX-14b).
std::pair<int, int> compassPixels(const QImage& image) {
    int yellow = 0;
    int blue = 0;
    const int w = image.width();
    const int h = image.height();
    for (int y = h * 2 / 3; y < h; ++y) {
        for (int x = w * 2 / 5; x < w * 3 / 5; ++x) {
            const QRgb c = image.pixel(x, y);
            yellow += (qRed(c) > 220 && qGreen(c) > 180 && qBlue(c) < 110) ? 1 : 0;
            blue += (qRed(c) < 140 && qGreen(c) > 140 && qBlue(c) > 220) ? 1 : 0;
        }
    }
    return {yellow, blue};
}

TEST(Viewport, ChaseViewShowsTheDroneAndTheHud) {
    DS_REQUIRE_OPENGL_45();

    ds::app::MainWindow window;
    QSignalSpy ready(window.viewport(), &ds::app::Viewport::glReady);
    ds::app::EnvironmentConfig env = window.scene()->environment();
    env.time = *ds::astro::parseUtc("2026-06-21T10:00:00Z");
    env.animate = false;
    window.scene()->setEnvironment(env);
    window.simulation()->setCameraMode(ds::graphics::CameraMode::Chase);
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    if (ready.isEmpty()) {
        ASSERT_TRUE(ready.wait(5000)) << "viewport did not initialise";
    }
    QTest::qWait(1500);  // Terrain streaming and exposure adaptation.

    const QImage image = window.viewport()->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        image.save(QString::fromUtf8(dir) + QStringLiteral("/chase_view.png"));
    }
    EXPECT_GT(window.viewport()->frameStats().objectsDrawn, 5);
    // The position panel (top left) has bright text with dark outlines.
    int bright = 0;
    int dark = 0;
    const double s = static_cast<double>(image.height()) / 800.0;
    for (int y = static_cast<int>(10 * s); y < static_cast<int>(60 * s); ++y) {
        for (int x = static_cast<int>(10 * s); x < static_cast<int>(300 * s); ++x) {
            const int l = qGray(image.pixel(x, y));
            bright += l > 200 ? 1 : 0;
            dark += l < 40 ? 1 : 0;
        }
    }
    EXPECT_GT(bright, 100);
    EXPECT_GT(dark, 100);
    EXPECT_TRUE(window.statusBar()
                    ->findChild<QLabel*>(QStringLiteral("PositionLabel"))
                    ->text()
                    .startsWith(QStringLiteral("UTM")));
    const auto [yellow, blue] = compassPixels(image);
    EXPECT_GT(yellow, 40) << "drone heading arrow";
    EXPECT_GT(blue, 15) << "magnetic north marker";

    // The free camera keeps the compass, with the view direction (FR-GFX-14b).
    window.simulation()->setCameraMode(ds::graphics::CameraMode::Free);
    window.scene()->camera().yawRad = 1.0;
    QTest::qWait(300);
    const QImage free = window.viewport()->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        free.save(QString::fromUtf8(dir) + QStringLiteral("/free_view.png"));
    }
    const auto [freeYellow, freeBlue] = compassPixels(free);
    EXPECT_GT(freeYellow, 40);
    EXPECT_GT(freeBlue, 15);
}

// A mission in flight (M5): the mission's items are drawn and the HUD shows the mission panel.
TEST(Viewport, MissionItemsAndPanelAreDrawn) {
    DS_REQUIRE_OPENGL_45();

    ds::app::MainWindow window;
    QSignalSpy ready(window.viewport(), &ds::app::Viewport::glReady);
    window.resize(1280, 800);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    if (ready.isEmpty()) {
        ASSERT_TRUE(ready.wait(5000)) << "viewport did not initialise";
    }
    QTest::qWait(500);
    const int freeObjects = window.viewport()->frameStats().objectsDrawn;
    const auto launched = window.launchMission("racing-01-gate-run");
    ASSERT_TRUE(launched.has_value()) << launched.error().message;
    QTest::qWait(2000);  // Terrain streaming and exposure adaptation.
    const QImage image = window.viewport()->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        image.save(QString::fromUtf8(dir) + QStringLiteral("/mission_view.png"));
    }
    // Pad, five gates (frame, posts, arrow) on top of the free-flight scene.
    EXPECT_GE(window.viewport()->frameStats().objectsDrawn, freeObjects + 6);
    EXPECT_TRUE(window.simulation()->missionStatus().active);
    window.endMission();
    EXPECT_FALSE(window.simulation()->missionActive());
}

// Mean absolute grey-level difference of two images of the same size, 0–255.
double meanDifference(const QImage& a, const QImage& b) {
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < a.height(); y += 4) {
        for (int x = 0; x < a.width(); x += 4) {
            sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
            ++n;
        }
    }
    return sum / std::max(n, 1);
}

// Pearson correlation of the grey levels of two images of the same size: 1 for the same
// picture, independent of overall brightness and contrast (exposure adaptation).
double correlation(const QImage& a, const QImage& b) {
    double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
    int n = 0;
    for (int y = 0; y < a.height(); y += 4) {
        for (int x = 0; x < a.width(); x += 4) {
            const double va = qGray(a.pixel(x, y));
            const double vb = qGray(b.pixel(x, y));
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
            ++n;
        }
    }
    const double cov = sab - sa * sb / n;
    const double var = (saa - sa * sa / n) * (sbb - sb * sb / n);
    return var > 0.0 ? cov / std::sqrt(var) : 0.0;
}

// The viewport's image once the view is steady (terrain streamed, exposure adapted): two grabs
// in a row nearly equal. Slow builds (ASan, Valgrind) need many more frames to get there.
QImage settledFrame(ds::app::Viewport& viewport, int timeoutMs) {
    QImage previous = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QTest::qWait(250);
        QImage next = viewport.grabFramebuffer().convertToFormat(QImage::Format_RGB32);
        const bool steady = meanDifference(next, previous) < 1.0;
        previous = std::move(next);
        if (steady) {
            break;
        }
    }
    return previous;
}

// Qt replaces the viewport's OpenGL context when the window's graphics device is lost (for
// example after suspend/resume with the NVIDIA driver) or the widget moves to another window:
// it sends WindowAboutToChangeInternal and WindowChangeInternal, destroys the context and calls
// initializeGL() again in a new one. The renderer must drop its old objects with the old
// context and rebuild them in the new one. Regression: the old objects were deleted with the
// new context current, deleting the new renderer's objects that had the same names; the next
// indexed draw then ran with no element buffer and crashed inside libnvidia-glcore.
TEST(Viewport, SurvivesTheContextBeingRecreated) {
    DS_REQUIRE_OPENGL_45();

    ds::app::MainWindow window;
    QSignalSpy ready(window.viewport(), &ds::app::Viewport::glReady);
    ds::app::EnvironmentConfig env = window.scene()->environment();
    env.time = *ds::astro::parseUtc("2026-06-21T10:00:00Z");
    env.animate = false;
    window.scene()->setEnvironment(env);
    window.simulation()->setCameraMode(ds::graphics::CameraMode::Chase);
    window.resize(960, 600);
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    if (ready.isEmpty()) {
        ASSERT_TRUE(ready.wait(5000)) << "viewport did not initialise";
    }
    QTest::qWait(500);
    const QImage first = settledFrame(*window.viewport(), 20000);
    if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
        first.save(QString::fromUtf8(dir) + QStringLiteral("/recreated_0.png"));
    }
    const int objects = window.viewport()->frameStats().objectsDrawn;
    ASSERT_GT(objects, 5);

    for (int round = 1; round <= 2; ++round) {
        const auto initialisations = ready.count();
        QEvent aboutToChange(QEvent::WindowAboutToChangeInternal);
        QCoreApplication::sendEvent(window.viewport(), &aboutToChange);
        QEvent changed(QEvent::WindowChangeInternal);
        QCoreApplication::sendEvent(window.viewport(), &changed);
        if (ready.count() == initialisations) {
            ASSERT_TRUE(ready.wait(5000)) << "viewport was not initialised again";
        }
        // The new context starts from scratch (GPU caches, exposure): wait for a steady view.
        QTest::qWait(500);
        const QImage image = settledFrame(*window.viewport(), 20000);
        if (const char* dir = std::getenv("DS_TEST_IMAGE_DIR")) {
            image.save(QString::fromUtf8(dir) + QStringLiteral("/recreated_%1.png").arg(round));
        }
        EXPECT_EQ(window.viewport()->frameStats().objectsDrawn, objects) << "round " << round;
        ASSERT_EQ(image.size(), first.size());
        // Same view: terrain, drone and sky drawn again. With the bug only the sky and the HUD
        // were left (correlation about 0.76). Brightness may differ while exposure adapts.
        EXPECT_GT(correlation(image, first), 0.95) << "round " << round;
    }
}

}  // namespace
