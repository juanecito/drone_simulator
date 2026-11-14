// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>

/// @file
/// @brief Internal: Meeus tables 47.A and 47.B (data generated into moon_tables_data.cpp).
/// @ingroup astro

namespace ds::astro::detail {

/// @brief Term of the lunar longitude and distance series (Meeus table 47.A).
struct MoonLrTerm {
    int d;     ///< Multiple of the mean elongation D.
    int m;     ///< Multiple of the Sun's mean anomaly M.
    int mp;    ///< Multiple of the Moon's mean anomaly M′.
    int f;     ///< Multiple of the argument of latitude F.
    int sumL;  ///< Coefficient of the sine for longitude, 10⁻⁶ degrees.
    int sumR;  ///< Coefficient of the cosine for distance, 10⁻³ km.
};

/// @brief Term of the lunar latitude series (Meeus table 47.B).
struct MoonBTerm {
    int d;     ///< Multiple of the mean elongation D.
    int m;     ///< Multiple of the Sun's mean anomaly M.
    int mp;    ///< Multiple of the Moon's mean anomaly M′.
    int f;     ///< Multiple of the argument of latitude F.
    int sumB;  ///< Coefficient of the sine for latitude, 10⁻⁶ degrees.
};

/// @brief Table 47.A.
extern const std::array<MoonLrTerm, 60> kMoonLr;
/// @brief Table 47.B.
extern const std::array<MoonBTerm, 60> kMoonB;

}  // namespace ds::astro::detail
