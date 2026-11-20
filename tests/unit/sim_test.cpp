// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the simulation loop (FR-LOOP-*): fixed-step clock, triple buffer, event bus,
// world, loop thread, events and the HUD model.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ds/common/event_bus.hpp"
#include "ds/common/json.hpp"
#include "ds/common/math.hpp"
#include "ds/common/triple_buffer.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/sim/events.hpp"
#include "ds/sim/fixed_step.hpp"
#include "ds/sim/hud.hpp"
#include "ds/sim/simulation_loop.hpp"
#include "ds/sim/world.hpp"

namespace {

using namespace ds;
using namespace ds::sim;

physics::AerialProfile cameraProfile() {
    auto p = physics::loadAerialProfile(std::filesystem::path(DS_TEST_ASSETS_DIR) / "vehicles" /
                                        "aerial" / "camera-250g.json");
    EXPECT_TRUE(p.has_value());
    return p ? *p : physics::AerialProfile{};
}

WorldConfig cameraWorld() {
    WorldConfig c;
    c.profile = cameraProfile();
    c.seed = 5;
    return c;
}

// --- Fixed step ------------------------------------------------------------------------------

TEST(FixedStepClock, AccumulatesAndClamps) {
    FixedStepClock clock;
    clock.setRate(500.0);
    EXPECT_DOUBLE_EQ(clock.step(), 0.002);
    EXPECT_EQ(clock.advance(0.0101), 5);
    EXPECT_NEAR(clock.alpha(), 0.05, 1e-6);
    EXPECT_EQ(clock.advance(0.0019), 1);  // 0.0001 + 0.0019 = one step.
    clock.setRate(10.0);
    EXPECT_DOUBLE_EQ(clock.rate(), FixedStepClock::kMinRateHz);
    clock.setRate(1e6);
    EXPECT_DOUBLE_EQ(clock.rate(), FixedStepClock::kMaxRateHz);
    clock.setTimeScale(100.0);
    EXPECT_DOUBLE_EQ(clock.timeScale(), FixedStepClock::kMaxTimeScale);
    clock.setTimeScale(0.0);
    EXPECT_DOUBLE_EQ(clock.timeScale(), FixedStepClock::kMinTimeScale);
}

TEST(FixedStepClock, TimeScaleAndCatchUpLimit) {
    FixedStepClock clock;
    clock.setRate(1000.0);
    clock.setTimeScale(2.0);
    EXPECT_EQ(clock.advance(0.01), 20);
    clock.setTimeScale(1.0);
    clock.reset();
    // A 2 s stall: at most 0.25 s is simulated, the rest is dropped (no spiral of death).
    EXPECT_EQ(clock.advance(2.0), 250);
    EXPECT_NEAR(clock.droppedSeconds(), 1.75, 1e-9);
}

// --- Triple buffer and event bus -------------------------------------------------------------

TEST(TripleBuffer, ReaderSeesLatestCompleteValue) {
    common::TripleBuffer<int> buffer(0);
    EXPECT_FALSE(buffer.update());
    buffer.writeBuffer() = 1;
    buffer.publish();
    buffer.writeBuffer() = 2;
    buffer.publish();
    EXPECT_TRUE(buffer.update());
    EXPECT_EQ(buffer.readBuffer(), 2);
    EXPECT_FALSE(buffer.update());
    EXPECT_EQ(buffer.readBuffer(), 2);
}

TEST(TripleBuffer, ConcurrentProducerConsumerNeverTears) {
    struct Pair {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
    };
    common::TripleBuffer<Pair> buffer;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= 200000; ++i) {
            Pair& p = buffer.writeBuffer();
            p.a = i;
            p.b = ~i;
            buffer.publish();
        }
        done = true;
    });
    std::uint64_t last = 0;
    bool ok = true;
    while (!done.load() || buffer.update()) {
        if (buffer.update()) {
            const Pair& p = buffer.readBuffer();
            ok = ok && p.b == ~p.a && p.a >= last;
            last = p.a;
        }
    }
    producer.join();
    EXPECT_TRUE(ok);
    (void)buffer.update();
    EXPECT_EQ(buffer.readBuffer().a, 200000U);
}

