// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/input/input_selector.hpp"

#include <cmath>
#include <utility>

namespace ds::input {

void InputSelector::keyboardAction(KeyboardAction action) {
    const std::scoped_lock lock(mutex_);
    actions_.push_back(action);
}

void InputSelector::setJoystick(std::optional<InputMapping> mapping) {
    const std::scoped_lock lock(mutex_);
    mapping_ = std::move(mapping);
    mappingChanged_ = true;
}

ControlInputs InputSelector::poll(double dt) {
    std::vector<KeyboardAction> actions;
    {
        const std::scoped_lock lock(mutex_);
        actions.swap(actions_);
        if (mappingChanged_) {
            activeMapping_ = mapping_;
            mappingChanged_ = false;
            lastRaw_ = {};
            joystickState_ = ControlInputs{.source = InputSource::Joystick};
        }
    }
    for (const KeyboardAction a : actions) {
        switch (a) {
            case KeyboardAction::ToggleArm: keyboard_.toggleArm(); break;
            case KeyboardAction::Disarm: keyboard_.setArm(false); break;
            case KeyboardAction::Mode1: keyboard_.setFlightMode(0); break;
            case KeyboardAction::Mode2: keyboard_.setFlightMode(1); break;
            case KeyboardAction::Mode3: keyboard_.setFlightMode(2); break;
            case KeyboardAction::Mode4: keyboard_.setFlightMode(3); break;
            case KeyboardAction::ToggleLights: keyboard_.toggleLights(); break;
            case KeyboardAction::Reset: keyboard_.reset(); break;
        }
    }

    if (activeMapping_ && joysticks_ != nullptr) {
        const RawJoystickState raw = joysticks_->state(activeMapping_->deviceGuid);
        ControlInputs next = mapControls(*activeMapping_, raw, lastRaw_, joystickState_);
        if (raw.connected && activeMapping_->filterHz > 0.0) {
            const double a = 1.0 - std::exp(-6.283185307179586 * activeMapping_->filterHz * dt);
            next.throttle = joystickState_.throttle + a * (next.throttle - joystickState_.throttle);
            next.roll = joystickState_.roll + a * (next.roll - joystickState_.roll);
            next.pitch = joystickState_.pitch + a * (next.pitch - joystickState_.pitch);
            next.yaw = joystickState_.yaw + a * (next.yaw - joystickState_.yaw);
        }
        if (raw.connected) {
            lastRaw_ = raw;
        }
        joystickState_ = next;
        source_ = InputSource::Joystick;
        return next;
    }
    source_ = InputSource::Keyboard;
    if (!keyboardAllowed_.load()) {
        ControlInputs none = keyboard_.state();
        none.throttle = none.roll = none.pitch = none.yaw = 0.0;
        none.linkValid = false;
        return none;
    }
    return keyboard_.update(keys_.load(), dt);
}

}  // namespace ds::input
