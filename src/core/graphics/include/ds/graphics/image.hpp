// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "ds/common/error.hpp"

/// @file
/// @brief Minimal image container and PGM reader for textures shipped as assets.
/// @ingroup graphics

namespace ds::graphics {

/// @brief 8-bit single-channel image, row-major from the top-left corner.
struct GrayImage {
    int width = 0;                     ///< Columns.
    int height = 0;                    ///< Rows.
    std::vector<std::uint8_t> pixels;  ///< width × height values.
};

/// @brief Reads a binary PGM (P5, maxval ≤ 255).
[[nodiscard]] common::Result<GrayImage> loadPgm(const std::filesystem::path& path);

}  // namespace ds::graphics
