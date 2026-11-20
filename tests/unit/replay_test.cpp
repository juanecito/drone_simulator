// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of session recording and replays (FR-X-05, FR-X-06; M7 step 7): the input
// stream, the replay log, the 20 Hz telemetry and bit-exact replays of aerial and ground
// sessions.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "ds/common/json.hpp"
#include "ds/common/random.hpp"
#include "ds/sim/hud.hpp"
#include "ds/sim/recording.hpp"
#include "ds/sim/world.hpp"

namespace {

using namespace ds;
using namespace ds::sim;

physics::VehicleProfile profile(const char* type, const char* name) {
    const auto path = std::filesystem::path(DS_TEST_ASSETS_DIR) / "vehicles" / type / name;
    if (std::string_view(type) == "ground") {
        auto p = physics::loadGroundProfile(path);
        EXPECT_TRUE(p.has_value());
        return p ? physics::VehicleProfile(*p) : physics::VehicleProfile{};
    }
    auto p = physics::loadAerialProfile(path);
    EXPECT_TRUE(p.has_value());
    return p ? physics::VehicleProfile(*p) : physics::VehicleProfile{};
}

weather::WeatherSettings showers() {
    auto json = common::parseJson(R"({
        "seed": 42, "preset": "light-rain",
        "timeline": [{"atS": 20, "preset": "thunderstorm", "transitionS": 10}]
    })");
    EXPECT_TRUE(json.has_value());
    auto settings = weather::WeatherSettings::fromJson(*json);
    EXPECT_TRUE(settings.has_value()) << (settings ? "" : settings.error().message);
    return settings.value_or(weather::WeatherSettings{});
}

// Pilot controls that change every tick (stick noise) and every few seconds (manoeuvres).
input::ControlInputs pilot(int i, common::GaussianRandom& noise, bool ground) {
    input::ControlInputs c;
    c.flightMode = 1;
    c.arm = i > 10;
    const double t = i * 0.002;
    if (i < 20) {
        return c;  // Sticks centred, throttle down: the arm switch is accepted.
    }
    if (ground) {
        c.drive = std::clamp(0.6 + 0.3 * std::sin(t) + noise.next(0.02), -1.0, 1.0);
        c.steering = std::clamp(0.5 * std::sin(0.7 * t) + noise.next(0.02), -1.0, 1.0);
        c.brake = std::fmod(t, 9.0) > 8.0;
    } else {
        c.throttle = i < 1500 ? 0.75 : std::clamp(0.5 + noise.next(0.03), 0.0, 1.0);
        c.roll = std::clamp(0.3 * std::sin(0.9 * t) + noise.next(0.02), -1.0, 1.0);
        c.pitch = std::clamp(0.25 * std::cos(0.5 * t) + noise.next(0.02), -1.0, 1.0);
        c.yaw = std::clamp(0.2 * std::sin(0.3 * t), -1.0, 1.0);
    }
    return c;
}

struct Flown {
    physics::RigidBodyState body;
    SessionStats stats;
    double consumedWh = 0.0;
    std::uint64_t tick = 0;
    FlightRecording recording;
};

// Flies a session with mid-session changes in a world that already flew another one, and
// returns its recording and the final state.
Flown flySession(const physics::VehicleProfile& vehicle, bool ground) {
    WorldConfig config;
    config.profile = vehicle;
    config.seed = 1234;
    config.weather = showers();
    World world(std::move(config));
    world.setRecording(true);
    world.setSceneTag({{"terrain", "flat"}});
    common::GaussianRandom noise(9);
    // A first session: the replayed one starts from a world with a history.
    for (int i = 0; i < 1500; ++i) {
        world.tick(pilot(i, noise, ground), 0.002);
    }
    world.resetAt({12.0, 0.0, -30.0}, 0.7);
    const int ticks = 9000;
    for (int i = 0; i < ticks; ++i) {
        const double dt = i >= 4000 && i < 5000 ? 0.001 : 0.002;
        if (i == 2000) {
            physics::Failure f = physics::defaultFailure(physics::FailureType::MotorFailure);
            f.motor = ground ? 2 : 3;
            f.efficiency = 0.7;
            f.durationS = 3.0;
            world.injectFailure(f);
            world.injectFailure(physics::defaultFailure(physics::FailureType::GpsDrift));
        }
        if (i == 3500) {
            world.clearFailures();
            world.setWind({.level = 5, .fromDeg = 200.0, .randomDirection = false});
        }
        if (i == 6000) {
            world.setAir({.temperatureC = 2.0, .pressureHpa = 990.0, .humidityPct = 95.0});
            world.setVisibility(3000.0);
            world.setUtc(world.utc() + std::chrono::hours(3));
        }
        world.tick(pilot(i, noise, ground), dt);
    }
    Flown f;
    f.body = world.vehicle().bodyState();
    f.stats = world.stats();
    f.consumedWh = world.vehicle().battery().consumedWh();
    f.tick = world.tickCount();
    world.reset();
    auto recordings = world.takeRecordings();
    EXPECT_EQ(recordings.size(), 2U);
    if (!recordings.empty()) {
        f.recording = std::move(recordings.back());
    }
    return f;
}