TEST(EventBus, TypedSubscriptionsAndUnsubscribe) {
    EventBus bus;
    int crashes = 0;
    int all = 0;
    const auto typed =
        bus.subscribe<CrashEvent>([&](const CrashEvent& e) { crashes += e.reason == "x" ? 1 : 0; });
    const auto any = bus.subscribe([&](const Event&) { ++all; });
    bus.publish(CrashEvent{"x", 3.0});
    bus.publish(ArmingEvent{true});
    EXPECT_EQ(crashes, 1);
    EXPECT_EQ(all, 2);
    EXPECT_EQ(bus.subscriberCount(), 2U);
    EXPECT_TRUE(bus.unsubscribe(typed));
    EXPECT_FALSE(bus.unsubscribe(typed));
    bus.publish(CrashEvent{"x", 3.0});
    EXPECT_EQ(crashes, 1);
    EXPECT_EQ(all, 3);
    EXPECT_TRUE(bus.unsubscribe(any));
    EXPECT_FALSE(describe(CrashEvent{"hard landing", 7.0}).empty());
    EXPECT_NE(describe(ArmRefusedEvent{"Throttle not low"}).find("Throttle"), std::string::npos);
}

// --- World -----------------------------------------------------------------------------------

TEST(World, MapsControlsToPilotCommands) {
    EXPECT_EQ(toFlightMode(0), physics::FlightMode::Acro);
    EXPECT_EQ(toFlightMode(1), physics::FlightMode::Angle);
    EXPECT_EQ(toFlightMode(2), physics::FlightMode::AltitudeHold);
    EXPECT_EQ(toFlightMode(3), physics::FlightMode::PositionHold);
    input::ControlInputs c;
    c.throttle = 0.3;
    c.roll = -0.5;
    c.arm = true;
    c.linkValid = false;
    c.returnToHome = true;
    const physics::PilotCommand p = toPilotCommand(c);
    EXPECT_DOUBLE_EQ(p.throttle, 0.3);
    EXPECT_DOUBLE_EQ(p.roll, -0.5);
    EXPECT_TRUE(p.arm);
    EXPECT_FALSE(p.linkValid);
    EXPECT_TRUE(p.returnToHome);
}

TEST(World, StartsOnTheGroundAndPublishesArmingEvents) {
    WorldConfig config = cameraWorld();
    config.ground = flatGround(12.0);
    config.startPosition = physics::Vec3(30.0, 0.0, -40.0);
    World world(std::move(config));
    const physics::Vec3 start = world.vehicle().bodyState().position;
    EXPECT_DOUBLE_EQ(start.x, 30.0);
    EXPECT_DOUBLE_EQ(start.z, -40.0);
    EXPECT_NEAR(start.y, 12.0 + world.vehicle().restHeightM(), 0.01);

    std::vector<Event> events;
    world.events().subscribe([&](const Event& e) { events.push_back(e); });
    input::ControlInputs c;
    c.arm = true;  // On at start: refused (switch not cycled).
    world.tick(c, 0.002);
    c.arm = false;
    world.tick(c, 0.002);
    c.arm = true;
    world.tick(c, 0.002);
    ASSERT_TRUE(world.vehicle().armed());
    EXPECT_TRUE(std::ranges::any_of(
        events, [](const Event& e) { return std::holds_alternative<ArmRefusedEvent>(e); }));
    EXPECT_TRUE(std::ranges::any_of(events, [](const Event& e) {
        const auto* a = std::get_if<ArmingEvent>(&e);
        return a != nullptr && a->armed;
    }));
    EXPECT_EQ(world.tickCount(), 3U);
    EXPECT_NEAR(world.time(), 0.006, 1e-12);
}

