// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "ds/input/joystick_service.hpp"
#include "ds/input/keyboard.hpp"
#include "ds/input/mapping.hpp"

/// @file
/// @brief Chooses between the keyboard and a joystick and produces the controls of each
/// simulation tick (FR-IN-04/05). Thread-safe: the GUI sets keys and the device, the
/// simulation thread polls.
/// @ingroup input

namespace ds::input {

/// @brief One-shot keyboard actions.
enum class KeyboardAction {
    ToggleArm,
    Disarm,
    Mode1,
    Mode2,
    Mode3,
    Mode4,
    ToggleLights,
    ToggleReturnToHome,
    Reset
};

/// @brief Input source selection and polling.
class InputSelector {
public:
    /// @brief Uses @p joysticks (may be null: keyboard only).
    explicit InputSelector(const JoystickService* joysticks = nullptr) noexcept
        : joysticks_(joysticks) {}

    /// @brief Keys currently held (FlightKey bits). Any thread.
    void setKeys(std::uint32_t keys) noexcept { keys_.store(keys); }
    /// @brief Queues a keyboard action. Any thread.
    void keyboardAction(KeyboardAction action);
    /// @brief Flies with the joystick mapped by @p mapping, or with the keyboard if empty.
    void setJoystick(std::optional<InputMapping> mapping);
    /// @brief True if keyboard flying is allowed (missions may forbid it, FR-IN-05).
    void setKeyboardAllowed(bool allowed) noexcept { keyboardAllowed_.store(allowed); }

    /// @brief Controls for the next tick (simulation thread).
    ControlInputs poll(double dt);
    /// @brief Source of the last poll().
    [[nodiscard]] InputSource activeSource() const noexcept { return source_.load(); }

private:
    const JoystickService* joysticks_;
    std::atomic<std::uint32_t> keys_{0};
    std::atomic<bool> keyboardAllowed_{true};
    std::atomic<InputSource> source_{InputSource::Keyboard};
    mutable std::mutex mutex_;
    std::vector<KeyboardAction> actions_;
    std::optional<InputMapping> mapping_;
    bool mappingChanged_ = false;
    // Simulation-thread state.
    KeyboardControl keyboard_;
    std::optional<InputMapping> activeMapping_;
    RawJoystickState lastRaw_;
    ControlInputs joystickState_{.source = InputSource::Joystick};
};

}  // namespace ds::input
