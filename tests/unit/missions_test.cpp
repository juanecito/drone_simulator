// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>

#include "ds/common/json_schema.hpp"
#include "ds/missions/catalog.hpp"
#include "ds/missions/flight_rules.hpp"
#include "ds/missions/mission_loader.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/missions/mission_world.hpp"

#include <unistd.h>

namespace {

using namespace ds::missions;
using ds::common::Json;
using ds::items::Vec3;
namespace fs = std::filesystem;

const fs::path kFixtures = fs::path(DS_TEST_FIXTURES_DIR) / "missions";

const ds::common::JsonSchema& schema() {
    static const auto s = [] {
        auto loaded = ds::common::JsonSchema::load(fs::path(DS_TEST_ASSETS_DIR) / "missions" /
                                                   "mission.schema.json");
        EXPECT_TRUE(loaded.has_value());
        return std::move(*loaded);
    }();
    return s;
}

ValidationContext context() {
    ValidationContext ctx;
    ctx.schema = &schema();
    addBuiltInKeys(ctx, DS_TEST_ASSETS_DIR);
    ctx.terrainPresets = {"rolling-hills", "alpine", "flatlands", "island", "coast"};
    return ctx;
}

Json fixture(const char* name) {
    std::ifstream in(kFixtures / name);
    return Json::parse(in);
}

MissionDefinition definition(const Json& json) {
    auto r = validateMission(json, context());
    EXPECT_TRUE(r.valid()) << r.report();
    return std::move(*r.definition);
}

std::string errorsOf(const Json& json) {
    std::string out;
    for (const MissionIssue& e : validateMission(json, context()).errors) {
        out += e.text() + "\n";
    }
    return out;
}

// ----- loading and validation ----------------------------------------------------------

TEST(MissionLoader, SpecificationExamplesAreValid) {
    const auto ctx = context();
    for (const char* name : {"precision-landing-01.mission.json", "rover-delivery-01.mission.json"}) {
        const auto r = loadMissionFile(kFixtures / name, ctx);
        EXPECT_TRUE(r.valid()) << r.report();
        EXPECT_TRUE(r.warnings.empty()) << r.report();
        EXPECT_EQ(r.sha256.size(), 64U);
    }
    const auto m = definition(fixture("precision-landing-01.mission.json"));
    EXPECT_EQ(m.vehicle.type, ds::physics::VehicleType::Aerial);
    EXPECT_EQ(m.rules.maxAltitudeAglM, 120.0);
    EXPECT_TRUE(m.rules.specialRules.empty());
    ASSERT_EQ(m.objectives.size(), 2U);
    EXPECT_EQ(m.objectives[1].after, std::vector<std::string>{"takeoff"});
    EXPECT_EQ(m.objectives[1].maxTouchdownSpeedMs, 0.7);
    EXPECT_EQ(m.scoring.stars.size(), 2U);
    EXPECT_EQ(m.failures.front().params["magnitudeM"], 3);
    const auto g = definition(fixture("rover-delivery-01.mission.json"));
    EXPECT_FALSE(g.rules.maxAltitudeAglM);  // No height limit for ground drones.
    EXPECT_EQ(g.terrain.seed, 734211U);
}

TEST(MissionLoader, ReportsPathsAndReadableMessages) {
    Json m = fixture("precision-landing-01.mission.json");
    m["objectives"][1]["target"] = "pad2";
    EXPECT_NE(errorsOf(m).find("objectives[1].target: unknown item \"pad2\""), std::string::npos);

    m = fixture("precision-landing-01.mission.json");
    m["items"][0]["colour"] = "red";
    EXPECT_NE(errorsOf(m).find("items[0].colour: unknown property \"colour\""), std::string::npos);
    m["items"][0].erase("colour");
    m["items"][0]["x-colour"] = "red";  // Extension fields are allowed.
    EXPECT_EQ(errorsOf(m), "");

    m = fixture("precision-landing-01.mission.json");
    m["schemaVersion"] = 2;
    EXPECT_NE(errorsOf(m).find("newer than this application supports"), std::string::npos);

    EXPECT_FALSE(validateMission(Json::parse("[1, 2]"), context()).valid());
}

TEST(MissionLoader, ObjectivesMustSuitTheVehicleType) {
    Json m = fixture("rover-delivery-01.mission.json");
    m["objectives"].push_back({{"id", "up"}, {"type", "reachAltitude"}, {"altitudeAglM", 10}});
    EXPECT_NE(errorsOf(m).find("\"reachAltitude\" is not available for ground vehicles"),
              std::string::npos);
    m = fixture("rover-delivery-01.mission.json");
    m["rules"]["maxAltitudeAglM"] = 50;
    EXPECT_NE(errorsOf(m).find("altitude limits do not apply to ground vehicles"),
              std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["rules"]["maxStuckS"] = 10;
    EXPECT_NE(errorsOf(m).find("rules.maxStuckS: only applies to ground vehicles"),
              std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["vehicle"]["profile"] = "no-such-drone";
    EXPECT_NE(errorsOf(m).find("unknown vehicle profile \"no-such-drone\""), std::string::npos);
}

TEST(MissionLoader, DependenciesAndReferences) {
    Json m = fixture("precision-landing-01.mission.json");
    m["objectives"][0]["after"] = {"land"};
    EXPECT_NE(errorsOf(m).find("dependency cycle"), std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["objectives"][1]["after"] = {"nope"};
    EXPECT_NE(errorsOf(m).find("objectives[1].after[0]: unknown objective \"nope\""),
              std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["objectives"].push_back({{"id", "g"}, {"type", "gateCourse"}, {"gates", {"pad"}}});
    EXPECT_NE(errorsOf(m).find("item \"pad\" is a landingPad, expected a gate"), std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["items"].push_back({{"type", "waypoint"}, {"id", "pad"}, {"local", {0, 5, 0}}});
    EXPECT_NE(errorsOf(m).find("duplicate item id \"pad\""), std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["items"][0]["widthM"] = 3;
    EXPECT_NE(errorsOf(m).find("items[0].widthM: does not apply to a landingPad"),
              std::string::npos);
    // Ramps and bridges (FR-ITM-13): the bridge's own properties are checked.
    m = fixture("precision-landing-01.mission.json");
    m["items"].push_back(
        {{"type", "ramp"}, {"id", "ramp"}, {"local", {5, 0, 0}}, {"sizeM", {1.0, 0.25, 1.5}}});
    m["items"].push_back({{"type", "bridge"},
                          {"id", "bridge"},
                          {"local", {-5, 0, 0}},
                          {"sizeM", {1.0, 0.5, 3.0}},
                          {"rampLengthM", 2.0},
                          {"kerbs", false}});
    EXPECT_EQ(errorsOf(m), "");
    m["items"][1]["kerbs"] = true;
    m["items"][1]["rampLengthM"] = 2.0;
    EXPECT_NE(errorsOf(m).find("rampLengthM: does not apply to a ramp"), std::string::npos);
    EXPECT_NE(errorsOf(m).find("kerbs: does not apply to a ramp"), std::string::npos);
}

TEST(MissionLoader, ObstacleModelFiles) {
    // FR-GFX-12: an obstacle may be drawn from a mesh file, kept inside the model folders.
    Json m = fixture("precision-landing-01.mission.json");
    m["items"].push_back({{"type", "obstacle"},
                          {"id", "crate"},
                          {"local", {5, 0, 0}},
                          {"sizeM", {1.0, 1.0, 1.0}},
                          {"model", "props/crate.glb"}});
    EXPECT_EQ(errorsOf(m), "");
    m["items"][1]["model"] = "props/../../secret.glb";
    EXPECT_NE(errorsOf(m).find("items[1].model: must not leave the model folders"),
              std::string::npos);
    m["items"][1]["model"] = "crate.glb";
    m["items"][0]["model"] = "pad.glb";
    EXPECT_NE(errorsOf(m).find("items[0].model: does not apply to a landingPad"),
              std::string::npos);
}

TEST(MissionLoader, PlacementAndReachability) {
    Json m = fixture("precision-landing-01.mission.json");
    m["items"].push_back({{"type", "waypoint"}, {"id", "w"}, {"local", {2000, 5, 0}}});
    EXPECT_NE(errorsOf(m).find("is outside the terrain extent"), std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["items"].push_back({{"type", "gate"}, {"id", "high"}, {"local", {0, 150, 0}}});
    EXPECT_NE(errorsOf(m).find("above the 120 m height limit"), std::string::npos);
    m["rules"]["maxAltitudeAglM"] = nullptr;  // The override lifts the limit…
    EXPECT_EQ(errorsOf(m), "");
    const auto d = definition(m);  // …and appears in the briefing's special rules.
    EXPECT_EQ(d.rules.specialRules.front(), "No height limit (the 120 m AGL rule is lifted)");
    m = fixture("precision-landing-01.mission.json");
    m["items"].push_back({{"type", "noFlyZone"}, {"id", "nfz"}, {"geo", {46.5098, 7.9795, 0}},
                          {"radiusM", 50}});
    EXPECT_NE(errorsOf(m).find("contains the spawn point"), std::string::npos);
    m = fixture("rover-delivery-01.mission.json");
    m["terrain"].erase("extent");
    m["spawn"] = {{"geo", {46.5, 7.9, 0}}};
    EXPECT_NE(errorsOf(m).find("spawn: geographic positions need terrain.origin"),
              std::string::npos);
}

TEST(MissionLoader, WeatherPlausibility) {
    Json m = fixture("precision-landing-01.mission.json");
    m["environment"]["weather"]["preset"] = "heavy-snow";
    EXPECT_NE(errorsOf(m).find("snow at 18 °C is not possible"), std::string::npos);
    m = fixture("precision-landing-01.mission.json");
    m["environment"]["weather"]["preset"] = "fog";
    m["environment"]["weather"]["humidityPct"] = 30;
    EXPECT_NE(errorsOf(m).find("fog needs a humidity of at least 80 %"), std::string::npos);
    m = fixture("rover-delivery-01.mission.json");
    m["environment"]["weather"]["timeline"].push_back({{"atS", 100}, {"preset", "clear"}});
    EXPECT_NE(errorsOf(m).find("increasing time order"), std::string::npos);
}

TEST(MissionLoader, WeatherGeneratorSettings) {
    const auto r = validateMission(fixture("rover-delivery-01.mission.json"), context());
    ASSERT_TRUE(r.valid()) << r.report();
    const ds::weather::WeatherSettings& w = r.definition->environment.weather.settings;
    EXPECT_EQ(w.seed, 55U);
    EXPECT_EQ(w.preset, "overcast");
    EXPECT_EQ(w.overrides.temperatureC, 12.0);
    EXPECT_EQ(w.overrides.windLevel, 2);
    ASSERT_EQ(w.timeline.size(), 1U);
    EXPECT_EQ(w.timeline[0].preset, "light-rain");
    const ds::weather::WeatherGenerator g(w);
    EXPECT_EQ(g.plannedAt(0.0).precipitation.type, ds::weather::PrecipitationType::None);
    EXPECT_EQ(g.plannedAt(400.0).precipitation.type, ds::weather::PrecipitationType::Rain);
}

TEST(MissionLoader, FileTerrainPathsAreChecked) {
    const fs::path dir = fs::temp_directory_path() / std::format("ds_mission_files_{}", ::getpid());
    fs::create_directories(dir);
    Json m = fixture("rover-delivery-01.mission.json");
    m["terrain"] = Json::parse(R"({"source": "file", "format": "raster",
        "elevationFiles": [{"path": "dem/missing.tif"}],
        "origin": {"geo": [40.4, -3.7, 0]}})");
    auto r = validateMission(m, context(), dir);
    EXPECT_TRUE(r.valid()) << r.report();
    ASSERT_EQ(r.unavailable.size(), 1U);  // Valid but not playable (FR-MIS-02).
    EXPECT_NE(r.unavailable[0].find("dem/missing.tif"), std::string::npos);
    m["terrain"]["elevationFiles"][0]["path"] = "../escape.tif";
    r = validateMission(m, context(), dir);
    EXPECT_FALSE(r.valid());
    m["terrain"]["elevationFiles"][0]["path"] = "dem/a.tif";
    m["terrain"]["surface"] = "dsm";
    EXPECT_NE(errorsOf(m).find("not supported in v1.0"), std::string::npos);
    fs::remove_all(dir);
}

// ----- running missions ----------------------------------------------------------------

// Drives a runner with straight-line moves on flat ground at height 0.
struct Pilot {
    MissionRunner& runner;
    VehicleSample s;
    double dt = 0.02;

    void tick(std::span<const VehicleIncident> incidents = {}) {
        s.timeS += dt;
        runner.update(s, incidents);
    }
    void moveTo(const Vec3& target, double seconds, bool onGround = false) {
        const Vec3 start = s.position;
        s.velocity = (target - start) / seconds;
        s.onGround = onGround;
        const int steps = static_cast<int>(std::round(seconds / dt));
        for (int i = 1; i <= steps && running(); ++i) {
            s.position = start + (target - start) * (static_cast<double>(i) / steps);
            s.aglM = s.position.y;
            tick();
        }
        s.velocity = Vec3(0.0);
    }
    void wait(double seconds) {
        const int steps = static_cast<int>(std::round(seconds / dt));
        for (int i = 0; i < steps && running(); ++i) {
            tick();
        }
    }
    void land(const Vec3& at, double descentMs) {
        moveTo(at + Vec3(0, 3, 0), 3.0);
        moveTo(at, 3.0 / descentMs);  // Touchdown speed = descent rate of the last step.
        s.onGround = true;
        tick();
        wait(1.5);
    }
    [[nodiscard]] bool running() const {
        return runner.state() == MissionState::InProgress || runner.state() == MissionState::PreFlight;
    }
    void arm() {
        s.armed = true;
        tick();
    }
};

std::unique_ptr<MissionRunner> makeRunner(MissionDefinition m, LineOfSightFunction los = {}) {
    auto r = MissionRunner::create(std::move(m), MissionPlacement{}, std::move(los));
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
    return std::move(*r);
}

Pilot startFlight(MissionRunner& runner) {
    Pilot p{runner, {}};
    p.s.position = runner.spawnPosition();
    p.s.onGround = true;
    p.s.batteryPct = 100.0;
    runner.beginPreFlight();
    EXPECT_TRUE(runner.preflightProblems(p.s).empty());
    p.tick();
    EXPECT_EQ(runner.state(), MissionState::PreFlight);  // Not armed yet.
    p.arm();
    EXPECT_EQ(runner.state(), MissionState::InProgress);
    return p;
}

TEST(MissionRunner, PrecisionLandingSucceedsWithThreeStars) {
    auto runner = makeRunner(definition(fixture("precision-landing-01.mission.json")));
    const Vec3 pad = runner->items().find("pad")->transform.position;
    EXPECT_NEAR(pad.y, 2.0, 1e-9);  // AGL 2 m on flat ground.
    EXPECT_GT(pad.x, 50.0);          // East of the origin…
    EXPECT_LT(pad.z, -200.0);        // …and north of it.
    Pilot p = startFlight(*runner);
    EXPECT_EQ(runner->objectives()[0].state, ObjectiveState::Active);
    EXPECT_EQ(runner->objectives()[1].state, ObjectiveState::Locked);
    p.moveTo(p.s.position + Vec3(0, 12, 0), 6.0);
    EXPECT_EQ(runner->objectives()[0].state, ObjectiveState::Completed);
    EXPECT_EQ(runner->objectives()[1].state, ObjectiveState::Active);
    p.moveTo(pad + Vec3(0.1, 10, 0), 30.0);
    p.land(pad + Vec3(0.1, 0, 0), 0.5);
    ASSERT_EQ(runner->state(), MissionState::Succeeded);
    const auto& r = *runner->result();
    EXPECT_EQ(r.stars, 3);
    EXPECT_NEAR(*r.worstErrorM, 0.1, 1e-6);
    EXPECT_GT(r.score.total, 1500.0);
    EXPECT_EQ(r.toJson()["objectives"][1]["state"], "completed");
    runner->enterDebrief();
    EXPECT_EQ(runner->state(), MissionState::Debrief);
    runner->retry();
    EXPECT_EQ(runner->state(), MissionState::Briefing);
    EXPECT_EQ(runner->objectives()[0].state, ObjectiveState::Locked);
    EXPECT_FALSE(runner->result());
}

TEST(MissionRunner, HardLandingFailsTheMission) {
    auto runner = makeRunner(definition(fixture("precision-landing-01.mission.json")));
    const Vec3 pad = runner->items().find("pad")->transform.position;
    Pilot p = startFlight(*runner);
    p.moveTo(p.s.position + Vec3(0, 12, 0), 6.0);
    p.moveTo(pad + Vec3(0, 10, 0), 30.0);
    p.land(pad, 1.5);
    ASSERT_EQ(runner->state(), MissionState::Failed);
    EXPECT_NE(runner->result()->failureReason.find("hard landing: 1.5 m/s"), std::string::npos);
    EXPECT_EQ(runner->result()->stars, 0);
}

TEST(MissionRunner, HeightRuleWarnsThenFails) {
    MissionDefinition m = definition(fixture("precision-landing-01.mission.json"));
    m.rules.timeLimitS.reset();
    auto runner = makeRunner(m);
    Pilot p = startFlight(*runner);
    p.moveTo(p.s.position + Vec3(0, 110, 0), 20.0);
    ASSERT_EQ(runner->warnings().size(), 1U);  // Warning above 100 m.
    EXPECT_EQ(runner->warnings()[0].rule, FlightRule::Height);
    EXPECT_FALSE(runner->warnings()[0].broken);
    p.moveTo(p.s.position + Vec3(0, 15, 0), 2.0);
    p.wait(2.0);
    EXPECT_EQ(runner->state(), MissionState::InProgress);  // Within the 3 s grace…
    p.wait(2.0);
    ASSERT_EQ(runner->state(), MissionState::Failed);  // …then failed.
    EXPECT_NE(runner->result()->failureReason.find("limit 120 m"), std::string::npos);

    m.rules.maxAltitudeAglM.reset();  // Override: no limit.
    auto free = makeRunner(m);
    Pilot q = startFlight(*free);
    q.moveTo(q.s.position + Vec3(0, 200, 0), 20.0);
    q.wait(10.0);
    EXPECT_EQ(free->state(), MissionState::InProgress);
    EXPECT_TRUE(free->warnings().empty());
}

TEST(MissionRunner, VisualLineOfSight) {
    MissionDefinition m = definition(fixture("precision-landing-01.mission.json"));
    m.rules.timeLimitS.reset();
    // Terrain hides everything west of x = −100 m.
    auto runner = makeRunner(m, [](const Vec3&, const Vec3& to) { return to.x > -100.0; });
    Pilot p = startFlight(*runner);
    const Vec3 home = p.s.position;
    p.moveTo(home + Vec3(-70, 20, 0), 10.0);  // Hidden for the last ~1.1 s…
    p.wait(3.0);  // …and 3 s more: ~4.3 s is allowed (maxHiddenS 5).
    EXPECT_EQ(runner->state(), MissionState::InProgress);
    ASSERT_FALSE(runner->warnings().empty());
    EXPECT_EQ(runner->warnings()[0].rule, FlightRule::VlosHidden);
    p.moveTo(home + Vec3(0, 20, 0), 2.0);
    EXPECT_EQ(runner->state(), MissionState::InProgress);
    EXPECT_TRUE(runner->warnings().empty());
    p.moveTo(home + Vec3(600, 20, 0), 30.0);  // Too far from the pilot.
    p.wait(4.0);
    ASSERT_EQ(runner->state(), MissionState::Failed);
    EXPECT_NE(runner->result()->failureReason.find("Line of sight"), std::string::npos);
}

TEST(MissionRunner, NightRequiresLights) {
    RulesSpec rules;
    FlightRulesMonitor monitor(rules, ds::physics::VehicleType::Aerial, Vec3(0), nullptr, {});
    FlightRuleSample s{0.0, Vec3(10, 20, 0), 20.0, true, false, -10.0, 1e9};
    std::optional<RuleWarning> broken;
    for (int i = 0; i < 200 && !broken; ++i) {
        s.timeS += 0.02;
        broken = monitor.update(s);
    }
    ASSERT_TRUE(broken);
    EXPECT_EQ(broken->rule, FlightRule::VlosLights);
    EXPECT_NEAR(s.timeS, 3.04, 0.05);  // After the 3 s grace.
    s.lightsOn = true;
    s.timeS += 0.02;
    EXPECT_FALSE(monitor.update(s));
    EXPECT_TRUE(monitor.warnings().empty());
    s.sunElevationDeg = 30.0;  // Daytime: lights not needed.
    s.lightsOn = false;
    EXPECT_FALSE(monitor.update(s));
    EXPECT_EQ(monitor.warningEpisodes(), 1);
}

TEST(MissionRunner, CrashTimeLimitAndFailureInjection) {
    auto runner = makeRunner(definition(fixture("precision-landing-01.mission.json")));
    Pilot p = startFlight(*runner);
    p.moveTo(p.s.position + Vec3(0, 20, 0), 5.0);
    p.wait(90.0);
    const auto events = runner->takeEvents();
    const auto injected = std::ranges::find(events, MissionEvent::Kind::FailureInjected,
                                            &MissionEvent::kind);
    ASSERT_NE(injected, events.end());
    EXPECT_EQ(injected->text, "gpsDrift");
    EXPECT_EQ(injected->params["magnitudeM"], 3);
    EXPECT_NEAR(injected->timeS, 90.0, 0.05);
    p.wait(250.0);
    ASSERT_EQ(runner->state(), MissionState::Failed);
    EXPECT_EQ(runner->result()->failureReason, "time limit exceeded");

    runner->retry();
    Pilot q = startFlight(*runner);
    const VehicleIncident crash{VehicleIncident::Kind::Crash, 6.0, "hit the ground"};
    q.tick(std::span(&crash, 1));
    ASSERT_EQ(runner->state(), MissionState::Failed);
    EXPECT_EQ(runner->result()->failureReason, "crashed: hit the ground");

    runner->retry();
    Pilot r = startFlight(*runner);
    r.s.batteryPct = 14.0;  // Below rules.minBatteryPct (15 %).
    r.tick();
    EXPECT_EQ(runner->result()->failureReason, "battery below 15 %");

    runner->retry();
    startFlight(*runner);
    runner->abort();
    EXPECT_EQ(runner->state(), MissionState::Aborted);
}

Json gateMission(bool strict) {
    Json m = fixture("precision-landing-01.mission.json");
    m["items"] = Json::parse(R"([
        {"type": "gate", "id": "g1", "local": [0, 5, -50]},
        {"type": "gate", "id": "g2", "local": [50, 5, -50]}])");
    m["spawn"] = {{"local", {0, 0, 0}}};
    m["objectives"] = Json::array();
    m["objectives"].push_back({{"id", "course"}, {"type", "gateCourse"}, {"gates", {"g1", "g2"}},
                               {"strictOrder", strict}});
    m["failures"] = Json::array();
    m["scoring"] = Json::object();
    return m;
}

TEST(MissionRunner, GateCourseInOrder) {
    auto runner = makeRunner(definition(gateMission(false)));
    Pilot p = startFlight(*runner);
    p.moveTo(Vec3(0, 5, 0), 2.0);
    p.moveTo(Vec3(0, 5, -60), 6.0);  // Through g1 northwards.
    EXPECT_EQ(runner->objectives()[0].detail, "gate 2 of 2");
    p.moveTo(Vec3(50, 5, -40), 6.0);
    p.moveTo(Vec3(50, 5, -60), 2.0);  // Through g2.
    ASSERT_EQ(runner->state(), MissionState::Succeeded);
    EXPECT_EQ(runner->result()->wrongGates, 0);
}

TEST(MissionRunner, WrongGateCostsPointsOrFailsWhenStrict) {
    auto lenient = makeRunner(definition(gateMission(false)));
    Pilot p = startFlight(*lenient);
    p.moveTo(Vec3(50, 5, 0), 5.0);
    p.moveTo(Vec3(50, 5, -60), 6.0);  // g2 first: wrong gate.
    EXPECT_EQ(lenient->state(), MissionState::InProgress);
    p.moveTo(Vec3(0, 5, -40), 6.0);
    p.moveTo(Vec3(0, 5, -60), 2.0);  // g1
    p.moveTo(Vec3(50, 5, -40), 6.0);
    p.moveTo(Vec3(50, 5, -60), 2.0);  // g2
    ASSERT_EQ(lenient->state(), MissionState::Succeeded);
    EXPECT_EQ(lenient->result()->wrongGates, 1);
    EXPECT_EQ(lenient->result()->score.penalties, -25.0);

    auto strict = makeRunner(definition(gateMission(true)));
    Pilot q = startFlight(*strict);
    q.moveTo(Vec3(0, 5, -60), 6.0);
    q.moveTo(Vec3(0, 5, -40), 2.0);  // Back through g1 against its direction.
    ASSERT_EQ(strict->state(), MissionState::Failed);
    EXPECT_NE(strict->result()->failureReason.find("wrong direction"), std::string::npos);
}

TEST(MissionRunner, GroundDeliveryAndParking) {
    auto runner = makeRunner(definition(fixture("rover-delivery-01.mission.json")));
    EXPECT_EQ(runner->pilotPosition(), Vec3(200, 0, -260));
    Pilot p = startFlight(*runner);
    const auto onGround = [&](const Vec3& to, double s) { p.moveTo(to, s, true); };
    onGround(Vec3(40, 0, -15), 10.0);
    p.wait(1.2);
    EXPECT_DOUBLE_EQ(runner->payloadKg(), 1.2);
    const auto events = runner->takeEvents();
    EXPECT_NE(std::ranges::find(events, MissionEvent::Kind::PayloadAttached, &MissionEvent::kind),
              events.end());
    const Vec3 bay(380, 0, -520);
    onGround(bay + Vec3(0.2, 0, 0), 60.0);
    p.s.headingDeg = 30.0;  // Outside the 10° tolerance of the bay (45°).
    p.wait(2.5);
    EXPECT_DOUBLE_EQ(runner->payloadKg(), 0.0);  // Unloaded…
    EXPECT_EQ(runner->state(), MissionState::InProgress);  // …but not parked yet.
    p.s.headingDeg = 50.0;
    p.wait(1.5);
    ASSERT_EQ(runner->state(), MissionState::Succeeded);
    EXPECT_EQ(runner->result()->stars, 3);  // Fast and within 0.25 m.

    runner->retry();
    Pilot q = startFlight(*runner);
    q.s.rolledOver = true;
    q.tick();
    EXPECT_EQ(runner->result()->failureReason, "the vehicle rolled over");
}

TEST(MissionRunner, SurveyCoverage) {
    Json m = fixture("precision-landing-01.mission.json");
    m["items"] = Json::parse(R"([{"type": "surveyArea", "id": "field", "local": [-50, 0, -150],
                                  "polygon": [[0, 0], [100, 0], [100, 40], [0, 40]]}])");
    m["spawn"] = {{"local", {0, 0, 0}}};
    m["objectives"] = Json::parse(R"([{"id": "scan", "type": "survey", "target": "field",
        "minAltitudeAglM": 20, "maxAltitudeAglM": 40, "swathWidthM": 20, "coveragePct": 90}])");
    m["failures"] = Json::array();
    m["scoring"] = Json::object();
    m["rules"].erase("timeLimitS");
    auto runner = makeRunner(definition(m));
    Pilot p = startFlight(*runner);
    p.moveTo(Vec3(-55, 30, -145), 10.0);
    p.moveTo(Vec3(55, 30, -145), 10.0);
    EXPECT_NEAR(*runner->surveyCoverage("scan"), 0.375, 0.1);
    p.moveTo(Vec3(55, 30, -132), 2.0);
    p.moveTo(Vec3(-55, 30, -132), 10.0);
    p.moveTo(Vec3(-55, 30, -118), 2.0);
    p.moveTo(Vec3(55, 30, -118), 10.0);
    EXPECT_EQ(runner->state(), MissionState::Succeeded);
}

TEST(MissionRunner, SequentialOrderAndOptionalBonus) {
    Json m = gateMission(false);
    m["objectiveOrder"] = "sequential";
    m["objectives"] = Json::parse(R"([
        {"id": "up", "type": "reachAltitude", "altitudeAglM": 10},
        {"id": "bonus", "type": "reachWaypoint", "position": {"local": [0, 30, 0]}, "optional": true,
         "bonus": 150},
        {"id": "home", "type": "returnHome"}])");
    auto runner = makeRunner(definition(m));
    Pilot p = startFlight(*runner);
    EXPECT_EQ(runner->objectives()[2].state, ObjectiveState::Locked);  // Waits for "up".
    p.moveTo(Vec3(0, 30, 0), 6.0);
    EXPECT_EQ(runner->objectives()[1].state, ObjectiveState::Completed);
    p.moveTo(Vec3(0, 3, 0), 6.0);
    p.land(Vec3(0.5, 0, 0), 0.5);
    ASSERT_EQ(runner->state(), MissionState::Succeeded);
    EXPECT_EQ(runner->result()->score.bonus, 150.0);
}

TEST(MissionScoring, StarsFollowTheThresholds) {
    MissionDefinition m = definition(fixture("precision-landing-01.mission.json"));
    MissionResult r;
    r.outcome = MissionState::Succeeded;
    r.durationS = 200.0;
    r.batteryPct = 60.0;
    ObjectiveStatus land;
    land.id = "land";
    land.state = ObjectiveState::Completed;
    land.errorM = 0.2;
    land.maxErrorM = 0.5;
    ObjectiveStatus up;
    up.id = "takeoff";
    up.state = ObjectiveState::Completed;
    r.objectives = {up, land};
    scoreResult(m, r);
    EXPECT_EQ(r.stars, 2);  // ≤ 240 s but not ≤ 150 s.
    EXPECT_DOUBLE_EQ(r.score.completion, 1000.0);
    EXPECT_DOUBLE_EQ(r.score.time, 300.0 * (1.0 - 200.0 / 300.0));
    EXPECT_DOUBLE_EQ(r.score.precision, 200.0 * 0.6);
    r.durationS = 100.0;
    scoreResult(m, r);
    EXPECT_EQ(r.stars, 3);
    r.objectives[1].errorM = 0.4;  // Too imprecise for the 3rd star.
    scoreResult(m, r);
    EXPECT_EQ(r.stars, 2);
    r.outcome = MissionState::Failed;
    r.objectives[1].state = ObjectiveState::Failed;
    r.collisions = 100;
    scoreResult(m, r);
    EXPECT_EQ(r.stars, 0);
    EXPECT_EQ(r.score.total, 0.0);  // Never negative.
}

// ----- catalogue -----------------------------------------------------------------------

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name)
        : path(fs::temp_directory_path() / std::format("{}_{}", name, ::getpid())) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

