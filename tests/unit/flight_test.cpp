// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the aerial vehicle (SPECIFICATION §4.6): reference profiles, derived
// performance (NFR-REAL-01), wind balance (NFR-REAL-08), flight controller, contacts, crashes
// and determinism.

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/aerial_vehicle.hpp"
#include "ds/physics/flight_controller.hpp"
#include "ds/sim/world.hpp"
#include "ds/weather/wind_levels.hpp"

namespace {

using namespace ds;
using namespace ds::physics;

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kDt = 1.0 / 500.0;
constexpr std::array<const char*, 3> kProfiles{"fpv-5in-racer", "camera-250g", "survey-1500g"};

AerialProfile loadProfile(std::string_view key) {
    const auto path = std::filesystem::path(DS_TEST_ASSETS_DIR) / "vehicles" / "aerial" /
                      (std::string(key) + ".json");
    auto profile = loadAerialProfile(path);
    EXPECT_TRUE(profile.has_value()) << key << ": " << profile.error().message;
    return profile ? *profile : AerialProfile{};
}

EnvironmentSample stillAir() {
    EnvironmentSample env;
    env.air = isaAir(0.0);
    env.ambientC = 15.0;
    return env;
}

/// Runs a world with the given controls for @p seconds.
void run(sim::World& world, const input::ControlInputs& controls, double seconds) {
    const int ticks = static_cast<int>(std::lround(seconds / kDt));
    for (int i = 0; i < ticks; ++i) {
        world.tick(controls, kDt);
    }
}

/// Arms in altitude hold and climbs (3 m/s for @p climbSeconds); returns the controls left in
/// hover.
input::ControlInputs takeOff(sim::World& world, double climbSeconds = 2.5) {
    input::ControlInputs c;
    c.flightMode = 2;  // Altitude hold.
    c.throttle = 0.5;
    run(world, c, 0.5);
    c.arm = true;
    run(world, c, 0.5);
    EXPECT_TRUE(world.vehicle().controller().armed());
    c.throttle = 1.0;
    run(world, c, climbSeconds);
    c.throttle = 0.5;
    run(world, c, 6.0);
    return c;
}

sim::World makeWorld(const AerialProfile& profile, std::uint64_t seed = 7, int windLevel = 0) {
    sim::WorldConfig config;
    config.profile = profile;
    config.seed = seed;
    config.wind.level = windLevel;
    return sim::World(std::move(config));
}

/// Steps a disarmed vehicle under manual motor commands, optionally pinned in place.
struct TestStand {
    AerialVehicle vehicle;
    EnvironmentSample env = stillAir();

    explicit TestStand(const AerialProfile& profile) : vehicle(profile, 3) {
        vehicle.reset(Vec3(0.0), 0.0, env);
    }
    void step(const std::optional<RigidBodyState>& pinned) {
        vehicle.updateSensors(env, kDt);
        vehicle.updateController(PilotCommand{}, kDt);
        vehicle.integrate(env, kDt);
        vehicle.resolveCollisions(env, kDt);
        if (pinned) {
            vehicle.setBodyState(*pinned);
        }
    }
};

// --- Profiles --------------------------------------------------------------------------------

TEST(AerialProfiles, BuiltInProfilesLoadAndRoundTrip) {
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        EXPECT_EQ(p.key, key);
        EXPECT_FALSE(p.name.empty());
        ASSERT_TRUE(p.reference.has_value()) << key;
        const common::Json json = aerialProfileToJson(p);
        const auto back = aerialProfileFromJson(json);
        ASSERT_TRUE(back.has_value()) << back.error().message;
        EXPECT_EQ(aerialProfileToJson(*back), json) << key;
    }
}