TEST(World, BarometerStartsAtTheTakeOffAltitude) {
    // Regression: the air was sampled at world height 0 instead of on the ground, so the
    // barometric altitude started hundreds of metres off on high terrain.
    WorldConfig config = cameraWorld();
    config.ground = flatGround(400.0);
    config.originAltitudeM = 650.0;
    World world(std::move(config));
    WorldSnapshot s;
    world.fillSnapshot(s);
    EXPECT_NEAR(s.telemetry.estimate.altitudeM, s.amslM(), 1.0);
    for (int i = 0; i < 500; ++i) {
        world.tick({}, 0.002);
    }
    world.fillSnapshot(s);
    EXPECT_NEAR(s.telemetry.estimate.altitudeM, s.amslM(), 2.0);
    EXPECT_NEAR(s.telemetry.estimate.verticalSpeedMs, 0.0, 0.3);
}

TEST(World, SnapshotAndSessionStats) {
    WorldConfig config = cameraWorld();
    config.originAltitudeM = 650.0;
    config.wind.level = 3;
    config.startUtc =
        astro::UtcTime(std::chrono::sys_days(std::chrono::year(2026) / std::chrono::June / 21));
    World world(std::move(config));
    input::ControlInputs c;
    c.flightMode = 2;
    c.throttle = 0.5;
    world.tick(c, 0.002);
    c.arm = true;  // Centred throttle: allowed in altitude hold.
    world.tick(c, 0.002);
    for (int i = 0; i < 2499; ++i) {
        c.throttle = i < 1000 ? 1.0 : 0.5;
        world.tick(c, 0.002);
    }
    WorldSnapshot s;
    world.fillSnapshot(s);
    EXPECT_TRUE(s.hasVehicle);
    EXPECT_EQ(s.profileKey, "camera-250g");
    EXPECT_EQ(s.tick, world.tickCount());
    EXPECT_NEAR(s.simTimeS, 5.002, 1e-9);
    EXPECT_EQ(s.utc, world.utc());
    EXPECT_EQ(std::chrono::floor<std::chrono::seconds>(s.utc - world.config().startUtc).count(), 5);
    EXPECT_GT(s.aglM, 1.0);
    EXPECT_NEAR(s.amslM(), 650.0 + s.body.position.y, 1e-9);
    EXPECT_EQ(s.windLevel, 3);
    EXPECT_GT(s.windMeanSpeedMs, 0.0);
    EXPECT_TRUE(s.telemetry.armed);
    EXPECT_GT(s.stats.flightTimeS, 4.5);
    EXPECT_GT(s.stats.maxAglM, 1.0);
    EXPECT_GT(s.stats.energyWh, 0.0);
    EXPECT_EQ(s.stats.crashes, 0);

    const std::uint64_t resets = s.resetCount;
    world.reset();
    world.fillSnapshot(s);
    EXPECT_EQ(s.resetCount, resets + 1);
    EXPECT_FALSE(s.telemetry.armed);
    EXPECT_LT(s.aglM, 0.05);
}

weather::WeatherSettings weatherFrom(const char* text) {
    auto json = common::parseJson(text);
    EXPECT_TRUE(json.has_value());
    auto settings = weather::WeatherSettings::fromJson(*json);
    EXPECT_TRUE(settings.has_value());
    return settings.value_or(weather::WeatherSettings{});
}

TEST(World, WeatherTimelineChangesWindAndAir) {
    WorldConfig config = cameraWorld();
    config.weather = weatherFrom(R"({"seed": 3, "preset": "clear", "temperatureC": 20,
        "timeline": [{"atS": 10, "transitionS": 10, "preset": "light-rain", "temperatureC": 10,
                      "wind": {"level": 5, "fromDeg": 90}}]})");
    World world(std::move(config));
    const input::ControlInputs idle;
    WorldSnapshot s;
    for (int i = 0; i < 2500; ++i) {
        world.tick(idle, 0.002);
    }
    world.fillSnapshot(s);
    EXPECT_NEAR(s.weatherTimeS, 5.0, 1e-6);
    EXPECT_EQ(s.windLevel, 0);
    EXPECT_EQ(s.weather.precipitation.type, weather::PrecipitationType::None);
    EXPECT_DOUBLE_EQ(world.environment().ambientC, 20.0);
    EXPECT_DOUBLE_EQ(glm::length(world.wind().meanVelocity()), 0.0);

    for (int i = 0; i < 10'000; ++i) {
        world.tick(idle, 0.002);
    }
    world.fillSnapshot(s);
    EXPECT_EQ(s.windLevel, 5);
    EXPECT_DOUBLE_EQ(s.windMeanSpeedMs, 9.4);
    EXPECT_EQ(s.weather.precipitation.type, weather::PrecipitationType::Rain);
    EXPECT_DOUBLE_EQ(world.environment().ambientC, 10.0);
    EXPECT_LT(world.wind().meanVelocity().x, 0.0);  // From the east: the air moves west.
    EXPECT_FALSE(s.weather.clouds.empty());

    world.reset();  // The weather starts again with the session.
    world.fillSnapshot(s);
    EXPECT_DOUBLE_EQ(s.weatherTimeS, 0.0);
    EXPECT_EQ(s.windLevel, 0);
}

