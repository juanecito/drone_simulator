// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <span>

/// @file
/// @brief Internal: truncated VSOP87D series of the Earth (data generated into
/// vsop87_earth_data.cpp).
/// @ingroup astro

namespace ds::astro::detail {

/// @brief One periodic term A·cos(B + C·τ), τ in Julian millennia of TT from J2000.
struct VsopTerm {
    double a;  ///< Amplitude A (rad or AU).
    double b;  ///< Phase B (rad).
    double c;  ///< Frequency C (rad per millennium).
};

/// @brief Heliocentric longitude L (rad) series, by power of τ (0…5), mean dynamical ecliptic
/// and equinox of date.
extern const std::array<std::span<const VsopTerm>, 6> kEarthL;
/// @brief Heliocentric latitude B (rad) series, by power of τ (0…5).
extern const std::array<std::span<const VsopTerm>, 6> kEarthB;
/// @brief Radius vector R (AU) series, by power of τ (0…5).
extern const std::array<std::span<const VsopTerm>, 6> kEarthR;

}  // namespace ds::astro::detail
