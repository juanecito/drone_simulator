// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "ds/input/control.hpp"

/// @file
/// @brief Keyboard flying (FR-IN-05), default Mode 2 layout: W/S throttle, A/D yaw, arrows
/// pitch/roll, Space arm/disarm, 1–4 flight modes. The GUI only reports which keys are held.
/// @ingroup input

namespace ds::input {

/// @brief Keys that fly the drone (bit flags).
enum class FlightKey : std::uint32_t {
    ThrottleUp = 1U << 0U,    ///< W.
    ThrottleDown = 1U << 1U,  ///< S.
    YawLeft = 1U << 2U,       ///< A.
    YawRight = 1U << 3U,      ///< D.
    PitchForward = 1U << 4U,  ///< Up arrow.
    PitchBack = 1U << 5U,     ///< Down arrow.
    RollLeft = 1U << 6U,      ///< Left arrow.
    RollRight = 1U << 7U,     ///< Right arrow.
};

/// @brief Turns held keys into smooth stick movements.
/// @details Sticks move towards ±deflection while a key is held and spring back when it is
/// released. The throttle is "sticky" (it stays where W/S left it) except in altitude-hold
/// modes, where it springs back to the centre (hold altitude) like a gamepad stick.
class KeyboardControl {
public:
    /// @brief Stick deflection reached with a key held.
    static constexpr double kDeflection = 0.6;
    /// @brief Stick movement speed, full travel per second.
    static constexpr double kStickRate = 3.0;
    /// @brief Throttle change per second while W/S is held (sticky throttle).
    static constexpr double kThrottleRate = 0.5;

    /// @brief Advances by @p dt with @p keys held (FlightKey bits).
    ControlInputs update(std::uint32_t keys, double dt);
    /// @brief Space: toggles the arm switch.
    void toggleArm() noexcept { state_.arm = !state_.arm; }
    /// @brief Sets the arm switch.
    void setArm(bool arm) noexcept { state_.arm = arm; }
    /// @brief Keys 1–4: selects flight mode @p mode (0-based).
    void setFlightMode(int mode) noexcept;
    /// @brief Toggles the lights.
    void toggleLights() noexcept { state_.lights = !state_.lights; }
    /// @brief Throttle to zero, sticks centred, disarmed (vehicle reset).
    void reset() noexcept;
    /// @brief Current controls.
    [[nodiscard]] const ControlInputs& state() const noexcept { return state_; }

private:
    ControlInputs state_{.source = InputSource::Keyboard};
};

}  // namespace ds::input