TEST(World, CloudsAndFogLimitTheVisibilityAtTheVehicle) {
    WorldConfig config = cameraWorld();
    config.weather = weatherFrom(R"({"seed": 1, "preset": "overcast",
        "clouds": [{"type": "stratus", "coverOktas": 8, "baseM": 30, "topM": 300}]})");
    World world(std::move(config));
    world.tick({}, 0.002);
    EXPECT_DOUBLE_EQ(world.visibilityAtVehicle(), 20'000.0);
    physics::RigidBodyState body = world.vehicle().bodyState();
    body.position.y += 100.0;
    world.vehicle().setBodyState(body);
    world.tick({}, 0.002);
    EXPECT_DOUBLE_EQ(world.visibilityAtVehicle(), weather::kInCloudVisibilityM);

    // Changing the air keeps the mission's sky.
    world.setAir({.temperatureC = 5.0, .pressureHpa = 1000.0, .humidityPct = 70.0});
    EXPECT_DOUBLE_EQ(world.air().temperatureC, 5.0);
    EXPECT_EQ(world.weather().conditions.clouds.size(), 1U);
    world.setVisibility(800.0);
    EXPECT_DOUBLE_EQ(world.weather().conditions.visibilityM, 800.0);
}

TEST(World, BasicSettingsBecomeAClearSky) {
    WorldConfig config = cameraWorld();
    config.wind = {.level = 2, .fromDeg = 180.0, .randomDirection = false};
    config.air.temperatureC = -5.0;
    config.visibilityM = 7000.0;
    World world(std::move(config));
    EXPECT_EQ(world.weatherSettings().preset, "clear");
    EXPECT_DOUBLE_EQ(world.weather().conditions.windLevel, 2.0);
    EXPECT_DOUBLE_EQ(world.weather().conditions.windFromDeg, 180.0);
    EXPECT_DOUBLE_EQ(world.air().temperatureC, -5.0);
    EXPECT_DOUBLE_EQ(world.weather().conditions.visibilityM, 7000.0);
    world.setWind({.level = 6, .fromDeg = 0.0, .randomDirection = false});
    EXPECT_DOUBLE_EQ(world.wind().params().meanSpeed10mMs, 12.3);
}

// --- Loop ------------------------------------------------------------------------------------

TEST(SimulationLoop, SynchronousTicksCommandsAndTimeScale) {
    int polls = 0;
    SimulationLoop loop(std::make_unique<World>(cameraWorld()), [&](double) {
        ++polls;
        return input::ControlInputs{};
    });
    loop.runTicks(100);
    EXPECT_EQ(loop.world().tickCount(), 100U);
    EXPECT_EQ(polls, 100);
    ASSERT_TRUE(loop.acquireSnapshot());
    EXPECT_EQ(loop.snapshot().tick, 100U);
    bool ran = false;
    loop.post([&](World&) { ran = true; });
    loop.runTicks(1);
    EXPECT_TRUE(ran);
    EXPECT_TRUE(loop.setTimeScale(2.0).has_value());
    loop.setMissionActive(true);
    EXPECT_FALSE(loop.setTimeScale(2.0).has_value());  // Real time only in missions.
    EXPECT_TRUE(loop.setTimeScale(1.0).has_value());
    loop.setMissionActive(false);
    EXPECT_TRUE(loop.setTimeScale(0.5).has_value());
}