TEST(MissionCatalog, ScanFindsFilesFoldersAndDuplicates) {
    TempDir builtIn("ds_missions_builtin");
    TempDir user("ds_missions_user");
    fs::copy_file(kFixtures / "precision-landing-01.mission.json",
                  builtIn.path / "precision-landing-01.mission.json");
    fs::create_directories(user.path / "rover" / "dem");
    fs::copy_file(kFixtures / "rover-delivery-01.mission.json",
                  user.path / "rover" / "rover.mission.json");
    fs::copy_file(kFixtures / "precision-landing-01.mission.json",
                  user.path / "copy.mission.json");  // Same id as the built-in one.
    std::ofstream(user.path / "broken.mission.json") << "{ not json";
    std::ofstream(user.path / "notes.json") << "{}";  // Not a mission file.
    const auto entries = scanMissions(builtIn.path, user.path, context());
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_EQ(entries[0].origin, MissionOrigin::BuiltIn);
    EXPECT_TRUE(entries[0].load.valid());
    const auto byKey = [&](const std::string& file) {
        return *std::ranges::find_if(entries, [&](const CatalogEntry& e) {
            return e.load.file.filename() == file;
        });
    };
    EXPECT_FALSE(byKey("broken.mission.json").load.valid());
    EXPECT_EQ(byKey("broken.mission.json").key, "broken");
    const auto copy = byKey("copy.mission.json");
    EXPECT_FALSE(copy.load.valid());
    EXPECT_NE(copy.load.report().find("already used by"), std::string::npos);
    EXPECT_TRUE(byKey("rover.mission.json").load.valid());
    EXPECT_EQ(byKey("rover.mission.json").key, "rover-delivery-01");
}

