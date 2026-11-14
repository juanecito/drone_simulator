// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/astro/nutation.hpp"

#include <cmath>

#include "ds/astro/time.hpp"
#include "ds/common/math.hpp"

namespace ds::astro {

Nutation nutation(double jde) noexcept {
    using common::degToRad;
    const double t = (jde - kJ2000) / 36525.0;
    const double sunMeanLon = degToRad(280.4665 + 36000.7698 * t);
    const double moonMeanLon = degToRad(218.3165 + 481267.8813 * t);
    const double node =
        degToRad(125.04452 - 1934.136261 * t + 0.0020708 * t * t + t * t * t / 450000.0);
    const double dPsiArcsec = -17.20 * std::sin(node) - 1.32 * std::sin(2.0 * sunMeanLon) -
                              0.23 * std::sin(2.0 * moonMeanLon) + 0.21 * std::sin(2.0 * node);
    const double dEpsArcsec = 9.20 * std::cos(node) + 0.57 * std::cos(2.0 * sunMeanLon) +
                              0.10 * std::cos(2.0 * moonMeanLon) - 0.09 * std::cos(2.0 * node);
    const double eps0Arcsec =
        84381.448 - 46.8150 * t - 0.00059 * t * t + 0.001813 * t * t * t;  // 23°26'21.448"
    return {dPsiArcsec / 3600.0, dEpsArcsec / 3600.0, eps0Arcsec / 3600.0};
}

}  // namespace ds::astro
