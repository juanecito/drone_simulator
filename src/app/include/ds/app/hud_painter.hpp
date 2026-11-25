// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QFont>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>

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
    /// Height kept free in the bottom-left corner (the chase top view): the left
    /// vehicle-state column is drawn above it.
    double bottomReservedLeft = 0.0;
    /// Height kept free in the bottom-right corner (the chase behind view).
    double bottomReservedRight = 0.0;
};

/// @brief Paints @p hud into @p area with @p painter: green text in hudFont() and green
/// symbols, each with a dark outline so they stay readable over sky and snow; amber and red
/// mark cautions and warnings. The position and mission panels sit on plates that enclose
/// every glyph.
void paintHud(QPainter& painter, const QRect& area, const sim::HudModel& hud,
              const HudPaintOptions& options);

/// @brief The HUD font at @p pointSize: from `fonts/` in the asset directory, the bundled
/// cockpit-display font B612 Regular, otherwise the 14-segment display font DSEG14 Classic
/// Regular, otherwise the system monospace font.
/// @details Loaded into the application on first use; call from the GUI thread.
[[nodiscard]] QFont hudFont(double pointSize);

/// @brief Distance between two HUD text lines in @p font.
[[nodiscard]] double hudLineHeight(const QFont& font);

/// @brief Paints the frame and the @p label of a picture-in-picture inset at @p area.
void paintInsetFrame(QPainter& painter, const QRectF& area, const QString& label, double scale);

/// @brief Paints an outlined heading chevron centred at @p at (marks the vehicle on a map
/// view such as the top inset); @p headingDeg is clockwise from the top of the image.
void paintMapMarker(QPainter& painter, QPointF at, double headingDeg, double scale);

/// @brief Paints the north-up compass rose (FR-GFX-14b) centred at @p centre.
/// @details True (geographic) north is at the top with the cardinal points N, E, S, W; a pale
/// "MN" marker outside the ring shows magnetic north, turned by the declination; a green
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
