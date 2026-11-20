// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the aerial vehicle (SPECIFICATION §4.6): reference profiles, derived
// performance (NFR-REAL-01), wind balance (NFR-REAL-08), flight controller, contacts, crashes
// and determinism.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

#include "ds/common/math.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/aerial_vehicle.hpp"
#include "ds/physics/failures.hpp"
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
    EXPECT_TRUE(world.vehicle().armed());
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

TEST(AerialVehicleFlight, PositionHoldInSteadyWindMatchesDragBalance) {
    // NFR-REAL-08 in closed loop: hovering in Position hold in a steady wind, the mean tilt and
    // battery power match windHoldState() within ±10 % (the figures of docs/FLIGHT_MODEL.md),
    // at a light wind and at 80 % of the profile's maximum.
    for (const char* key : kProfiles) {
        AerialProfile p = loadProfile(key);
        p.sensors.gpsAcquisitionS = 1.0;
        const double limit = computeAerialStats(p).maxWindMs;
        for (const double wind : {5.0, 0.8 * limit}) {
            AerialVehicle vehicle(p, 3);
            EnvironmentSample env = stillAir();
            // Nose into the wind (heading west), as windHoldState() assumes: the frontal and side
            // drag areas differ.
            vehicle.reset(Vec3(0.0), -90.0 * kDeg, env);
            PilotCommand cmd;
            cmd.mode = FlightMode::AltitudeHold;
            cmd.throttle = 0.5;
            const auto fly = [&](double seconds, const auto& each) {
                for (int i = 0; i < static_cast<int>(std::lround(seconds / kDt)); ++i) {
                    env.air = isaAir(vehicle.bodyState().position.y);  // The barometer.
                    vehicle.updateSensors(env, kDt);
                    vehicle.updateController(cmd, kDt);
                    vehicle.integrate(env, kDt);
                    vehicle.resolveCollisions(env, kDt);
                    each();
                }
            };
            const auto nothing = [] {};
            fly(1.0, nothing);
            cmd.arm = true;
            fly(0.5, nothing);
            cmd.throttle = 1.0;
            fly(4.0, nothing);  // Climb to ≈ 12 m, well out of ground effect.
            cmd.throttle = 0.5;
            cmd.mode = FlightMode::PositionHold;
            env.wind = Vec3(wind, 0.0, 0.0);  // Steady, from the west.
            fly(25.0, nothing);
            ASSERT_EQ(vehicle.telemetry().mode, FlightMode::PositionHold) << key;
            double tilt = 0.0;
            double power = 0.0;
            int n = 0;
            fly(10.0, [&] {
                tilt += tiltAngle(vehicle.bodyState().orientation);
                power += vehicle.battery().voltage() * vehicle.battery().current();
                ++n;
            });
            tilt /= n;
            power /= n;
            const WindHoldState hold =
                windHoldState(p, wind, env.air.densityKgM3, vehicle.battery().stateOfCharge());
            EXPECT_NEAR(tilt / kDeg, hold.tiltDeg, 0.1 * hold.tiltDeg) << key << " wind " << wind;
            EXPECT_NEAR(power, hold.powerW, 0.1 * hold.powerW) << key << " wind " << wind;
            const Vec3 v = vehicle.bodyState().velocity;
            EXPECT_LT(std::hypot(v.x, v.z), 0.5) << key << " wind " << wind;
        }
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

TEST(AerialVehicle, RainWetsTheAirframeAndPushesItDown) {
    // FR-WX-06: a water film (≈ 0.1 mm over twice the drag areas) in heavy rain, blown off
    // after the rain; the drops' momentum is a small downward force.
    const AerialProfile p = loadProfile("survey-1500g");
    AerialVehicle v(p, 3);
    EnvironmentSample env = stillAir();
    v.reset(Vec3(0.0), 0.0, env);
    const double dryMass = v.mass().massKg;
    env.precipitation = {weather::PrecipitationType::Rain, 12.0};
    for (int i = 0; i < 30'000; ++i) {  // 5 min.
        v.integrate(env, 0.01);
        v.resolveCollisions(env, 0.01);
    }
    const FrameParams& f = p.frame;
    const double film =
        1000.0 * 1e-4 * 2.0 * (f.dragAreaForwardM2 + f.dragAreaSideM2 + f.dragAreaVerticalM2);
    EXPECT_NEAR(v.waterMassKg(), film, 0.01 * film);
    EXPECT_NEAR(v.mass().massKg - dryMass, v.waterMassKg(), 1e-4);  // Updated every 0.1 g.

    RigidBodyState still = v.bodyState();  // Level, at rest.
    still.velocity = Vec3(0.0);
    const Vec3 force = v.precipitationForce(still, env);
    const double fall = weather::fallSpeedMs(env.precipitation);
    const double expected = -12.0 / 3600.0 * fall * f.dragAreaVerticalM2;
    EXPECT_NEAR(force.y, expected, 0.01 * std::abs(expected));  // Resting slightly tilted.
    EXPECT_LT(std::abs(force.y), 0.01 * 9.81 * dryMass);        // Under 1 % of the weight.
    RigidBodyState fast = still;
    fast.velocity = Vec3(10.0, 0.0, 0.0);
    EXPECT_LT(v.precipitationForce(fast, env).x, 0.0);  // Flying into the rain slows it.

    env.precipitation = {};
    for (int i = 0; i < 90'000; ++i) {  // 15 min: three drying time constants.
        v.integrate(env, 0.01);
        v.resolveCollisions(env, 0.01);
    }
    EXPECT_LT(v.waterMassKg(), 0.06 * film);
    EXPECT_DOUBLE_EQ(v.precipitationForce(still, env).y, 0.0);
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

TEST(AerialVehicle, ObstacleBounceFollowsMaterialAndMass) {
    // Into a wall facing +x at 1 m/s, sliding along it at 0.5 m/s (disarmed: no crash).
    const auto bounce = [](double restitution, double friction, double obstacleMassKg,
                           double* vehicleMassKg = nullptr) {
        TestStand stand(loadProfile("survey-1500g"));
        RigidBodyState s = stand.vehicle.bodyState();
        s.position.y = 5.0;
        s.velocity = Vec3(-1.0, 0.0, 0.5);
        stand.vehicle.setBodyState(s);
        stand.vehicle.resolveObstacles([&](const Vec3&) {
            return std::optional(ObstacleContact{.normal = Vec3(1.0, 0.0, 0.0),
                                                 .depthM = 0.01,
                                                 .name = "Wall",
                                                 .restitution = restitution,
                                                 .friction = friction,
                                                 .massKg = obstacleMassKg});
        });
        if (vehicleMassKg != nullptr) {
            *vehicleMassKg = stand.vehicle.mass().massKg;
        }
        return stand.vehicle.bodyState().velocity;
    };
    constexpr double kInf = std::numeric_limits<double>::infinity();
    Vec3 v = bounce(0.2, 0.0, kInf);  // Frictionless: only the normal speed changes.
    EXPECT_NEAR(v.x, 0.2, 1e-9);
    EXPECT_NEAR(v.z, 0.5, 1e-9);
    v = bounce(0.05, 0.0, kInf);  // Foliage barely bounces.
    EXPECT_NEAR(v.x, 0.05, 1e-9);
    v = bounce(0.2, 0.2, kInf);  // Friction takes μ · 1.2 m/s of the sliding speed.
    EXPECT_NEAR(v.z, 0.5 - 0.24, 1e-9);
    v = bounce(0.2, 0.9, kInf);  // ...but never reverses it.
    EXPECT_NEAR(v.z, 0.0, 1e-9);
    // An obstacle as heavy as the drone takes half of the impulse.
    double m = 0.0;
    (void)bounce(0.0, 0.0, kInf, &m);
    v = bounce(0.0, 0.0, m);
    EXPECT_NEAR(v.x, -0.5, 1e-9);
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

// --- Position hold and return to home (M7 step 5) ---------------------------------------------

/// Takes off in altitude hold with a GPS fix after 1 s, then switches to position hold.
sim::World gpsWorld(const char* key, int windLevel = 0) {
    AerialProfile p = loadProfile(key);
    p.sensors.gpsAcquisitionS = 1.0;
    return makeWorld(p, 7, windLevel);
}

TEST(PositionHold, HoldsInWindUpToTheProfileLimitAndDriftsBeyond) {
    // FR-WIND-05/06 and the "Wind levels" validation: the controller holds its estimated
    // position within 2 m up to the wind the profile can resist (within 1 m in calm air); the
    // true position wanders only with the GPS error (about 1–1.5 m 1σ, slowly varying).
    const auto& table = weather::WindLevelTable::builtIn();
    for (const char* key : kProfiles) {
        const AerialProfile p = loadProfile(key);
        const int maxLevel = table.highestLevelWithGustsAtMost(computeAerialStats(p).maxWindMs);
        for (const int level : {0, maxLevel}) {
            sim::World world = gpsWorld(key, level);
            input::ControlInputs c = takeOff(world);
            c.flightMode = 3;
            run(world, c, 10.0);
            ASSERT_EQ(world.vehicle().telemetry().mode, FlightMode::PositionHold) << key;
            const Vec3 start = world.vehicle().bodyState().position;
            const Vec3 estimateStart = world.vehicle().telemetry().estimate.position;
            double estimateError = 0.0;
            double sumSq = 0.0;
            const int n = 60 * 500;
            for (int i = 0; i < n; ++i) {
                world.tick(c, kDt);
                const Vec3 d = world.vehicle().bodyState().position - start;
                const Vec3 e = world.vehicle().telemetry().estimate.position - estimateStart;
                estimateError = std::max(estimateError, std::hypot(e.x, e.z));
                sumSq += d.x * d.x + d.z * d.z;
            }
            EXPECT_LT(estimateError, level == 0 ? 1.0 : 2.0) << key << " level " << level;
            EXPECT_LT(std::sqrt(sumSq / n), 2.0 * p.sensors.gpsHorizontalNoiseM)
                << key << " level " << level;
            EXPECT_FALSE(world.vehicle().telemetry().crashed) << key;
        }
        // Where the mean wind itself is 20 % beyond the limit, the drone is blown away (the
        // gusts alone are brief: with mostly linear rotor drag, a gust peak above the limit
        // only nudges the drone).
        int beyond = maxLevel + 1;
        while (beyond <= 10 &&
               table.level(beyond).meanSpeedMs < 1.2 * computeAerialStats(p).maxWindMs) {
            ++beyond;
        }
        if (beyond > 10) {
            continue;  // The racer resists every level.
        }
        // (Measured from the take-off spot: the drone is blown downwind while it climbs, then
        // sinks into the weaker wind near the ground when the thrust runs out.)
        sim::World world = gpsWorld(key, beyond);
        const Vec3 start = world.vehicle().bodyState().position;
        input::ControlInputs c = takeOff(world);
        c.flightMode = 3;
        run(world, c, 30.0);
        const Vec3 d = world.vehicle().bodyState().position - start;
        EXPECT_GT(std::hypot(d.x, d.z), 20.0) << key << " level " << beyond;
    }
}

TEST(PositionHold, SticksSetTheSpeedAndReleasingThemStops) {
    for (const char* key : kProfiles) {
        sim::World world = gpsWorld(key);
        const ControllerParams& cp = loadProfile(key).controller;
        input::ControlInputs c = takeOff(world);
        c.flightMode = 3;
        c.pitch = 1.0;
        run(world, c, 6.0);
        Vec3 v = world.vehicle().bodyState().velocity;
        EXPECT_NEAR(std::hypot(v.x, v.z), cp.maxHorizontalSpeedMs, 0.6) << key;
        c.pitch = 0.0;
        const Vec3 released = world.vehicle().bodyState().position;
        run(world, c, 6.0);
        v = world.vehicle().bodyState().velocity;
        const Vec3 d = world.vehicle().bodyState().position - released;
        EXPECT_LT(std::hypot(v.x, v.z), 0.5) << key;
        EXPECT_LT(std::hypot(d.x, d.z), 10.0) << key;  // About 5–8 m from 8 m/s.
        const Vec3 stopped = world.vehicle().bodyState().position;
        run(world, c, 5.0);
        const Vec3 drift = world.vehicle().bodyState().position - stopped;
        EXPECT_LT(std::hypot(drift.x, drift.z), 1.5) << key;
    }
}

TEST(ReturnToHome, ClimbsReturnsAndLandsAtHome) {
    for (const char* key : kProfiles) {
        sim::World world = gpsWorld(key, 3);
        const ControllerParams& cp = loadProfile(key).controller;
        const Vec3 home = world.vehicle().bodyState().position;
        input::ControlInputs c = takeOff(world);
        c.flightMode = 3;
        c.pitch = 1.0;
        c.roll = 0.5;
        run(world, c, 8.0);
        c.pitch = c.roll = 0.0;
        c.flightMode = 1;  // RTH works from any mode.
        c.returnToHome = true;
        double maxY = 0.0;
        double t = 0.0;
        std::vector<RthStage> stages;
        while (world.vehicle().armed() && t < 120.0) {
            world.tick(c, kDt);
            t += kDt;
            maxY = std::max(maxY, world.vehicle().bodyState().position.y);
            const VehicleTelemetry tel = world.vehicle().telemetry();
            if (tel.rthStage && (stages.empty() || stages.back() != *tel.rthStage)) {
                stages.push_back(*tel.rthStage);
            }
        }
        EXPECT_EQ(stages, (std::vector{RthStage::Climb, RthStage::Return, RthStage::Descend}))
            << key;
        EXPECT_FALSE(world.vehicle().armed()) << key;  // Landed and disarmed.
        EXPECT_FALSE(world.vehicle().telemetry().crashed) << key;
        EXPECT_GT(maxY, home.y + cp.rthAltitudeM - 2.0) << key;
        const Vec3 d = world.vehicle().bodyState().position - home;
        EXPECT_LT(std::hypot(d.x, d.z), 3.0 * loadProfile(key).sensors.gpsHorizontalNoiseM) << key;
        // Disarmed with the switch on: it must be turned off before arming again.
        c.arm = false;
        world.tick(c, kDt);
        c.arm = true;
        c.flightMode = 2;
        c.throttle = 0.5;
        world.tick(c, kDt);
        EXPECT_FALSE(world.vehicle().armed()) << key;
        EXPECT_EQ(world.vehicle().telemetry().armBlock, ArmBlock::RthSwitchOn) << key;
    }
}

TEST(ReturnToHome, FailsafeReturnsHomeWithGps) {
    sim::World world = gpsWorld("camera-250g");
    const Vec3 home = world.vehicle().bodyState().position;
    input::ControlInputs c = takeOff(world);
    c.flightMode = 3;
    c.pitch = 1.0;
    run(world, c, 6.0);
    c.pitch = 0.0;
    c.linkValid = false;
    run(world, c, 2.0);
    EXPECT_TRUE(world.vehicle().telemetry().failsafe);
    EXPECT_EQ(world.vehicle().telemetry().mode, FlightMode::ReturnToHome);
    run(world, c, 90.0);
    EXPECT_FALSE(world.vehicle().armed());
    const Vec3 d = world.vehicle().bodyState().position - home;
    EXPECT_LT(std::hypot(d.x, d.z), 4.0);
}

// --- Failure injection (M7 step 6, FR-PHY-15) -------------------------------------------------

TEST(FailureInjector, TimingCombinationAndNames) {
    for (const FailureType t : kFailureTypes) {
        EXPECT_EQ(parseFailureType(failureTypeName(t)), t);
        EXPECT_FALSE(failureTitle(t).empty());
    }
    EXPECT_FALSE(parseFailureType("meteorStrike").has_value());

    FailureInjector f(3);
    Failure drift = defaultFailure(FailureType::GpsDrift);
    drift.driftMs = 2.0;
    drift.magnitudeM = 5.0;
    drift.durationS = 10.0;
    f.inject(drift);
    Failure motor = defaultFailure(FailureType::MotorFailure);
    motor.motor = 3;
    motor.efficiency = 0.5;
    f.inject(motor);
    f.inject(motor);  // Two faults on one motor multiply.
    Failure cell = defaultFailure(FailureType::BatteryDrop);
    cell.fraction = 0.25;
    f.inject(cell);
    EXPECT_EQ(f.takeBatteryDrops(), std::vector<double>{0.25});
    EXPECT_TRUE(f.takeBatteryDrops().empty());  // Once.
    EXPECT_EQ(f.active().size(), 3U);

    EXPECT_TRUE(f.advance(1.0).empty());
    VehicleFaults v = f.vehicleFaults();
    EXPECT_NEAR(glm::length(v.sensors.gpsOffset), 2.0, 1e-9);
    EXPECT_NEAR(v.sensors.gpsOffset.y, 0.0, 1e-12);  // Horizontal.
    EXPECT_DOUBLE_EQ(v.motorEfficiency[2], 0.25);
    EXPECT_DOUBLE_EQ(v.motorEfficiency[0], 1.0);
    f.advance(5.0);
    EXPECT_NEAR(glm::length(f.vehicleFaults().sensors.gpsOffset), 5.0, 1e-9);  // Capped.
    const std::vector<Failure> ended = f.advance(5.0);
    ASSERT_EQ(ended.size(), 1U);
    EXPECT_EQ(ended[0].type, FailureType::GpsDrift);
    EXPECT_EQ(f.vehicleFaults().sensors.gpsOffset, Vec3(0.0));

    Failure rc = defaultFailure(FailureType::RcLoss);
    f.inject(rc);
    EXPECT_TRUE(f.rcLost());
    f.clear();
    EXPECT_FALSE(f.rcLost());
    EXPECT_EQ(f.vehicleFaults(), VehicleFaults{});
}

TEST(FailureInjection, SensorFaultsReachTheReadings) {
    AerialProfile p = loadProfile("camera-250g");
    p.sensors.gpsAcquisitionS = 0.0;
    SensorSuite clean(p.sensors, 5);
    SensorSuite faulty(p.sensors, 5);
    faulty.setFaults({.gpsLost = false,
                      .gpsOffset = Vec3(3.0, 0.0, -4.0),
                      .compassOffsetRad = 0.5,
                      .baroOffsetM = 7.0});
    SensorTruth t;
    for (int i = 0; i < 100; ++i) {
        clean.update(t, kDt);
        faulty.update(t, kDt);
    }
    const SensorReadings& a = clean.readings();
    const SensorReadings& b = faulty.readings();
    EXPECT_NEAR(glm::length(b.gps.position - a.gps.position), 5.0, 1e-9);
    EXPECT_NEAR(b.mag.headingRad - a.mag.headingRad, 0.5, 1e-9);
    EXPECT_NEAR(b.baro.altitudeM - a.baro.altitudeM, 7.0, 1e-9);
    faulty.setFaults({.gpsLost = true});
    for (int i = 0; i < 100; ++i) {
        faulty.update(t, kDt);
    }
    EXPECT_FALSE(faulty.readings().gps.fix);
    EXPECT_EQ(faulty.readings().gps.satellites, 0);
}

TEST(FailureInjection, GpsLossDropsPositionHoldUntilTheFixReturns) {
    sim::World world = gpsWorld("camera-250g");
    input::ControlInputs c = takeOff(world);
    c.flightMode = 3;
    run(world, c, 3.0);
    ASSERT_EQ(world.vehicle().telemetry().mode, FlightMode::PositionHold);
    Failure loss = defaultFailure(FailureType::GpsLoss);
    loss.durationS = 5.0;
    world.injectFailure(loss);
    run(world, c, 1.0);
    EXPECT_FALSE(world.vehicle().telemetry().gps.fix);
    EXPECT_EQ(world.vehicle().telemetry().mode, FlightMode::AltitudeHold);
    EXPECT_TRUE(world.vehicle().telemetry().gpsModeUnavailable);
    run(world, c, 5.0);
    EXPECT_TRUE(world.failures().active().empty());
    EXPECT_EQ(world.vehicle().telemetry().mode, FlightMode::PositionHold);
}

TEST(FailureInjection, GpsDriftPullsPositionHoldAway) {
    // Position hold follows the GPS: the drone moves opposite to the drift, by its size.
    sim::World world = gpsWorld("camera-250g");
    input::ControlInputs c = takeOff(world);
    c.flightMode = 3;
    run(world, c, 5.0);
    const Vec3 before = world.vehicle().bodyState().position;
    Failure drift = defaultFailure(FailureType::GpsDrift);
    drift.magnitudeM = 15.0;
    drift.driftMs = 1.0;
    world.injectFailure(drift);
    run(world, c, 25.0);
    const Vec3 d = world.vehicle().bodyState().position - before;
    const Vec3 dir = world.failures().active().front().direction;
    EXPECT_NEAR(glm::dot(Vec3(d.x, 0.0, d.z), dir), -15.0, 3.0);
}

TEST(FailureInjection, MotorFailureDegradesOrBringsTheDroneDown) {
    // 70 % thrust left on one motor: a lurch, then the controller commands that motor
    // harder and the drone flies on level.
    for (const char* key : kProfiles) {
        sim::World degraded = gpsWorld(key);
        input::ControlInputs c = takeOff(degraded);
        const double height = degraded.vehicle().bodyState().position.y;
        Failure weak = defaultFailure(FailureType::MotorFailure);
        weak.motor = 2;
        weak.efficiency = 0.7;
        degraded.injectFailure(weak);
        run(degraded, c, 10.0);
        const VehicleTelemetry t = degraded.vehicle().telemetry();
        EXPECT_FALSE(t.crashed) << key;
        EXPECT_NEAR(degraded.vehicle().bodyState().position.y, height, 1.5) << key;
        EXPECT_LT(tiltAngle(degraded.vehicle().bodyState().orientation), 5.0 * kDeg) << key;
    }
    input::ControlInputs c;

    // A dead motor on a quad: no yaw-free hover is possible; it spins and falls.
    sim::World dead = gpsWorld("camera-250g");
    c = takeOff(dead, 6.0);
    Failure stop = defaultFailure(FailureType::MotorFailure);
    stop.motor = 1;
    dead.injectFailure(stop);
    run(dead, c, 8.0);
    EXPECT_EQ(dead.vehicle().telemetry().motorCommand[0], 0.0);
    EXPECT_TRUE(dead.vehicle().telemetry().crashed || dead.vehicle().bodyState().position.y < 1.0);
}

TEST(FailureInjection, BatteryCellFaultCompassAndBaro) {
    sim::World world = gpsWorld("camera-250g");
    input::ControlInputs c = takeOff(world);
    const double soc = world.vehicle().battery().stateOfCharge();
    Failure cell = defaultFailure(FailureType::BatteryDrop);
    cell.fraction = 0.3;
    world.injectFailure(cell);
    run(world, c, 0.1);
    EXPECT_NEAR(world.vehicle().battery().stateOfCharge(), soc - 0.3, 0.02);

    // Compass interference: the estimated heading follows the compass.
    Failure compass = defaultFailure(FailureType::CompassError);
    compass.offsetDeg = 40.0;
    world.injectFailure(compass);
    run(world, c, 15.0);
    const double trueYaw = eulerFromOrientation(world.vehicle().bodyState().orientation).yaw;
    const double estimated = world.vehicle().telemetry().estimate.euler.yaw;
    EXPECT_NEAR(common::wrapRadiansPi(estimated - trueYaw) / kDeg, 40.0, 4.0);

    // Barometer reading 0.2 m/s too high for 20 s: altitude hold sinks about 4 m.
    world.clearFailures();
    run(world, c, 3.0);
    const double before = world.vehicle().bodyState().position.y;
    Failure baro = defaultFailure(FailureType::BaroDrift);
    baro.durationS = 20.0;
    world.injectFailure(baro);
    run(world, c, 19.5);
    EXPECT_NEAR(world.vehicle().bodyState().position.y - before, -3.9, 1.0);
}

TEST(FailureInjection, RcLossStartsTheFailsafeAndAResetEndsEverything) {
    sim::World world = gpsWorld("camera-250g");
    input::ControlInputs c = takeOff(world);
    Failure rc = defaultFailure(FailureType::RcLoss);
    rc.durationS = 4.0;
    world.injectFailure(rc);
    run(world, c, 2.0);
    EXPECT_TRUE(world.vehicle().telemetry().failsafe);
    EXPECT_EQ(world.vehicle().telemetry().mode, FlightMode::ReturnToHome);
    run(world, c, 3.0);
    EXPECT_FALSE(world.vehicle().telemetry().failsafe);  // Link back: the pilot has control.

    world.injectFailure(defaultFailure(FailureType::GpsLoss));
    world.reset();
    EXPECT_TRUE(world.failures().active().empty());
}

TEST(FlightController, GpsModesNeedAFix) {
    const AerialProfile p = loadProfile("camera-250g");
    ControllerHarness h(p);
    PilotCommand c;
    c.mode = FlightMode::PositionHold;
    c.throttle = 0.5;
    h.tick(c);
    c.arm = true;
    h.tick(c);
    EXPECT_FALSE(h.fc.armed());
    EXPECT_EQ(h.fc.armBlock(), ArmBlock::NoGpsFix);
    c.arm = false;
    c.returnToHome = true;
    h.tick(c);
    c.arm = true;
    c.mode = FlightMode::AltitudeHold;
    h.tick(c);
    EXPECT_EQ(h.fc.armBlock(), ArmBlock::RthSwitchOn);
    c.arm = false;
    c.returnToHome = false;
    h.tick(c);
    c.arm = true;
    h.tick(c);
    ASSERT_TRUE(h.fc.armed());
    EXPECT_FALSE(h.fc.home().has_value());
    // Position hold without a fix flies as altitude hold; the RTH switch is ignored.
    c.mode = FlightMode::PositionHold;
    h.tick(c, 10);
    EXPECT_EQ(h.fc.mode(), FlightMode::AltitudeHold);
    EXPECT_TRUE(h.fc.gpsModeUnavailable());
    c.mode = FlightMode::Angle;
    c.returnToHome = true;
    h.tick(c, 10);
    EXPECT_EQ(h.fc.mode(), FlightMode::Angle);
    EXPECT_TRUE(h.fc.gpsModeUnavailable());
    // The first fix sets home; then both work.
    h.state.positionValid = true;
    h.state.position = Vec3(40.0, 20.0, -30.0);
    h.tick(c, 10);
    ASSERT_TRUE(h.fc.home().has_value());
    EXPECT_EQ(*h.fc.home(), Vec3(40.0, 0.0, -30.0));
    EXPECT_EQ(h.fc.mode(), FlightMode::ReturnToHome);
    EXPECT_EQ(h.fc.rthStage(), RthStage::Descend);  // At home already: lands at once.
    EXPECT_FALSE(h.fc.gpsModeUnavailable());
}

TEST(FlightController, AngleModeLimitsTiltAndSelfLevels) {
    // NFR-REAL-04 (self-levelling modes): full stick holds the profile's maximum angle;
    // centring the stick levels the drone without a sustained oscillation or drift — also
    // without a GPS fix (none here), where the attitude rests on the accelerometer alone. While
    // the drone brakes from the bank, rotor drag reads as tilt (as on real flight controllers):
    // the drone leans a few degrees against its motion, then settles within ≈ 25 s to the
    // sensor biases. Flown in altitude hold (Angle + throttle hold) so the bank costs no height.
    for (const char* key : kProfiles) {
        AerialProfile p = loadProfile(key);
        p.sensors.gpsAcquisitionS = 1e6;
        sim::World world = makeWorld(p);
        input::ControlInputs c = takeOff(world, 4.0);
        c.roll = 1.0;
        run(world, c, 1.5);
        EXPECT_NEAR(eulerFromOrientation(world.vehicle().bodyState().orientation).roll / kDeg,
                    p.controller.maxAngleDeg, 3.0)
            << key;
        c.roll = 0.0;
        double peak = 0.0;
        double late = 0.0;
        for (int i = 0; i < 30 * 500; ++i) {
            world.tick(c, kDt);
            const double tilt = tiltAngle(world.vehicle().bodyState().orientation);
            if (i >= 2 * 500) {
                peak = std::max(peak, tilt);
            }
            if (i >= 25 * 500) {
                late = std::max(late, tilt);
            }
        }
        const RigidBodyState& b = world.vehicle().bodyState();
        EXPECT_LT(peak, 10.0 * kDeg) << key;  // Braking from 5–12 m/s.
        EXPECT_LT(late, 1.5 * kDeg) << key;
        EXPECT_LT(std::hypot(b.velocity.x, b.velocity.z), 1.5) << key;
        EXPECT_FALSE(world.vehicle().telemetry().crashed) << key;
        EXPECT_FALSE(world.vehicle().telemetry().estimate.positionValid) << key;
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
        c.throttle = world.aerialVehicle()->controller().collective();
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
