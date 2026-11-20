// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/input/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace ds::input {

CalibrationWizard::CalibrationWizard(int axisCount, InputMapping base)
    : axisCount_(std::max(axisCount, 0)), mapping_(std::move(base)) {
    const auto n = static_cast<std::size_t>(axisCount_);
    centre_.assign(n, 0.0);
    min_.assign(n, 1.0);
    max_.assign(n, -1.0);
    current_.assign(n, 0.0);
    used_.assign(n, false);
}

bool CalibrationWizard::ground() const noexcept {
    return mapping_.vehicle == VehicleControls::Ground;
}

std::string_view CalibrationWizard::prompt() const noexcept {
    if (ground()) {
        switch (step_) {
            case Step::CenterSticks:
                return "Centre the sticks or the steering wheel and release the triggers, then "
                       "press Next.";
            case Step::MoveExtremes:
                return "Move every stick, trigger and the steering wheel through its full travel "
                       "several times, then press Next.";
            case Step::DriveForward:
                return "Drive forward: push the drive stick fully forward, or press the forward "
                       "trigger fully, and hold it, then press Next.";
            case Step::DriveReverse:
                return driveIsStick_
                           ? "Reverse: pull the same control fully back and hold it, then press "
                             "Next."
                           : "Reverse: release the forward trigger, press the reverse trigger "
                             "fully and hold it, then press Next.";
            case Step::SteerRight:
                return "Steer fully right (stick or wheel) and hold it, then press Next.";
            case Step::Done: return "Calibration complete.";
            default: break;
        }
    }
    switch (step_) {
        case Step::CenterSticks:
            return "Centre all sticks and put the throttle at minimum, then press Next.";
        case Step::MoveExtremes:
            return "Move both sticks slowly to every corner several times, then press Next.";
        case Step::ThrottleUp:
            return "Push the throttle stick fully up and hold it, then press Next.";
        case Step::YawRight: return "Push the yaw stick fully right and hold it, then press Next.";
        case Step::PitchForward:
            return "Push the pitch stick fully forward and hold it, then press Next.";
        case Step::RollRight:
            return "Push the roll stick fully right and hold it, then press Next.";
        case Step::Done: return "Calibration complete.";
        default: break;
    }
    return "";
}

void CalibrationWizard::sample(const RawJoystickState& raw) {
    for (std::size_t i = 0; i < current_.size() && i < raw.axes.size(); ++i) {
        current_[i] = raw.axes[i];
        if (step_ == Step::CenterSticks) {
            centre_[i] = raw.axes[i];
        }
        if (step_ != Step::CenterSticks) {
            min_[i] = std::min(min_[i], raw.axes[i]);
            max_[i] = std::max(max_[i], raw.axes[i]);
        }
    }
}