TEST(AerialProfiles, ParameterTableCoversTheJsonAndRanges) {
    const auto params = aerialParameters();
    ASSERT_GT(params.size(), 30U);
    for (const char* key : kProfiles) {
        AerialProfile p = loadProfile(key);
        const common::Json json = aerialProfileToJson(p);
        for (const ParamDescriptor& d : params) {
            const double v = d.get(p);
            EXPECT_GE(v, d.min) << key << " " << d.pointer;
            EXPECT_LE(v, d.max) << key << " " << d.pointer;
            const common::Json::json_pointer pointer{std::string(d.pointer)};
            ASSERT_TRUE(json.contains(pointer)) << d.pointer;
            EXPECT_DOUBLE_EQ(json.at(pointer).get<double>(), v) << d.pointer;
            AerialProfile copy = p;
            d.set(copy, d.max);
            EXPECT_DOUBLE_EQ(d.get(copy), d.max) << d.pointer;
        }
    }
}

TEST(AerialProfiles, RejectsOutOfRangeValues) {
    common::Json json = aerialProfileToJson(loadProfile("camera-250g"));
    json["motor"]["kv"] = -5.0;
    const auto bad = aerialProfileFromJson(json);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().message.find("kv"), std::string::npos) << bad.error().message;
    json = aerialProfileToJson(loadProfile("camera-250g"));
    json["battery"]["cellsInSeries"] = "six";
    EXPECT_FALSE(aerialProfileFromJson(json).has_value());
    EXPECT_FALSE(aerialProfileFromJson(common::Json::array()).has_value());
}

TEST(AerialProfiles, DerivedFiguresMatchReferenceData) {
    // NFR-REAL-01: hover throttle, T/W, hover endurance and max level speed within ±10 % of
    // the figures derived from the published component data.
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        const AerialStats s = computeAerialStats(p);
        const ReferenceFigures& r = *p.reference;
        EXPECT_NEAR(s.hoverThrottle, r.hoverThrottle, 0.1 * r.hoverThrottle) << key;
        EXPECT_NEAR(s.thrustToWeight, r.thrustToWeight, 0.1 * r.thrustToWeight) << key;
        EXPECT_NEAR(s.hoverEnduranceMin, r.hoverEnduranceMin, 0.1 * r.hoverEnduranceMin) << key;
        EXPECT_NEAR(s.maxLevelSpeedMs, r.maxLevelSpeedMs, 0.1 * r.maxLevelSpeedMs) << key;
        EXPECT_GT(s.maxWindMs, 0.0) << key;
    }
}

TEST(AerialProfiles, MaximumWindLevels) {
    // Documented in docs/FLIGHT_MODEL.md: the highest level whose gusts stay within the wind
    // the profile can hold.
    const auto& table = weather::WindLevelTable::builtIn();
    const auto level = [&](const char* key) {
        return table.highestLevelWithGustsAtMost(computeAerialStats(loadProfile(key)).maxWindMs);
    };
    EXPECT_EQ(level("fpv-5in-racer"), 8);
    EXPECT_EQ(level("camera-250g"), 5);
    EXPECT_EQ(level("survey-1500g"), 5);
}

// --- Dynamic validation (NFR-REAL-01, NFR-REAL-08) -------------------------------------------

TEST(AerialVehicleFlight, HoverThrottleAndCurrentMatchTheModel) {
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        const AerialStats stats = computeAerialStats(p);
        sim::World world = makeWorld(p);
        const input::ControlInputs c = takeOff(world);
        double command = 0.0;
        double current = 0.0;
        double maxTilt = 0.0;
        const double startY = world.vehicle().bodyState().position.y;
        const int n = 2500;
        for (int i = 0; i < n; ++i) {
            world.tick(c, kDt);
            const VehicleTelemetry t = world.vehicle().telemetry();
            for (const double m : t.motorCommand) {
                command += m / 4.0;
            }
            current += t.battery.current;
            maxTilt = std::max(maxTilt, tiltAngle(world.vehicle().bodyState().orientation));
        }
        const RigidBodyState& b = world.vehicle().bodyState();
        EXPECT_GT(b.position.y, 2.0) << key;
        EXPECT_NEAR(b.position.y, startY, 0.5) << key;  // Altitude hold.
        EXPECT_LT(maxTilt, 5.0 * kDeg) << key;
        EXPECT_NEAR(command / n, stats.hoverThrottle, 0.03 * stats.hoverThrottle) << key;
        EXPECT_NEAR(current / n, stats.hoverCurrentA, 0.1 * stats.hoverCurrentA) << key;
        EXPECT_FALSE(world.vehicle().telemetry().crashed) << key;
    }
}

