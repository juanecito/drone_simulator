// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "ds/terrain/chunk.hpp"

/// @file
/// @brief Distance-based level-of-detail selection over the chunk quadtree (FR-TER-04/06).
/// @ingroup terrain

namespace ds::terrain {

/// @brief LOD selection tuning.
struct SelectionSettings {
    /// A chunk is split into its four children when the camera is closer than
    /// `splitFactor × chunk size` (larger ⇒ more detail).
    double splitFactor = 2.0;
    /// Chunks farther than this from the camera (horizontally) are not selected.
    double viewRadiusM = 40'000.0;
};

/// @brief Height range of a generated chunk, or `std::nullopt` if it is not ready.
using ChunkReadiness = std::function<std::optional<std::pair<float, float>>(const ChunkId&)>;

/// @brief Result of a selection pass.
struct ChunkSelection {
    std::vector<ChunkId> render;  ///< Ready chunks to draw (no overlaps, no gaps among them).
    /// Chunks that should be generated, most urgent first (with their priority: lower is
    /// more urgent).
    std::vector<std::pair<ChunkId, double>> wanted;
};

/// @brief Chooses the chunks to draw for a camera at (@p x, @p y, @p z).
/// @details Starting from the coarsest level, a chunk is replaced by its four children when
/// the camera is close enough and all four are ready; otherwise the chunk itself is drawn and
/// the missing children are reported as wanted. Chunks outside @p extent or beyond the view
/// radius are never selected or requested.
[[nodiscard]] ChunkSelection selectChunks(const ChunkLayout& layout,
                                          const std::optional<Extent>& extent, double x, double y,
                                          double z, const SelectionSettings& settings,
                                          const ChunkReadiness& ready);

}  // namespace ds::terrain