int CalibrationWizard::mostDeflectedAxis() const noexcept {
    int best = -1;
    double bestValue = 0.15;
    for (std::size_t i = 0; i < current_.size(); ++i) {
        const double d = std::abs(current_[i] - centre_[i]);
        if (d > bestValue) {
            bestValue = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int CalibrationWizard::pickAxis(double needed) const {
    int best = -1;
    double bestRel = needed;
    for (std::size_t i = 0; i < current_.size(); ++i) {
        if (used_[i]) {
            continue;
        }
        const double range = std::max(max_[i] - min_[i], 1e-6);
        const double rel = std::abs(current_[i] - centre_[i]) / range;
        if (rel >= bestRel) {
            bestRel = rel;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool CalibrationWizard::restsAtAnEnd(std::size_t i) const {
    const double range = max_[i] - min_[i];
    if (range <= 1e-6) {
        return false;
    }
    const double margin = kTriggerRestFraction * range;
    return centre_[i] - min_[i] < margin || max_[i] - centre_[i] < margin;
}

bool CalibrationWizard::assignStick(std::size_t slot, int axis) {
    const auto i = static_cast<std::size_t>(axis);
    AxisCalibration& c = mapping_.channels[slot];
    const double deadzone = c.deadzone;
    const double expo = c.expo;
    c = AxisCalibration{};
    c.axis = axis;
    c.deadzone = deadzone;
    c.expo = expo;
    c.min = min_[i];
    c.max = max_[i];
    c.center = centre_[i];
    c.inverted = current_[i] < centre_[i];
    used_[i] = true;
    return true;
}

void CalibrationWizard::assignTrigger(std::size_t slot, int axis) {
    const auto i = static_cast<std::size_t>(axis);
    AxisCalibration& c = mapping_.channels[slot];
    const double deadzone = c.axis >= 0 ? c.deadzone : 0.05;
    const double expo = c.expo;
    c = AxisCalibration{};
    c.axis = axis;
    c.deadzone = deadzone;
    c.expo = expo;
    // Released at the resting end, pressed at the far end (calibrateTrigger()).
    c.min = centre_[i];
    c.center = centre_[i];
    c.max = current_[i] >= centre_[i] ? max_[i] : min_[i];
    used_[i] = true;
}

bool CalibrationWizard::nextGround() {
    const auto slot = [](GroundChannel ch) { return static_cast<std::size_t>(ch); };
    const auto clear = [&](GroundChannel ch) { mapping_.channels[slot(ch)].axis = -1; };
    switch (step_) {
        case Step::DriveForward: {
            const int axis = pickAxis(0.5 * kMinDeflection);
            if (axis < 0) {
                error_ = "No movement detected — push the drive control fully and hold it.";
                return false;
            }
            driveIsStick_ = !restsAtAnEnd(static_cast<std::size_t>(axis));
            if (driveIsStick_) {
                assignStick(slot(GroundChannel::Drive), axis);
                clear(GroundChannel::Forward);
                clear(GroundChannel::Reverse);
            } else {
                assignTrigger(slot(GroundChannel::Forward), axis);
                clear(GroundChannel::Drive);
            }
            error_.clear();
            step_ = Step::DriveReverse;
            return true;
        }
        case Step::DriveReverse: {
            if (driveIsStick_) {
                // The drive stick, now on the other side of its centre.
                const AxisCalibration& d = mapping_.channels[slot(GroundChannel::Drive)];
                const auto i = static_cast<std::size_t>(d.axis);
                const double range = std::max(max_[i] - min_[i], 1e-6);
                const double rel = (current_[i] - centre_[i]) / range;
                const bool back =
                    d.inverted ? rel > 0.25 * kMinDeflection : rel < -0.25 * kMinDeflection;
                if (!back) {
                    error_ = "Pull the same drive control fully back and hold it.";
                    return false;
                }
            } else {
                const int axis = pickAxis(0.5 * kMinDeflection);
                if (axis < 0 || !restsAtAnEnd(static_cast<std::size_t>(axis))) {
                    error_ = "No reverse trigger detected — press the other trigger fully.";
                    return false;
                }
                assignTrigger(slot(GroundChannel::Reverse), axis);
            }
            error_.clear();
            step_ = Step::SteerRight;
            return true;
        }
        case Step::SteerRight: {
            const int axis = pickAxis(0.5 * kMinDeflection);
            if (axis < 0) {
                error_ = "No steering movement detected — steer fully right and hold it.";
                return false;
            }
            if (restsAtAnEnd(static_cast<std::size_t>(axis))) {
                error_ = "That is a trigger — steer with a stick or the wheel.";
                return false;
            }
            assignStick(slot(GroundChannel::Steering), axis);
            error_.clear();
            step_ = Step::Done;
            return true;
        }
        default: return false;
    }
}

bool CalibrationWizard::assign(Channel channel) {
    // The axis moved furthest from its centre, among those not assigned yet.
    int best = -1;
    double bestRel = 0.0;
    for (std::size_t i = 0; i < current_.size(); ++i) {
        if (used_[i]) {
            continue;
        }
        const double range = std::max(max_[i] - min_[i], 1e-6);
        const double rel = std::abs(current_[i] - centre_[i]) / range;
        if (rel > bestRel) {
            bestRel = rel;
            best = static_cast<int>(i);
        }
    }
    // The throttle starts at one end, so it travels the whole range; sticks travel half.
    const double needed = channel == Channel::Throttle ? kMinDeflection : 0.5 * kMinDeflection;
    if (best < 0 || bestRel < needed) {
        error_ = "No stick movement detected — move the stick fully and hold it.";
        return false;
    }
    const auto i = static_cast<std::size_t>(best);
    AxisCalibration& c = mapping_.channels[static_cast<std::size_t>(channel)];
    const double deadzone = c.deadzone;
    const double expo = c.expo;
    c = AxisCalibration{};
    c.axis = best;
    c.deadzone = deadzone;
    c.expo = expo;
    c.min = min_[i];
    c.max = max_[i];
    if (channel == Channel::Throttle) {
        // The throttle was at minimum during the centre step.
        c.inverted = current_[i] < centre_[i];
        c.center = 0.5 * (min_[i] + max_[i]);
        if (mapping_.throttleCentred) {
            c.center = centre_[i];
        }
    } else {
        c.center = centre_[i];
        c.inverted = current_[i] < centre_[i];
    }
    used_[i] = true;
    error_.clear();
    return true;
}

bool CalibrationWizard::next() {
    switch (step_) {
        case Step::CenterSticks: step_ = Step::MoveExtremes; return true;
        case Step::MoveExtremes: {
            const bool moved = std::ranges::any_of(
                std::views::iota(std::size_t{0}, min_.size()),
                [&](std::size_t i) { return max_[i] - min_[i] > kMinDeflection; });
            if (!moved) {
                error_ = "No stick movement detected — move both sticks to every corner.";
                return false;
            }
            error_.clear();
            step_ = ground() ? Step::DriveForward : Step::ThrottleUp;
            return true;
        }
        case Step::ThrottleUp:
            if (!assign(Channel::Throttle)) {
                return false;
            }
            step_ = Step::YawRight;
            return true;
        case Step::YawRight:
            if (!assign(Channel::Yaw)) {
                return false;
            }
            step_ = Step::PitchForward;
            return true;
        case Step::PitchForward:
            if (!assign(Channel::Pitch)) {
                return false;
            }
            step_ = Step::RollRight;
            return true;
        case Step::RollRight:
            if (!assign(Channel::Roll)) {
                return false;
            }
            step_ = Step::Done;
            return true;
        case Step::DriveForward:
        case Step::DriveReverse:
        case Step::SteerRight: return nextGround();
        case Step::Done: return true;
    }
    return false;
}

}  // namespace ds::input
