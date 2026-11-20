// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the RC input (FR-INP-*): axis calibration, mappings, switches, keyboard
// control, calibration wizard and the SDL joystick service with a virtual device.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ds/input/calibration.hpp"
#include "ds/input/input_selector.hpp"
#include "ds/input/joystick_service.hpp"
#include "ds/input/keyboard.hpp"
#include "ds/input/mapping.hpp"

namespace {

using namespace ds::input;

RawJoystickState rawState(std::vector<double> axes, std::vector<bool> buttons = {}) {
    RawJoystickState r;
    r.axes = std::move(axes);
    r.buttons = std::move(buttons);
    r.connected = true;
    return r;
}

// --- Axis calibration ------------------------------------------------------------------------

TEST(AxisCalibration, CentredAxisDeadzoneExpoAndInversion) {
    AxisCalibration c{.axis = 0, .min = -0.8, .center = 0.1, .max = 0.9, .deadzone = 0.0};
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 0.1, false, false), 0.0);
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 0.9, false, false), 1.0);
    EXPECT_DOUBLE_EQ(calibrateAxis(c, -0.8, false, false), -1.0);
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 0.5, false, false), 0.5);  // Asymmetric halves.
    EXPECT_DOUBLE_EQ(calibrateAxis(c, -0.35, false, false), -0.5);
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 5.0, false, false), 1.0);  // Clamped.
    c.inverted = true;
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 0.9, false, false), -1.0);
    c.inverted = false;
    c.deadzone = 0.1;
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 0.15, false, false), 0.0);
    EXPECT_NEAR(calibrateAxis(c, 0.9, false, false), 1.0, 1e-12);  // Full travel kept.
    c.deadzone = 0.0;
    c.expo = 1.0;
    EXPECT_NEAR(calibrateAxis(c, 0.5, false, false), 0.125, 1e-12);
    c.axis = -1;
    EXPECT_DOUBLE_EQ(calibrateAxis(c, 0.9, false, false), 0.0);  // Unassigned.
}

TEST(AxisCalibration, Throttle) {
    // Radio throttle: full travel → 0 … 1, no deadzone.
    const AxisCalibration t{.axis = 2, .min = -1.0, .center = 0.0, .max = 1.0, .deadzone = 0.0};
    EXPECT_DOUBLE_EQ(calibrateAxis(t, -1.0, true, false), 0.0);
    EXPECT_DOUBLE_EQ(calibrateAxis(t, 0.0, true, false), 0.5);
    EXPECT_DOUBLE_EQ(calibrateAxis(t, 1.0, true, false), 1.0);
    // Gamepad (spring-centred) throttle: centre → 0.5.
    const AxisCalibration g{.axis = 1, .inverted = true, .deadzone = 0.05};
    EXPECT_DOUBLE_EQ(calibrateAxis(g, 0.0, true, true), 0.5);
    EXPECT_DOUBLE_EQ(calibrateAxis(g, -1.0, true, true), 1.0);  // Stick up.
    EXPECT_DOUBLE_EQ(calibrateAxis(g, 1.0, true, true), 0.0);
}

// --- Mappings --------------------------------------------------------------------------------

