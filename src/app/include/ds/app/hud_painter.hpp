// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QPointF>
#include <QRect>

#include <optional>

#include "ds/graphics/camera.hpp"
#include "ds/sim/hud.hpp"

class QPainter;

/// @file
/// @brief Paints the HUD (OSD, position panel and compass) over the 3D view (FR-GFX-14,
/// FR-GFX-14a, FR-GFX-14b).
/// @ingroup app

namespace ds::app {

/// @brief What to paint.
struct HudPaintOptions {
    bool horizon = false;  ///< Artificial horizon and pitch ladder (FPV view).
    bool blinkOn = true;   ///< Phase of flashing warnings.
    double scale = 1.0;    ///< Text scale (device-independent pixels per point).
    /// Camera of the frame, to place the guidance markers (none drawn if null).
    const graphics::Camera* camera = nullptr;
};

/// @brief Paints @p hud into @p area with @p painter (antialiased text with a dark outline so
/// it stays readable over sky and snow).
void paintHud(QPainter& painter, const QRect& area, const sim::HudModel& hud,
              const HudPaintOptions& options);

/// @brief Paints the north-up compass rose (FR-GFX-14b) centred at @p centre.
/// @details True (geographic) north is at the top with the cardinal points N, E, S, W; a blue
/// "MN" marker outside the ring shows magnetic north, turned by the declination; a yellow
/// arrow shows the vehicle's heading, and, if given, a dashed line the camera's view heading
/// (free camera). The declination text is drawn under the rose.
/// @param painter Painter (its font sets the text size).
/// @param centre Centre of the rose.
/// @param radius Radius of the ring in pixels.
/// @param compass Headings and declination.
/// @param viewHeadingDeg Camera heading from true north, or empty.
void paintCompass(QPainter& painter, QPointF centre, double radius, const sim::HudCompass& compass,
                  std::optional<double> viewHeadingDeg = std::nullopt);

/// @brief Screen position of the world point @p world seen by @p camera in @p area, or nullopt
/// if it is behind the camera.
[[nodiscard]] std::optional<QPointF> projectToScreen(const graphics::Camera& camera,
                                                     const QRectF& area, const glm::dvec3& world);

/// @brief Radius of the compass ring for a HUD text @p scale.
[[nodiscard]] constexpr double compassRadius(double scale) noexcept { return 52.0 * scale; }

}  // namespace ds::app
