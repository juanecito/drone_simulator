// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/exposure.hpp"

#include <algorithm>
#include <cmath>

namespace ds::graphics {

double ev100FromLuminance(double luminance) noexcept {
    return std::log2(std::max(luminance, 1e-9) * 100.0 / 12.5);
}

double exposureFromEv100(double ev100) noexcept { return 1.0 / (1.2 * std::exp2(ev100)); }

double nightBiasEv(double ev100) noexcept { return std::clamp((8.0 - ev100) * 0.35, 0.0, 6.0); }

double autoExposureEv100(double luminance, double compensationEv) noexcept {
    const double ev = ev100FromLuminance(luminance);
    return std::clamp(ev + nightBiasEv(ev) + compensationEv, kMinEv100, kMaxEv100);
}

double adaptEv100(double current, double target, double dtSeconds) noexcept {
    const double tau = target < current ? 0.6 : 2.5;  // Lower EV = brighter image = eye opens.
    return current + (target - current) * (1.0 - std::exp(-dtSeconds / tau));
}

}  // namespace ds::graphics
