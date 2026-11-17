// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/viewport.hpp"

#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "ds/app/asset_paths.hpp"
#include "ds/app/hud_painter.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/common/log.hpp"
#include "ds/common/math.hpp"
#include "ds/input/keyboard.hpp"

namespace ds::app {
namespace {

// Bridges the Qt-free renderer to Qt's function resolver (SPECIFICATION §3.1).
graphics::GlProcAddress resolveGlFunction(void* user, const char* name) {
    return static_cast<QOpenGLContext*>(user)->getProcAddress(name);
}

int toDevicePixels(int logical, qreal ratio) {
    return static_cast<int>(std::lround(static_cast<qreal>(logical) * ratio));
}

constexpr double kMouseRadiansPerPixel = 0.0025;
constexpr double kMaxFrameSeconds = 0.25;

#ifdef NDEBUG
constexpr bool kHotReload = false;
#else
constexpr bool kHotReload = true;  // FR-GFX-04: shader hot reload in Debug builds.
#endif

}  // namespace

Viewport::Viewport(SceneController* scene, SimulationController* simulation, QWidget* parent)
    : QOpenGLWidget(parent),
      scene_(scene),
      simulation_(scene != nullptr ? simulation : nullptr),
      renderer_(std::make_unique<graphics::Renderer>()) {
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
}

Viewport::~Viewport() {
    // GL resources owned by the renderer must be released with the context current.
    if (QOpenGLContext* glContext = context()) {
        // ~QOpenGLWidget destroys the context after this object is gone: no callback then.
        disconnect(glContext, &QOpenGLContext::aboutToBeDestroyed, this, &Viewport::releaseGl);
        makeCurrent();
        renderer_.reset();
        doneCurrent();
    }
}

void Viewport::releaseGl() {
    // The context is about to be destroyed (Qt replaces it when the window's graphics device
    // is lost or the widget changes window). Its objects are deleted while it can still be
    // made current; initializeGL() then rebuilds them in the new context.
    makeCurrent();
    renderer_->releaseResources();
    doneCurrent();
    log::info("OpenGL context replaced; renderer resources released");
}

void Viewport::setRenderSettings(const graphics::RenderSettings& settings) {
    renderer_->setSettings(settings);
}

void Viewport::initializeGL() {
    QOpenGLContext* glContext = context();
    if (glContext == nullptr || !glContext->isValid()) {
        const QString message =
            tr("Could not create an OpenGL context. Drone Simulator needs OpenGL %1.%2 (core "
               "profile) or newer; check that the graphics driver is installed.")
                .arg(graphics::Renderer::kRequiredMajor)
                .arg(graphics::Renderer::kRequiredMinor);
        log::error("{}", message.toStdString());
        emit glFailed(message);
        return;
    }

    // Direct connection: the slot makes the dying context current (Qt's documented pattern).
    connect(glContext, &QOpenGLContext::aboutToBeDestroyed, this, &Viewport::releaseGl,
            static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
    auto info = renderer_->initialize(&resolveGlFunction, glContext);
    if (!info) {
        log::error("OpenGL initialisation failed: {}", info.error().message);
        emit glFailed(QString::fromStdString(info.error().message));
        return;
    }
    glInfo_ = *info;

    if (scene_ != nullptr) {
        const std::filesystem::path assets = findAssetDirectory();
        auto loaded = renderer_->loadResources({.shaderDirectory = assets / "shaders",
                                                .moonTexture = assets / "sky" / "moon_albedo.pgm"});
        if (!loaded) {
            log::error("{}", loaded.error().message);
            emit glFailed(tr("The 3D view could not be set up:\n\n%1")
                              .arg(QString::fromStdString(loaded.error().message)));
            return;
        }
        // Continuous rendering, paced by the buffer swap (V-Sync).
        connect(this, &QOpenGLWidget::frameSwapped, this, qOverload<>(&QWidget::update),
                Qt::UniqueConnection);
    }
    clock_.start();
    statsClock_.start();
    exposureClock_.start();
    reloadClock_.start();
    emit glReady(QStringLiteral("OpenGL %1.%2 — %3")
                     .arg(info->major)
                     .arg(info->minor)
                     .arg(QString::fromStdString(info->renderer)));
}

void Viewport::paintGL() {
    const qreal ratio = devicePixelRatioF();
    const graphics::FrameTarget target{
        .framebuffer = defaultFramebufferObject(),
        .width = toDevicePixels(width(), ratio),
        .height = toDevicePixels(height(), ratio),
    };
    if (scene_ == nullptr || !renderer_->hasResources()) {
        renderer_->renderFrame(target);
        return;
    }
    renderScene(target);
}

void Viewport::renderScene(const graphics::FrameTarget& target) {
    const double dt = std::min(static_cast<double>(clock_.nsecsElapsed()) * 1e-9, kMaxFrameSeconds);
    clock_.restart();
    animationTime_ += dt;

    if (kHotReload && reloadClock_.elapsed() > 1000) {
        reloadClock_.restart();
        if (auto reloaded = renderer_->reloadChangedShaders(); !reloaded) {
            log::error("{}", reloaded.error().message);
        }
    }

    const double aspect =
        static_cast<double>(target.width) / static_cast<double>(std::max(target.height, 1));
    // One raw pointer per call: the QPointer is only re-read between events.
    SimulationController* const sim = simulation_.data();
    if (sim != nullptr) {
        sim->updateFrame(dt, aspect);
    }
    const bool vehicleView = sim != nullptr && sim->vehicleView();
    graphics::FreeFlyInput input;
    if (!vehicleView) {
        input.forward = (keys_.forward ? 1.0 : 0.0) - (keys_.back ? 1.0 : 0.0);
        input.right = (keys_.right ? 1.0 : 0.0) - (keys_.left ? 1.0 : 0.0);
        input.up = (keys_.up ? 1.0 : 0.0) - (keys_.down ? 1.0 : 0.0);
        input.boost = keys_.boost;
    }
    input.yawDeltaRad = std::exchange(yawDelta_, 0.0);
    input.pitchDeltaRad = std::exchange(pitchDelta_, 0.0);
    scene_->freeFly().update(scene_->camera(), input, dt);
    scene_->advance(dt);
    const graphics::Camera camera =
        (sim != nullptr && vehicleView) ? sim->vehicleCamera() : scene_->camera();

    const auto chunks =
        scene_->streamer().update(camera.position.x, camera.position.y, camera.position.z);
    const TerrainConfig& terrainConfig = scene_->terrain();
    const EnvironmentConfig& env = scene_->environment();
    graphics::FrameInput frame;
    frame.camera = camera;
    frame.originAltitudeM = terrainConfig.origin.heightM;
    frame.sky = scene_->sky().state();
    frame.stars = &scene_->sky().stars();
    frame.starsVersion = scene_->sky().starsVersion();
    frame.terrain.chunks = chunks;
    frame.terrain.gridSize = scene_->layout().gridSize;
    frame.terrain.extent = terrainConfig.extent;
    frame.terrain.waterLevelM = scene_->source()->waterLevel();
    frame.terrain.waterBeyondExtent = terrainConfig.params.edge == terrain::EdgeMode::Water;
    frame.exposure = env.exposure;
    frame.visibilityM = env.visibilityM;
    frame.dtSeconds = dt;
    frame.timeSeconds = animationTime_;
    frame.resetExposure = scene_->takeExposureReset();
    if (sim != nullptr) {
        frame.objects = sim->drawItems();
    }
    renderer_->renderFrame(target, frame);
    lastCamera_ = camera;
    if (sim != nullptr && vehicleView) {
        paintHud(*sim);
    } else if (sim != nullptr) {
        paintFreeCompass(*sim, camera);
    }

    ++framesSinceStats_;
    if (exposureClock_.elapsed() >= 500) {
        exposureClock_.restart();
        exposure_ = renderer_->readExposure();
    }
    if (const qint64 ms = statsClock_.elapsed(); ms >= 250) {
        fps_ = 1000.0 * framesSinceStats_ / static_cast<double>(ms);
        framesSinceStats_ = 0;
        statsClock_.restart();
        emit statsUpdated();
    }
}

bool Viewport::setKey(int key, bool pressed) {
    switch (key) {
        case Qt::Key_W:
        case Qt::Key_Up: keys_.forward = pressed; return true;
        case Qt::Key_S:
        case Qt::Key_Down: keys_.back = pressed; return true;
        case Qt::Key_A:
        case Qt::Key_Left: keys_.left = pressed; return true;
        case Qt::Key_D:
        case Qt::Key_Right: keys_.right = pressed; return true;
        case Qt::Key_E:
        case Qt::Key_PageUp: keys_.up = pressed; return true;
        case Qt::Key_Q:
        case Qt::Key_PageDown: keys_.down = pressed; return true;
        case Qt::Key_Shift: keys_.boost = pressed; return true;
        default: return false;
    }
}

bool Viewport::flightKey(SimulationController& sim, int key, bool pressed) {
    using input::FlightKey;
    FlightKey bit{};
    switch (key) {
        case Qt::Key_W: bit = FlightKey::ThrottleUp; break;
        case Qt::Key_S: bit = FlightKey::ThrottleDown; break;
        case Qt::Key_A: bit = FlightKey::YawLeft; break;
        case Qt::Key_D: bit = FlightKey::YawRight; break;
        case Qt::Key_Up: bit = FlightKey::PitchForward; break;
        case Qt::Key_Down: bit = FlightKey::PitchBack; break;
        case Qt::Key_Left: bit = FlightKey::RollLeft; break;
        case Qt::Key_Right: bit = FlightKey::RollRight; break;
        default: return false;
    }
    const auto mask = static_cast<std::uint32_t>(bit);
    flightKeys_ = pressed ? (flightKeys_ | mask) : (flightKeys_ & ~mask);
    sim.setFlightKeys(flightKeys_);
    return true;
}

bool Viewport::commandKey(SimulationController& sim, int key) {
    using input::KeyboardAction;
    switch (key) {
        case Qt::Key_C: sim.cycleCameraMode(); return true;
        case Qt::Key_P: sim.togglePause(); return true;
        case Qt::Key_R: sim.reset(); return true;
        default: break;
    }
    if (!sim.vehicleView()) {
        return false;
    }
    switch (key) {
        case Qt::Key_Space: sim.keyboardAction(KeyboardAction::ToggleArm); return true;
        case Qt::Key_1: sim.keyboardAction(KeyboardAction::Mode1); return true;
        case Qt::Key_2: sim.keyboardAction(KeyboardAction::Mode2); return true;
        case Qt::Key_3: sim.keyboardAction(KeyboardAction::Mode3); return true;
        case Qt::Key_4: sim.keyboardAction(KeyboardAction::Mode4); return true;
        case Qt::Key_L: sim.keyboardAction(KeyboardAction::ToggleLights); return true;
        default: return false;
    }
}

void Viewport::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && captured_) {
        releaseMouse();
        return;
    }
    if (event->isAutoRepeat()) {
        // Held keys stay held; swallow the repeats of keys we handle.
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    if (SimulationController* const sim = simulation_.data()) {
        if (commandKey(*sim, event->key())) {
            if (!sim->vehicleView()) {
                flightKeys_ = 0;
                sim->setFlightKeys(0);
            }
            return;
        }
        if (sim->vehicleView() && flightKey(*sim, event->key(), true)) {
            return;
        }
    }
    if (!setKey(event->key(), true)) {
        QOpenGLWidget::keyPressEvent(event);
    }
}

void Viewport::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        QOpenGLWidget::keyReleaseEvent(event);
        return;
    }
    // Releases always clear both sets (the view may have changed while the key was held).
    SimulationController* const sim = simulation_.data();
    const bool flight = sim != nullptr && flightKey(*sim, event->key(), false);
    const bool camera = setKey(event->key(), false);
    if (!flight && !camera) {
        QOpenGLWidget::keyReleaseEvent(event);
    }
}