TEST(InputMapping, RadioChannelOrders) {
    const auto aetr = radioMapping("aetr");
    ASSERT_TRUE(aetr.has_value());
    EXPECT_EQ(aetr->channels[static_cast<std::size_t>(Channel::Roll)].axis, 0);
    EXPECT_EQ(aetr->channels[static_cast<std::size_t>(Channel::Pitch)].axis, 1);
    EXPECT_EQ(aetr->channels[static_cast<std::size_t>(Channel::Throttle)].axis, 2);
    EXPECT_EQ(aetr->channels[static_cast<std::size_t>(Channel::Yaw)].axis, 3);
    const auto taer = radioMapping("TAER");
    ASSERT_TRUE(taer.has_value());
    EXPECT_EQ(taer->channels[static_cast<std::size_t>(Channel::Throttle)].axis, 0);
    EXPECT_FALSE(radioMapping("AETT").has_value());
    EXPECT_FALSE(radioMapping("AET").has_value());

    // Sticks and switches: arm on axis 4, three-position mode switch on axis 5.
    const auto out = mapControls(*aetr, rawState({0.5, -1.0, 1.0, 0.0, 1.0, 0.0}), {}, {});
    EXPECT_DOUBLE_EQ(out.roll, (0.5 - 0.02) / 0.98);  // Default 2 % deadzone.
    EXPECT_DOUBLE_EQ(out.pitch, -1.0);
    EXPECT_DOUBLE_EQ(out.throttle, 1.0);
    EXPECT_TRUE(out.arm);
    EXPECT_EQ(out.flightMode, 1);
    EXPECT_EQ(out.source, InputSource::Joystick);
    EXPECT_EQ(mapControls(*aetr, rawState({0, 0, 0, 0, -1.0, -1.0}), {}, {}).flightMode, 0);
    EXPECT_EQ(mapControls(*aetr, rawState({0, 0, 0, 0, -1.0, 1.0}), {}, {}).flightMode, 2);
    EXPECT_FALSE(mapControls(*aetr, rawState({0, 0, 0, 0, -1.0, 1.0}), {}, {}).arm);
}

TEST(InputMapping, GamepadModesAndToggleButtons) {
    EXPECT_FALSE(gamepadMapping(0).has_value());
    EXPECT_FALSE(gamepadMapping(5).has_value());
    const auto mode2 = gamepadMapping(2);
    ASSERT_TRUE(mode2.has_value());
    EXPECT_TRUE(mode2->throttleCentred);
    // Mode 2: left stick throttle (axis 1) + yaw (axis 0); right stick roll (2) + pitch (3).
    std::vector<double> axes{0.0, -1.0, 0.0, 0.0};
    std::vector<bool> released(16, false);
    std::vector<bool> armPressed = released;
    armPressed[6] = true;
    std::vector<bool> modePressed = released;
    modePressed[10] = true;

    ControlInputs s = mapControls(*mode2, rawState(axes, released), {}, {});
    EXPECT_DOUBLE_EQ(s.throttle, 1.0);
    EXPECT_FALSE(s.arm);
    // A press toggles once; holding the button does not toggle again.
    const RawJoystickState up = rawState(axes, released);
    const RawJoystickState down = rawState(axes, armPressed);
    s = mapControls(*mode2, down, up, s);
    EXPECT_TRUE(s.arm);
    s = mapControls(*mode2, down, down, s);
    EXPECT_TRUE(s.arm);
    s = mapControls(*mode2, up, down, s);
    s = mapControls(*mode2, down, up, s);
    EXPECT_FALSE(s.arm);
    // The mode button cycles Angle → Altitude hold → Acro → Angle.
    const RawJoystickState mode = rawState(axes, modePressed);
    std::vector<int> seen;
    for (int i = 0; i < 3; ++i) {
        s = mapControls(*mode2, mode, up, s);
        seen.push_back(s.flightMode);
    }
    EXPECT_EQ(seen, (std::vector<int>{2, 0, 1}));
}

TEST(InputMapping, DisconnectKeepsSticksAndReportsLinkLoss) {
    const auto m = radioMapping("AETR");
    ControlInputs s = mapControls(*m, rawState({0.0, 0.0, 0.2, 0.0, 1.0, 0.0}), {}, {});
    EXPECT_TRUE(s.linkValid);
    RawJoystickState gone;
    gone.connected = false;
    const ControlInputs lost = mapControls(*m, gone, {}, s);
    EXPECT_FALSE(lost.linkValid);
    EXPECT_EQ(lost.throttle, s.throttle);
    EXPECT_TRUE(lost.arm);
}