TEST(SimulationLoop, ThreadRunsPausesStepsAndStops) {
    SimulationLoop loop(std::make_unique<World>(cameraWorld()),
                        [](double) { return input::ControlInputs{}; });
    loop.start();
    EXPECT_EQ(loop.state(), RunState::Running);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    loop.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)loop.acquireSnapshot();
    const std::uint64_t paused = loop.snapshot().tick;
    EXPECT_GT(paused, 10U);
    EXPECT_EQ(loop.state(), RunState::Paused);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)loop.acquireSnapshot();
    EXPECT_EQ(loop.snapshot().tick, paused);
    EXPECT_EQ(loop.snapshot().state, RunState::Paused);
    loop.step();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (loop.snapshot().tick == paused && std::chrono::steady_clock::now() < deadline) {
        (void)loop.acquireSnapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(loop.snapshot().tick, paused + 1);
    loop.resume();
    EXPECT_EQ(loop.state(), RunState::Running);
    loop.stop();
    EXPECT_EQ(loop.state(), RunState::Stopped);
    loop.shutdown();
}

TEST(SimulationLoop, EventsAreTimestamped) {
    SimulationLoop loop(std::make_unique<World>(cameraWorld()), [](double) {
        input::ControlInputs c;
        c.arm = true;  // Refused: switch on at start.
        return c;
    });
    loop.runTicks(10);
    const auto events = loop.takeEvents();
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(std::holds_alternative<ArmRefusedEvent>(events.front().event));
    EXPECT_GE(events.front().timeS, 0.0);
    EXPECT_TRUE(loop.takeEvents().empty());
}

// --- HUD -------------------------------------------------------------------------------------

TEST(Hud, GroupsDigitsAndFormatsUtm) {
    EXPECT_EQ(groupDigits(4474567.84, 1), "4 474 567.8");
    EXPECT_EQ(groupDigits(440123.0, 0), "440 123");
    EXPECT_EQ(groupDigits(12.5, 1), "12.5");
    EXPECT_EQ(groupDigits(-1234.5, 1), "-1 234.5");
    // On the equator at the central meridian of zone 31: 500 000 E, 0 N.
    const PositionText t = formatPosition({0.0, 3.0, 0.0}, 712.44, 35.2, false, true);
    EXPECT_EQ(t.utm, "UTM 31N  500 000.0 E  0.0 N");
    EXPECT_NE(t.elevation.find("712.4"), std::string::npos) << t.elevation;
    EXPECT_NE(t.elevation.find("AGL 35.2"), std::string::npos) << t.elevation;
    EXPECT_FALSE(t.latLon.empty());
    const PositionText polar = formatPosition({85.0, 3.0, 0.0}, 0.0, std::nullopt, false, false);
    EXPECT_NE(polar.utm.find("outside"), std::string::npos);
    EXPECT_EQ(polar.elevation.find("AGL"), std::string::npos);
    EXPECT_TRUE(polar.latLon.empty());
}

