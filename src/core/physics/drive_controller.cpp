// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/drive_controller.hpp"

#include <algorithm>
#include <cmath>

namespace ds::physics {
namespace {

constexpr double kNeutralBand = 0.1;   // Drive stick counts as neutral below this.
constexpr double kStopSpeedMs = 0.05;  // Speed control: brake below this with a zero target.

}  // namespace

std::string_view driveModeName(DriveMode mode) noexcept {
    switch (mode) {
        case DriveMode::Manual: return "Manual";
        case DriveMode::SpeedControl: return "Speed";
    }
    return "?";
}

DriveOutput mixArcade(double throttle, double steering, double steeringGain) noexcept {
    const double t = std::clamp(throttle, -1.0, 1.0);
    const double s = std::clamp(steering, -1.0, 1.0) * steeringGain;
    double left = t + s;
    double right = t - s;
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    left /= scale;
    right /= scale;
    return {.left = left, .right = right, .brake = false};
}

double throttleCurve(double x, double expo) noexcept {
    const double e = std::clamp(expo, 0.0, 1.0);
    return (1.0 - e) * x + e * x * x * x;
}

DriveController::DriveController(const DriveControllerParams& params) : params_(params) { reset(); }

void DriveController::reset() {
    armed_ = false;
    switchWasLow_ = false;
    mode_ = DriveMode::Manual;
    armBlock_ = ArmBlock::None;
    failsafeActive_ = false;
    linkLostTime_ = 0.0;
    integrator_ = 0.0;
    setpoint_ = 0.0;
    armedTime_ = 0.0;
}

void DriveController::forceDisarm() noexcept {
    armed_ = false;
    switchWasLow_ = false;
    integrator_ = 0.0;
}

ArmBlock DriveController::preArmCheck(const PilotCommand& command,
                                      const DriveConditions& conditions) const noexcept {
    if (!command.linkValid) {
        return ArmBlock::LinkLost;
    }
    if (conditions.crashed) {
        return ArmBlock::Crashed;
    }
    if (conditions.rolledOver) {
        return ArmBlock::RolledOver;
    }
    if (conditions.inWater) {
        return ArmBlock::InWater;
    }
    if (conditions.batteryCritical) {
        return ArmBlock::BatteryCritical;
    }
    if (!switchWasLow_) {
        return ArmBlock::SwitchNotCycled;
    }
    if (std::abs(command.drive) > kNeutralBand) {
        return ArmBlock::ThrottleNotCentred;
    }
    return ArmBlock::None;
}

DriveOutput DriveController::update(const PilotCommand& command, const DriveConditions& conditions,
                                    double dt) {
    // Arming: the switch must be seen off before it can arm (no arming at power-on).
    if (!command.arm) {
        if (armed_) {
            forceDisarm();
        }
        switchWasLow_ = true;
        armBlock_ = ArmBlock::None;
    } else if (!armed_) {
        armBlock_ = preArmCheck(command, conditions);
        if (armBlock_ == ArmBlock::None) {
            armed_ = true;
            armedTime_ = 0.0;
            integrator_ = 0.0;
        } else if (armBlock_ != ArmBlock::SwitchNotCycled) {
            switchWasLow_ = false;  // Cycle the switch to try again.
        }
    }
    if (conditions.crashed || conditions.rolledOver || conditions.inWater) {
        forceDisarm();
    }

    // Failsafe: RC link lost for longer than the delay → stop and hold.
    if (!command.linkValid) {
        linkLostTime_ += dt;
        failsafeActive_ = armed_ && linkLostTime_ >= params_.failsafeDelayS;
    } else {
        linkLostTime_ = 0.0;
        failsafeActive_ = false;
    }

    if (mode_ != command.driveMode) {
        mode_ = command.driveMode;
        integrator_ = 0.0;
    }
    setpoint_ = 0.0;
    if (!armed_) {
        return {.left = 0.0, .right = 0.0, .brake = true};
    }
    armedTime_ += dt;
    if (failsafeActive_) {
        integrator_ = 0.0;
        return {.left = 0.0, .right = 0.0, .brake = true};
    }

    const double steering = std::clamp(command.steering, -1.0, 1.0);
    double throttle = 0.0;
    switch (mode_) {
        case DriveMode::Manual:
            throttle = throttleCurve(std::clamp(command.drive, -1.0, 1.0), params_.throttleExpo);
            break;
        case DriveMode::SpeedControl: {
            setpoint_ = std::clamp(command.drive, -1.0, 1.0) * params_.cruiseSpeedMs;
            const double error = setpoint_ - conditions.odometrySpeedMs;
            const double p = params_.speedP * error;
            const double candidate = integrator_ + params_.speedI * error * dt;
            // Integrate only while the output is not saturated in the error's direction.
            if (std::abs(p + candidate) < 1.0 ||
                std::abs(p + candidate) < std::abs(p + integrator_)) {
                integrator_ = std::clamp(candidate, -1.0, 1.0);
            }
            throttle = std::clamp(p + integrator_, -1.0, 1.0);
            if (setpoint_ == 0.0 && std::abs(conditions.odometrySpeedMs) < kStopSpeedMs) {
                integrator_ = 0.0;
                DriveOutput hold = mixArcade(0.0, steering, params_.steeringGain);
                hold.brake = std::abs(steering) < kNeutralBand;
                return hold;
            }
            break;
        }
    }
    DriveOutput out = mixArcade(throttle, steering, params_.steeringGain);
    out.brake = command.brake;
    if (out.brake) {
        out.left = 0.0;
        out.right = 0.0;
        integrator_ = 0.0;
    }
    return out;
}

}  // namespace ds::physics
