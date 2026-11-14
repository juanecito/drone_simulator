// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>

#include "ds/graphics/camera.hpp"

#include <glm/glm.hpp>

/// @file
/// @brief Cascaded shadow maps for the Sun or the Moon (FR-GFX-10): cascade split distances
/// and stable light matrices.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Number of cascades.
inline constexpr int kShadowCascades = 3;

/// @brief One cascade.
struct ShadowCascade {
    /// Camera-relative position → light clip space: x, y in [−1, 1], depth in [0, 1] (0 = the
    /// side facing the light). Casters closer to the light are clamped to 0 (depth clamp).
    glm::mat4 matrix{1.0F};
    float farM = 0.0F;         ///< View distance where the cascade ends.
    float radiusM = 0.0F;      ///< Half size of the square it covers.
    float texelM = 0.0F;       ///< Size of one shadow-map texel.
    float depthRangeM = 0.0F;  ///< Metres covered by the depth range.
};

/// @brief Cascade setup for one frame.
struct ShadowSetup {
    std::array<ShadowCascade, kShadowCascades> cascades{};  ///< Near to far.
    glm::vec3 lightDirection{0.0F, 1.0F, 0.0F};             ///< Towards the light.
    bool enabled = false;  ///< False when no light is high enough to cast shadows.
};

/// @brief View distances where the cascades end (default 25, 120, 600 m).
inline constexpr std::array<double, kShadowCascades> kDefaultShadowSplitsM{25.0, 120.0, 600.0};

/// @brief Computes the cascades for @p camera and a light in direction @p towardsLight
/// (world, unit). Matrices take camera-relative positions; each cascade is a bounding square
/// of its slice of the view frustum, snapped to whole texels in world space so shadows do not
/// shimmer as the camera moves.
/// @param camera View camera (its position is the origin of the camera-relative space).
/// @param towardsLight Direction towards the light, world frame (normalised here).
/// @param resolution Shadow map size in texels.
/// @param splitsM View distances where the cascades end, near to far.
/// @param depthExtraM Extra depth behind the slice (away from the light) for receivers.
[[nodiscard]] ShadowSetup computeShadowCascades(
    const Camera& camera, const glm::dvec3& towardsLight, int resolution,
    const std::array<double, kShadowCascades>& splitsM = kDefaultShadowSplitsM,
    double depthExtraM = 50.0);

}  // namespace ds::graphics
