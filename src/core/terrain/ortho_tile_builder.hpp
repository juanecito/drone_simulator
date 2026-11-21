// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Private header: the steps shared by every orthophoto source to turn sampled texels
/// into a GPU-ready OrthoTile (soft edges, colour adjustments, premultiplied alpha, mipmaps).
/// @ingroup terrain

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "ds/terrain/orthophoto.hpp"

namespace ds::terrain::detail {

/// @brief Exact coordinate transformations per tile side (bilinear in between).
inline constexpr int kLattice = 17;
/// @brief Alpha ramps up over this many texels inside an edge of the imagery (FR-TER-I04).
inline constexpr int kFeatherPx = 2;

/// @brief Lookup table of the colour adjustments @p color.
[[nodiscard]] std::array<std::uint8_t, 256> colourTable(const OrthoColor& color);

/// @brief The tile @p key of @p n texels per side from texels sampled on the grid extended by
/// kFeatherPx on every side ((n + 2·kFeatherPx)² values, row by row): @p alpha is 1 where
/// there is imagery and 0 elsewhere, @p colour the straight sRGB colour (0 … 255). @p lut, if
/// not null, adjusts the colours. Returns nullptr if no texel is visible.
[[nodiscard]] std::shared_ptr<OrthoTile> finishOrthoTile(
    const OrthoTileKey& key, int n, std::span<const float> alpha,
    std::span<const std::array<float, 3>> colour, const std::array<std::uint8_t, 256>* lut);

}  // namespace ds::terrain::detail
