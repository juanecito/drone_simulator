// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/input/mapping.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace ds::input {
namespace {

constexpr std::array<SwitchFunction, 7> kFunctions{
    SwitchFunction::Arm,          SwitchFunction::FlightMode,     SwitchFunction::Beeper,
    SwitchFunction::ReturnToHome, SwitchFunction::PayloadRelease, SwitchFunction::CameraTrigger,
    SwitchFunction::Lights};

double rawAxis(const RawJoystickState& raw, int axis) noexcept {
    if (axis < 0 || static_cast<std::size_t>(axis) >= raw.axes.size()) {
        return 0.0;
    }
    return raw.axes[static_cast<std::size_t>(axis)];
}

bool rawButton(const RawJoystickState& raw, int button) noexcept {
    if (button < 0 || static_cast<std::size_t>(button) >= raw.buttons.size()) {
        return false;
    }
    return raw.buttons[static_cast<std::size_t>(button)];
}

}  // namespace

std::string_view channelName(Channel channel) noexcept {
    switch (channel) {
        case Channel::Throttle: return "throttle";
        case Channel::Roll: return "roll";
        case Channel::Pitch: return "pitch";
        case Channel::Yaw: return "yaw";
    }
    return "?";
}

std::string_view switchFunctionName(SwitchFunction function) noexcept {
    switch (function) {
        case SwitchFunction::Arm: return "arm";
        case SwitchFunction::FlightMode: return "flightMode";
        case SwitchFunction::Beeper: return "beeper";
        case SwitchFunction::ReturnToHome: return "returnToHome";
        case SwitchFunction::PayloadRelease: return "payloadRelease";
        case SwitchFunction::CameraTrigger: return "cameraTrigger";
        case SwitchFunction::Lights: return "lights";
    }
    return "?";
}

double calibrateAxis(const AxisCalibration& c, double raw, bool throttle,
                     bool throttleCentred) noexcept {
    if (c.axis < 0) {
        return 0.0;
    }
    double x = 0.0;
    if (throttle && !throttleCentred) {
        // Full travel: min → 0, max → 1 (the throttle stick has no centre spring).
        const double span = c.max - c.min;
        x = std::abs(span) > 1e-9 ? 2.0 * (raw - c.min) / span - 1.0 : 0.0;
    } else {
        // Two halves around the centre, so an off-centre spring still gives 0.
        const double upper = c.max - c.center;
        const double lower = c.center - c.min;
        if (raw >= c.center) {
            x = std::abs(upper) > 1e-9 ? (raw - c.center) / upper : 0.0;
        } else {
            x = std::abs(lower) > 1e-9 ? (raw - c.center) / lower : 0.0;
        }
    }
    x = std::clamp(x, -1.0, 1.0);
    if (c.inverted) {
        x = -x;
    }
    if (!(throttle && !throttleCentred)) {
        const double dz = std::clamp(c.deadzone, 0.0, 0.5);
        x = std::abs(x) <= dz ? 0.0 : std::copysign((std::abs(x) - dz) / (1.0 - dz), x);
        const double e = std::clamp(c.expo, 0.0, 1.0);
        x = (1.0 - e) * x + e * x * x * x;
    }
    return throttle ? 0.5 * (x + 1.0) : x;
}

