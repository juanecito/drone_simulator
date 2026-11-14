// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @defgroup terrain Terrain
/// @brief Terrain generator: procedural, online DEM, raster / LAS / LAZ files and orthophotos.
/// (SPECIFICATION §4.5).

/// @file
/// @brief Module identification for `ds_terrain`.
/// @ingroup terrain

namespace ds::terrain {

/// @brief Returns the name of this Core module ("terrain").
/// @details Used by the M0 skeleton tests to verify that every module builds and links.
[[nodiscard]] std::string_view moduleName() noexcept;

}  // namespace ds::terrain