void Viewport::paintHud(const SimulationController& sim) {
    QPainter painter(this);
    const qint64 ms = QDateTime::currentMSecsSinceEpoch();
    HudPaintOptions options;
    options.horizon = sim.cameraMode() == graphics::CameraMode::Fpv;
    options.blinkOn = (ms / 500) % 2 == 0;
    options.scale = std::clamp(static_cast<double>(height()) / 720.0, 0.8, 2.0);
    options.camera = &lastCamera_;
    ds::app::paintHud(painter, rect(), sim.hud(), options);
}

void Viewport::paintFreeCompass(const SimulationController& sim, const graphics::Camera& camera) {
    // Free camera: only the compass, with the drone's heading and the camera's (FR-GFX-14b).
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const double scale = std::clamp(static_cast<double>(height()) / 720.0, 0.8, 2.0);
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setBold(true);
    font.setPointSizeF(10.0 * scale);
    painter.setFont(font);
    const double radius = compassRadius(scale);
    const double margin = 12.0 * scale;
    const double line = QFontMetricsF(font).height() * 1.1;
    const QPointF centre(0.5 * width(), height() - margin - line - 4.0 - radius);
    paintCompass(painter, centre, radius, sim.hud().compass,
                 common::normalizeDegrees(common::radToDeg(camera.yawRad)));
}

