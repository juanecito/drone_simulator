// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <expected>

#include "ds/common/error.hpp"

/// @file
/// @brief Start-up check that an OpenGL 4.5 core context can be created (FR-GFX-01).
/// @ingroup app

namespace ds::app {

/// @brief Creates a throw-away context with `QSurfaceFormat::defaultFormat()` on an offscreen
/// surface and checks, by querying the context itself, that it is OpenGL 4.5 (or newer) core.
/// @details Needed because `QOpenGLWidget` never calls `initializeGL()` when context creation
/// fails, so the viewport alone cannot report that case. Requires a `QGuiApplication`.
/// @return Nothing on success, or an error whose message is meant for the user.
[[nodiscard]] std::expected<void, common::Error> checkOpenGlContext();

}  // namespace ds::app
