// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>
#include <vector>

#include "ds/input/mapping.hpp"

/// @file
/// @brief Calibration wizard logic (FR-IN-02): records stick centres and end points and finds
/// which axis is which, and in which direction, from guided stick movements — so the channel
/// order (AETR, TAER, …), the stick mode and axis inversion are detected automatically.
/// For ground mappings (FR-IN-03a) the steps find the drive control — a self-centring stick or
/// pistol-grip trigger, or a pair of gamepad triggers (forward and reverse) — and the steering.
/// The GUI shows the prompts and feeds raw samples; this class has no GUI or device code.
/// @ingroup input

namespace ds::input {

/// @brief Guided calibration.
class CalibrationWizard {
public:
    /// @brief Steps in order.
    enum class Step {
        CenterSticks,  ///< Leave the sticks centred (throttle low) and confirm.
        MoveExtremes,  ///< Move all sticks to every corner, then confirm.
        ThrottleUp,    ///< Push the throttle to the top.
        YawRight,      ///< Push yaw fully right.
        PitchForward,  ///< Push pitch fully forward.
        RollRight,     ///< Push roll fully right.
        DriveForward,  ///< Ground: push the drive control (stick or trigger) fully forward.
        DriveReverse,  ///< Ground: pull it fully back (the same stick, or the other trigger).
        SteerRight,    ///< Ground: steer fully right.
        Done,          ///< Finished: result() is valid.
    };

    /// @brief Starts a calibration of a device with @p axisCount axes, keeping the switches
    /// and other settings of @p base; its vehicle type chooses the aerial or ground steps.
    CalibrationWizard(int axisCount, InputMapping base);

    /// @brief Current step.
    [[nodiscard]] Step step() const noexcept { return step_; }
    /// @brief Instruction for the current step.
    [[nodiscard]] std::string_view prompt() const noexcept;
    /// @brief Feeds one raw sample (call continuously while the wizard is open).
    void sample(const RawJoystickState& raw);
    /// @brief Confirms the current step ("Next"). Returns false with error() set if the step
    /// is not complete (e.g. no axis moved enough, or the same axis was used twice).
    bool next();
    /// @brief Why the last next() failed.
    [[nodiscard]] std::string_view error() const noexcept { return error_; }
    /// @brief The calibrated mapping (valid once step() == Done).
    [[nodiscard]] const InputMapping& result() const noexcept { return mapping_; }
    /// @brief Axis currently deflected most from its centre (−1 if none), for live feedback.
    [[nodiscard]] int mostDeflectedAxis() const noexcept;

    /// @brief Minimum deflection (fraction of the axis range) to recognise a movement.
    static constexpr double kMinDeflection = 0.5;
    /// @brief An axis that rests within this fraction of its range from one end is a trigger.
    static constexpr double kTriggerRestFraction = 0.1;

private:
    bool assign(Channel channel);
    /// Unused axis deflected most (relative to its range), or −1 below @p needed.
    [[nodiscard]] int pickAxis(double needed) const;
    /// True if axis @p i rests at one end of its travel (a trigger).
    [[nodiscard]] bool restsAtAnEnd(std::size_t i) const;
    bool assignStick(std::size_t slot, int axis);
    void assignTrigger(std::size_t slot, int axis);
    bool nextGround();
    [[nodiscard]] bool ground() const noexcept;

    int axisCount_;
    InputMapping mapping_;
    Step step_ = Step::CenterSticks;
    std::vector<double> centre_;
    std::vector<double> min_;
    std::vector<double> max_;
    std::vector<double> current_;
    std::vector<bool> used_;
    std::string error_;
    bool driveIsStick_ = true;  ///< Ground: drive on a stick (else on two triggers).
};

}  // namespace ds::input
