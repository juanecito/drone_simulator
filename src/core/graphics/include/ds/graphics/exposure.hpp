// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <glm/glm.hpp>

/// @file
/// @brief Photographic exposure for HDR rendering (FR-GFX-08c). The GPU auto-exposure in
/// `assets/shaders/exposure.comp` uses the same formulas.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Automatic or manual exposure.
enum class ExposureMode {
    Auto,    ///< Follows the scene luminance with eye-like adaptation.
    Manual,  ///< Fixed EV100.
};

/// @brief Exposure settings (Environment tab).
struct ExposureSettings {
    ExposureMode mode = ExposureMode::Auto;  ///< Mode.
    double manualEv100 = 14.0;               ///< EV100 used in manual mode (≈ sunny day: 15).
    double compensationEv = 0.0;             ///< Added to the automatic EV (positive = darker).
    /// @brief Equality.
    bool operator==(const ExposureSettings&) const = default;
};

/// @brief Lowest automatic EV100 (moonless night).
inline constexpr double kMinEv100 = -12.0;
/// @brief Highest automatic EV100 (snow in full sun).
inline constexpr double kMaxEv100 = 18.0;

/// @brief EV100 that renders average scene luminance @p luminance (cd/m²) as middle grey
/// (reflected-light meter constant K = 12.5).
[[nodiscard]] double ev100FromLuminance(double luminance) noexcept;

/// @brief Scale from scene luminance to the renderer's pre-exposed values (saturation-based
/// sensitivity: 1 / (1.2 · 2^EV100)).
[[nodiscard]] double exposureFromEv100(double ev100) noexcept;

/// @brief Extra EV added in dark scenes so that night stays dark, as the eye perceives it
/// (0 above EV100 8, up to +6 at EV100 −9 and below).
[[nodiscard]] double nightBiasEv(double ev100) noexcept;

/// @brief Target EV100 of the automatic exposure for average luminance @p luminance.
[[nodiscard]] double autoExposureEv100(double luminance, double compensationEv) noexcept;

/// @brief Colour temperature the renderer treats as white, K: midday daylight (sun plus
/// sky), like a camera's "daylight" white balance. Lower temperatures (sunset, headlights)
/// look warm, the shade and twilight look blue.
inline constexpr double kDaylightWhiteK = 6000.0;

/// @brief Chromatic adaptation (Bradford, von Kries in cone space) in linear sRGB that turns
/// the illuminant of linear sRGB colour @p sourceWhite into neutral grey of the same
/// luminance; sRGB's own white (D65) is the target.
[[nodiscard]] glm::mat3 whiteBalanceMatrix(const glm::dvec3& sourceWhite) noexcept;

/// @brief Moves @p current towards @p target over @p dtSeconds (faster when the scene gets
/// brighter, like the eye: ≈ 0.6 s up, 2.5 s down).
[[nodiscard]] double adaptEv100(double current, double target, double dtSeconds) noexcept;

}  // namespace ds::graphics