TEST(Hud, LevelsAndWarnings) {
    WorldSnapshot s;
    s.hasVehicle = true;
    s.telemetry.battery.cells = 2;
    s.telemetry.battery.voltage = 7.4;
    const geo::LocalFrame frame({40.4, -3.7, 650.0});
    s.aglM = 50.0;
    EXPECT_EQ(buildHud(s, frame, {}).altitudeAgl.level, HudLevel::Normal);
    s.aglM = 105.0;
    EXPECT_EQ(buildHud(s, frame, {}).altitudeAgl.level, HudLevel::Caution);
    s.aglM = 125.0;
    HudModel h = buildHud(s, frame, {});
    EXPECT_EQ(h.altitudeAgl.level, HudLevel::Warning);
    EXPECT_FALSE(h.warnings.empty());

    s.aglM = 10.0;
    s.telemetry.crashed = true;
    s.telemetry.battery.warning = physics::BatteryWarning::Critical;
    h = buildHud(s, frame, {});
    EXPECT_EQ(h.armed.text, "CRASHED");
    EXPECT_EQ(h.battery.level, HudLevel::Warning);
    EXPECT_GE(h.warnings.size(), 2U);
    EXPECT_NE(h.battery.text.find("3.70 V/cell"), std::string::npos) << h.battery.text;
    EXPECT_NE(h.positionUtm.find("UTM 30T"), std::string::npos) << h.positionUtm;

    s.windLevel = 0;
    EXPECT_EQ(buildHud(s, frame, {}).wind.text, "WIND 0 calm");
    EXPECT_FALSE(buildHud(s, frame, {}).windArrowRad.has_value());
    s.windLevel = 4;
    s.windFromDeg = 270.0;
    s.windMeanSpeedMs = 6.7;
    s.windGustMs = 9.5;
    h = buildHud(s, frame, {});
    ASSERT_TRUE(h.windArrowRad.has_value());
    EXPECT_NEAR(*h.windArrowRad, std::numbers::pi / 2.0, 1e-9);  // Blowing to the east.
    HudOptions hidden;
    hidden.showWind = false;
    EXPECT_TRUE(buildHud(s, frame, hidden).wind.text.empty());
    HudOptions imperial;
    imperial.imperial = true;
    EXPECT_NE(buildHud(s, frame, imperial).altitudeAgl.text.find("ft"), std::string::npos);
}

TEST(Hud, GroundDroneReadOutsAndWarnings) {
    // FR-GFX-20: attitude with the rollover warning, slope, slip, drive mode.
    WorldSnapshot s;
    s.hasVehicle = true;
    s.telemetry.type = physics::VehicleType::Ground;
    s.telemetry.battery.cells = 3;
    s.telemetry.battery.voltage = 11.4;
    s.telemetry.armed = true;
    physics::GroundTelemetry& g = s.telemetry.ground;
    g.wheelCount = 4;
    g.contact = {true, true, true, false};
    g.slip = {0.05, 0.25, 0.6, 0.0};
    g.maxSlip = 0.6;
    g.slopeDeg = 12.4;
    g.surface = physics::SurfaceMaterial::Sand;
    g.speedMs = 1.2;
    g.odometerM = 123.4;
    s.attitude.pitch = common::degToRad(4.0);
    s.attitude.roll = common::degToRad(-3.0);
    s.aglM = 0.0;
    const geo::LocalFrame frame({40.4, -3.7, 650.0});

    HudModel h = buildHud(s, frame, {});
    ASSERT_TRUE(h.ground.has_value());
    EXPECT_EQ(h.ground->pitch.text, "PITCH +4°");
    EXPECT_EQ(h.ground->roll.text, "ROLL -3°");
    EXPECT_EQ(h.ground->roll.level, HudLevel::Normal);
    EXPECT_EQ(h.ground->slope.text, "SLOPE 12° sand");
    EXPECT_EQ(h.ground->slip.text, "SLIP 60%");
    EXPECT_EQ(h.ground->slip.level, HudLevel::Warning);
    EXPECT_EQ(h.ground->odometer.text, "ODO 123 m");
    EXPECT_EQ(h.ground->wheelCount, 4);
    EXPECT_FALSE(h.ground->wheelContact[3]);
    EXPECT_DOUBLE_EQ(h.ground->wheelSlip[1], 0.25);
    EXPECT_TRUE(h.altitudeAgl.text.empty());  // Altitudes belong to aerial drones.
    EXPECT_TRUE(h.verticalSpeed.text.empty());
    EXPECT_EQ(h.mode.text, "Manual");
    EXPECT_EQ(h.speed.text.rfind("R ", 0), std::string::npos);
    EXPECT_TRUE(h.warnings.empty());

    // Load transfer towards the tipping point: amber, then red.
    g.loadTransfer = -0.7;
    h = buildHud(s, frame, {});
    EXPECT_EQ(h.ground->roll.level, HudLevel::Caution);
    EXPECT_DOUBLE_EQ(h.ground->loadTransfer, -0.7);
    ASSERT_EQ(h.warnings.size(), 1U);
    EXPECT_EQ(h.warnings[0].text, "ROLLOVER RISK");
    EXPECT_EQ(h.warnings[0].level, HudLevel::Caution);
    g.loadTransfer = 0.9;
    h = buildHud(s, frame, {});
    EXPECT_EQ(h.ground->stability, HudLevel::Warning);
    EXPECT_EQ(h.warnings[0].level, HudLevel::Warning);

    // Rolled over, stuck, in water; reversing; Speed control shows the set speed.
    g.rolledOver = true;
    g.stuckS = 3.0;
    g.inWater = true;
    g.speedMs = -0.5;
    g.mode = physics::DriveMode::SpeedControl;
    g.cruiseSetpointMs = 1.5;
    h = buildHud(s, frame, {});
    std::vector<std::string> texts;
    for (const HudItem& w : h.warnings) {
        texts.push_back(w.text);
    }
    EXPECT_EQ(texts,
              (std::vector<std::string>{"ROLLED OVER — PRESS R TO RESET", "IN WATER", "STUCK"}));
    EXPECT_EQ(h.speed.text.rfind("R ", 0), 0U) << h.speed.text;
    EXPECT_NE(h.mode.text.find("Speed"), std::string::npos) << h.mode.text;
    EXPECT_NE(h.mode.text.find("km/h"), std::string::npos) << h.mode.text;

    // Aerial drones have no ground block.
    s.telemetry.type = physics::VehicleType::Aerial;
    EXPECT_FALSE(buildHud(s, frame, {}).ground.has_value());
}