TEST(AerialVehicleFlight, TestStandThrustToWeight) {
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        TestStand stand(p);
        RigidBodyState pinned = stand.vehicle.bodyState();
        pinned.position.y = 20.0;
        stand.vehicle.setBodyState(pinned);
        stand.vehicle.overrideMotors(std::array<double, 4>{1.0, 1.0, 1.0, 1.0}, true);
        for (int i = 0; i < 500; ++i) {
            stand.step(pinned);
        }
        const double tw =
            stand.vehicle.telemetry().totalThrustN / (stand.vehicle.mass().massKg * kGravity);
        EXPECT_NEAR(tw, computeAerialStats(p).thrustToWeight,
                    0.1 * computeAerialStats(p).thrustToWeight)
            << key;
    }
}

TEST(AerialVehicleFlight, MaxLevelSpeedInAltitudeHold) {
    for (const char* key : kProfiles) {
        AerialProfile p = loadProfile(key);
        p.sensors.gpsAcquisitionS = 1.0;  // GPS-aided attitude while accelerating.
        const double expected = computeAerialStats(p).maxLevelSpeedMs;
        sim::World world = makeWorld(p);
        input::ControlInputs c = takeOff(world, 8.0);
        c.pitch = 1.0;
        run(world, c, 20.0);
        const Vec3 v = world.vehicle().bodyState().velocity;
        EXPECT_NEAR(std::hypot(v.x, v.z), expected, 0.1 * expected) << key;
        EXPECT_FALSE(world.vehicle().telemetry().crashed) << key;
    }
}

TEST(AerialVehicleFlight, SteadyWindBalanceMatchesDragModel) {
    // NFR-REAL-08: at the tilt and command from windHoldState() the forces balance within
    // ±10 %, and the battery current matches.
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        TestStand stand(p);
        const double wind = 8.0;
        stand.env.wind = Vec3(wind, 0.0, 0.0);  // Blowing towards the east.
        const WindHoldState hold = windHoldState(p, wind, stand.env.air.densityKgM3,
                                                 stand.vehicle.battery().stateOfCharge());
        ASSERT_TRUE(hold.feasible) << key;
        RigidBodyState pinned;
        pinned.position = Vec3(0.0, 30.0, 0.0);
        // Nose into the wind (heading west), pitched down: leans west like the model assumes.
        pinned.orientation = orientationFromEuler({0.0, -hold.tiltDeg * kDeg, -90.0 * kDeg});
        stand.vehicle.setBodyState(pinned);
        stand.vehicle.overrideMotors(
            std::array<double, 4>{hold.throttle, hold.throttle, hold.throttle, hold.throttle},
            true);
        for (int i = 0; i < 500; ++i) {
            stand.step(pinned);
        }
        stand.step(std::nullopt);
        const Vec3 a = stand.vehicle.bodyState().velocity / kDt;
        const double dragAccel = kGravity * std::tan(hold.tiltDeg * kDeg);
        EXPECT_LT(std::abs(a.x), 0.1 * dragAccel) << key << " ax " << a.x;
        EXPECT_LT(std::abs(a.y), 0.1 * kGravity) << key << " ay " << a.y;
        EXPECT_NEAR(stand.vehicle.battery().current(), hold.currentA, 0.1 * hold.currentA) << key;
    }
}

// --- Vehicle ---------------------------------------------------------------------------------

