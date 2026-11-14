// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

#include "ds/graphics/renderer.hpp"

/// @file
/// @brief 3D viewport widget hosting the OpenGL renderer (FR-UI-01, FR-GFX-01, FR-UI-05).
/// @ingroup app

namespace ds::app {

class SceneController;
class SimulationController;

/// @brief Central widget of the main window: owns the OpenGL 4.5 context and the Renderer,
/// renders the scene continuously, drives the free-fly camera and routes the flight keys.
/// @details The context format comes from `QSurfaceFormat::defaultFormat()`, which main()
/// sets to 4.5 core before `QApplication` is created.
///
/// Free camera: click to capture the mouse (look around), `Esc` to release it;
/// `W`/`S`/`A`/`D` (or arrows) move, `E`/`Q` (or Page Up/Down) climb and descend, `Shift` is
/// 10× faster, the mouse wheel changes the speed.
///
/// Vehicle cameras (FR-IN-05): `W`/`S` throttle, `A`/`D` yaw, arrows pitch and roll, `Space`
/// arms or disarms, `1`–`4` select the flight mode, `L` the lights. Always: `C` next camera,
/// `P` pause or resume, `R` reset the vehicle.
class Viewport final : public QOpenGLWidget {
    Q_OBJECT

public:
    /// @brief Creates the viewport. Without @p scene it only clears the frame; without
    /// @p simulation there is no vehicle (free camera only).
    explicit Viewport(SceneController* scene = nullptr, SimulationController* simulation = nullptr,
                      QWidget* parent = nullptr);
    ~Viewport() override;

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;
    Viewport(Viewport&&) = delete;
    Viewport& operator=(Viewport&&) = delete;

    /// @brief Context information, available after glReady() was emitted.
    [[nodiscard]] const std::optional<graphics::GlInfo>& glInfo() const noexcept { return glInfo_; }
    /// @brief True while the mouse is captured for camera control.
    [[nodiscard]] bool mouseCaptured() const noexcept { return captured_; }
    /// @brief Frames per second, averaged over the last quarter second.
    [[nodiscard]] double framesPerSecond() const noexcept { return fps_; }
    /// @brief Counters of the last frame.
    [[nodiscard]] const graphics::FrameStats& frameStats() const noexcept {
        return renderer_->lastStats();
    }
    /// @brief Last auto-exposure read-out (refreshed twice a second).
    [[nodiscard]] const graphics::ExposureReadout& exposure() const noexcept { return exposure_; }
    /// @brief The camera of the last frame (free or vehicle camera).
    [[nodiscard]] const graphics::Camera& lastCamera() const noexcept { return lastCamera_; }
    /// @brief Changes the quality settings.
    void setRenderSettings(const graphics::RenderSettings& settings);

    /// @brief Releases the mouse (also bound to `Esc`).
    void releaseMouse();

signals:
    /// @brief The context was created and validated.
    void glReady(const QString& summary);
    /// @brief The context could not be used; @p message is meant for the user.
    void glFailed(const QString& message);
    /// @brief Emitted about four times a second with fresh frame statistics.
    void statsUpdated();
    /// @brief Mouse capture started or ended.
    void mouseCaptureChanged(bool captured);

protected:
    /// @brief Loads OpenGL, validates the context and loads the shaders.
    void initializeGL() override;
    /// @brief Advances the scene and renders one frame at device-pixel resolution.
    void paintGL() override;
    /// @brief Movement keys and `Esc` (releases the mouse).
    void keyPressEvent(QKeyEvent* event) override;
    /// @brief Movement keys.
    void keyReleaseEvent(QKeyEvent* event) override;
    /// @brief A left click captures the mouse (FR-UI-05).
    void mousePressEvent(QMouseEvent* event) override;
    /// @brief Mouse look while captured.
    void mouseMoveEvent(QMouseEvent* event) override;
    /// @brief Changes the free-fly speed.
    void wheelEvent(QWheelEvent* event) override;
    /// @brief Stops all movement when the view loses focus.
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct Keys {
        bool forward = false, back = false, left = false, right = false;
        bool up = false, down = false, boost = false;
    };
    bool setKey(int key, bool pressed);
    bool flightKey(SimulationController& sim, int key, bool pressed);
    bool commandKey(SimulationController& sim, int key);
    void captureMouse();
    void renderScene(const graphics::FrameTarget& target);
    void paintHud(const SimulationController& sim);
    void paintFreeCompass(const SimulationController& sim, const graphics::Camera& camera);

    SceneController* scene_;
    QPointer<SimulationController> simulation_;  // Cleared if it is deleted first.
    std::uint32_t flightKeys_ = 0;
    graphics::Camera lastCamera_;
    std::unique_ptr<graphics::Renderer> renderer_;
    std::optional<graphics::GlInfo> glInfo_;
    Keys keys_;
    bool captured_ = false;
    QPoint lastMouse_;
    double yawDelta_ = 0.0;
    double pitchDelta_ = 0.0;
    QElapsedTimer clock_;
    QElapsedTimer statsClock_;
    QElapsedTimer exposureClock_;
    QElapsedTimer reloadClock_;
    double animationTime_ = 0.0;
    int framesSinceStats_ = 0;
    double fps_ = 0.0;
    graphics::ExposureReadout exposure_;
};

}  // namespace ds::app
