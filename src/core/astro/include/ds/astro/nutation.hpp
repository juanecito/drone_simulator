// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

/// @file
/// @brief Nutation and obliquity of the ecliptic (Meeus ch. 22).
/// @ingroup astro

namespace ds::astro {

/// @brief Nutation and obliquity at an instant.
struct Nutation {
    double longitudeDeg = 0.0;      ///< Δψ, nutation in longitude.
    double obliquityDeg = 0.0;      ///< Δε, nutation in obliquity.
    double meanObliquityDeg = 0.0;  ///< ε0, mean obliquity of the ecliptic.

    /// @brief True obliquity ε = ε0 + Δε.
    [[nodiscard]] double trueObliquityDeg() const noexcept {
        return meanObliquityDeg + obliquityDeg;
    }
};

/// @brief Nutation for a Julian Ephemeris Day: Meeus' abridged series (accuracy 0.5" in Δψ,
/// 0.1" in Δε) and the IAU 1980 mean obliquity (Meeus eq. 22.2).
[[nodiscard]] Nutation nutation(double julianEphemerisDay) noexcept;

}  // namespace ds::astro