TEST(AerialVehicle, IdealFreeFall) {
    // NFR-REAL-02 on the full vehicle: motors off and no drag.
    TestStand stand(loadProfile("camera-250g"));
    stand.vehicle.setIdealConditions(true);
    RigidBodyState s;
    s.position = Vec3(0.0, 500.0, 0.0);
    stand.vehicle.setBodyState(s);
    for (int i = 0; i < 2500; ++i) {
        stand.step(std::nullopt);
    }
    const double drop = 500.0 - stand.vehicle.bodyState().position.y;
    EXPECT_NEAR(drop, 0.5 * kGravity * 25.0, 0.5 * kGravity * 25.0 * 1e-3);
}

TEST(AerialVehicle, RestsOnTheGroundAndSurvivesGentleDrops) {
    TestStand stand(loadProfile("camera-250g"));
    for (int i = 0; i < 1000; ++i) {
        stand.step(std::nullopt);
    }
    EXPECT_TRUE(stand.vehicle.telemetry().onGround);
    EXPECT_NEAR(stand.vehicle.bodyState().position.y, stand.vehicle.restHeightM(), 0.01);
    RigidBodyState s = stand.vehicle.bodyState();
    s.position.y += 0.2;
    stand.vehicle.setBodyState(s);
    (void)stand.vehicle.takeEvents();
    for (int i = 0; i < 1000; ++i) {
        stand.step(std::nullopt);
    }
    EXPECT_FALSE(stand.vehicle.telemetry().crashed);
    EXPECT_LT(tiltAngle(stand.vehicle.bodyState().orientation), 5.0 * kDeg);
}

TEST(AerialVehicle, HardImpactCrashes) {
    TestStand stand(loadProfile("camera-250g"));
    RigidBodyState s = stand.vehicle.bodyState();
    s.position.y = 30.0;
    stand.vehicle.setBodyState(s);
    std::vector<VehicleEvent> events;
    for (int i = 0; i < 2500; ++i) {
        stand.step(std::nullopt);
        auto e = stand.vehicle.takeEvents();
        events.insert(events.end(), e.begin(), e.end());
    }
    EXPECT_TRUE(stand.vehicle.telemetry().crashed);
    const auto crash = std::ranges::find(events, VehicleEvent::Kind::Crash, &VehicleEvent::kind);
    ASSERT_NE(crash, events.end());
    EXPECT_GT(crash->value, 10.0);  // Impact speed ≈ √(2·g·30) ≈ 24 m/s minus drag.
    EXPECT_FALSE(crash->text.empty());
    // A crashed vehicle refuses to arm.
    PilotCommand arm;
    arm.arm = true;
    stand.vehicle.updateController(arm, kDt);
    EXPECT_FALSE(stand.vehicle.controller().armed());
}

TEST(AerialVehicle, FallingIntoWater) {
    TestStand stand(loadProfile("camera-250g"));
    stand.env.waterLevelM = 2.0;
    RigidBodyState s = stand.vehicle.bodyState();
    s.position.y = 6.0;
    stand.vehicle.setBodyState(s);
    std::vector<VehicleEvent> events;
    for (int i = 0; i < 2500; ++i) {
        stand.step(std::nullopt);
        auto e = stand.vehicle.takeEvents();
        events.insert(events.end(), e.begin(), e.end());
    }
    EXPECT_NE(std::ranges::find(events, VehicleEvent::Kind::WaterEntered, &VehicleEvent::kind),
              events.end());
    EXPECT_TRUE(stand.vehicle.telemetry().crashed);
    // Buoyancy keeps it near the surface instead of sinking to the bottom.
    EXPECT_GT(stand.vehicle.bodyState().position.y, 1.0);
}