TEST(MissionCatalog, AnyJsonMissionIsFound) {
    TempDir user("ds_missions_json");
    fs::copy_file(kFixtures / "precision-landing-01.mission.json", user.path / "landing.json");
    std::ofstream(user.path / "draft.json") << R"({"objectives": []})";   // Unfinished mission.
    std::ofstream(user.path / "typo.json") << R"({"schemaVersion": 1,)";  // Malformed.
    std::ofstream(user.path / "settings.json") << R"({"volume": 3})";     // Not a mission.
    std::ofstream(user.path / "list.json") << "[1, 2]";
    std::ofstream(user.path / "mission.schema.json") << R"({"schemaVersion": 1})";
    std::ofstream(user.path / ".landing.json.swp.json") << R"({"schemaVersion": 1})";
    std::ofstream(user.path / "readme.txt") << R"({"schemaVersion": 1})";
    fs::create_directories(user.path / "valley");
    fs::copy_file(kFixtures / "rover-delivery-01.mission.json", user.path / "valley" / "valley.json");
    std::ofstream(user.path / "valley" / "metadata.json") << R"({"author": "me"})";

    std::vector<std::string> names;
    for (const fs::path& f : findMissionFiles(user.path)) {
        names.push_back(f.lexically_relative(user.path).string());
    }
    EXPECT_EQ(names, (std::vector<std::string>{"draft.json", "landing.json", "typo.json",
                                               "valley/valley.json"}));

    const auto entries = scanMissions({}, user.path, context());
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_FALSE(entries[0].load.valid());  // draft.json: listed with its errors.
    EXPECT_EQ(entries[0].key, "draft");
    EXPECT_TRUE(entries[1].load.valid()) << entries[1].load.report();
    EXPECT_EQ(entries[1].key, "precision-landing-01");
    EXPECT_FALSE(entries[2].load.valid());
    EXPECT_EQ(entries[2].key, "typo");
    EXPECT_TRUE(entries[3].load.valid()) << entries[3].load.report();

    // Plain .json missions can be imported too, alone or as a folder.
    TempDir dest("ds_missions_json_import");
    EXPECT_TRUE(importMission(user.path / "landing.json", dest.path));
    EXPECT_TRUE(importMission(user.path / "valley", dest.path));
    EXPECT_FALSE(importMission(user.path / "settings.json", dest.path));
}

