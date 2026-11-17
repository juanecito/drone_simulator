// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <string_view>

#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/estimator.hpp"
#include "ds/physics/pilot_command.hpp"

/// @file
/// @brief Onboard flight controller of the aerial drone (FR-PHY-12/13/14): rate curves,
/// cascaded PID (angle → rate) with filtered D-term, X-quad mixer with airmode, flight modes
/// Acro / Angle / Altitude hold, arming with pre-arm checks, RC-loss failsafe (landing).
/// @details Implemented from the control-engineering literature; no code from existing
/// flight-control firmware is used (SPECIFICATION §12.3).
/// @ingroup physics

namespace ds::physics {

/// @brief Facts about the vehicle the controller may know.
struct VehicleConditions {
    bool batteryCritical = false;  ///< Battery critical or empty.
    bool crashed = false;          ///< The vehicle crashed.
    bool landed = false;           ///< Landing detector (ground contact).
};

/// @brief Output of one controller tick.
struct ControllerOutput {
    std::array<double, AerialProfile::kMotorCount> motors{};  ///< ESC commands 0 … 1.
    bool saturated = false;  ///< The mixer had to limit the torque demand.
};

/// @brief Rate (°/s) for a stick deflection @p x with curve @p curve.
[[nodiscard]] double rateFromStick(const RateCurve& curve, double x) noexcept;

/// @brief PID controller with derivative on measurement, first-order D-term filter and
/// clamped integrator.
class Pid {
public:
    /// @brief Gains @p gains, D-term filter cut-off @p cutoffHz.
    void configure(const PidGains& gains, double cutoffHz) noexcept;
    /// @brief Clears the integrator and the filter.
    void reset() noexcept;
    /// @brief One step: setpoint and measurement in rad/s.
    double update(double setpoint, double measurement, double dt, bool freezeIntegrator) noexcept;
    /// @brief Limits the integrator to ±@p limit.
    void limitIntegrator(double limit) noexcept;
    /// @brief Integrator value.
    [[nodiscard]] double integrator() const noexcept { return integrator_; }

private:
    PidGains gains_;
    double cutoffHz_ = 100.0;
    double integrator_ = 0.0;
    double lastMeasurement_ = 0.0;
    double dFiltered_ = 0.0;
    bool first_ = true;
};

/// @brief Mixes a collective command and normalised roll/pitch/yaw torques into the four
/// motors (X layout); with @p airmode the collective moves to keep full torque authority.
[[nodiscard]] ControllerOutput mixQuadX(const AerialProfile& profile, double collective,
                                        const Vec3& torque, bool airmode) noexcept;

/// @brief The flight controller.
class FlightController {
public:
    /// @brief Controller for @p profile.
    explicit FlightController(const AerialProfile& profile);

    /// @brief Power-on state: disarmed, Angle mode, arm switch must be cycled.
    void reset();
    /// @brief Runs one tick; returns the ESC commands (all zero when disarmed).
    ControllerOutput update(const PilotCommand& command, const EstimatedState& state,
                            const VehicleConditions& conditions, double dt);
    /// @brief Disarms immediately (crash, battery depleted).
    void forceDisarm() noexcept;

    /// @brief True while armed.
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    /// @brief Active mode.
    [[nodiscard]] FlightMode mode() const noexcept { return mode_; }
    /// @brief True while the RC-loss failsafe is active.
    [[nodiscard]] bool failsafe() const noexcept { return failsafeActive_; }
    /// @brief Reason of the last refused arming (None if armed or never refused).
    [[nodiscard]] ArmBlock armBlock() const noexcept { return armBlock_; }
    /// @brief Result of the pre-arm checks for @p command now.
    [[nodiscard]] ArmBlock preArmCheck(const PilotCommand& command, const EstimatedState& state,
                                       const VehicleConditions& conditions) const noexcept;
    /// @brief Rate setpoint of the last tick (body p, q, r), rad/s.
    [[nodiscard]] const Vec3& rateSetpoint() const noexcept { return rateSetpoint_; }
    /// @brief Altitude-hold target (barometric), m.
    [[nodiscard]] double altitudeTarget() const noexcept { return altitudeTarget_; }
    /// @brief Collective (0–1) of the last tick.
    [[nodiscard]] double collective() const noexcept { return collective_; }
    /// @brief Seconds since arming.
    [[nodiscard]] double armedTime() const noexcept { return armedTime_; }

private:
    void enterMode(FlightMode mode, const EstimatedState& state);

    AerialProfile profile_;
    std::array<Pid, 3> pid_;
    bool armed_ = false;
    bool switchWasLow_ = false;
    bool lastArmSwitch_ = false;
    FlightMode mode_ = FlightMode::Angle;
    ArmBlock armBlock_ = ArmBlock::None;
    bool failsafeActive_ = false;
    double linkLostTime_ = 0.0;
    double landedTime_ = 0.0;
    PilotCommand lastValid_;
    Vec3 rateSetpoint_{0.0};
    double altitudeTarget_ = 0.0;
    double climbIntegrator_ = 0.0;
    double collective_ = 0.0;
    double armedTime_ = 0.0;
    bool lastSaturated_ = false;
};

}  // namespace ds::physics
