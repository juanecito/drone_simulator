// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>

/// @file
/// @brief Seeded 2D simplex noise with analytic derivatives (FR-TER-P01).
/// @ingroup terrain

namespace ds::terrain {

/// @brief Value and gradient of a noise function.
struct NoiseSample {
    double value = 0.0;  ///< Noise value, about [−1, 1].
    double dx = 0.0;     ///< ∂value/∂x.
    double dy = 0.0;     ///< ∂value/∂y.
};

/// @brief 2D simplex noise (Perlin 2001; formulation after Gustavson, 2005) with a
/// permutation table derived from a 64-bit seed.
/// @details Implemented from the published algorithm. Deterministic: the same seed gives the
/// same values in every run (FR-TER-03). Thread-safe (immutable after construction).
class SimplexNoise {
public:
    /// @brief Builds the permutation table from @p seed.
    explicit SimplexNoise(std::uint64_t seed) noexcept;

    /// @brief Noise value at (@p x, @p y), about [−1, 1].
    [[nodiscard]] double value(double x, double y) const noexcept;

    /// @brief Noise value and analytic gradient at (@p x, @p y).
    [[nodiscard]] NoiseSample sample(double x, double y) const noexcept;

private:
    std::array<std::uint8_t, 512> perm_{};
};

}  // namespace ds::terrain