TEST(MissionCatalog, ImportCopiesFilesAndFolders) {
    TempDir src("ds_missions_import_src");
    TempDir user("ds_missions_import_user");
    fs::create_directories(src.path / "valley" / "dem");
    fs::copy_file(kFixtures / "rover-delivery-01.mission.json",
                  src.path / "valley" / "valley.mission.json");
    std::ofstream(src.path / "valley" / "dem" / "a.tif") << "x";
    const auto folder = importMission(src.path / "valley", user.path);
    ASSERT_TRUE(folder) << folder.error().message;
    EXPECT_EQ(*folder, user.path / "valley" / "valley.mission.json");
    EXPECT_TRUE(fs::exists(user.path / "valley" / "dem" / "a.tif"));
    EXPECT_FALSE(importMission(src.path / "valley", user.path));  // Exists.
    EXPECT_TRUE(importMission(src.path / "valley", user.path, true));
    const auto file = importMission(kFixtures / "precision-landing-01.mission.json", user.path);
    ASSERT_TRUE(file);
    EXPECT_TRUE(fs::exists(*file));
    std::ofstream(src.path / "x.json") << "{}";
    EXPECT_FALSE(importMission(src.path / "x.json", user.path));
    EXPECT_FALSE(importMission(src.path, user.path));  // No mission file at its top.
}

