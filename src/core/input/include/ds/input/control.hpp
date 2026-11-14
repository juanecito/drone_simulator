// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

/// @file
/// @brief Normalised pilot controls produced by any input device (FR-IN-01…05).
/// @ingroup input

namespace ds::input {

/// @brief Flight modes as selected on the transmitter (mapped to the vehicle's modes by the
/// simulation): 0 = Acro, 1 = Angle, 2 = Altitude hold, 3 = Position hold (M7).
inline constexpr int kFlightModeCount = 4;

/// @brief Where the controls come from.
enum class InputSource { None, Keyboard, Joystick };

/// @brief Controls of one simulation tick.
struct ControlInputs {
    double throttle = 0.0;                   ///< 0 … 1.
    double roll = 0.0;                       ///< −1 … 1, positive = right.
    double pitch = 0.0;                      ///< −1 … 1, positive = stick forward.
    double yaw = 0.0;                        ///< −1 … 1, positive = right.
    bool arm = false;                        ///< Arm switch.
    int flightMode = 1;                      ///< 0 … kFlightModeCount − 1.
    bool beeper = false;                     ///< Beeper switch.
    bool returnToHome = false;               ///< RTH switch (M7).
    bool payloadRelease = false;             ///< Payload release (M5).
    bool cameraTrigger = false;              ///< Camera trigger.
    bool lights = false;                     ///< Vehicle lights.
    bool linkValid = true;                   ///< False when the device is gone (failsafe).
    InputSource source = InputSource::None;  ///< Device type.
    /// @brief Equality (tests).
    bool operator==(const ControlInputs&) const = default;
};

}  // namespace ds::input
