// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cmath>
#include <cstdint>

/// @file
/// @brief Small deterministic random-number utilities (FR-X-05).
/// @ingroup common

namespace ds::common {

/// @brief SplitMix64 generator (Steele, Lea and Flood, 2014): fast, deterministic, 64-bit.
/// @details Used to derive seeds and permutation tables; the same seed gives the same
/// sequence on every platform.
class SplitMix64 {
public:
    /// @brief Starts the sequence from @p seed.
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    /// @brief Next 64-bit value.
    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    /// @brief Uniform integer in [0, @p bound) (@p bound > 0), without modulo bias.
    constexpr std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t threshold = (0 - bound) % bound;
        for (;;) {
            const std::uint64_t r = next();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    /// @brief Uniform double in [0, 1).
    constexpr double uniform() noexcept { return static_cast<double>(next() >> 11U) * 0x1.0p-53; }

private:
    std::uint64_t state_;
};

/// @brief Deterministic normal (Gaussian) random numbers: SplitMix64 + Box–Muller.
/// @details Unlike `std::normal_distribution`, the sequence is the same with every standard
/// library, which the deterministic simulation needs (FR-X-05).
class GaussianRandom {
public:
    /// @brief Starts the sequence from @p seed.
    explicit GaussianRandom(std::uint64_t seed) noexcept : uniform_(seed) {}

    /// @brief Standard normal value (mean 0, standard deviation 1).
    double next() noexcept {
        if (hasSpare_) {
            hasSpare_ = false;
            return spare_;
        }
        const double u1 = 1.0 - uniform_.uniform();  // (0, 1]: log() stays finite.
        const double u2 = uniform_.uniform();
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double a = 6.283185307179586 * u2;
        spare_ = r * std::sin(a);
        hasSpare_ = true;
        return r * std::cos(a);
    }

    /// @brief Normal value with standard deviation @p sigma.
    double next(double sigma) noexcept { return sigma * next(); }

    /// @brief Uniform value in [0, 1).
    double uniform() noexcept { return uniform_.uniform(); }

private:
    SplitMix64 uniform_;
    double spare_ = 0.0;
    bool hasSpare_ = false;
};

/// @brief Mixes @p value into a well-distributed 64-bit hash (SplitMix64 finaliser).
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    return SplitMix64(value).next();
}

/// @brief A fresh, non-deterministic 64-bit seed (for the "Random" buttons, FR-TER-P02).
[[nodiscard]] std::uint64_t randomSeed();

}  // namespace ds::common