ControlInputs mapControls(const InputMapping& m, const RawJoystickState& raw,
                          const RawJoystickState& previousRaw, const ControlInputs& previous) {
    ControlInputs out = previous;
    out.source = InputSource::Joystick;
    out.linkValid = raw.connected;
    if (!raw.connected) {
        return out;
    }
    auto channel = [&](Channel ch) {
        const AxisCalibration& c = m.channels[static_cast<std::size_t>(ch)];
        return calibrateAxis(c, rawAxis(raw, c.axis), ch == Channel::Throttle, m.throttleCentred);
    };
    out.throttle = channel(Channel::Throttle);
    out.roll = channel(Channel::Roll);
    out.pitch = channel(Channel::Pitch);
    out.yaw = channel(Channel::Yaw);
    for (const SwitchMapping& s : m.switches) {
        bool on = false;
        int position = 0;  // 0 low, 1 middle, 2 high.
        if (s.button >= 0 && s.toggle) {
            const bool pressed = rawButton(raw, s.button) && !rawButton(previousRaw, s.button);
            if (!pressed) {
                continue;
            }
            switch (s.function) {
                case SwitchFunction::Arm: out.arm = !out.arm; break;
                case SwitchFunction::Lights: out.lights = !out.lights; break;
                case SwitchFunction::Beeper: out.beeper = !out.beeper; break;
                case SwitchFunction::ReturnToHome: out.returnToHome = !out.returnToHome; break;
                case SwitchFunction::FlightMode: {
                    const auto it = std::ranges::find(s.modes, out.flightMode);
                    const auto index = it == s.modes.end() ? 0 : (it - s.modes.begin() + 1) % 3;
                    out.flightMode = std::clamp(s.modes[static_cast<std::size_t>(index)], 0,
                                                kFlightModeCount - 1);
                    break;
                }
                case SwitchFunction::PayloadRelease:
                case SwitchFunction::CameraTrigger: break;
            }
            continue;
        }
        if (s.button >= 0) {
            on = rawButton(raw, s.button) != s.inverted;
            position = on ? 2 : 0;
        } else if (s.axis >= 0) {
            const double v = (s.inverted ? -1.0 : 1.0) * rawAxis(raw, s.axis);
            on = v > 0.0;
            position = v < -0.33 ? 0 : (v > 0.33 ? 2 : 1);
        } else {
            continue;
        }
        switch (s.function) {
            case SwitchFunction::Arm: out.arm = on; break;
            case SwitchFunction::FlightMode:
                out.flightMode = std::clamp(s.modes[static_cast<std::size_t>(position)], 0,
                                            kFlightModeCount - 1);
                break;
            case SwitchFunction::Beeper: out.beeper = on; break;
            case SwitchFunction::ReturnToHome: out.returnToHome = on; break;
            case SwitchFunction::PayloadRelease: out.payloadRelease = on; break;
            case SwitchFunction::CameraTrigger: out.cameraTrigger = on; break;
            case SwitchFunction::Lights: out.lights = on; break;
        }
    }
    return out;
}

common::Json InputMapping::toJson() const {
    common::Json j = common::Json::object();
    j["schemaVersion"] = 1;
    j["deviceGuid"] = deviceGuid;
    j["deviceName"] = deviceName;
    j["name"] = name;
    j["throttleCentred"] = throttleCentred;
    j["filterHz"] = filterHz;
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const AxisCalibration& c = channels[i];
        j["channels"][std::string(channelName(static_cast<Channel>(i)))] = {
            {"axis", c.axis}, {"min", c.min},           {"center", c.center},
            {"max", c.max},   {"inverted", c.inverted}, {"deadzone", c.deadzone},
            {"expo", c.expo}};
    }
    j["switches"] = common::Json::array();
    for (const SwitchMapping& s : switches) {
        j["switches"].push_back({{"function", switchFunctionName(s.function)},
                                 {"button", s.button},
                                 {"axis", s.axis},
                                 {"inverted", s.inverted},
                                 {"toggle", s.toggle},
                                 {"modes", s.modes}});
    }
    return j;
}

common::Result<InputMapping> InputMapping::fromJson(const common::Json& json) {
    if (!json.is_object()) {
        return common::makeError("input mapping: the document must be a JSON object");
    }
    common::JsonReader r(json);
    InputMapping m;
    r.integer("schemaVersion", 1, 1);
    m.deviceGuid = r.string("deviceGuid", "");
    m.deviceName = r.string("deviceName", "");
    m.name = r.string("name", "Default");
    m.throttleCentred = r.boolean("throttleCentred", false);
    m.filterHz = r.number("filterHz", 0.0, 200.0, 0.0);
    common::JsonReader ch = r.object("channels");
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        common::JsonReader c = ch.object(channelName(static_cast<Channel>(i)));
        AxisCalibration& a = m.channels[i];
        a.axis = c.integer("axis", -1, 255);
        a.min = c.number("min", -1.0, 1.0);
        a.center = c.number("center", -1.0, 1.0);
        a.max = c.number("max", -1.0, 1.0);
        a.inverted = c.boolean("inverted", false);
        a.deadzone = c.number("deadzone", 0.0, 0.5, 0.02);
        a.expo = c.number("expo", 0.0, 1.0, 0.0);
    }
    const std::size_t count = r.optionalArray("switches");
    for (std::size_t i = 0; i < count; ++i) {
        common::JsonReader e = r.element("switches", i);
        SwitchMapping s;
        const std::string fn = e.string("function");
        bool found = false;
        for (const SwitchFunction f : kFunctions) {
            if (switchFunctionName(f) == fn) {
                s.function = f;
                found = true;
            }
        }
        if (!found) {
            e.fail("function", "unknown switch function");
        }
        s.button = e.integer("button", -1, 255, -1);
        s.axis = e.integer("axis", -1, 255, -1);
        s.inverted = e.boolean("inverted", false);
        s.toggle = e.boolean("toggle", false);
        const std::size_t modes = e.optionalArray("modes");
        for (std::size_t k = 0; k < std::min<std::size_t>(modes, 3); ++k) {
            s.modes[k] = static_cast<int>(
                e.element("modes", k).asNumber(0.0, static_cast<double>(kFlightModeCount - 1)));
        }
        m.switches.push_back(s);
    }
    if (!r.ok()) {
        return common::makeError("input mapping: " + r.errorText());
    }
    return m;
}