TEST(MissionCatalog, DefaultMissionIsValidAndSimple) {
    const fs::path file =
        fs::path(DS_TEST_ASSETS_DIR) / "default-missions" / "first-flight.mission.json";
    const auto r = loadMissionFile(file, context());
    ASSERT_TRUE(r.valid()) << r.report();
    EXPECT_TRUE(r.warnings.empty()) << r.report();
    EXPECT_TRUE(r.unavailable.empty());  // Procedural terrain: playable offline.
    const MissionDefinition& m = *r.definition;
    EXPECT_EQ(m.id, "first-flight");
    EXPECT_EQ(m.difficulty, 1);
    EXPECT_TRUE(m.chapter.empty());  // Outside the campaign: always unlocked.
    EXPECT_FALSE(m.rules.timeLimitS);
    EXPECT_TRUE(m.failures.empty());

    // It can be flown: climb, beacon, back to the pad.
    auto runner = makeRunner(m);
    Pilot p = startFlight(*runner);
    p.moveTo(Vec3(0, 10, 0), 5.0);
    p.moveTo(Vec3(0, 10, -60), 12.0);
    p.moveTo(Vec3(0.2, 3, 0), 12.0);
    p.land(Vec3(0.2, 0, 0), 0.5);
    ASSERT_EQ(runner->state(), MissionState::Succeeded);
    EXPECT_EQ(runner->result()->stars, 3);
}