TEST(Hud, ReturnToHomeAndGpsModeWarnings) {
    WorldSnapshot s;
    s.hasVehicle = true;
    s.telemetry.armed = true;
    s.telemetry.mode = physics::FlightMode::ReturnToHome;
    s.telemetry.rthStage = physics::RthStage::Climb;
    const geo::LocalFrame frame({40.4, -3.7, 650.0});
    HudModel h = buildHud(s, frame, {});
    EXPECT_EQ(h.mode.text, "RTH climb");
    EXPECT_EQ(h.mode.level, HudLevel::Caution);

    s.telemetry.failsafe = true;
    s.telemetry.rthStage = physics::RthStage::Return;
    h = buildHud(s, frame, {});
    EXPECT_EQ(h.mode.text, "FAILSAFE RTH return");
    const auto has = [&](const std::string& text) {
        return std::ranges::any_of(h.warnings, [&](const HudItem& w) { return w.text == text; });
    };
    EXPECT_TRUE(has("RC LINK LOST — RETURNING HOME"));

    s.telemetry.failsafe = false;
    s.telemetry.rthStage.reset();
    s.telemetry.mode = physics::FlightMode::AltitudeHold;
    s.telemetry.gpsModeUnavailable = true;
    h = buildHud(s, frame, {});
    EXPECT_EQ(h.mode.text, "Alt hold");
    EXPECT_TRUE(has("NO GPS — ALTITUDE HOLD ONLY"));
    s.controls.returnToHome = true;
    h = buildHud(s, frame, {});
    EXPECT_TRUE(has("NO GPS — RTH UNAVAILABLE"));
}

TEST(Hud, WeatherItemInMetarStyle) {
    WorldSnapshot s;
    s.weather = weather::defaultConditions();
    s.visibilityM = 50'000.0;
    HudItem item = weatherItem(s, false);
    EXPECT_EQ(item.text, "WX SKC VIS 50 km 15 °C");
    EXPECT_EQ(item.level, HudLevel::Normal);

    s.weather.clouds = {{weather::CloudType::Cirrus, 2.0, 7000.0, 7500.0},
                        {weather::CloudType::Stratocumulus, 0.2, 500.0, 800.0}};
    std::ranges::sort(s.weather.clouds, {}, &weather::CloudLayer::baseM);
    s.weather.precipitation = {weather::PrecipitationType::Rain, 1.0};
    s.weather.air.temperatureC = 12.4;
    s.visibilityM = 6000.0;
    item = weatherItem(s, false);
    // The layer under half an okta does not show.
    EXPECT_EQ(item.text, "WX FEW 7000 m -RA VIS 6 km 12 °C");
    EXPECT_EQ(item.level, HudLevel::Caution);

    s.weather.clouds = {{weather::CloudType::Cumulonimbus, 6.0, 900.0, 9000.0}};
    s.weather.precipitation = {weather::PrecipitationType::Rain, 12.0};
    s.weather.thunderstorm = true;
    s.visibilityM = 2500.0;
    item = weatherItem(s, true);
    EXPECT_EQ(item.text, "WX BKN 2953 ft CB +TSRA VIS 1.6 mi 54 °F");
    EXPECT_EQ(item.level, HudLevel::Warning);

    // Inside the cloud the visibility at the vehicle is what counts.
    s.weather = weather::defaultConditions();
    s.visibilityM = weather::kInCloudVisibilityM;
    item = weatherItem(s, false);
    EXPECT_EQ(item.text, "WX SKC VIS 60 m 15 °C");
    EXPECT_EQ(item.level, HudLevel::Warning);
    s.hasVehicle = true;
    EXPECT_EQ(buildHud(s, geo::LocalFrame({40.4, -3.7, 650.0}), {}).weather.text, item.text);
}

