// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cmath>
#include <numbers>

/// @file
/// @brief Angle constants and helpers shared by the Core modules.
/// @ingroup common

namespace ds::common {

/// @brief π.
inline constexpr double kPi = std::numbers::pi;
/// @brief 2π.
inline constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// @brief Degrees → radians.
[[nodiscard]] constexpr double degToRad(double degrees) noexcept { return degrees * (kPi / 180.0); }

/// @brief Radians → degrees.
[[nodiscard]] constexpr double radToDeg(double radians) noexcept { return radians * (180.0 / kPi); }

/// @brief Wraps an angle in degrees into [0, 360).
[[nodiscard]] inline double normalizeDegrees(double degrees) noexcept {
    double result = std::fmod(degrees, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result >= 360.0 ? 0.0 : result;
}

/// @brief Wraps an angle in degrees into [-180, 180).
[[nodiscard]] inline double wrapDegrees180(double degrees) noexcept {
    return normalizeDegrees(degrees + 180.0) - 180.0;
}

/// @brief Wraps an angle in radians into [0, 2π).
[[nodiscard]] inline double normalizeRadians(double radians) noexcept {
    double result = std::fmod(radians, kTwoPi);
    if (result < 0.0) {
        result += kTwoPi;
    }
    return result >= kTwoPi ? 0.0 : result;
}

/// @brief Wraps an angle in radians into [-π, π).
[[nodiscard]] inline double wrapRadiansPi(double radians) noexcept {
    return normalizeRadians(radians + kPi) - kPi;
}

}  // namespace ds::common