common::Result<InputMapping> radioMapping(std::string_view order) {
    std::string upper(order);
    std::ranges::transform(upper, upper.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    std::string sorted = upper;
    std::ranges::sort(sorted);
    if (sorted != "AERT") {
        return common::makeError(
            std::format("channel order \"{}\" must be a permutation of AETR", order));
    }
    InputMapping m;
    m.name = "Radio " + upper;
    for (std::size_t i = 0; i < 4; ++i) {
        const Channel ch = upper[i] == 'A'   ? Channel::Roll
                           : upper[i] == 'E' ? Channel::Pitch
                           : upper[i] == 'T' ? Channel::Throttle
                                             : Channel::Yaw;
        m.channels[static_cast<std::size_t>(ch)].axis = static_cast<int>(i);
    }
    m.channels[static_cast<std::size_t>(Channel::Throttle)].deadzone = 0.0;
    SwitchMapping arm{.function = SwitchFunction::Arm, .axis = 4};
    SwitchMapping mode{.function = SwitchFunction::FlightMode, .axis = 5, .modes = {0, 1, 2}};
    m.switches = {arm, mode};
    return m;
}

common::Result<InputMapping> gamepadMapping(int mode) {
    if (mode < 1 || mode > 4) {
        return common::makeError(std::format("stick mode {} must be 1, 2, 3 or 4", mode));
    }
    // Axes: 0 left X, 1 left Y (down = +), 2 right X, 3 right Y.
    struct Layout {
        int throttle, yaw, pitch, roll;
    };
    constexpr std::array<Layout, 4> kLayouts{{
        {3, 0, 1, 2},  // Mode 1: pitch + yaw left, throttle + roll right.
        {1, 0, 3, 2},  // Mode 2: throttle + yaw left, pitch + roll right.
        {3, 2, 1, 0},  // Mode 3: pitch + roll left, throttle + yaw right.
        {1, 2, 3, 0},  // Mode 4: throttle + roll left, pitch + yaw right.
    }};
    const Layout l = kLayouts[static_cast<std::size_t>(mode - 1)];
    InputMapping m;
    m.name = std::format("Gamepad mode {}", mode);
    m.throttleCentred = true;
    auto& t = m.channels[static_cast<std::size_t>(Channel::Throttle)];
    t.axis = l.throttle;
    t.inverted = true;  // Stick up (negative Y) = more throttle.
    t.deadzone = 0.05;
    auto& p = m.channels[static_cast<std::size_t>(Channel::Pitch)];
    p.axis = l.pitch;
    p.inverted = true;  // Stick up = forward.
    p.deadzone = 0.05;
    p.expo = 0.3;
    auto& r = m.channels[static_cast<std::size_t>(Channel::Roll)];
    r.axis = l.roll;
    r.deadzone = 0.05;
    r.expo = 0.3;
    auto& y = m.channels[static_cast<std::size_t>(Channel::Yaw)];
    y.axis = l.yaw;
    y.deadzone = 0.05;
    y.expo = 0.2;
    // SDL gamepad buttons: 6 = start, 10 = right shoulder.
    m.switches = {SwitchMapping{.function = SwitchFunction::Arm, .button = 6, .toggle = true},
                  SwitchMapping{.function = SwitchFunction::FlightMode,
                                .button = 10,
                                .toggle = true,
                                .modes = {1, 2, 0}}};
    return m;
}

}  // namespace ds::input