// Replays @p log in a different world (other seed, vehicle, weather and history).
void expectExactReplay(const Flown& live, bool ground) {
    WorldConfig config;
    config.profile = profile("aerial", "fpv-5in-racer.json");
    config.seed = 77;
    config.wind.level = 2;
    World world(std::move(config));
    common::GaussianRandom noise(3);
    for (int i = 0; i < 700; ++i) {
        world.tick(pilot(i, noise, false), 0.004);
    }
    // The log goes through its stored form first.
    const FlightRecording& r = live.recording;
    auto log = ReplayLog::fromJson(*common::parseJson(r.replay.metaJson().dump()));
    ASSERT_TRUE(log.has_value()) << log.error().message;
    log->inputs = r.replay.inputs;
    log->ticks = r.replay.ticks;
    ASSERT_TRUE(world.startReplay(std::move(*log), nullptr).has_value());
    EXPECT_TRUE(world.replayStatus().active);
    EXPECT_NEAR(world.replayStatus().durationS, 17.0, 1e-9);
    // The controls and time step the loop passes are ignored.
    for (std::uint64_t i = 0; i < r.replay.ticks; ++i) {
        world.tick(pilot(static_cast<int>(i), noise, !ground), 0.0037);
    }
    EXPECT_FALSE(world.replayStatus().finished);
    const physics::RigidBodyState& body = world.vehicle().bodyState();
    EXPECT_EQ(body.position, live.body.position);
    EXPECT_EQ(body.velocity, live.body.velocity);
    EXPECT_EQ(body.orientation, live.body.orientation);
    EXPECT_EQ(world.vehicle().battery().consumedWh(), live.consumedWh);
    EXPECT_EQ(world.stats().maxSpeedMs, live.stats.maxSpeedMs);
    EXPECT_EQ(world.stats().distanceM, live.stats.distanceM);
    EXPECT_EQ(world.stats().crashes, live.stats.crashes);
    EXPECT_EQ(world.tickCount(), live.tick);
    EXPECT_EQ(world.takeRecordings().size(), 0U);  // Replays are not recorded.

    // At the end the world stops advancing.
    world.tick({}, 0.002);
    EXPECT_TRUE(world.replayStatus().finished);
    world.tick({}, 0.002);
    EXPECT_EQ(world.vehicle().bodyState().position, live.body.position);
    EXPECT_EQ(world.tickCount(), live.tick);
}

// --- Input stream ----------------------------------------------------------------------------

TEST(ReplayInputs, RoundTripOfQuantisedControls) {
    common::GaussianRandom noise(1);
    std::vector<input::ControlInputs> inputs;
    for (int i = 0; i < 3000; ++i) {
        input::ControlInputs c = pilot(i, noise, i % 2 == 0);
        c.flightMode = (i / 500) % 5;
        c.beeper = i % 7 == 0;
        c.returnToHome = i > 2000;
        c.linkValid = i < 2900;
        c.lights = i % 300 < 10;
        inputs.push_back(quantized(c));
    }
    const auto bytes = encodeInputs(inputs);
    const auto decoded = decodeInputs(bytes);
    ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
    EXPECT_EQ(*decoded, inputs);
    EXPECT_EQ(quantized(quantized(inputs[5])), quantized(inputs[5]));
}