TEST(AerialVehicle, NonFiniteStateIsReset) {
    TestStand stand(loadProfile("fpv-5in-racer"));
    RigidBodyState s = stand.vehicle.bodyState();
    s.position.y = 10.0;
    s.velocity.x = std::numeric_limits<double>::quiet_NaN();
    stand.vehicle.setBodyState(s);
    stand.step(std::nullopt);
    const auto events = stand.vehicle.takeEvents();
    EXPECT_NE(std::ranges::find(events, VehicleEvent::Kind::NumericalReset, &VehicleEvent::kind),
              events.end());
    EXPECT_TRUE(isFinite(stand.vehicle.bodyState()));
}

TEST(AerialVehicle, PayloadChangesMassAndHoverPower) {
    const AerialProfile p = loadProfile("survey-1500g");
    sim::World world = makeWorld(p);
    const double empty = world.vehicle().mass().massKg;
    world.vehicle().attachPayload(0.3, Vec3(0.0, 0.0, 0.08), 0.01);
    EXPECT_NEAR(world.vehicle().mass().massKg, empty + 0.3, 1e-9);
    const input::ControlInputs c = takeOff(world);
    double loaded = 0.0;
    for (int i = 0; i < 1000; ++i) {
        world.tick(c, kDt);
        loaded += world.vehicle().telemetry().totalThrustN / 1000.0;
    }
    EXPECT_NEAR(loaded, (empty + 0.3) * kGravity, 0.05 * (empty + 0.3) * kGravity);
    world.vehicle().releasePayload();
    EXPECT_NEAR(world.vehicle().mass().massKg, empty, 1e-9);
    run(world, c, 2.0);
    EXPECT_FALSE(world.vehicle().telemetry().crashed);  // Climbs briefly, then holds.
}

TEST(AerialVehicle, SimulationIsDeterministic) {
    const AerialProfile p = loadProfile("camera-250g");
    const auto fly = [&](std::uint64_t seed) {
        sim::World world = makeWorld(p, seed, 4);
        input::ControlInputs c = takeOff(world);
        c.roll = 0.3;
        c.yaw = -0.2;
        run(world, c, 2.0);
        return world.vehicle().bodyState();
    };
    const RigidBodyState a = fly(21);
    const RigidBodyState b = fly(21);
    const RigidBodyState c = fly(22);
    EXPECT_EQ(a.position, b.position);
    EXPECT_EQ(a.orientation, b.orientation);
    EXPECT_EQ(a.velocity, b.velocity);
    EXPECT_NE(a.position, c.position);
}

// --- Flight controller -----------------------------------------------------------------------

TEST(FlightController, RateCurve) {
    const RateCurve curve{200.0, 670.0, 0.5};
    EXPECT_DOUBLE_EQ(rateFromStick(curve, 0.0), 0.0);
    EXPECT_NEAR(rateFromStick(curve, 1.0), 670.0, 1e-9);
    EXPECT_NEAR(rateFromStick(curve, -1.0), -670.0, 1e-9);
    EXPECT_NEAR(rateFromStick(curve, -0.4), -rateFromStick(curve, 0.4), 1e-12);
    EXPECT_NEAR(rateFromStick(curve, 0.01), 2.0, 0.05);  // centerDps near the centre.
    double previous = 0.0;
    for (int i = 1; i <= 100; ++i) {
        const double r = rateFromStick(curve, i / 100.0);
        EXPECT_GT(r, previous);
        previous = r;
    }
}

