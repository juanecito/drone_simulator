// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/flight_controller.hpp"

#include <algorithm>
#include <cmath>

#include "ds/common/math.hpp"

namespace ds::physics {
namespace {

constexpr double kIntegratorLimit = 0.25;
constexpr double kIdleIntegratorLimit = 0.05;
constexpr double kLowThrottle = 0.05;
constexpr double kMaxArmTiltRad = 0.4363323129985824;  // 25°
constexpr double kAltHoldDeadband = 0.1;
constexpr double kLandedDisarmS = 1.0;

double applyDeadband(double x, double band) noexcept {
    if (std::abs(x) <= band) {
        return 0.0;
    }
    return (x - std::copysign(band, x)) / (1.0 - band);
}

}  // namespace

std::string_view flightModeName(FlightMode mode) noexcept {
    switch (mode) {
        case FlightMode::Acro: return "Acro";
        case FlightMode::Angle: return "Angle";
        case FlightMode::AltitudeHold: return "Alt hold";
    }
    return "?";
}

std::string_view armBlockText(ArmBlock block) noexcept {
    switch (block) {
        case ArmBlock::None: return "";
        case ArmBlock::ThrottleNotLow: return "Throttle not at minimum";
        case ArmBlock::NotLevel: return "Not level";
        case ArmBlock::BatteryCritical: return "Battery critical";
        case ArmBlock::Crashed: return "Crashed — reset the drone";
        case ArmBlock::LinkLost: return "No RC link";
        case ArmBlock::SwitchNotCycled: return "Turn the arm switch off first";
    }
    return "";
}

double rateFromStick(const RateCurve& curve, double x) noexcept {
    const double s = std::clamp(x, -1.0, 1.0);
    const double extra = std::max(0.0, curve.maxDps - curve.centerDps);
    return curve.centerDps * s + extra * s * std::pow(std::abs(s), 1.0 + 4.0 * curve.expo);
}

void Pid::configure(const PidGains& gains, double cutoffHz) noexcept {
    gains_ = gains;
    cutoffHz_ = cutoffHz;
}

void Pid::reset() noexcept {
    integrator_ = 0.0;
    dFiltered_ = 0.0;
    first_ = true;
}

double Pid::update(double setpoint, double measurement, double dt, bool freezeIntegrator) noexcept {
    const double error = setpoint - measurement;
    if (!freezeIntegrator) {
        integrator_ =
            std::clamp(integrator_ + gains_.i * error * dt, -kIntegratorLimit, kIntegratorLimit);
    }
    // Derivative on measurement (no kick on setpoint steps), first-order low-pass.
    const double derivative = first_ ? 0.0 : -(measurement - lastMeasurement_) / dt;
    first_ = false;
    lastMeasurement_ = measurement;
    const double alpha = 1.0 - std::exp(-common::kTwoPi * cutoffHz_ * dt);
    dFiltered_ += alpha * (derivative - dFiltered_);
    return gains_.p * error + integrator_ + gains_.d * dFiltered_;
}

void Pid::limitIntegrator(double limit) noexcept {
    integrator_ = std::clamp(integrator_, -limit, limit);
}

ControllerOutput mixQuadX(const AerialProfile& profile, double collective, const Vec3& torque,
                          bool airmode) noexcept {
    const auto positions = profile.motorPositions();
    const auto spins = AerialProfile::spinDirections();
    double maxX = 0.0;
    double maxY = 0.0;
    for (const Vec3& p : positions) {
        maxX = std::max(maxX, std::abs(p.x));
        maxY = std::max(maxY, std::abs(p.y));
    }
    std::array<double, AerialProfile::kMotorCount> part{};
    double lo = 0.0;
    double hi = 0.0;
    for (std::size_t i = 0; i < part.size(); ++i) {
        // Roll right ⇐ left motors up; pitch up ⇐ front motors up; yaw right ⇐ CCW motors up.
        part[i] = 0.5 * (-positions[i].y / maxY * torque.x + positions[i].x / maxX * torque.y -
                         spins[i] * torque.z);
        lo = std::min(lo, part[i]);
        hi = std::max(hi, part[i]);
    }
    ControllerOutput out;
    const double range = hi - lo;
    if (range > 1.0) {
        for (double& p : part) {
            p /= range;
        }
        lo /= range;
        hi /= range;
        out.saturated = true;
    }
    double c = std::clamp(collective, 0.0, 1.0);
    if (c + hi > 1.0) {
        c = 1.0 - hi;
        out.saturated = true;
    }
    if (airmode && c + lo < 0.0) {
        c = -lo;
    }
    for (std::size_t i = 0; i < part.size(); ++i) {
        const double u = c + part[i];
        if (u < 0.0) {
            out.saturated = true;
        }
        out.motors[i] = std::clamp(u, 0.0, 1.0);
    }
    return out;
}

FlightController::FlightController(const AerialProfile& profile) : profile_(profile) { reset(); }

void FlightController::reset() {
    for (std::size_t i = 0; i < pid_.size(); ++i) {
        pid_[i].configure(profile_.controller.pid[i], profile_.controller.dTermCutoffHz);
        pid_[i].reset();
    }
    armed_ = false;
    switchWasLow_ = false;
    lastArmSwitch_ = false;
    mode_ = FlightMode::Angle;
    armBlock_ = ArmBlock::None;
    failsafeActive_ = false;
    linkLostTime_ = 0.0;
    landedTime_ = 0.0;
    lastValid_ = {};
    rateSetpoint_ = Vec3(0.0);
    climbIntegrator_ = 0.0;
    collective_ = 0.0;
    armedTime_ = 0.0;
    lastSaturated_ = false;
}

void FlightController::forceDisarm() noexcept {
    armed_ = false;
    switchWasLow_ = false;
    failsafeActive_ = false;
}

ArmBlock FlightController::preArmCheck(const PilotCommand& command, const EstimatedState& state,
                                       const VehicleConditions& conditions) const noexcept {
    if (conditions.crashed) {
        return ArmBlock::Crashed;
    }
    if (!command.linkValid) {
        return ArmBlock::LinkLost;
    }
    if (!switchWasLow_) {
        return ArmBlock::SwitchNotCycled;
    }
    // In altitude hold the centred stick means "hold": arming needs it at or below centre.
    const double throttleLimit =
        command.mode == FlightMode::AltitudeHold ? 0.5 + kAltHoldDeadband / 2.0 : kLowThrottle;
    if (command.throttle > throttleLimit) {
        return ArmBlock::ThrottleNotLow;
    }
    if (std::abs(state.euler.roll) > kMaxArmTiltRad ||
        std::abs(state.euler.pitch) > kMaxArmTiltRad) {
        return ArmBlock::NotLevel;
    }
    if (conditions.batteryCritical) {
        return ArmBlock::BatteryCritical;
    }
    return ArmBlock::None;
}

void FlightController::enterMode(FlightMode mode, const EstimatedState& state) {
    if (mode == mode_) {
        return;
    }
    if (mode == FlightMode::AltitudeHold) {
        // Bumpless: hold the current altitude with the current collective as hover estimate.
        altitudeTarget_ = state.altitudeM;
        const double tilt = std::max(0.3, std::cos(state.euler.roll) * std::cos(state.euler.pitch));
        climbIntegrator_ = collective_ * std::sqrt(tilt);
    }
    mode_ = mode;
}

ControllerOutput FlightController::update(const PilotCommand& input, const EstimatedState& state,
                                          const VehicleConditions& conditions, double dt) {
    const ControllerParams& cp = profile_.controller;

    // --- RC link and failsafe. ---
    PilotCommand cmd = input;
    if (cmd.linkValid) {
        linkLostTime_ = 0.0;
        failsafeActive_ = false;
        lastValid_ = cmd;
    } else {
        linkLostTime_ += dt;
        // Stage 1: hold the last throttle with centred sticks; stage 2: land.
        cmd = lastValid_;
        cmd.roll = cmd.pitch = cmd.yaw = 0.0;
        cmd.mode = FlightMode::Angle;
        cmd.linkValid = false;
        if (armed_ && linkLostTime_ >= cp.failsafeDelayS) {
            failsafeActive_ = true;
        }
    }

    // --- Arming. ---
    if (!input.arm) {
        switchWasLow_ = true;
    }
    if (input.linkValid) {
        if (input.arm && !lastArmSwitch_ && !armed_) {
            armBlock_ = preArmCheck(cmd, state, conditions);
            if (armBlock_ == ArmBlock::None) {
                armed_ = true;
                armedTime_ = 0.0;
                altitudeTarget_ = state.altitudeM;
                climbIntegrator_ = 0.0;
                for (Pid& pid : pid_) {
                    pid.reset();
                }
            }
        } else if (!input.arm && armed_) {
            armed_ = false;
            switchWasLow_ = true;
        }
        lastArmSwitch_ = input.arm;
    }
    if (conditions.crashed) {
        forceDisarm();
    }
    if (!armed_) {
        collective_ = 0.0;
        rateSetpoint_ = Vec3(0.0);
        mode_ = cmd.mode;
        return {};
    }
    armedTime_ += dt;

    // --- Failsafe landing: level, descend, disarm on the ground. ---
    double climbSetpoint = 0.0;
    bool verticalSpeedControl = false;
    if (failsafeActive_) {
        enterMode(FlightMode::AltitudeHold, state);
        verticalSpeedControl = true;
        climbSetpoint = -cp.failsafeDescentMs;
        landedTime_ = conditions.landed ? landedTime_ + dt : 0.0;
        if (landedTime_ >= kLandedDisarmS) {
            forceDisarm();
            return {};
        }
    } else {
        enterMode(cmd.mode, state);
    }

    // --- Rate setpoints. ---
    const double maxAngle = common::degToRad(cp.maxAngleDeg);
    Vec3 rates(0.0);
    const double yawRate = common::degToRad(rateFromStick(cp.rates[2], cmd.yaw));
    if (mode_ == FlightMode::Acro) {
        rates = Vec3(common::degToRad(rateFromStick(cp.rates[0], cmd.roll)),
                     -common::degToRad(rateFromStick(cp.rates[1], cmd.pitch)), yawRate);
    } else {
        const double rollTarget = std::clamp(cmd.roll, -1.0, 1.0) * maxAngle;
        const double pitchTarget = -std::clamp(cmd.pitch, -1.0, 1.0) * maxAngle;
        const double maxRoll = common::degToRad(cp.rates[0].maxDps);
        const double maxPitch = common::degToRad(cp.rates[1].maxDps);
        rates =
            Vec3(std::clamp(cp.levelGain * (rollTarget - state.euler.roll), -maxRoll, maxRoll),
                 std::clamp(cp.levelGain * (pitchTarget - state.euler.pitch), -maxPitch, maxPitch),
                 yawRate);
    }
    rateSetpoint_ = rates;

    // --- Collective. ---
    const double idle = cp.idleThrottle;
    if (mode_ == FlightMode::AltitudeHold) {
        if (!verticalSpeedControl) {
            const double stick = applyDeadband(2.0 * cmd.throttle - 1.0, kAltHoldDeadband);
            climbSetpoint = stick >= 0.0 ? stick * cp.maxClimbMs : stick * cp.maxDescentMs;
        }
        if (climbSetpoint != 0.0) {
            altitudeTarget_ = state.altitudeM;  // Follow while climbing or descending.
        } else {
            climbSetpoint = std::clamp(cp.altitudeGain * (altitudeTarget_ - state.altitudeM),
                                       -cp.maxDescentMs, cp.maxClimbMs);
        }
        const double error = climbSetpoint - state.verticalSpeedMs;
        climbIntegrator_ = std::clamp(climbIntegrator_ + cp.climbI * error * dt, 0.0, 1.0);
        const double tilt = std::max(0.5, std::cos(state.euler.roll) * std::cos(state.euler.pitch));
        collective_ =
            std::clamp((climbIntegrator_ + cp.climbP * error) / std::sqrt(tilt), 0.0, 1.0);
    } else {
        collective_ = std::clamp(cmd.throttle, 0.0, 1.0);
    }

    // --- Rate PIDs. ---
    const bool lowThrottle = collective_ < kLowThrottle;
    Vec3 torque(0.0);
    for (int axis = 0; axis < 3; ++axis) {
        const bool freeze = (lowThrottle && !cp.airmode) || lastSaturated_;
        Pid& pid = pid_[static_cast<std::size_t>(axis)];
        torque[axis] = pid.update(rates[axis], state.rates[axis], dt, freeze);
        if (lowThrottle) {
            pid.limitIntegrator(cp.airmode ? kIdleIntegratorLimit : 0.0);
        }
    }
    ControllerOutput out = mixQuadX(profile_, collective_, torque, cp.airmode);
    lastSaturated_ = out.saturated;
    for (double& m : out.motors) {
        m = idle + (1.0 - idle) * m;  // ESC command: idle spin while armed.
    }
    return out;
}

}  // namespace ds::physics