TEST(MissionCatalog, InstallDefaultMissions) {
    TempDir defaults("ds_missions_defaults");
    TempDir home("ds_missions_home");
    fs::copy_file(fs::path(DS_TEST_ASSETS_DIR) / "default-missions" / "first-flight.mission.json",
                  defaults.path / "first-flight.mission.json");
    std::ofstream(defaults.path / "notes.txt") << "not a mission";
    const fs::path user = home.path / "missions";

    auto created = installDefaultMissions(defaults.path, user);  // Creates the folder.
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_EQ(created->size(), 1U);
    const fs::path file = user / "first-flight.mission.json";
    EXPECT_EQ(created->front(), file);
    EXPECT_FALSE(fs::exists(user / "notes.txt"));
    EXPECT_EQ(fs::status(user).permissions() & fs::perms::all, fs::perms::owner_all);
    EXPECT_EQ(fs::status(file).permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_write);

    // The user's edits are kept.
    std::ofstream(file) << "{ \"id\": \"first-flight\", \"title\": \"edited\" }";
    created = installDefaultMissions(defaults.path, user);
    ASSERT_TRUE(created);
    EXPECT_TRUE(created->empty());
    EXPECT_NE(ds::common::readJsonFile(file, 1U << 20U)->dump().find("edited"), std::string::npos);

    // Not installed again when the mission lives under another name or in a folder…
    fs::create_directories(user / "mine");
    fs::rename(file, user / "mine" / "renamed.mission.json");
    EXPECT_TRUE(installDefaultMissions(defaults.path, user)->empty());
    EXPECT_FALSE(fs::exists(file));
    // …but restored once it is gone.
    fs::remove_all(user / "mine");
    created = installDefaultMissions(defaults.path, user);
    ASSERT_EQ(created->size(), 1U);
    EXPECT_TRUE(loadMissionFile(file, context()).valid());

    // A user folder that cannot be created is an error.
    std::ofstream(home.path / "file") << "x";
    EXPECT_FALSE(installDefaultMissions(defaults.path, home.path / "file" / "missions"));
}