TEST(ReplayInputs, SteadyControlsTakeOneByteATick) {
    std::vector<input::ControlInputs> inputs(10'000);
    for (input::ControlInputs& c : inputs) {
        c.throttle = 0.5;
        c.arm = true;
    }
    InputEncoder e;
    for (const input::ControlInputs& c : inputs) {
        e.add(c);
    }
    EXPECT_EQ(e.ticks(), 10'000U);
    EXPECT_LT(e.bytes().size(), 10'000U + 32U);
}

TEST(ReplayInputs, DamagedStreamsAreRejected) {
    std::vector<input::ControlInputs> inputs(3);
    inputs[1].throttle = 0.3;
    auto bytes = encodeInputs(inputs);
    EXPECT_FALSE(decodeInputs(std::vector<std::uint8_t>{1, 2, 3}).has_value());
    auto truncated = bytes;
    truncated.resize(truncated.size() - 1);
    EXPECT_FALSE(decodeInputs(truncated).has_value());
    auto otherFormat = bytes;
    otherFormat[4] = 99;
    EXPECT_FALSE(decodeInputs(otherFormat).has_value());
}

// --- Replay log --------------------------------------------------------------------------------

TEST(ReplayLog, MetaJsonRoundTripAndDuration) {
    ReplayLog log;
    log.header = {{"format", ReplayLog::kFormat}, {"dt", 0.002}};
    log.ticks = 1000;
    log.commands.push_back({100, {{"kind", "clearFailures"}}});
    log.commands.push_back({500, {{"kind", "dt"}, {"value", 0.001}}});
    log.truncated = true;
    auto back = ReplayLog::fromJson(log.metaJson());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->header, log.header);
    EXPECT_EQ(back->commands, log.commands);
    EXPECT_TRUE(back->truncated);
    EXPECT_TRUE(back->replayable);
    back->ticks = log.ticks;
    EXPECT_NEAR(back->durationS(), 500 * 0.002 + 500 * 0.001, 1e-12);
    EXPECT_FALSE(ReplayLog::fromJson(common::Json::object()).has_value());
    EXPECT_FALSE(
        ReplayLog::fromJson({{"format", 99}, {"header", common::Json::object()}}).has_value());
}

TEST(ReplayLog, FailuresRoundTripThroughJson) {
    physics::Failure f = physics::defaultFailure(physics::FailureType::BatteryDrop);
    f.fraction = 0.45;
    f.durationS = 12.0;
    const auto back = failureFromJson(failureToJson(f));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->type, f.type);
    EXPECT_EQ(back->fraction, 0.45);
    EXPECT_EQ(back->durationS, 12.0);
    EXPECT_FALSE(failureFromJson({{"type", "warp-drive"}}).has_value());
}

// --- Recording -----------------------------------------------------------------------------------

TEST(Recording, SessionsRunFromResetToReset) {
    WorldConfig config;
    config.profile = profile("aerial", "camera-250g.json");
    World world(std::move(config));
    EXPECT_FALSE(world.recording());
    world.tick({}, 0.002);
    world.reset();
    EXPECT_TRUE(world.takeRecordings().empty());  // Not recording.

    world.setRecording(true);
    world.reset();
    world.reset();  // A session without a tick is dropped.
    EXPECT_TRUE(world.takeRecordings().empty());
    world.setSceneTag({{"terrain", "hills"}});
    for (int i = 0; i < 1001; ++i) {
        world.tick({}, 0.002);
    }
    world.finishRecording();
    auto r = world.takeRecordings();
    ASSERT_EQ(r.size(), 1U);
    EXPECT_EQ(r[0].replay.ticks, 1001U);
    EXPECT_EQ(r[0].profileKey, "camera-250g");
    EXPECT_EQ(r[0].type, physics::VehicleType::Aerial);
    EXPECT_EQ(r[0].replay.header["scene"]["terrain"], "hills");
    EXPECT_EQ(r[0].replay.header["dt"], 0.002);
    EXPECT_TRUE(r[0].replay.replayable);
    EXPECT_NEAR(r[0].stats.durationS, 2.002, 1e-9);  // Of the session, not of the world.
    // 20 Hz telemetry from t = 0.
    ASSERT_EQ(r[0].samples.size(), 41U);
    for (std::size_t i = 0; i < r[0].samples.size(); ++i) {
        EXPECT_NEAR(r[0].samples[i].tS, static_cast<double>(i) / kTelemetryRateHz, 0.0021);
        EXPECT_GT(r[0].samples[i].batteryV, 7.0);
    }
}

TEST(Recording, SceneChangesBeforeTheFlightStartANewSession) {
    WorldConfig config;
    config.profile = profile("aerial", "camera-250g.json");
    World world(std::move(config));
    world.setRecording(true);
    world.reset();
    for (int i = 0; i < 100; ++i) {
        world.tick({}, 0.002);
    }
    // Not flown yet: the ground changes and a new, replayable session starts on it.
    world.setGround(flatGround(3.0), std::nullopt, 0.0);
    world.setSceneTag({{"terrain", "raised"}});
    EXPECT_EQ(world.takeRecordings().size(), 1U);
    input::ControlInputs c;
    c.flightMode = 2;
    for (int i = 0; i < 1000; ++i) {
        c.arm = i > 5;
        c.throttle = i < 10 ? 0.5 : 0.7;
        world.tick(c, 0.002);
    }
    EXPECT_GT(world.stats().flightTimeS, 0.0);
    // After flying the session cannot be replayed any more.
    world.setGround(flatGround(0.0), std::nullopt, 0.0);
    world.finishRecording();
    auto r = world.takeRecordings();
    ASSERT_EQ(r.size(), 1U);
    EXPECT_EQ(r[0].replay.header["scene"]["terrain"], "raised");
    EXPECT_FALSE(r[0].replay.replayable);
}

TEST(Recording, LongSessionsAreTruncated) {
    WorldConfig config;
    config.profile = profile("aerial", "camera-250g.json");
    World world(std::move(config));
    world.setRecording(true);
    world.reset();
    const int ticks = static_cast<int>(kMaxRecordedS / 0.25) + 10;
    for (int i = 0; i < ticks; ++i) {
        world.tick({}, 0.25);
    }
    world.finishRecording();
    auto r = world.takeRecordings();
    ASSERT_EQ(r.size(), 1U);
    EXPECT_TRUE(r[0].replay.truncated);
    EXPECT_EQ(r[0].replay.ticks, static_cast<std::uint64_t>(kMaxRecordedS / 0.25));
    EXPECT_LE(r[0].samples.size(), static_cast<std::size_t>(kMaxRecordedS * 20.0) + 1);
}

// --- Replay
// ----------------------------------------------------------------------------------------

TEST(Replay, AerialSessionsReplayExactly) {
    const Flown live = flySession(profile("aerial", "camera-250g.json"), false);
    ASSERT_GT(live.recording.replay.ticks, 0U);
    EXPECT_GT(live.stats.maxAglM, 2.0);  // It did fly.
    EXPECT_EQ(live.recording.replay.commands.size(), 9U);
    expectExactReplay(live, false);
}

TEST(Replay, GroundSessionsReplayExactly) {
    const Flown live = flySession(profile("ground", "rover-4wd-3kg.json"), true);
    ASSERT_GT(live.recording.replay.ticks, 0U);
    EXPECT_GT(live.stats.distanceM, 5.0);  // It did drive.
    expectExactReplay(live, true);
}

TEST(Replay, DamagedLogsAreRefused) {
    WorldConfig config;
    config.profile = profile("aerial", "camera-250g.json");
    World world(std::move(config));
    EXPECT_FALSE(world.startReplay(ReplayLog{}, nullptr).has_value());
    ReplayLog log;
    log.header = {{"format", ReplayLog::kFormat}, {"dt", 0.002}};
    EXPECT_FALSE(world.startReplay(log, nullptr).has_value());
    EXPECT_FALSE(world.replayStatus().active);
}

TEST(Replay, HudReadOut) {
    EXPECT_TRUE(replayItem({}).text.empty());
    const HudItem playing = replayItem({.active = true, .timeS = 72.4, .durationS = 605.0});
    EXPECT_EQ(playing.text, "REPLAY 1:12 / 10:05");
    EXPECT_EQ(playing.level, HudLevel::Normal);
    const HudItem ended =
        replayItem({.active = true, .timeS = 605.0, .durationS = 605.0, .finished = true});
    EXPECT_EQ(ended.text, "REPLAY ENDED 10:05");
    EXPECT_EQ(ended.level, HudLevel::Caution);
}

}  // namespace
