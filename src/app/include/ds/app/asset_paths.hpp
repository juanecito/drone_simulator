// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

/// @file
/// @brief Location of the read-only data files (shaders, sky data).
/// @ingroup app

namespace ds::app {

/// @brief Directory holding `shaders/` and `sky/`.
/// @details In order: `$DS_ASSETS_DIR`; `<executable>/../share` (installed layout,
/// `/opt/drone_simulator/share`); the source tree's `assets/` (development builds).
[[nodiscard]] std::filesystem::path findAssetDirectory();

}  // namespace ds::app