TEST(MissionCatalog, CampaignUnlocks) {
    const std::vector<CampaignMission> missions{
        {"b1", "basics", 1, std::nullopt},
        {"b2", "basics", 2, std::nullopt},
        {"b3", "basics", 3, std::nullopt},
        {"a1", "advanced", 1, UnlockSpec{{"b3"}, 2}},
        {"free", "", 0, std::nullopt},
    };
    auto open = unlockedMissions(missions, {});
    EXPECT_TRUE(open["b1"]);
    EXPECT_FALSE(open["b2"]);
    EXPECT_FALSE(open["a1"]);
    EXPECT_TRUE(open["free"]);
    open = unlockedMissions(missions, {{"b1", 1}, {"b2", 3}, {"b3", 1}});
    EXPECT_TRUE(open["b2"]);
    EXPECT_TRUE(open["b3"]);
    EXPECT_FALSE(open["a1"]);  // Needs 2 stars on b3.
    open = unlockedMissions(missions, {{"b3", 2}});
    EXPECT_TRUE(open["a1"]);
}

// File terrain takes its extent from the data, so the mission cannot check its positions
// against it: placing them does.
TEST(MissionWorld, PositionsOutsideTheTerrainDataAreRefused) {
    const MissionDefinition m = definition(fixture("rover-delivery-01.mission.json"));
    MissionPlacement placement;
    placement.extent = ds::terrain::Extent{2000.0, 2000.0};
    ASSERT_TRUE(buildItems(m, placement));  // The parking zone is 380 m east, 520 m north.
    placement.extent = ds::terrain::Extent{2000.0, 1000.0};
    auto items = buildItems(m, placement);
    ASSERT_FALSE(items);
    EXPECT_NE(items.error().message.find("380 m east, 520 m north of the terrain origin, "
                                         "outside the terrain data (2000 × 1000 m)"),
              std::string::npos)
        << items.error().message;
    auto runner = MissionRunner::create(m, placement);
    ASSERT_FALSE(runner);
    EXPECT_NE(runner.error().message.find("outside the terrain data"), std::string::npos);
}

TEST(MissionWorld, ItemsSerialiseBackToJson) {
    const MissionDefinition m = definition(fixture("rover-delivery-01.mission.json"));
    auto items = buildItems(m, MissionPlacement{});
    ASSERT_TRUE(items);
    const Json json = itemsToJson(*items);
    ASSERT_EQ(json.size(), 3U);
    EXPECT_EQ(json[2]["type"], "parkingZone");
    EXPECT_EQ(json[2]["local"], Json::parse("[380.0, 0.0, -520.0]"));
    EXPECT_DOUBLE_EQ(json[2]["headingDeg"].get<double>(), 45.0);
    EXPECT_EQ(json[2]["sizeM"], Json::parse("[2, 3]"));
    // The serialised items are valid mission items again.
    Json mission = fixture("rover-delivery-01.mission.json");
    mission["items"] = json;
    EXPECT_EQ(errorsOf(mission), "");
}

}  // namespace
