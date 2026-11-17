// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/physics/ground_profile.hpp"
#include "ds/physics/pilot_command.hpp"

/// @file
/// @brief Onboard drive controller of the ground drone (FR-GND-11): arming with pre-arm
/// checks, Manual mode (arcade mix of throttle and steering for skid steering), Speed control
/// (cruise at a set speed from the wheel odometry) and the RC-loss failsafe (stop).
/// @ingroup physics

namespace ds::physics {

/// @brief Facts about the rover the controller may know.
struct DriveConditions {
    bool batteryCritical = false;  ///< Battery critical or empty.
    bool crashed = false;          ///< Broken in a collision.
    bool rolledOver = false;       ///< On its side or roof.
    bool inWater = false;          ///< Drowned.
    double odometrySpeedMs = 0.0;  ///< Forward speed from the wheel encoders.
};

/// @brief Output of one controller tick: motor commands per side.
struct DriveOutput {
    double left = 0.0;   ///< Left motors, −1 … 1 (fraction of the battery voltage).
    double right = 0.0;  ///< Right motors, −1 … 1.
    bool brake = false;  ///< Brake the wheels (motors shorted, holding brake on).
};

/// @brief Skid-steer arcade mix: left = t + g·s, right = t − g·s, scaled back into [−1, 1]
/// so the ratio between the sides is kept.
[[nodiscard]] DriveOutput mixArcade(double throttle, double steering, double steeringGain) noexcept;

/// @brief Throttle curve: `(1 − e)·x + e·x³`.
[[nodiscard]] double throttleCurve(double x, double expo) noexcept;

/// @brief The drive controller.
class DriveController {
public:
    /// @brief Controller for @p params.
    explicit DriveController(const DriveControllerParams& params);

    /// @brief Power-on state: disarmed, Manual mode, arm switch must be cycled.
    void reset();
    /// @brief Runs one tick; motors off (and brake on) when disarmed.
    DriveOutput update(const PilotCommand& command, const DriveConditions& conditions, double dt);
    /// @brief Disarms immediately (crash, rollover, battery depleted).
    void forceDisarm() noexcept;

    /// @brief Result of the pre-arm checks for @p command now.
    [[nodiscard]] ArmBlock preArmCheck(const PilotCommand& command,
                                       const DriveConditions& conditions) const noexcept;
    /// @brief True while armed.
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    /// @brief Active mode.
    [[nodiscard]] DriveMode mode() const noexcept { return mode_; }
    /// @brief True while the RC-loss failsafe is active.
    [[nodiscard]] bool failsafe() const noexcept { return failsafeActive_; }
    /// @brief Reason of the last refused arming.
    [[nodiscard]] ArmBlock armBlock() const noexcept { return armBlock_; }
    /// @brief Speed-control target of the last tick, m/s.
    [[nodiscard]] double cruiseSetpoint() const noexcept { return setpoint_; }
    /// @brief Seconds since arming.
    [[nodiscard]] double armedTime() const noexcept { return armedTime_; }

private:
    DriveControllerParams params_;
    bool armed_ = false;
    bool switchWasLow_ = false;
    DriveMode mode_ = DriveMode::Manual;
    ArmBlock armBlock_ = ArmBlock::None;
    bool failsafeActive_ = false;
    double linkLostTime_ = 0.0;
    double integrator_ = 0.0;
    double setpoint_ = 0.0;
    double armedTime_ = 0.0;
};

}  // namespace ds::physics