TEST(Hud, CompassUsesTheWorldMagneticModel) {
    using namespace std::chrono;
    const auto utc = [](year_month_day d, hours h) {
        return time_point_cast<milliseconds>(sys_days{d} + h);
    };
    // Points and declinations from the official WMM2025 test values.
    HudCompass c = buildCompass({23.0, 63.0, 69000.0}, 90.0, utc(2026y / January / 1, 0h));
    EXPECT_NEAR(c.declinationDeg, 1.17, 0.006);
    EXPECT_NEAR(c.magneticHeadingDeg, 90.0 - 1.17, 0.006);
    EXPECT_EQ(c.heading.text, "HDG 090°T  089°M");
    EXPECT_EQ(c.declination.text, "DECL 1.2°E");
    EXPECT_EQ(c.declination.level, HudLevel::Normal);

    c = buildCompass({-36.0, -137.0, 6000.0}, 10.0, utc(2025y / July / 2, 12h));  // 2025.5
    EXPECT_NEAR(c.declinationDeg, 20.28, 0.006);
    EXPECT_NEAR(c.magneticHeadingDeg, 360.0 + 10.0 - 20.28, 0.006);  // Wraps below 0°.
    EXPECT_EQ(c.heading.text, "HDG 010°T  350°M");

    c = buildCompass({-66.0, -5.0, 37000.0}, 359.8, utc(2027y / January / 1, 0h));
    EXPECT_NEAR(c.declinationDeg, -17.22, 0.006);
    EXPECT_EQ(c.heading.text, "HDG 000°T  017°M");  // 359.8° rounds to 000°, not 360°.
    EXPECT_EQ(c.declination.text, "DECL 17.2°W");

    // Outside the model's years: still shown, flagged.
    c = buildCompass({40.4, -3.7, 650.0}, 0.0, utc(2020y / June / 1, 0h));
    EXPECT_EQ(c.declination.level, HudLevel::Caution);
    EXPECT_NE(c.declination.text.find("WMM-2025 2025–2030"), std::string::npos)
        << c.declination.text;
    // Near the north magnetic pole the horizontal field is too weak for a compass.
    c = buildCompass({89.0, -121.0, 0.0}, 0.0, utc(2025y / January / 1, 0h));
    EXPECT_EQ(c.declination.level, HudLevel::Warning);
    EXPECT_EQ(c.declination.text, "MAG UNRELIABLE");

    // The HUD fills the compass from the vehicle's attitude.
    WorldSnapshot s;
    s.hasVehicle = true;
    s.attitude.yaw = std::numbers::pi / 2.0;
    s.utc = utc(2026y / March / 1, 12h);
    const HudModel h = buildHud(s, geo::LocalFrame({40.4, -3.7, 650.0}), {});
    EXPECT_NEAR(h.compass.trueHeadingDeg, 90.0, 1e-9);
    EXPECT_DOUBLE_EQ(h.headingDeg, h.compass.trueHeadingDeg);
    EXPECT_GT(h.compass.declinationDeg, 0.0);  // Madrid, 2026: about 1.5° E.
    EXPECT_LT(h.compass.declinationDeg, 3.0);
}

}  // namespace
