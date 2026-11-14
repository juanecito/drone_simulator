// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/fixed_step.hpp"

#include <algorithm>
#include <cmath>

namespace ds::sim {

void FixedStepClock::setRate(double hz) noexcept { rate_ = std::clamp(hz, kMinRateHz, kMaxRateHz); }

void FixedStepClock::setTimeScale(double scale) noexcept {
    timeScale_ = std::clamp(scale, kMinTimeScale, kMaxTimeScale);
}

int FixedStepClock::advance(double realSeconds) noexcept {
    double real = std::max(realSeconds, 0.0);
    if (real > maxCatchUp_) {
        dropped_ += real - maxCatchUp_;  // Spiral-of-death guard.
        real = maxCatchUp_;
    }
    accumulator_ += real * timeScale_;
    const double stepLength = step();
    const auto steps = static_cast<int>(std::floor(accumulator_ / stepLength + 1e-9));
    accumulator_ = std::max(0.0, accumulator_ - steps * stepLength);
    return steps;
}

}  // namespace ds::sim
