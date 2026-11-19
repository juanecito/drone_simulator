// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/input/keyboard.hpp"

#include <algorithm>

namespace ds::input {
namespace {

bool held(std::uint32_t keys, FlightKey key) noexcept {
    return (keys & static_cast<std::uint32_t>(key)) != 0U;
}

double approach(double value, double target, double step) noexcept {
    return value < target ? std::min(value + step, target) : std::max(value - step, target);
}

double axisTarget(std::uint32_t keys, FlightKey negative, FlightKey positive,
                  double deflection = KeyboardControl::kDeflection) noexcept {
    return (held(keys, positive) ? deflection : 0.0) - (held(keys, negative) ? deflection : 0.0);
}

// Ground: either key of a pair (letters or arrows) moves the control.
double pairTarget(std::uint32_t keys, FlightKey negativeA, FlightKey negativeB, FlightKey positiveA,
                  FlightKey positiveB, double deflection) noexcept {
    const bool positive = held(keys, positiveA) || held(keys, positiveB);
    const bool negative = held(keys, negativeA) || held(keys, negativeB);
    return (positive ? deflection : 0.0) - (negative ? deflection : 0.0);
}

}  // namespace

ControlInputs KeyboardControl::update(std::uint32_t keys, double dt) {
    const double step = kStickRate * dt;
    state_.roll =
        approach(state_.roll, axisTarget(keys, FlightKey::RollLeft, FlightKey::RollRight), step);
    state_.pitch = approach(state_.pitch,
                            axisTarget(keys, FlightKey::PitchBack, FlightKey::PitchForward), step);
    state_.yaw =
        approach(state_.yaw, axisTarget(keys, FlightKey::YawLeft, FlightKey::YawRight), step);
    const bool up = held(keys, FlightKey::ThrottleUp);
    const bool down = held(keys, FlightKey::ThrottleDown);
    if (state_.flightMode >= 2) {
        // Altitude hold: centre = hold, W climbs, S descends.
        const double target = 0.5 + (up ? 0.5 : 0.0) - (down ? 0.5 : 0.0);
        state_.throttle = approach(state_.throttle, target, step);
    } else {
        const double rate = kThrottleRate * dt;
        state_.throttle =
            std::clamp(state_.throttle + (up ? rate : 0.0) - (down ? rate : 0.0), 0.0, 1.0);
    }
    // Ground controls.
    state_.drive =
        approach(state_.drive,
                 pairTarget(keys, FlightKey::ThrottleDown, FlightKey::PitchBack,
                            FlightKey::ThrottleUp, FlightKey::PitchForward, kDriveDeflection),
                 step);
    state_.steering =
        approach(state_.steering,
                 pairTarget(keys, FlightKey::YawLeft, FlightKey::RollLeft, FlightKey::YawRight,
                            FlightKey::RollRight, kSteeringDeflection),
                 step);
    state_.brake = held(keys, FlightKey::Brake);
    state_.linkValid = true;
    state_.source = InputSource::Keyboard;
    return state_;
}

void KeyboardControl::setFlightMode(int mode) noexcept {
    const int previous = state_.flightMode;
    state_.flightMode = std::clamp(mode, 0, kFlightModeCount - 1);
    // Entering altitude hold with a sticky throttle: centre it so the drone holds altitude.
    if (state_.flightMode >= 2 && previous < 2) {
        state_.throttle = 0.5;
    } else if (state_.flightMode < 2 && previous >= 2) {
        state_.throttle = std::min(state_.throttle, 0.5);
    }
}

void KeyboardControl::reset() noexcept {
    const int mode = state_.flightMode;
    state_ = ControlInputs{.source = InputSource::Keyboard};
    state_.flightMode = mode;
}

}  // namespace ds::input
