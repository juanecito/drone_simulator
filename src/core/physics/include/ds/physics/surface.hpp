// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "ds/terrain/chunk.hpp"

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

/// @brief Name used in files and the GUI ("paved", "grass", …).
[[nodiscard]] std::string_view surfaceMaterialName(SurfaceMaterial material) noexcept;

/// @brief Material for @p name, or nullopt.
[[nodiscard]] std::optional<SurfaceMaterial> parseSurfaceMaterial(std::string_view name) noexcept;

/// @brief Tyre surface of a terrain material (rock counts as paved ground).
[[nodiscard]] SurfaceMaterial surfaceFromTerrain(terrain::Material material) noexcept;

/// @brief Every material, in declaration order.
[[nodiscard]] std::span<const SurfaceMaterial> allSurfaceMaterials() noexcept;

}  // namespace ds::physics
