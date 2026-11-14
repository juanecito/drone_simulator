// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QRect>

#include "ds/sim/hud.hpp"

class QPainter;

/// @file
/// @brief Paints the HUD (OSD and position panel) over the 3D view (FR-GFX-14, FR-GFX-14a).
/// @ingroup app

namespace ds::app {

/// @brief What to paint.
struct HudPaintOptions {
    bool horizon = false;  ///< Artificial horizon and pitch ladder (FPV view).
    bool blinkOn = true;   ///< Phase of flashing warnings.
    double scale = 1.0;    ///< Text scale (device-independent pixels per point).
};

/// @brief Paints @p hud into @p area with @p painter (antialiased text with a dark outline so
/// it stays readable over sky and snow).
void paintHud(QPainter& painter, const QRect& area, const sim::HudModel& hud,
              const HudPaintOptions& options);

}  // namespace ds::app