TEST(InputMapping, JsonRoundTripAndValidation) {
    auto m = gamepadMapping(1);
    ASSERT_TRUE(m.has_value());
    m->deviceGuid = "03000000de280000ff11000001000000";
    m->deviceName = "Test pad";
    m->filterHz = 30.0;
    const auto back = InputMapping::fromJson(m->toJson());
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, *m);

    auto json = m->toJson();
    json["switches"][0]["function"] = "selfDestruct";
    EXPECT_FALSE(InputMapping::fromJson(json).has_value());
    json = m->toJson();
    json["channels"]["roll"]["deadzone"] = 0.9;
    EXPECT_FALSE(InputMapping::fromJson(json).has_value());
    EXPECT_FALSE(InputMapping::fromJson(ds::common::Json::array()).has_value());
}

// --- Keyboard --------------------------------------------------------------------------------

constexpr auto key(FlightKey k) { return static_cast<std::uint32_t>(k); }

TEST(AxisCalibration, Trigger) {
    const AxisCalibration t{.axis = 5, .min = 0.0, .center = 0.0, .max = 1.0, .deadzone = 0.1};
    EXPECT_DOUBLE_EQ(calibrateTrigger(t, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(calibrateTrigger(t, 0.08), 0.0);  // Resting trigger inside the deadzone.
    EXPECT_NEAR(calibrateTrigger(t, 0.55), 0.5, 1e-12);
    EXPECT_DOUBLE_EQ(calibrateTrigger(t, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(calibrateTrigger(t, 2.0), 1.0);  // Clamped.
    // A trigger reported as −1 (released) … 1 (pressed) by a raw joystick.
    AxisCalibration raw{.axis = 2, .min = -1.0, .center = -1.0, .max = 1.0, .deadzone = 0.0};
    EXPECT_DOUBLE_EQ(calibrateTrigger(raw, -1.0), 0.0);
    EXPECT_DOUBLE_EQ(calibrateTrigger(raw, 0.0), 0.5);
    raw.inverted = true;
    EXPECT_DOUBLE_EQ(calibrateTrigger(raw, -1.0), 1.0);
    raw.axis = -1;
    EXPECT_DOUBLE_EQ(calibrateTrigger(raw, 1.0), 0.0);  // Unassigned.
}

TEST(GroundMapping, GamepadTriggersStickAndButtons) {
    const InputMapping m = groundGamepadMapping(true);
    EXPECT_EQ(m.vehicle, VehicleControls::Ground);
    // Axes: left X, left Y, right X, right Y, left trigger, right trigger.
    ControlInputs c = mapControls(m, rawState({0.0, 0.0, 0.0, 0.0, 0.0, 1.0}), {}, {});
    EXPECT_DOUBLE_EQ(c.drive, 1.0);
    EXPECT_DOUBLE_EQ(c.throttle, 0.0);  // Ground mappings leave the aerial sticks alone.
    c = mapControls(m, rawState({-1.0, 0.0, 0.0, 0.0, 1.0, 0.0}), {}, {});
    EXPECT_DOUBLE_EQ(c.drive, -1.0);
    EXPECT_DOUBLE_EQ(c.steering, -1.0);
    c = mapControls(m, rawState({0.0, 0.0, 0.0, 0.0, 1.0, 1.0}), {}, {});
    EXPECT_DOUBLE_EQ(c.drive, 0.0);  // Both triggers cancel.

    // South button brakes while held; start toggles the arm switch; the right shoulder
    // switches Manual (1) and Speed control (2).
    std::vector<bool> none(11, false);
    std::vector<bool> south = none;
    south[0] = true;
    EXPECT_TRUE(mapControls(m, rawState({0, 0, 0, 0, 0, 0}, south), {}, {}).brake);
    EXPECT_FALSE(mapControls(m, rawState({0, 0, 0, 0, 0, 0}, none), {}, {}).brake);
    std::vector<bool> shoulder = none;
    shoulder[10] = true;
    const RawJoystickState up = rawState({0, 0, 0, 0, 0, 0}, none);
    const RawJoystickState down = rawState({0, 0, 0, 0, 0, 0}, shoulder);
    c = mapControls(m, up, {}, {});
    EXPECT_EQ(c.flightMode, 1);
    c = mapControls(m, down, up, c);
    EXPECT_EQ(c.flightMode, 2);
    c = mapControls(m, up, down, c);
    c = mapControls(m, down, up, c);
    EXPECT_EQ(c.flightMode, 1);

    const InputMapping sticks = groundGamepadMapping(false);
    c = mapControls(sticks, rawState({0.0, -1.0, 0.5, 0.0, 0.0, 0.0}), {}, {});
    EXPECT_DOUBLE_EQ(c.drive, 1.0);  // Left stick up.
    EXPECT_GT(c.steering, 0.0);      // Right stick right.
}

TEST(GroundMapping, RadioChannelOrder) {
    EXPECT_FALSE(groundRadioMapping("AETX").has_value());
    const auto m = groundRadioMapping("TAER");
    ASSERT_TRUE(m.has_value());
    // T A E R: aileron (steering) on axis 1, elevator (drive) on axis 2.
    const ControlInputs c = mapControls(*m, rawState({-1.0, 0.5, 1.0, 0.0, 1.0, 1.0}), {}, {});
    EXPECT_DOUBLE_EQ(c.drive, 1.0);
    EXPECT_GT(c.steering, 0.4);
    EXPECT_TRUE(c.arm);
    EXPECT_EQ(c.flightMode, 2);  // Mode switch high: Speed control.
    EXPECT_EQ(mapControls(*m, rawState({0, 0, 0, 0, -1.0, 0.0}), {}, {}).flightMode, 1);
}

TEST(GroundMapping, JsonKeepsTheVehicleType) {
    InputMapping m = groundGamepadMapping(true);
    m.deviceGuid = "0300abcd";
    const ds::common::Json json = m.toJson();
    EXPECT_EQ(json["vehicleType"], "ground");
    EXPECT_TRUE(json["channels"].contains("forward"));
    EXPECT_TRUE(json["channels"].contains("steering"));
    const auto back = InputMapping::fromJson(json);
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, m);

    // Mappings saved before ground drones existed have no type: aerial.
    auto old = gamepadMapping(2)->toJson();
    old.erase("vehicleType");
    const auto aerial = InputMapping::fromJson(old);
    ASSERT_TRUE(aerial.has_value());
    EXPECT_EQ(aerial->vehicle, VehicleControls::Aerial);
    // Ground channel names do not fit an aerial mapping.
    auto mixed = json;
    mixed["vehicleType"] = "aerial";
    EXPECT_FALSE(InputMapping::fromJson(mixed).has_value());
    mixed["vehicleType"] = "boat";
    EXPECT_FALSE(InputMapping::fromJson(mixed).has_value());
}

TEST(KeyboardControl, GroundDriveSpringsBackAndBrakes) {
    KeyboardControl k;
    ControlInputs c;
    for (int i = 0; i < 100; ++i) {
        c = k.update(static_cast<std::uint32_t>(FlightKey::ThrottleUp) |
                         static_cast<std::uint32_t>(FlightKey::YawLeft),
                     0.01);
    }
    EXPECT_DOUBLE_EQ(c.drive, KeyboardControl::kDriveDeflection);
    EXPECT_DOUBLE_EQ(c.steering, -KeyboardControl::kSteeringDeflection);
    EXPECT_FALSE(c.brake);
    // Released: back to neutral (not sticky like the aerial throttle).
    for (int i = 0; i < 100; ++i) {
        c = k.update(static_cast<std::uint32_t>(FlightKey::Brake), 0.01);
    }
    EXPECT_DOUBLE_EQ(c.drive, 0.0);
    EXPECT_DOUBLE_EQ(c.steering, 0.0);
    EXPECT_TRUE(c.brake);
    // The arrows drive and steer too.
    for (int i = 0; i < 100; ++i) {
        c = k.update(static_cast<std::uint32_t>(FlightKey::PitchBack) |
                         static_cast<std::uint32_t>(FlightKey::RollRight),
                     0.01);
    }
    EXPECT_DOUBLE_EQ(c.drive, -KeyboardControl::kDriveDeflection);
    EXPECT_DOUBLE_EQ(c.steering, KeyboardControl::kSteeringDeflection);
}

TEST(KeyboardControl, SticksRampAndReturnToCentre) {
    KeyboardControl kb;
    ControlInputs s;
    for (int i = 0; i < 100; ++i) {
        s = kb.update(key(FlightKey::RollRight) | key(FlightKey::PitchForward), 0.01);
    }
    EXPECT_DOUBLE_EQ(s.roll, KeyboardControl::kDeflection);
    EXPECT_DOUBLE_EQ(s.pitch, KeyboardControl::kDeflection);
    EXPECT_EQ(s.source, InputSource::Keyboard);
    s = kb.update(0, 0.05);
    EXPECT_NEAR(s.roll, KeyboardControl::kDeflection - KeyboardControl::kStickRate * 0.05, 1e-12);
    for (int i = 0; i < 100; ++i) {
        s = kb.update(key(FlightKey::YawLeft), 0.01);
    }
    EXPECT_DOUBLE_EQ(s.roll, 0.0);
    EXPECT_DOUBLE_EQ(s.yaw, -KeyboardControl::kDeflection);
}

TEST(KeyboardControl, ThrottleIsStickyInRateModesAndCentredInAltitudeHold) {
    KeyboardControl kb;
    kb.setFlightMode(1);
    ControlInputs s;
    for (int i = 0; i < 100; ++i) {
        s = kb.update(key(FlightKey::ThrottleUp), 0.01);
    }
    EXPECT_NEAR(s.throttle, KeyboardControl::kThrottleRate, 1e-9);
    s = kb.update(0, 0.5);
    EXPECT_NEAR(s.throttle, KeyboardControl::kThrottleRate, 1e-9);  // Stays.
    kb.setFlightMode(2);
    EXPECT_DOUBLE_EQ(kb.state().throttle, 0.5);
    for (int i = 0; i < 100; ++i) {
        s = kb.update(key(FlightKey::ThrottleUp), 0.01);
    }
    EXPECT_DOUBLE_EQ(s.throttle, 1.0);
    for (int i = 0; i < 100; ++i) {
        s = kb.update(0, 0.01);
    }
    EXPECT_DOUBLE_EQ(s.throttle, 0.5);  // Spring-centred: hold altitude.
    kb.toggleArm();
    EXPECT_TRUE(kb.state().arm);
    kb.reset();
    EXPECT_FALSE(kb.state().arm);
    EXPECT_EQ(kb.state().flightMode, 2);
}

TEST(InputSelector, KeyboardActionsAndDisabledKeyboard) {
    InputSelector selector;
    selector.keyboardAction(KeyboardAction::Mode1);
    selector.keyboardAction(KeyboardAction::ToggleArm);
    ControlInputs s = selector.poll(0.01);
    EXPECT_EQ(s.flightMode, 0);
    EXPECT_TRUE(s.arm);
    EXPECT_EQ(selector.activeSource(), InputSource::Keyboard);
    selector.keyboardAction(KeyboardAction::Disarm);
    EXPECT_FALSE(selector.poll(0.01).arm);
    selector.setKeyboardAllowed(false);
    selector.setKeys(key(FlightKey::RollRight));
    s = selector.poll(0.5);
    EXPECT_FALSE(s.linkValid);
    EXPECT_DOUBLE_EQ(s.roll, 0.0);
}

// --- Calibration wizard ----------------------------------------------------------------------

TEST(CalibrationWizard, AssignsAxesFromGuidedMoves) {
    // Device: throttle on axis 3 (−1 low, reversed), yaw 0, pitch 2 (forward = negative),
    // roll 1, plus an idle axis 4.
    CalibrationWizard w(5, InputMapping{});
    EXPECT_EQ(w.step(), CalibrationWizard::Step::CenterSticks);
    EXPECT_FALSE(w.prompt().empty());
    w.sample(rawState({0.02, -0.01, 0.0, 0.95, 0.0}));
    ASSERT_TRUE(w.next());
    // Not moved yet: refused with a message.
    w.sample(rawState({0.02, -0.01, 0.0, 0.95, 0.0}));
    EXPECT_FALSE(w.next());
    EXPECT_FALSE(w.error().empty());
    for (const double v : {-1.0, 1.0}) {
        w.sample(rawState({v, v, v, v, 0.0}));
    }
    ASSERT_TRUE(w.next());
    EXPECT_EQ(w.step(), CalibrationWizard::Step::ThrottleUp);
    w.sample(rawState({0.0, 0.0, 0.0, -1.0, 0.0}));
    EXPECT_EQ(w.mostDeflectedAxis(), 3);
    ASSERT_TRUE(w.next());
    w.sample(rawState({1.0, 0.0, 0.0, 0.95, 0.0}));
    ASSERT_TRUE(w.next());
    w.sample(rawState({0.0, 0.0, -1.0, 0.95, 0.0}));
    ASSERT_TRUE(w.next());
    // Roll: nothing moved → refused, then moved.
    w.sample(rawState({0.0, 0.0, 0.0, 0.95, 0.0}));
    EXPECT_FALSE(w.next());
    w.sample(rawState({0.0, 1.0, 0.0, 0.95, 0.0}));
    ASSERT_TRUE(w.next());
    ASSERT_EQ(w.step(), CalibrationWizard::Step::Done);

    const InputMapping& m = w.result();
    EXPECT_EQ(m.channels[static_cast<std::size_t>(Channel::Throttle)].axis, 3);
    EXPECT_EQ(m.channels[static_cast<std::size_t>(Channel::Yaw)].axis, 0);
    EXPECT_EQ(m.channels[static_cast<std::size_t>(Channel::Pitch)].axis, 2);
    EXPECT_EQ(m.channels[static_cast<std::size_t>(Channel::Roll)].axis, 1);
    // The calibrated mapping reproduces the guided moves.
    const auto at = [&](std::vector<double> axes) {
        return mapControls(m, rawState(axes), {}, {});
    };
    EXPECT_NEAR(at({0.02, -0.01, 0.0, -1.0, 0.0}).throttle, 1.0, 1e-9);
    EXPECT_NEAR(at({0.02, -0.01, 0.0, 1.0, 0.0}).throttle, 0.0, 1e-9);
    EXPECT_NEAR(at({1.0, -0.01, 0.0, 0.95, 0.0}).yaw, 1.0, 1e-9);
    EXPECT_NEAR(at({0.02, -0.01, -1.0, 0.95, 0.0}).pitch, 1.0, 1e-9);
    EXPECT_NEAR(at({0.02, 1.0, 0.0, 0.95, 0.0}).roll, 1.0, 1e-9);
    EXPECT_NEAR(at({0.02, -0.01, 0.0, 0.95, 0.0}).roll, 0.0, 1e-9);
}

namespace {

InputMapping groundBase() {
    InputMapping m = groundGamepadMapping(true);
    m.name = "Calibrated";
    return m;
}

std::size_t slot(GroundChannel ch) { return static_cast<std::size_t>(ch); }

}  // namespace

TEST(CalibrationWizard, GroundDriveStickAndSteering) {
    // Pistol-grip radio or gamepad sticks: drive on axis 1 (forward = negative), steering on
    // axis 2, both self-centring; axis 0 idle.
    CalibrationWizard w(3, groundBase());
    w.sample(rawState({0.0, 0.01, -0.02}));
    ASSERT_TRUE(w.next());
    for (const double v : {-1.0, 1.0}) {
        w.sample(rawState({0.0, v, v}));
    }
    ASSERT_TRUE(w.next());
    EXPECT_EQ(w.step(), CalibrationWizard::Step::DriveForward);
    EXPECT_NE(w.prompt().find("forward"), std::string_view::npos);
    w.sample(rawState({0.0, -1.0, -0.02}));
    ASSERT_TRUE(w.next());
    EXPECT_EQ(w.step(), CalibrationWizard::Step::DriveReverse);
    // Reverse must be the same stick pulled back: still forward is refused.
    EXPECT_FALSE(w.next());
    EXPECT_FALSE(w.error().empty());
    w.sample(rawState({0.0, 1.0, -0.02}));
    ASSERT_TRUE(w.next());
    EXPECT_EQ(w.step(), CalibrationWizard::Step::SteerRight);
    w.sample(rawState({0.0, 0.01, 1.0}));
    ASSERT_TRUE(w.next());
    ASSERT_EQ(w.step(), CalibrationWizard::Step::Done);

    const InputMapping& m = w.result();
    EXPECT_EQ(m.vehicle, VehicleControls::Ground);
    EXPECT_EQ(m.name, "Calibrated");
    EXPECT_EQ(m.channels[slot(GroundChannel::Drive)].axis, 1);
    EXPECT_EQ(m.channels[slot(GroundChannel::Steering)].axis, 2);
    // The preset's triggers are dropped: the stick drives.
    EXPECT_EQ(m.channels[slot(GroundChannel::Forward)].axis, -1);
    EXPECT_EQ(m.channels[slot(GroundChannel::Reverse)].axis, -1);
    // The switches of the base mapping are kept.
    EXPECT_EQ(m.switches, groundBase().switches);
    const auto at = [&](std::vector<double> axes) {
        return mapControls(m, rawState(axes), {}, {});
    };
    EXPECT_NEAR(at({0.0, -1.0, -0.02}).drive, 1.0, 1e-9);
    EXPECT_NEAR(at({0.0, 1.0, -0.02}).drive, -1.0, 1e-9);
    EXPECT_NEAR(at({0.0, 0.01, -0.02}).drive, 0.0, 1e-9);
    EXPECT_NEAR(at({0.0, 0.01, 1.0}).steering, 1.0, 1e-9);
    EXPECT_NEAR(at({0.0, 0.01, -1.0}).steering, -1.0, 1e-9);
    EXPECT_NEAR(at({0.0, 0.01, -0.02}).steering, 0.0, 1e-9);
}

TEST(CalibrationWizard, GroundTriggerPairAndSteeringStick) {
    // Gamepad: steering on axis 0 (reversed: right = negative), triggers on axes 4 (reverse)
    // and 5 (forward) resting at −1, a raw joystick API reporting −1 … 1.
    CalibrationWizard w(6, InputMapping{.vehicle = VehicleControls::Ground});
    w.sample(rawState({0.0, 0.0, 0.0, 0.0, -1.0, -1.0}));
    ASSERT_TRUE(w.next());
    for (const double v : {-1.0, 1.0}) {
        w.sample(rawState({v, v, v, v, v, v}));
    }
    ASSERT_TRUE(w.next());
    w.sample(rawState({0.0, 0.0, 0.0, 0.0, -1.0, 1.0}));
    ASSERT_TRUE(w.next());
    EXPECT_NE(w.prompt().find("trigger"), std::string_view::npos);
    // Nothing pressed for reverse; then a stick instead of a trigger: both refused.
    w.sample(rawState({0.0, 0.0, 0.0, 0.0, -1.0, -1.0}));
    EXPECT_FALSE(w.next());
    w.sample(rawState({0.0, 0.0, 0.0, 1.0, -1.0, -1.0}));
    EXPECT_FALSE(w.next());
    w.sample(rawState({0.0, 0.0, 0.0, 0.0, 1.0, -1.0}));
    ASSERT_TRUE(w.next());
    // Steering right on a stick wired reversed.
    w.sample(rawState({-1.0, 0.0, 0.0, 0.0, -1.0, -1.0}));
    ASSERT_TRUE(w.next());
    ASSERT_EQ(w.step(), CalibrationWizard::Step::Done);

    const InputMapping& m = w.result();
    EXPECT_EQ(m.channels[slot(GroundChannel::Drive)].axis, -1);
    EXPECT_EQ(m.channels[slot(GroundChannel::Forward)].axis, 5);
    EXPECT_EQ(m.channels[slot(GroundChannel::Reverse)].axis, 4);
    EXPECT_EQ(m.channels[slot(GroundChannel::Steering)].axis, 0);
    const auto at = [&](std::vector<double> axes) {
        return mapControls(m, rawState(axes), {}, {});
    };
    EXPECT_NEAR(at({0.0, 0.0, 0.0, 0.0, -1.0, -1.0}).drive, 0.0, 1e-9);
    EXPECT_NEAR(at({0.0, 0.0, 0.0, 0.0, -1.0, 1.0}).drive, 1.0, 1e-9);
    EXPECT_NEAR(at({0.0, 0.0, 0.0, 0.0, 1.0, -1.0}).drive, -1.0, 1e-9);
    EXPECT_GT(at({0.0, 0.0, 0.0, 0.0, -1.0, 0.0}).drive, 0.4);
    EXPECT_NEAR(at({-1.0, 0.0, 0.0, 0.0, -1.0, -1.0}).steering, 1.0, 1e-9);
    EXPECT_NEAR(at({1.0, 0.0, 0.0, 0.0, -1.0, -1.0}).steering, -1.0, 1e-9);
}

TEST(CalibrationWizard, GroundSteeringOnATriggerIsRefused) {
    // Forward on a trigger, reverse on the other, then the user presses a third
    // trigger-like axis (a throttle lever resting at an end) to steer.
    CalibrationWizard w(4, InputMapping{.vehicle = VehicleControls::Ground});
    w.sample(rawState({0.0, -1.0, -1.0, -1.0}));
    ASSERT_TRUE(w.next());
    for (const double v : {-1.0, 1.0}) {
        w.sample(rawState({v, v, v, v}));
    }
    ASSERT_TRUE(w.next());
    w.sample(rawState({0.0, 1.0, -1.0, -1.0}));
    ASSERT_TRUE(w.next());
    w.sample(rawState({0.0, -1.0, 1.0, -1.0}));
    ASSERT_TRUE(w.next());
    w.sample(rawState({0.0, -1.0, -1.0, 1.0}));
    EXPECT_FALSE(w.next());
    EXPECT_NE(w.error().find("trigger"), std::string_view::npos);
    w.sample(rawState({1.0, -1.0, -1.0, -1.0}));
    ASSERT_TRUE(w.next());
    EXPECT_EQ(w.result().channels[slot(GroundChannel::Steering)].axis, 0);
}

// --- SDL joystick service --------------------------------------------------------------------

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

TEST(JoystickService, VirtualDeviceDrivesTheSelector) {
    JoystickService service;
    const auto started = service.start();
    if (!started) {
        GTEST_SKIP() << "SDL joystick subsystem unavailable: " << started.error().message;
    }
    const auto guid = service.attachVirtualDevice("Virtual radio", 6, 4);
    ASSERT_TRUE(guid.has_value()) << guid.error().message;
    ASSERT_TRUE(waitFor([&] { return !service.devices().empty(); }));
    const auto devices = service.devices();
    const auto it = std::ranges::find(devices, *guid, &DeviceInfo::guid);
    ASSERT_NE(it, devices.end());
    EXPECT_EQ(it->axes, 6);
    EXPECT_EQ(it->buttons, 4);

    auto mapping = radioMapping("AETR");
    ASSERT_TRUE(mapping.has_value());
    mapping->deviceGuid = *guid;
    InputSelector selector(&service);
    selector.setJoystick(*mapping);
    service.setVirtualAxis(*guid, 0, 1.0);   // Full right roll.
    service.setVirtualAxis(*guid, 2, -1.0);  // Throttle low.
    service.setVirtualAxis(*guid, 4, 1.0);   // Arm switch on.
    ASSERT_TRUE(waitFor([&] {
        const ControlInputs s = selector.poll(0.002);
        return s.roll > 0.95 && s.arm && s.throttle < 0.05;
    }));
    EXPECT_EQ(selector.activeSource(), InputSource::Joystick);

    service.detachVirtualDevice(*guid);
    ASSERT_TRUE(waitFor([&] { return !selector.poll(0.002).linkValid; }));
    service.stop();
    EXPECT_FALSE(service.running());
}

}  // namespace