TEST(FlightController, MixerDirectionsAndAirmode) {
    const AerialProfile p = loadProfile("fpv-5in-racer");
    // Motor order: front-right, rear-right, rear-left, front-left.
    const ControllerOutput roll = mixQuadX(p, 0.5, Vec3(0.1, 0.0, 0.0), false);
    EXPECT_GT(roll.motors[2], roll.motors[1]);  // Left motors push to roll right.
    EXPECT_GT(roll.motors[3], roll.motors[0]);
    const ControllerOutput pitch = mixQuadX(p, 0.5, Vec3(0.0, 0.1, 0.0), false);
    EXPECT_GT(pitch.motors[0], pitch.motors[1]);  // Front motors push to pitch up.
    EXPECT_GT(pitch.motors[3], pitch.motors[2]);
    const ControllerOutput yaw = mixQuadX(p, 0.5, Vec3(0.0, 0.0, 0.1), false);
    const auto spins = AerialProfile::spinDirections();
    for (std::size_t i = 0; i < 4; ++i) {
        // Yaw right: the counter-clockwise (−1) props speed up, their reaction turns the frame.
        EXPECT_EQ(yaw.motors[i] > 0.5, spins[i] < 0.0) << i;
    }
    for (const ControllerOutput* out : {&roll, &pitch, &yaw}) {
        double sum = 0.0;
        for (const double m : out->motors) {
            sum += m;
        }
        EXPECT_NEAR(sum, 2.0, 1e-12);  // Collective preserved.
        EXPECT_FALSE(out->saturated);
    }
    // Zero throttle: without airmode the torque is clipped, with airmode it is kept.
    const ControllerOutput clipped = mixQuadX(p, 0.0, Vec3(0.2, 0.0, 0.0), false);
    const ControllerOutput air = mixQuadX(p, 0.0, Vec3(0.2, 0.0, 0.0), true);
    EXPECT_NEAR(air.motors[3] - air.motors[0], 0.2, 1e-12);
    EXPECT_LT(clipped.motors[3] - clipped.motors[0], 0.2);
    for (const double m : clipped.motors) {
        EXPECT_GE(m, 0.0);
    }
    // Excessive demand saturates and is scaled into range.
    const ControllerOutput sat = mixQuadX(p, 0.9, Vec3(2.0, 0.0, 0.0), true);
    EXPECT_TRUE(sat.saturated);
    for (const double m : sat.motors) {
        EXPECT_GE(m, 0.0);
        EXPECT_LE(m, 1.0);
    }
}

struct ControllerHarness {
    FlightController fc;
    EstimatedState state;
    VehicleConditions conditions{.landed = true};

    explicit ControllerHarness(const AerialProfile& p) : fc(p) {}
    ControllerOutput tick(const PilotCommand& c, int ticks = 1) {
        ControllerOutput out;
        for (int i = 0; i < ticks; ++i) {
            out = fc.update(c, state, conditions, kDt);
        }
        return out;
    }
};

TEST(FlightController, PreArmChecks) {
    const AerialProfile p = loadProfile("camera-250g");
    ControllerHarness h(p);
    PilotCommand c;
    c.arm = true;
    h.tick(c);
    EXPECT_FALSE(h.fc.armed());
    EXPECT_EQ(h.fc.armBlock(), ArmBlock::SwitchNotCycled);
    c.arm = false;
    h.tick(c);
    // Throttle up in Angle mode.
    c.throttle = 0.4;
    c.arm = true;
    h.tick(c);
    EXPECT_EQ(h.fc.armBlock(), ArmBlock::ThrottleNotLow);
    EXPECT_FALSE(h.fc.armed());
    // Altitude hold accepts a centred throttle.
    c.arm = false;
    h.tick(c);
    c.mode = FlightMode::AltitudeHold;
    c.throttle = 0.5;
    c.arm = true;
    EXPECT_EQ(h.fc.preArmCheck(c, h.state, h.conditions), ArmBlock::None);
    // Tilted, crashed, battery and link.
    EstimatedState tilted = h.state;
    tilted.attitude = orientationFromEuler({40.0 * kDeg, 0.0, 0.0});
    tilted.euler = eulerFromOrientation(tilted.attitude);
    EXPECT_EQ(h.fc.preArmCheck(c, tilted, h.conditions), ArmBlock::NotLevel);
    EXPECT_EQ(h.fc.preArmCheck(c, h.state, {.crashed = true, .landed = true}), ArmBlock::Crashed);
    EXPECT_EQ(h.fc.preArmCheck(c, h.state, {.batteryCritical = true, .landed = true}),
              ArmBlock::BatteryCritical);
    PilotCommand lost = c;
    lost.linkValid = false;
    EXPECT_EQ(h.fc.preArmCheck(lost, h.state, h.conditions), ArmBlock::LinkLost);
    EXPECT_FALSE(armBlockText(ArmBlock::NotLevel).empty());

    const ControllerOutput out = h.tick(c);
    EXPECT_TRUE(h.fc.armed());
    EXPECT_EQ(h.fc.mode(), FlightMode::AltitudeHold);
    for (const double m : out.motors) {
        EXPECT_GT(m, 0.0);  // Idle spin.
    }
    c.arm = false;
    const ControllerOutput off = h.tick(c);
    EXPECT_FALSE(h.fc.armed());
    for (const double m : off.motors) {
        EXPECT_EQ(m, 0.0);
    }
}

