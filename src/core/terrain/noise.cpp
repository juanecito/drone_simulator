// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/noise.hpp"

#include <cmath>
#include <numeric>
#include <utility>

#include "ds/common/random.hpp"

namespace ds::terrain {
namespace {

// Skewing factors for 2D simplex noise: F2 = (√3 − 1)/2, G2 = (3 − √3)/6.
constexpr double kF2 = 0.36602540378443864676;
constexpr double kG2 = 0.21132486540518711775;

// Twelve gradient directions (unit length: x² + y² = 1), evenly spread on the circle.
constexpr std::array<std::array<double, 2>, 12> kGradients{{
    {1.0, 0.0},
    {0.86602540378443865, 0.5},
    {0.5, 0.86602540378443865},
    {0.0, 1.0},
    {-0.5, 0.86602540378443865},
    {-0.86602540378443865, 0.5},
    {-1.0, 0.0},
    {-0.86602540378443865, -0.5},
    {-0.5, -0.86602540378443865},
    {0.0, -1.0},
    {0.5, -0.86602540378443865},
    {0.86602540378443865, -0.5},
}};

// Scale so that the result spans about [−1, 1] (measured maximum ≈ 0.0141 before scaling).
constexpr double kScale = 70.0;

}  // namespace

SimplexNoise::SimplexNoise(std::uint64_t seed) noexcept {
    std::array<std::uint8_t, 256> p{};
    std::iota(p.begin(), p.end(), std::uint8_t{0});
    common::SplitMix64 rng(seed);
    for (std::size_t i = p.size() - 1; i > 0; --i) {  // Fisher–Yates.
        const auto j = static_cast<std::size_t>(rng.below(i + 1));
        std::swap(p[i], p[j]);
    }
    for (std::size_t i = 0; i < perm_.size(); ++i) {
        perm_[i] = p[i & 255U];
    }
}

double SimplexNoise::value(double x, double y) const noexcept { return sample(x, y).value; }

NoiseSample SimplexNoise::sample(double x, double y) const noexcept {
    const double s = (x + y) * kF2;
    const double fi = std::floor(x + s);
    const double fj = std::floor(y + s);
    const double t = (fi + fj) * kG2;
    const double x0 = x - (fi - t);
    const double y0 = y - (fj - t);

    const int i1 = x0 > y0 ? 1 : 0;
    const int j1 = 1 - i1;
    const double x1 = x0 - i1 + kG2;
    const double y1 = y0 - j1 + kG2;
    const double x2 = x0 - 1.0 + 2.0 * kG2;
    const double y2 = y0 - 1.0 + 2.0 * kG2;

    // Lattice coordinates wrapped to the table; exact for |x|, |y| < 2^52.
    const auto ii = static_cast<std::size_t>(static_cast<std::int64_t>(fi) & 255);
    const auto jj = static_cast<std::size_t>(static_cast<std::int64_t>(fj) & 255);
    const std::size_t g0 = perm_[ii + perm_[jj]] % 12U;
    const std::size_t g1 =
        perm_[ii + static_cast<std::size_t>(i1) + perm_[jj + static_cast<std::size_t>(j1)]] % 12U;
    const std::size_t g2 = perm_[ii + 1 + perm_[jj + 1]] % 12U;

    NoiseSample result;
    const auto corner = [&result](double cx, double cy, std::size_t g) {
        const double t0 = 0.5 - cx * cx - cy * cy;
        if (t0 <= 0.0) {
            return;
        }
        const auto& grad = kGradients[g];
        const double dot = grad[0] * cx + grad[1] * cy;
        const double t2 = t0 * t0;
        const double t4 = t2 * t2;
        result.value += t4 * dot;
        // d/dx (t⁴·dot) = −8·t³·x·dot + t⁴·gx.
        result.dx += -8.0 * t2 * t0 * cx * dot + t4 * grad[0];
        result.dy += -8.0 * t2 * t0 * cy * dot + t4 * grad[1];
    };
    corner(x0, y0, g0);
    corner(x1, y1, g1);
    corner(x2, y2, g2);
    result.value *= kScale;
    result.dx *= kScale;
    result.dy *= kScale;
    return result;
}

}  // namespace ds::terrain
