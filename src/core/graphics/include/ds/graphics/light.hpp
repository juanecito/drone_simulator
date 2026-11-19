// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

#include <glm/glm.hpp>

/// @file
/// @brief Local lights that illuminate the scene: vehicle headlights and spotlights
/// (FR-GFX-08d). Photometric units: luminous intensity in candela, illuminance in lux.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Most spot lights drawn in one frame (extra lights are ignored).
inline constexpr std::size_t kMaxSpotLights = 8;

/// @brief A spot light: a point source whose intensity falls off outside a cone.
/// @details Illuminance at distance d on a surface facing the light is `I · cone / d²`, where
/// the cone factor is 1 inside @ref innerAngleRad, 0 outside @ref outerAngleRad and smooth in
/// between; a window brings it smoothly to 0 at @ref rangeM so distant surfaces cost nothing.
/// No shadows yet.
struct SpotLight {
    glm::dvec3 position{0.0};              ///< World position.
    glm::dvec3 direction{0.0, 0.0, -1.0};  ///< Unit beam axis, world.
    glm::vec3 color{1.0F};                 ///< Linear colour with luminance 1.
    double intensityCd = 0.0;              ///< Luminous intensity on the axis, candela.
    double innerAngleRad = 0.17;           ///< Half-angle of the full-intensity core.
    double outerAngleRad = 0.44;           ///< Half-angle where the beam ends.
    double rangeM = 60.0;                  ///< Distance where the light is cut off.
};

/// @brief Illuminance (lux) the light gives at @p point on a surface with unit normal
/// @p normal: the CPU mirror of `spotIrradiance()` in `lighting.glsl` (tests, tools).
[[nodiscard]] double spotIlluminance(const SpotLight& light, const glm::dvec3& point,
                                     const glm::dvec3& normal) noexcept;

/// @brief Warm white of LED headlights (≈ 5000 K), luminance 1.
[[nodiscard]] glm::vec3 headlightColor() noexcept;

}  // namespace ds::graphics