void Viewport::captureMouse() {
    if (captured_) {
        return;
    }
    captured_ = true;
    setMouseTracking(true);
    setCursor(Qt::BlankCursor);
    grabMouse();
    grabKeyboard();
    lastMouse_ = mapFromGlobal(QCursor::pos());
    emit mouseCaptureChanged(true);
}

void Viewport::releaseMouse() {
    if (!captured_) {
        return;
    }
    captured_ = false;
    QWidget::releaseMouse();
    releaseKeyboard();
    setMouseTracking(false);
    unsetCursor();
    keys_ = {};
    emit mouseCaptureChanged(false);
}

void Viewport::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const SimulationController* const sim = simulation_.data();
    const bool vehicleView = sim != nullptr && sim->vehicleView();
    if (event->button() == Qt::LeftButton && scene_ != nullptr && !vehicleView) {
        captureMouse();
    }
    QOpenGLWidget::mousePressEvent(event);
}

void Viewport::mouseMoveEvent(QMouseEvent* event) {
    if (!captured_) {
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    const QPoint delta = pos - lastMouse_;
    yawDelta_ += delta.x() * kMouseRadiansPerPixel;
    pitchDelta_ -= delta.y() * kMouseRadiansPerPixel;
    lastMouse_ = pos;
    // Re-centre near the edges so the pointer never stops at the screen border (platforms
    // that forbid warping, such as Wayland, simply keep the relative motion).
    const QRect inner = rect().adjusted(width() / 4, height() / 4, -width() / 4, -height() / 4);
    if (!inner.contains(pos)) {
        const QPoint centre = rect().center();
        QCursor::setPos(mapToGlobal(centre));
        lastMouse_ = centre;
    }
}

void Viewport::wheelEvent(QWheelEvent* event) {
    if (scene_ != nullptr) {
        const double steps = event->angleDelta().y() / 120.0;
        auto& freeFly = scene_->freeFly();
        freeFly.setBaseSpeed(std::clamp(freeFly.baseSpeed() * std::pow(1.25, steps), 1.0, 2000.0));
    }
    event->accept();
}

void Viewport::focusOutEvent(QFocusEvent* event) {
    keys_ = {};
    if (SimulationController* const sim = simulation_.data()) {
        flightKeys_ = 0;
        sim->setFlightKeys(0);
    }
    QOpenGLWidget::focusOutEvent(event);
}

}  // namespace ds::app
