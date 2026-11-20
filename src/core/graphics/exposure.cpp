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

glm::mat3 whiteBalanceMatrix(const glm::dvec3& sourceWhite) noexcept {
    // Column-major: m[column][row].
    const glm::dmat3 xyzFromRgb = glm::transpose(glm::dmat3(0.4124, 0.3576, 0.1805,  //
                                                            0.2126, 0.7152, 0.0722,  //
                                                            0.0193, 0.1192, 0.9505));
    const glm::dmat3 bradford = glm::transpose(glm::dmat3(0.8951, 0.2664, -0.1614,  //
                                                          -0.7502, 1.7135, 0.0367,  //
                                                          0.0389, -0.0685, 1.0296));
    const glm::dvec3 xyzSource = xyzFromRgb * sourceWhite;
    const glm::dvec3 xyzTarget = xyzFromRgb * glm::dvec3(xyzSource.y);  // D65, same Y.
    const glm::dvec3 lmsSource = bradford * xyzSource;
    const glm::dvec3 lmsTarget = bradford * xyzTarget;
    glm::dmat3 scale(1.0);
    for (int i = 0; i < 3; ++i) {
        scale[i][i] = lmsTarget[i] / std::max(lmsSource[i], 1e-12);
    }
    return glm::mat3(glm::inverse(xyzFromRgb) * glm::inverse(bradford) * scale * bradford *
                     xyzFromRgb);
}

double adaptEv100(double current, double target, double dtSeconds) noexcept {
    const double tau = target < current ? 0.6 : 2.5;  // Lower EV = brighter image = eye opens.
    return current + (target - current) * (1.0 - std::exp(-dtSeconds / tau));
}

}  // namespace ds::graphics