TEST(FlightController, FailsafeLandsThenDisarms) {
    const AerialProfile p = loadProfile("camera-250g");
    ControllerHarness h(p);
    PilotCommand c;
    c.mode = FlightMode::AltitudeHold;
    c.throttle = 0.5;
    h.tick(c);
    c.arm = true;
    h.tick(c);
    ASSERT_TRUE(h.fc.armed());
    h.conditions.landed = false;
    h.state.altitudeM = 20.0;
    h.tick(c, 500);
    c.linkValid = false;
    h.tick(c, 250);  // 0.5 s: holding, not yet landing.
    EXPECT_FALSE(h.fc.failsafe());
    EXPECT_TRUE(h.fc.armed());
    h.tick(c, 300);  // Past the 1 s delay.
    EXPECT_TRUE(h.fc.failsafe());
    EXPECT_TRUE(h.fc.armed());
    EXPECT_EQ(h.fc.mode(), FlightMode::AltitudeHold);
    h.conditions.landed = true;
    h.tick(c, 600);
    EXPECT_FALSE(h.fc.armed());
    // Link back: the switch must be cycled before arming again.
    c.linkValid = true;
    h.tick(c, 10);
    EXPECT_FALSE(h.fc.failsafe());
    EXPECT_FALSE(h.fc.armed());
}

TEST(FlightController, AngleModeLimitsTilt) {
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        sim::World world = makeWorld(p);
        input::ControlInputs c = takeOff(world);
        c.flightMode = 1;  // Angle.
        c.roll = 1.0;
        run(world, c, 1.5);
        const Euler e = eulerFromOrientation(world.vehicle().bodyState().orientation);
        EXPECT_NEAR(e.roll / kDeg, p.controller.maxAngleDeg, 3.0) << key;
        c.roll = 0.0;
        run(world, c, 3.0);
        EXPECT_LT(std::abs(eulerFromOrientation(world.vehicle().bodyState().orientation).roll),
                  3.0 * kDeg)
            << key;
    }
}

TEST(FlightController, AcroRateStepHasNoSustainedOscillation) {
    // NFR-REAL-04: step metrics are documented in docs/FLIGHT_MODEL.md; here: overshoot below
    // 30 % and, 0.8 s after the step, the rate stays within 10 % (no sustained oscillation).
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        sim::World world = makeWorld(p);
        input::ControlInputs c = takeOff(world);
        c.flightMode = 0;
        c.throttle = world.vehicle().controller().collective();
        run(world, c, 0.3);
        c.roll = 0.5;
        const double target = rateFromStick(p.controller.rates[0], 0.5) * kDeg;
        double peak = 0.0;
        double lateError = 0.0;
        for (int i = 0; i < 500; ++i) {
            world.tick(c, kDt);
            const double r = world.vehicle().bodyState().angularVelocity.x;
            peak = std::max(peak, r);
            if (i >= 400) {
                lateError = std::max(lateError, std::abs(r - target));
            }
        }
        EXPECT_LT(peak, 1.3 * target) << key;
        EXPECT_LT(lateError, 0.1 * target) << key;
    }
}

}  // namespace
