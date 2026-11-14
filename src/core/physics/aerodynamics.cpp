// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/aerodynamics.hpp"

#include <algorithm>
#include <cmath>

namespace ds::physics {

Vec3 bodyDrag(const FrameParams& frame, const Vec3& v, double rho) noexcept {
    const double q = 0.5 * rho;
    return {-q * frame.dragAreaForwardM2 * std::abs(v.x) * v.x,
            -q * frame.dragAreaSideM2 * std::abs(v.y) * v.y,
            -q * frame.dragAreaVerticalM2 * std::abs(v.z) * v.z};
}

Vec3 rotorDrag(const PropellerParams& prop, double thrustN, double omegaRad,
               const Vec3& v) noexcept {
    const double tip = std::max(omegaRad, 1.0) * 0.5 * prop.diameterM;
    // In-plane air velocity (rotor plane ⟂ body z).
    const Vec3 inPlane(v.x, v.y, 0.0);
    return -prop.rotorDragCoefficient * thrustN / tip * inPlane;
}

double groundEffectGain(double heightM, double rotorRadiusM) noexcept {
    const double z = std::max(heightM, 0.5 * rotorRadiusM);
    const double r = rotorRadiusM / (4.0 * z);
    return std::min(1.4, 1.0 / (1.0 - r * r));
}

double hoverInducedVelocity(double thrustN, double rho, double diskAreaM2) noexcept {
    return std::sqrt(std::max(thrustN, 0.0) / (2.0 * rho * diskAreaM2));
}

double vortexRingFactor(double descentMs, double inducedMs) noexcept {
    if (inducedMs <= 0.0 || descentMs <= 0.0) {
        return 1.0;
    }
    const double r = descentMs / inducedMs;
    if (r < 0.3 || r > 2.0) {
        return 1.0;
    }
    const double x = (r - 1.0) / 0.4;
    return 1.0 - 0.25 * std::exp(-x * x);
}

}  // namespace ds::physics
