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
constexpr double kStickDeadband = 0.05;                // Position hold: centred sticks.
constexpr double kBrakeDoneMs = 0.3;                   // Braking ends below this speed.
constexpr double kRthClimbToleranceM = 1.0;            // Climb done this close to the height.
constexpr double kRthYawGain = 2.0;                    // Turn rate per radian of error, 1/s.
constexpr double kRthMaxYawRate = 1.5707963267948966;  // 90°/s.
constexpr double kRthTurnRadiusM = 5.0;                // Closer than this: keep the heading.

// A mode in which the throttle stick commands the climb rate.
bool holdsAltitude(FlightMode mode) noexcept {
    return mode == FlightMode::AltitudeHold || mode == FlightMode::PositionHold ||
           mode == FlightMode::ReturnToHome;
}

bool usesGps(FlightMode mode) noexcept {
    return mode == FlightMode::PositionHold || mode == FlightMode::ReturnToHome;
}

Vec3 horizontal(const Vec3& v) noexcept { return {v.x, 0.0, v.z}; }

// Bearing (clockwise from north) of the world vector @p d.
double bearing(const Vec3& d) noexcept { return std::atan2(d.x, -d.z); }

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
        case FlightMode::PositionHold: return "Pos hold";
        case FlightMode::ReturnToHome: return "RTH";
    }
    return "?";
}

