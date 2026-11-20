// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/powertrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include "ds/common/math.hpp"

namespace ds::physics {
namespace {

struct CurvePoint {
    double x;
    double y;
};

double interpolate(std::span<const CurvePoint> curve, double x) noexcept {
    if (x <= curve.front().x) {
        return curve.front().y;
    }
    if (x >= curve.back().x) {
        return curve.back().y;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (x <= curve[i].x) {
            const double t = (x - curve[i - 1].x) / (curve[i].x - curve[i - 1].x);
            return curve[i - 1].y + t * (curve[i].y - curve[i - 1].y);
        }
    }
    return curve.back().y;
}

// Typical open-circuit voltage curves at 25 °C (rested cells), per cell.
constexpr std::array<CurvePoint, 12> kLiPoOcv{{{0.00, 3.27},
                                               {0.05, 3.61},
                                               {0.10, 3.69},
                                               {0.20, 3.73},
                                               {0.30, 3.77},
                                               {0.40, 3.79},
                                               {0.50, 3.82},
                                               {0.60, 3.87},
                                               {0.70, 3.92},
                                               {0.80, 3.97},
                                               {0.90, 4.07},
                                               {1.00, 4.20}}};
constexpr std::array<CurvePoint, 12> kLiIonOcv{{{0.00, 3.00},
                                                {0.05, 3.30},
                                                {0.10, 3.45},
                                                {0.20, 3.55},
                                                {0.30, 3.62},
                                                {0.40, 3.68},
                                                {0.50, 3.74},
                                                {0.60, 3.82},
                                                {0.70, 3.90},
                                                {0.80, 3.98},
                                                {0.90, 4.07},
                                                {1.00, 4.18}}};
// Usable capacity vs. temperature (fraction of the 25 °C rating).
constexpr std::array<CurvePoint, 7> kLiPoCapacity{{{-20.0, 0.60},
                                                   {-10.0, 0.75},
                                                   {0.0, 0.85},
                                                   {10.0, 0.94},
                                                   {25.0, 1.00},
                                                   {40.0, 1.02},
                                                   {60.0, 1.02}}};
constexpr std::array<CurvePoint, 7> kLiIonCapacity{{{-20.0, 0.65},
                                                    {-10.0, 0.78},
                                                    {0.0, 0.88},
                                                    {10.0, 0.95},
                                                    {25.0, 1.00},
                                                    {40.0, 1.01},
                                                    {60.0, 1.01}}};

constexpr double kKelvin = 273.15;
constexpr double kArrheniusK = 2257.0;      // R(0 °C) ≈ 2·R(25 °C).
constexpr double kPolarisationShare = 0.5;  // RC-branch resistance relative to R0 at 25 °C.
// The RC branch stands for charge transfer and diffusion, which slow down much more in the
// cold than the ohmic resistance: Ea/R ≈ 6000 K for LiPo pouch cells, ≈ 5000 K for
// cylindrical Li-ion cells (whose ohmic resistance is already higher).
constexpr double kLiPoPolarisationK = 6000.0;
constexpr double kLiIonPolarisationK = 5000.0;
constexpr double kPolarisationTauS = 15.0;
constexpr double kWarningHoldS = 1.0;
// The ESCs cut off only after the loaded voltage stays below the cut-off for this long, so a
// spin-up current spike does not empty the pack.
constexpr double kCutoffHoldS = 0.5;

}  // namespace

double openCircuitCellVoltage(Chemistry chemistry, double soc) noexcept {
    return chemistry == Chemistry::LiPo ? interpolate(kLiPoOcv, soc) : interpolate(kLiIonOcv, soc);
}

double capacityFactor(Chemistry chemistry, double temperatureC) noexcept {
    return chemistry == Chemistry::LiPo ? interpolate(kLiPoCapacity, temperatureC)
                                        : interpolate(kLiIonCapacity, temperatureC);
}

double resistanceFactor(double temperatureC) noexcept {
    const double t = std::clamp(temperatureC, -40.0, 80.0) + kKelvin;
    return std::exp(kArrheniusK * (1.0 / t - 1.0 / (25.0 + kKelvin)));
}

double polarisationFactor(Chemistry chemistry, double temperatureC) noexcept {
    const double t = std::clamp(temperatureC, -40.0, 80.0) + kKelvin;
    const double k = chemistry == Chemistry::LiPo ? kLiPoPolarisationK : kLiIonPolarisationK;
    return std::exp(k * (1.0 / t - 1.0 / (25.0 + kKelvin)));
}

const char* chemistryName(Chemistry chemistry) noexcept {
    return chemistry == Chemistry::LiPo ? "lipo" : "liion";
}

Battery::Battery(const BatteryParams& params, double temperatureC) : params_(params) {
    reset(temperatureC);
}

void Battery::reset(double temperatureC, double soc) {
    temperatureC_ = temperatureC;
    consumedAh_ = (1.0 - std::clamp(soc, 0.0, 1.0)) * params_.capacityAh *
                  capacityFactor(params_.chemistry, temperatureC);
    consumedWh_ = 0.0;
    polarisationV_ = 0.0;
    current_ = 0.0;
    lowTimer_ = 0.0;
    criticalTimer_ = 0.0;
    cutoffTimer_ = 0.0;
    warning_ = BatteryWarning::None;
    voltage_ = openCircuitVoltage();
}

double Battery::stateOfCharge() const noexcept {
    const double usable = params_.capacityAh * capacityFactor(params_.chemistry, temperatureC_);
    return std::clamp(1.0 - consumedAh_ / usable, 0.0, 1.0);
}

double Battery::packResistance() const noexcept {
    return params_.cellResistanceOhm * params_.cellsInSeries * resistanceFactor(temperatureC_);
}

double Battery::openCircuitVoltage() const noexcept {
    return params_.cellsInSeries * openCircuitCellVoltage(params_.chemistry, stateOfCharge());
}

double Battery::voltageAt(double currentA) const noexcept {
    if (depleted()) {
        return 0.0;
    }
    return std::max(0.0, openCircuitVoltage() - currentA * packResistance() - polarisationV_);
}

void Battery::update(double currentA, double ambientC, double dt) {
    if (depleted()) {
        current_ = 0.0;
        voltage_ = 0.0;
        return;
    }
    current_ = std::max(0.0, currentA);
    const double r0 = packResistance();
    // RC branch: V_rc → I·R1 with time constant τ.
    const double r1 = kPolarisationShare * params_.cellResistanceOhm * params_.cellsInSeries *
                      polarisationFactor(params_.chemistry, temperatureC_);
    polarisationV_ += (current_ * r1 - polarisationV_) * (1.0 - std::exp(-dt / kPolarisationTauS));
    voltage_ = std::max(0.0, openCircuitVoltage() - current_ * r0 - polarisationV_);
    consumedAh_ += current_ * dt / 3600.0;
    consumedWh_ += voltage_ * current_ * dt / 3600.0;
    // Lumped thermal model: Joule heating against convective cooling.
    const double heat = current_ * current_ * (r0 + r1);
    temperatureC_ +=
        dt * (heat - params_.coolingWPerK * (temperatureC_ - ambientC)) / params_.heatCapacityJPerK;

    const double cell = voltage_ / params_.cellsInSeries;
    lowTimer_ = cell < params_.lowCellVoltage ? lowTimer_ + dt : 0.0;
    criticalTimer_ = cell < params_.criticalCellVoltage ? criticalTimer_ + dt : 0.0;
    cutoffTimer_ = cell <= params_.cutoffCellVoltage ? cutoffTimer_ + dt : 0.0;
    if (stateOfCharge() <= 0.0 || cutoffTimer_ >= kCutoffHoldS) {
        warning_ = BatteryWarning::Depleted;
        voltage_ = 0.0;
        current_ = 0.0;
    } else if (criticalTimer_ >= kWarningHoldS || warning_ == BatteryWarning::Critical) {
        warning_ = BatteryWarning::Critical;  // Latched until the pack is replaced.
    } else if (lowTimer_ >= kWarningHoldS || warning_ == BatteryWarning::Low) {
        warning_ = BatteryWarning::Low;
    }
}

PropCoefficients propCoefficients(const PropellerParams& prop, double j) noexcept {
    const double ratio = std::max(0.0, j) / std::max(prop.zeroThrustAdvanceRatio, 0.05);
    PropCoefficients c;
    c.ct = prop.thrustCoefficient * std::max(0.0, 1.0 - ratio);
    c.cq = prop.torqueCoefficient * std::max(0.7, 1.0 - 0.3 * ratio);
    return c;
}

RotorOutput propellerLoads(const PropellerParams& prop, double omegaRad, double rho,
                           double axialSpeed) noexcept {
    RotorOutput out;
    const double n = std::max(0.0, omegaRad) / common::kTwoPi;
    if (n <= 0.0) {
        return out;
    }
    const double d = prop.diameterM;
    const PropCoefficients c = propCoefficients(prop, axialSpeed / (n * d));
    const double d2 = d * d;
    out.thrustN = c.ct * rho * n * n * d2 * d2;
    out.torqueNm = c.cq * rho * n * n * d2 * d2 * d;
    return out;
}

double kvRadPerVolt(const MotorParams& motor) noexcept { return motor.kv * common::kTwoPi / 60.0; }

double steadyStateOmega(const MotorParams& motor, const PropellerParams& prop, double voltage,
                        double rho, double axialSpeed) noexcept {
    const double kv = kvRadPerVolt(motor);
    const double c = motor.resistanceOhm * motor.noLoadCurrentA - voltage;
    if (c >= 0.0) {
        return 0.0;  // Below the voltage needed to overcome the no-load losses.
    }
    // Torque Q = kq·ω² with C_Q evaluated at a first guess of J (two fixed-point passes).
    double omega = voltage * kv;
    for (int pass = 0; pass < 3; ++pass) {
        const double n = std::max(omega, 1.0) / common::kTwoPi;
        const PropCoefficients pc = propCoefficients(prop, axialSpeed / (n * prop.diameterM));
        const double d5 = std::pow(prop.diameterM, 5.0);
        const double kq = pc.cq * rho * d5 / (common::kTwoPi * common::kTwoPi);
        // V = ω/Kv + R·(I0 + Kv·kq·ω²)  ⇒  a·ω² + b·ω + c = 0.
        const double a = motor.resistanceOhm * kq * kv;
        const double b = 1.0 / kv;
        omega = a > 0.0 ? (-b + std::sqrt(b * b - 4.0 * a * c)) / (2.0 * a) : -c / b;
    }
    return std::max(0.0, omega);
}

double motorCurrent(const MotorParams& motor, double omegaRad, double torqueNm,
                    double omegaDot) noexcept {
    if (omegaRad <= 0.0 && omegaDot <= 0.0) {
        return 0.0;
    }
    // Kt = 1/Kv (SI). Regenerative braking is not modelled: the current never goes negative.
    const double kt = 1.0 / kvRadPerVolt(motor);
    return std::max(0.0,
                    motor.noLoadCurrentA + (torqueNm + motor.rotorInertiaKgM2 * omegaDot) / kt);
}

}  // namespace ds::physics
