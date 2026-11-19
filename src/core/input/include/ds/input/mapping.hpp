// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/input/control.hpp"

/// @file
/// @brief Mapping of joystick axes and buttons to pilot controls: calibration, channel order,
/// stick modes, deadzones, expo, switches (FR-IN-02/03/04), separately for aerial and ground
/// drones (FR-IN-03a). Stored as the `mapping_json` column of `input_profiles`.
/// @ingroup input

namespace ds::input {

/// @brief Stick channels of an aerial mapping.
enum class Channel { Throttle = 0, Roll = 1, Pitch = 2, Yaw = 3 };
/// @brief Number of channels (of either vehicle type).
inline constexpr std::size_t kChannelCount = 4;
/// @brief "throttle", "roll", "pitch", "yaw".
[[nodiscard]] std::string_view channelName(Channel channel) noexcept;

/// @brief Channels of a ground mapping (same slots as Channel).
/// @details Drive is a self-centring stick (forward/reverse, −1 … 1); Forward and Reverse are
/// triggers (0 … 1 over their travel) added to and subtracted from it, so a gamepad can drive
/// with its triggers, a stick, or both.
enum class GroundChannel { Drive = 0, Steering = 1, Forward = 2, Reverse = 3 };
/// @brief "drive", "steering", "forward", "reverse".
[[nodiscard]] std::string_view groundChannelName(GroundChannel channel) noexcept;
/// @brief Name of channel slot @p index for @p type.
[[nodiscard]] std::string_view channelName(VehicleControls type, std::size_t index) noexcept;

/// @brief Raw state of a joystick (axes normalised to −1 … 1 as reported by the device).
struct RawJoystickState {
    std::vector<double> axes;   ///< Axis values.
    std::vector<bool> buttons;  ///< Button states.
    bool connected = false;     ///< Device present.
};

/// @brief Calibration and shaping of one axis.
struct AxisCalibration {
    int axis = -1;           ///< Device axis index (−1 = unassigned).
    double min = -1.0;       ///< Raw value at one end of travel.
    double center = 0.0;     ///< Raw value at the centre (sticks with a spring).
    double max = 1.0;        ///< Raw value at the other end.
    bool inverted = false;   ///< Reverse the direction.
    double deadzone = 0.02;  ///< Ignored deflection around the centre (fraction).
    double expo = 0.0;       ///< 0 … 1: softer response around the centre.
    /// @brief Equality.
    bool operator==(const AxisCalibration&) const = default;
};

/// @brief Function of a switch.
enum class SwitchFunction {
    Arm,
    FlightMode,
    Beeper,
    ReturnToHome,
    PayloadRelease,
    CameraTrigger,
    Lights,
    Brake  ///< Ground: brake while held.
};
/// @brief Name used in JSON ("arm", "flightMode", …).
[[nodiscard]] std::string_view switchFunctionName(SwitchFunction function) noexcept;

/// @brief A switch on a button or on an axis (radios report switches as axes).
struct SwitchMapping {
    SwitchFunction function = SwitchFunction::Arm;  ///< What it does.
    int button = -1;                                ///< Button index, or −1.
    int axis = -1;                                  ///< Axis index, or −1.
    bool inverted = false;                          ///< Reverse.
    /// Momentary button that toggles the function on each press (Arm, Lights, …); for
    /// FlightMode each press steps through @c modes.
    bool toggle = false;
    /// For FlightMode on an axis: mode for the low, middle and high positions (a 2-position
    /// switch uses low and high). For FlightMode on a button: mode when released / pressed.
    std::array<int, 3> modes{0, 1, 2};
    /// @brief Equality.
    bool operator==(const SwitchMapping&) const = default;
};

/// @brief Complete mapping of one device for one vehicle type.
struct InputMapping {
    std::string deviceGuid;                             ///< SDL joystick GUID.
    std::string deviceName;                             ///< Device name (informative).
    std::string name = "Default";                       ///< Profile name.
    VehicleControls vehicle = VehicleControls::Aerial;  ///< Vehicle type it drives.
    /// Throttle, roll, pitch, yaw (aerial) or drive, steering, forward, reverse (ground).
    std::array<AxisCalibration, kChannelCount> channels{};
    std::vector<SwitchMapping> switches;  ///< Switches.
    /// Throttle on a self-centring stick (gamepads): centre = half throttle.
    bool throttleCentred = false;
    double filterHz = 0.0;  ///< Low-pass filter of the sticks (0 = off).
    /// @brief Equality.
    bool operator==(const InputMapping&) const = default;

    /// @brief Serialises to JSON (`input_profiles.mapping_json`).
    [[nodiscard]] common::Json toJson() const;
    /// @brief Parses JSON; errors name the JSON path.
    [[nodiscard]] static common::Result<InputMapping> fromJson(const common::Json& json);
};

/// @brief Mapping for an RC transmitter in joystick mode with the channel order @p order
/// (a permutation of "AETR": Aileron = roll, Elevator = pitch, Throttle, Rudder = yaw), e.g.
/// "AETR" or "TAER". Switches default to axes 4 (arm) and 5 (flight mode). Fails on a bad
/// order string.
[[nodiscard]] common::Result<InputMapping> radioMapping(std::string_view order);

/// @brief Mapping for a gamepad (SDL standard layout: left X/Y = axes 0/1, right X/Y = 2/3)
/// in stick mode @p mode (1–4; mode 2 = throttle and yaw on the left stick). Arm on the
/// "start" button (6), flight mode cycles on the shoulder buttons.
[[nodiscard]] common::Result<InputMapping> gamepadMapping(int mode);

/// @brief Ground mapping for an RC transmitter in joystick mode with the channel order
/// @p order (as radioMapping()): steering on the aileron stick, forward/reverse on the
/// self-centring elevator stick; arm on axis 4, drive mode on axis 5 (low and middle Manual,
/// high Speed control).
[[nodiscard]] common::Result<InputMapping> groundRadioMapping(std::string_view order);

/// @brief Ground mapping for a gamepad (SDL standard layout): with @p triggers the right
/// trigger drives forward and the left one reverses, steering on the left stick; otherwise
/// forward/reverse on the left stick and steering on the right stick. South button (A) brakes
/// while held, start arms, the right shoulder switches Manual / Speed control, the north
/// button (Y) toggles the lights.
[[nodiscard]] InputMapping groundGamepadMapping(bool triggers);

/// @brief Applies the calibration of one channel to a raw axis value: −1 … 1 for sticks,
/// 0 … 1 for the throttle.
[[nodiscard]] double calibrateAxis(const AxisCalibration& calibration, double raw, bool throttle,
                                   bool throttleCentred) noexcept;

/// @brief Applies the calibration of a trigger to a raw axis value: 0 at @c min (released),
/// 1 at @c max (pressed), with the deadzone at the released end; 0 if unassigned.
[[nodiscard]] double calibrateTrigger(const AxisCalibration& calibration, double raw) noexcept;

/// @brief Turns a raw device state into controls. @p previousRaw and @p previous are the
/// device state and controls of the previous poll (for toggle buttons). An aerial mapping
/// fills the stick fields, a ground mapping the drive, steering and brake fields.
[[nodiscard]] ControlInputs mapControls(const InputMapping& mapping, const RawJoystickState& raw,
                                        const RawJoystickState& previousRaw,
                                        const ControlInputs& previous);

}  // namespace ds::input
