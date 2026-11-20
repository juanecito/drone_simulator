// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ds/terrain/chunk.hpp"
#include "ds/weather/weather.hpp"

/// @file
/// @brief Terrain surface materials and their effect on tyres (FR-GND-04): peak friction and
/// rolling resistance per material.
/// @ingroup physics

namespace ds::physics {

/// @brief Surface material under a wheel.
enum class SurfaceMaterial {
    Paved,  ///< Asphalt, concrete or bare rock.
    Grass,  ///< Short grass on firm soil.
    Dirt,   ///< Dry dirt or gravel track.
    Sand,   ///< Loose dry sand.
    Snow,   ///< Packed snow.
    Mud,    ///< Wet, soft soil.
};

/// @brief Tyre–surface properties for a rubber RC tyre of reference grip.
struct SurfaceProperties {
    double peakFriction = 0.6;        ///< Peak friction coefficient μ (Magic Formula D / N).
    double rollingResistance = 0.05;  ///< Rolling-resistance coefficient C_rr (force / load).
};

/// @brief Properties of @p material.
/// @details Typical published values for pneumatic tyres (dry asphalt μ ≈ 0.9, gravel ≈ 0.6,
/// grass ≈ 0.5, sand ≈ 0.5, packed snow ≈ 0.3, mud ≈ 0.35), with rolling resistance raised for
/// small wheels sinking into soft ground (sand, mud). The profile's tyre scales both.
[[nodiscard]] SurfaceProperties surfaceProperties(SurfaceMaterial material) noexcept;

/// @brief Properties of @p material under the weather's @p cover (FR-WX-07).
/// @details Wetness blends each material towards its wet values: wet asphalt μ 0.6, wet grass
/// 0.35, dirt turning to mud (μ 0.4, C_rr 0.10), wet sand firmer (μ 0.55, C_rr 0.10), slush
/// on snow 0.2, wetter mud 0.25. Snow lying on the ground turns any material into snow from
/// 2 cm, and loose snow adds rolling resistance (2 × depth, at most 0.25).
[[nodiscard]] SurfaceProperties surfaceProperties(SurfaceMaterial material,
                                                  const weather::GroundCover& cover) noexcept;
/// @brief Name of the surface as the driver sees it: "snow" under 2 cm of snow or more,
/// "wet grass" from wetness 0.3, else the material's name.
[[nodiscard]] std::string surfaceDescription(SurfaceMaterial material,
                                             const weather::GroundCover& cover);
/// @brief Name used in files and the GUI ("paved", "grass", …).
[[nodiscard]] std::string_view surfaceMaterialName(SurfaceMaterial material) noexcept;

/// @brief Material for @p name, or nullopt.
[[nodiscard]] std::optional<SurfaceMaterial> parseSurfaceMaterial(std::string_view name) noexcept;

/// @brief Tyre surface of a terrain material (rock counts as paved ground).
[[nodiscard]] SurfaceMaterial surfaceFromTerrain(terrain::Material material) noexcept;

/// @brief Every material, in declaration order.
[[nodiscard]] std::span<const SurfaceMaterial> allSurfaceMaterials() noexcept;

}  // namespace ds::physics