std::string_view rthStageName(RthStage stage) noexcept {
    switch (stage) {
        case RthStage::Climb: return "climb";
        case RthStage::Return: return "return";
        case RthStage::Descend: return "land";
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
        case ArmBlock::ThrottleNotCentred: return "Throttle not in neutral";
        case ArmBlock::RolledOver: return "Rolled over — reset the vehicle";
        case ArmBlock::InWater: return "In the water — reset the vehicle";
        case ArmBlock::NoGpsFix: return "Position hold needs a GPS fix";
        case ArmBlock::RthSwitchOn: return "Turn the RTH switch off first";
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
    armAltitude_ = 0.0;
    home_.reset();
    holdPhase_ = HoldPhase::Braking;
    holdTarget_ = Vec3(0.0);
    velocityIntegrator_ = Vec3(0.0);
    rthStage_ = RthStage::Climb;
    rthAltitude_ = 0.0;
    gpsModeUnavailable_ = false;
}

std::optional<RthStage> FlightController::rthStage() const noexcept {
    return mode_ == FlightMode::ReturnToHome && armed_ ? std::optional(rthStage_) : std::nullopt;
}

std::optional<Vec3> FlightController::holdTarget() const noexcept {
    return mode_ == FlightMode::PositionHold && holdPhase_ == HoldPhase::Holding
               ? std::optional(holdTarget_)
               : std::nullopt;
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
    if (command.returnToHome) {
        return ArmBlock::RthSwitchOn;
    }
    // In altitude and position hold the centred stick means "hold": arming needs it at or
    // below centre.
    const double throttleLimit =
        holdsAltitude(command.mode) ? 0.5 + kAltHoldDeadband / 2.0 : kLowThrottle;
    if (command.throttle > throttleLimit) {
        return ArmBlock::ThrottleNotLow;
    }
    if (usesGps(command.mode) && !state.positionValid) {
        return ArmBlock::NoGpsFix;
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
    if (holdsAltitude(mode) && !holdsAltitude(mode_)) {
        // Bumpless: hold the current altitude with the current collective as hover estimate.
        altitudeTarget_ = state.altitudeM;
        const double tilt = std::max(0.3, std::cos(state.euler.roll) * std::cos(state.euler.pitch));
        climbIntegrator_ = collective_ * std::sqrt(tilt);
    }
    if (usesGps(mode) && !usesGps(mode_)) {
        velocityIntegrator_ = Vec3(0.0);
    }
    if (mode == FlightMode::PositionHold) {
        holdPhase_ = HoldPhase::Braking;
    }
    if (mode == FlightMode::ReturnToHome && home_) {
        const double distance = glm::length(horizontal(*home_ - state.position));
        rthStage_ = distance > kRthNoClimbRadiusM ? RthStage::Climb : RthStage::Return;
        rthAltitude_ =
            distance > kRthNoClimbRadiusM
                ? std::max(state.altitudeM, armAltitude_ + profile_.controller.rthAltitudeM)
                : state.altitudeM;
        holdTarget_ = horizontal(state.position);
    }
    mode_ = mode;
}

std::pair<double, double> FlightController::velocityControl(const Vec3& velocity,
                                                            const EstimatedState& state,
                                                            double dt) {
    const ControllerParams& cp = profile_.controller;
    const double maxAccel = kGravity * std::tan(common::degToRad(cp.maxAngleDeg));
    const Vec3 error = horizontal(velocity - state.velocity);
    const Vec3 integrator = velocityIntegrator_ + cp.velocityI * error * dt;
    Vec3 accel = cp.velocityP * error + integrator;
    const double norm = glm::length(accel);
    if (norm > maxAccel) {
        accel *= maxAccel / norm;  // Saturated: the integrator does not wind up.
    } else {
        velocityIntegrator_ = integrator;
        const double iNorm = glm::length(velocityIntegrator_);
        if (iNorm > maxAccel) {
            velocityIntegrator_ *= maxAccel / iNorm;
        }
    }
    // Into the heading frame: forward needs nose down, right needs right side down.
    const double north = -accel.z;
    const double east = accel.x;
    const double psi = state.euler.yaw;
    const double forward = north * std::cos(psi) + east * std::sin(psi);
    const double right = -north * std::sin(psi) + east * std::cos(psi);
    const double pitch = -std::atan(forward / kGravity);
    const double roll = std::atan(right * std::cos(pitch) / kGravity);
    return {roll, pitch};
}

Vec3 FlightController::positionHold(const PilotCommand& cmd, const EstimatedState& state) {
    const ControllerParams& cp = profile_.controller;
    const double forward = applyDeadband(std::clamp(cmd.pitch, -1.0, 1.0), kStickDeadband);
    const double right = applyDeadband(std::clamp(cmd.roll, -1.0, 1.0), kStickDeadband);
    const Vec3 position = horizontal(state.position);
    if (forward != 0.0 || right != 0.0) {
        // Sticks: horizontal speed in the heading frame.
        holdPhase_ = HoldPhase::Moving;
        const double psi = state.euler.yaw;
        const double vf = forward * cp.maxHorizontalSpeedMs;
        const double vr = right * cp.maxHorizontalSpeedMs;
        const double north = vf * std::cos(psi) - vr * std::sin(psi);
        const double east = vf * std::sin(psi) + vr * std::cos(psi);
        return {east, 0.0, -north};
    }
    if (holdPhase_ == HoldPhase::Moving) {
        holdPhase_ = HoldPhase::Braking;
    }
    if (holdPhase_ == HoldPhase::Braking) {
        if (glm::length(horizontal(state.velocity)) > kBrakeDoneMs) {
            return Vec3(0.0);
        }
        holdTarget_ = position;
        holdPhase_ = HoldPhase::Holding;
    }
    Vec3 v = cp.positionGain * (holdTarget_ - position);
    const double speed = glm::length(v);
    if (speed > cp.maxHorizontalSpeedMs) {
        v *= cp.maxHorizontalSpeedMs / speed;
    }
    return v;
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
        cmd.returnToHome = false;
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
                armAltitude_ = state.altitudeM;
                home_.reset();
                velocityIntegrator_ = Vec3(0.0);
                holdPhase_ = HoldPhase::Braking;
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
        gpsModeUnavailable_ = false;
        return {};
    }
    armedTime_ += dt;
    if (!home_ && state.positionValid) {
        home_ = horizontal(state.position);  // At arming, or at the first fix after it.
    }

    // --- Mode: the RTH switch or the failsafe take over; GPS modes need a fix. ---
    const bool gps = state.positionValid && home_.has_value();
    FlightMode wanted = cmd.mode;
    gpsModeUnavailable_ = false;
    if (failsafeActive_ || cmd.returnToHome) {
        if (gps) {
            wanted = FlightMode::ReturnToHome;
        } else if (cmd.returnToHome && !failsafeActive_) {
            gpsModeUnavailable_ = true;  // The switch is ignored.
        }
    }
    if (wanted == FlightMode::PositionHold && !state.positionValid) {
        wanted = FlightMode::AltitudeHold;
        gpsModeUnavailable_ = true;
    }
    if (wanted == FlightMode::ReturnToHome && !gps) {
        wanted = FlightMode::AltitudeHold;
    }

    // --- Failsafe landing without GPS, and the end of a return: descend, disarm on the
    // ground. ---
    double climbSetpoint = 0.0;
    bool verticalSpeedControl = false;
    if (failsafeActive_ && wanted != FlightMode::ReturnToHome) {
        enterMode(FlightMode::AltitudeHold, state);
        verticalSpeedControl = true;
        climbSetpoint = -cp.failsafeDescentMs;
    } else {
        enterMode(wanted, state);
    }

    // --- Horizontal control of the GPS modes. ---
    std::optional<std::pair<double, double>> gpsTilt;
    double yawRate = common::degToRad(rateFromStick(cp.rates[2], cmd.yaw));
    if (mode_ == FlightMode::PositionHold) {
        gpsTilt = velocityControl(positionHold(cmd, state), state, dt);
    } else if (mode_ == FlightMode::ReturnToHome) {
        const Vec3 position = horizontal(state.position);
        const Vec3 toHome = *home_ - position;
        const double distance = glm::length(toHome);
        Vec3 velocity(0.0);
        switch (rthStage_) {
            case RthStage::Climb:
                velocity = cp.positionGain * (holdTarget_ - position);
                if (state.altitudeM >= rthAltitude_ - kRthClimbToleranceM) {
                    rthStage_ = RthStage::Return;
                }
                break;
            case RthStage::Return:
                if (distance > 1e-6) {
                    velocity =
                        toHome / distance * std::min(cp.rthSpeedMs, cp.positionGain * distance);
                }
                if (distance < kRthArrivalRadiusM) {
                    rthStage_ = RthStage::Descend;
                }
                break;
            case RthStage::Descend: velocity = cp.positionGain * toHome; break;
        }
        gpsTilt = velocityControl(velocity, state, dt);
        verticalSpeedControl = true;
        climbSetpoint = rthStage_ == RthStage::Descend
                            ? -cp.failsafeDescentMs
                            : std::clamp(cp.altitudeGain * (rthAltitude_ - state.altitudeM),
                                         -cp.maxDescentMs, cp.maxClimbMs);
        if (rthStage_ != RthStage::Descend && distance > kRthTurnRadiusM) {
            // Nose towards home.
            const double error = common::wrapRadiansPi(bearing(toHome) - state.euler.yaw);
            yawRate = std::clamp(kRthYawGain * error, -kRthMaxYawRate, kRthMaxYawRate);
        } else {
            yawRate = 0.0;
        }
    }
    const bool landing =
        verticalSpeedControl && climbSetpoint < 0.0 &&
        (failsafeActive_ || (mode_ == FlightMode::ReturnToHome && rthStage_ == RthStage::Descend));
    landedTime_ = landing && conditions.landed ? landedTime_ + dt : 0.0;
    if (landedTime_ >= kLandedDisarmS) {
        forceDisarm();
        landedTime_ = 0.0;
        return {};
    }

    // --- Rate setpoints. ---
    const double maxAngle = common::degToRad(cp.maxAngleDeg);
    Vec3 rates(0.0);
    if (mode_ == FlightMode::Acro) {
        rates = Vec3(common::degToRad(rateFromStick(cp.rates[0], cmd.roll)),
                     -common::degToRad(rateFromStick(cp.rates[1], cmd.pitch)), yawRate);
    } else {
        const double rollTarget =
            gpsTilt ? gpsTilt->first : std::clamp(cmd.roll, -1.0, 1.0) * maxAngle;
        const double pitchTarget =
            gpsTilt ? gpsTilt->second : -std::clamp(cmd.pitch, -1.0, 1.0) * maxAngle;
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
    if (holdsAltitude(mode_)) {
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
