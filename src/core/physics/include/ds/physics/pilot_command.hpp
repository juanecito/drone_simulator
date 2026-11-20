// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

/// @file
/// @brief Pilot commands and operating modes shared by both vehicle types: what the pilot
/// asks for in one tick (aerial sticks or ground drive/steering), the flight and drive modes,
/// and the reasons arming can be refused (FR-PHY-13/14, FR-GND-11).
/// @ingroup physics

namespace ds::physics {

/// @brief Flight modes (FR-PHY-13). The pilot selects the first four; ReturnToHome is
/// entered with the RTH switch or by the RC-loss failsafe.
enum class FlightMode {
    Acro,          ///< Sticks command body rates.
    Angle,         ///< Sticks command tilt; self-levelling.
    AltitudeHold,  ///< Angle plus barometric altitude hold (throttle = climb rate).
    PositionHold,  ///< Altitude hold plus GPS position hold (sticks = horizontal speed).
    ReturnToHome,  ///< Climb, fly back to the take-off point and land (GPS).
};

/// @brief Stage of a return to home.
enum class RthStage {
    Climb,    ///< Climbing to the return height, holding position, turning towards home.
    Return,   ///< Flying back at the return height.
    Descend,  ///< Landing over home; disarms on the ground.
};

/// @brief Display name of @p stage ("climb", "return", "land").
[[nodiscard]] std::string_view rthStageName(RthStage stage) noexcept;

/// @brief Display name of @p mode ("Acro", "Angle", "Alt hold", "Pos hold", "RTH").
[[nodiscard]] std::string_view flightModeName(FlightMode mode) noexcept;

/// @brief Drive modes of the ground drone (FR-GND-11). Heading hold (S) and the waypoint
/// autopilot (C) come later.
enum class DriveMode {
    Manual,        ///< Throttle and steering go straight to the motors (arcade mix).
    SpeedControl,  ///< The throttle stick sets a cruise speed, held from the wheel odometry.
};

/// @brief Display name of @p mode ("Manual", "Speed").
[[nodiscard]] std::string_view driveModeName(DriveMode mode) noexcept;

/// @brief Pilot commands of one tick (from the RC transmitter, gamepad or keyboard). Aerial
/// drones read the stick fields, ground drones the drive fields; arming and the link state
/// are common.
struct PilotCommand {
    double throttle = 0.0;                    ///< 0 … 1 (aerial collective).
    double roll = 0.0;                        ///< −1 … 1, positive = roll right.
    double pitch = 0.0;                       ///< −1 … 1, positive = stick forward (nose down).
    double yaw = 0.0;                         ///< −1 … 1, positive = yaw right.
    double drive = 0.0;                       ///< Ground: −1 full reverse … 1 full forward.
    double steering = 0.0;                    ///< Ground: −1 … 1, positive = turn right.
    bool brake = false;                       ///< Ground: brake.
    bool arm = false;                         ///< Arm switch position.
    FlightMode mode = FlightMode::Angle;      ///< Selected flight mode (aerial).
    bool returnToHome = false;                ///< RTH switch (aerial).
    DriveMode driveMode = DriveMode::Manual;  ///< Selected drive mode (ground).
    bool linkValid = true;                    ///< False while the RC link is lost.
};

/// @brief Why arming was refused (FR-PHY-14).
enum class ArmBlock {
    None,                ///< Arming allowed.
    ThrottleNotLow,      ///< Throttle stick must be at minimum (aerial).
    NotLevel,            ///< Tilted more than 25° (aerial).
    BatteryCritical,     ///< Battery empty or critical.
    Crashed,             ///< Reset the vehicle first.
    LinkLost,            ///< No RC link.
    SwitchNotCycled,     ///< Arm switch was on at start-up or after a disarm.
    ThrottleNotCentred,  ///< Drive stick must be in neutral (ground).
    RolledOver,          ///< On its side or roof (ground).
    InWater,             ///< Drowned in water deeper than the wading depth (ground).
    NoGpsFix,            ///< Position hold selected without a GPS fix (aerial).
    RthSwitchOn,         ///< The RTH switch is on (aerial).
};

/// @brief Message for @p block.
[[nodiscard]] std::string_view armBlockText(ArmBlock block) noexcept;

}  // namespace ds::physics
