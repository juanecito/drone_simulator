// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Missions flown in the real simulation (M5): the world, the flight model, the flight controller
// and the mission runner together, driven by a simple autopilot on the pilot's sticks — the
// same inputs a pilot gives. Headless and deterministic.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ds/app/mission_validation.hpp"
#include "ds/common/math.hpp"
#include "ds/missions/mission_loader.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/missions/mission_world.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/sim/mission_session.hpp"
#include "ds/sim/world.hpp"

namespace ds {
namespace {

using missions::MissionState;
using missions::ObjectiveState;

constexpr double kDt = 1.0 / 500.0;

std::filesystem::path assets() { return DS_TEST_ASSETS_DIR; }

physics::AerialProfile cameraProfile() {
    auto p = physics::loadAerialProfile(assets() / "vehicles" / "aerial" / "camera-250g.json");
    EXPECT_TRUE(p.has_value());
    return *p;
}

missions::MissionDefinition loadMission(const std::filesystem::path& file) {
    auto validation = app::MissionValidation::create(assets(), {});
    EXPECT_TRUE(validation.has_value());
    auto result = missions::loadMissionFile(file, (*validation)->context);
    EXPECT_TRUE(result.valid()) << result.report();
    return *result.definition;
}

missions::MissionDefinition parseMission(const char* json) {
    auto validation = app::MissionValidation::create(assets(), {});
    EXPECT_TRUE(validation.has_value());
    auto parsed = common::parseJson(json);
    EXPECT_TRUE(parsed.has_value());
    auto result = missions::validateMission(*parsed, (*validation)->context);
    EXPECT_TRUE(result.valid()) << result.report();
    return *result.definition;
}

/// A world flying @p mission over flat ground at height 0.
struct MissionWorld {
    std::unique_ptr<sim::World> world;
    missions::MissionDefinition mission;

    explicit MissionWorld(missions::MissionDefinition m) : mission(std::move(m)) {
        missions::MissionPlacement placement;
        placement.origin = mission.terrain.origin;
        placement.ground = [](double, double) { return 0.0; };
        auto runner = missions::MissionRunner::create(mission, placement);
        EXPECT_TRUE(runner.has_value()) << (runner ? "" : runner.error().message);
        sim::WorldConfig c;
        c.profile = cameraProfile();
        c.seed = 7;
        c.ground = sim::flatGround(0.0);
        c.startPosition = (*runner)->spawnPosition();
        c.startHeadingRad = common::degToRad((*runner)->spawnHeadingDeg());
        c.startUtc = mission.environment.utc;
        c.geoOrigin = {mission.terrain.origin->latitudeDeg, mission.terrain.origin->longitudeDeg,
                       0.0};
        world = std::make_unique<sim::World>(std::move(c));
        world->setMission(std::make_unique<sim::MissionSession>(std::move(*runner)));
    }

    [[nodiscard]] sim::MissionStatus status() const {
        sim::WorldSnapshot s;
        world->fillSnapshot(s);
        return s.mission;
    }
};

/// Stick autopilot in altitude-hold mode: steers the horizontal velocity towards a point and
/// the throttle stick towards a height above the ground.
class Autopilot {
public:
    explicit Autopilot(sim::World& world) : world_(&world) {}

    /// Flies towards @p target at @p heightM above the ground (negative: land gently).
    input::ControlInputs controls(const glm::dvec3& target, double heightM, double maxSpeedMs,
                                  bool arm = true) const {
        const physics::RigidBodyState& b = world_->vehicle().bodyState();
        input::ControlInputs c;
        // Arming needs the switch to go from off to on.
        c.arm = arm && std::exchange(primed_, true);
        c.flightMode = 2;  // Altitude hold: the throttle stick sets the climb rate.
        c.lights = true;
        const double agl = b.position.y - world_->vehicle().restHeightM();
        const double climb = heightM < 0.0 ? -std::clamp(0.5 * agl, 0.3, 1.5)
                                           : std::clamp(0.8 * (heightM - agl), -2.5, 2.5);
        // Throttle centred until armed (arming is refused with the stick up).
        c.throttle = world_->vehicle().armed() ? climbStick(climb) : 0.5;
        // Horizontal: velocity towards the target, limited, then tilt (stick) from the
        // velocity error, in the drone's heading frame.
        glm::dvec2 to(target.x - b.position.x, target.z - b.position.z);
        const double distance = glm::length(to);
        glm::dvec2 want(0.0);
        if (distance > 1e-3) {
            want = to / distance * std::min(maxSpeedMs, 0.6 * distance);
        }
        const glm::dvec2 err = want - glm::dvec2(b.velocity.x, b.velocity.z);
        const double yaw = physics::eulerFromOrientation(b.orientation).yaw;
        const glm::dvec2 forward(std::sin(yaw), -std::cos(yaw));
        const glm::dvec2 right(std::cos(yaw), std::sin(yaw));
        c.pitch = std::clamp(0.12 * glm::dot(err, forward), -0.5, 0.5);
        c.roll = std::clamp(0.12 * glm::dot(err, right), -0.5, 0.5);
        return c;
    }

private:
    // Stick for a climb rate: outside the centre dead band, proportional up to maxClimbMs.
    [[nodiscard]] double climbStick(double climbMs) const {
        constexpr double kDeadband = 0.1;  // Half-width 0.05 around the centre.
        const double maxClimb = world_->aerialVehicle()->profile().controller.maxClimbMs;
        const double f = std::clamp(climbMs / maxClimb, -1.0, 1.0);
        if (std::abs(f) < 1e-3) {
            return 0.5;
        }
        return 0.5 + std::copysign(0.5 * kDeadband + std::abs(f) * (0.5 - 0.5 * kDeadband), f);
    }

    sim::World* world_;
    mutable bool primed_ = false;
};

double horizontalDistance(const glm::dvec3& a, const glm::dvec3& b) {
    return glm::length(glm::dvec2(a.x - b.x, a.z - b.z));
}

/// Runs ticks with @p controls until @p done or @p timeoutS of simulated time.
bool fly(sim::World& world, const std::function<input::ControlInputs()>& controls,
         const std::function<bool()>& done, double timeoutS) {
    const double end = world.time() + timeoutS;
    while (world.time() < end) {
        world.tick(controls(), kDt);
        if (done()) {
            return true;
        }
    }
    return false;
}

TEST(MissionFlight, FirstFlightIsFlownToThreeStars) {
    MissionWorld m(loadMission(assets() / "default-missions" / "first-flight.mission.json"));
    sim::World& w = *m.world;
    const Autopilot pilot(w);
    EXPECT_EQ(m.status().state, MissionState::PreFlight);
    EXPECT_TRUE(m.status().preflightProblems.empty());
    const glm::dvec3 pad = w.vehicle().bodyState().position;

    const auto running = [&] { return m.status().state == MissionState::InProgress; };
    const auto completed = [&](std::size_t i) {
        return m.status().objectives.at(i).state == ObjectiveState::Completed;
    };
    // Arm: the mission starts (PreFlight → InProgress).
    ASSERT_TRUE(fly(w, [&] { return pilot.controls(pad, 0.0, 0.0); }, running, 2.0));

    // Climb to 10 m, then to the beacon 60 m north at 10 m AGL.
    ASSERT_TRUE(
        fly(w, [&] { return pilot.controls(pad, 11.0, 0.0); }, [&] { return completed(0); }, 20.0));
    const auto beacon = m.status().objectives.at(1).targetPosition;
    ASSERT_TRUE(beacon.has_value());
    EXPECT_NEAR(beacon->z, -60.0, 0.5);
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(*beacon, 10.0, 6.0); }, [&] { return completed(1); }, 40.0));

    // Back over the pad, then down onto it.
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pad, 10.0, 6.0); },
        [&] {
            const auto& b = w.vehicle().bodyState();
            return horizontalDistance(b.position, pad) < 0.3 && glm::length(b.velocity) < 0.3;
        },
        40.0));
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pad, -2.0, 1.0); },
        [&] { return m.status().result.has_value(); }, 40.0));

    const sim::MissionStatus s = m.status();
    ASSERT_TRUE(s.result.has_value());
    EXPECT_EQ(s.result->outcome, MissionState::Succeeded) << s.result->failureReason;
    EXPECT_EQ(s.result->stars, 3);  // Under a minute.
    EXPECT_LT(s.result->durationS, 60.0);
    ASSERT_TRUE(s.result->objectives.at(2).errorM.has_value());
    EXPECT_LT(*s.result->objectives.at(2).errorM, 1.0);
    // The path was recorded, from the pad to the beacon and back.
    ASSERT_GE(s.path.size(), 20U);
    const double farthest = std::ranges::max(s.path, {}, [&](const glm::dvec3& p) {
                                return horizontalDistance(p, pad);
                            }).z;
    EXPECT_LT(farthest, -50.0);
}

TEST(MissionFlight, GateRunIsFlownThroughEveryGateInTheArrowDirection) {
    // The built-in racing mission: checks the pack's gate placement and arrows too.
    MissionWorld m(loadMission(assets() / "missions" / "racing-01-gate-run.mission.json"));
    sim::World& w = *m.world;
    const Autopilot pilot(w);
    const glm::dvec3 pad = w.vehicle().bodyState().position;
    const auto& course = m.mission.objectives.at(0);
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pad, 3.0, 0.0); },
        [&] { return w.vehicle().bodyState().position.y > 2.5; }, 10.0));
    for (const std::string& id : course.gates) {
        const sim::MissionStatus before = m.status();
        ASSERT_EQ(before.objectives.at(0).targetKey, id);
        const glm::dvec3 centre = *before.objectives.at(0).targetPosition;
        const auto item = std::ranges::find(m.mission.items, id, &missions::ItemSpec::id);
        ASSERT_NE(item, m.mission.items.end());
        const double heading = common::degToRad(item->headingDeg);
        const glm::dvec3 forward(std::sin(heading), 0.0, -std::cos(heading));
        // Line up 8 m before the gate, then through it to 8 m past it.
        ASSERT_TRUE(fly(
            w, [&] { return pilot.controls(centre - 8.0 * forward, centre.y, 5.0); },
            [&] {
                return horizontalDistance(w.vehicle().bodyState().position,
                                          centre - 8.0 * forward) < 1.0;
            },
            40.0))
            << id;
        ASSERT_TRUE(fly(
            w, [&] { return pilot.controls(centre + 8.0 * forward, centre.y, 4.0); },
            [&] {
                return m.status().objectives.at(0).targetKey != id ||
                       m.status().objectives.at(0).state == ObjectiveState::Completed;
            },
            20.0))
            << id << ": " << m.status().objectives.at(0).detail;
    }
    EXPECT_EQ(m.status().objectives.at(0).state, ObjectiveState::Completed);
}

TEST(MissionFlight, HeightLimitFailsTheMissionAfterTheGraceTime) {
    MissionWorld m(parseMission(R"({
        "schemaVersion": 1, "id": "too-high", "title": "Too high",
        "terrain": { "source": "procedural", "seed": 1, "preset": "flatlands",
                     "origin": { "geo": [40.4, -3.7, 0] } },
        "environment": { "utc": "2026-06-21T10:00:00Z",
                         "weather": { "seed": 1, "preset": "clear" } },
        "vehicle": { "type": "aerial", "profile": "camera-250g" },
        "spawn": { "local": [0, 0, 0] },
        "rules": { "allowKeyboard": true, "maxAltitudeAglM": 30, "graceS": 1 },
        "objectives": [ { "id": "up", "type": "reachAltitude", "altitudeAglM": 25 },
                        { "id": "far", "type": "reachWaypoint", "radiusM": 5,
                          "position": { "local": [0, 25, -900] } } ]
    })"));
    sim::World& w = *m.world;
    const Autopilot pilot(w);
    const glm::dvec3 home = w.vehicle().bodyState().position;
    bool warned = false;
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(home, 60.0, 0.0); },
        [&] {
            sim::WorldSnapshot snap;
            w.fillSnapshot(snap);
            warned = warned || !snap.ruleWarnings.empty();
            return snap.mission.result.has_value();
        },
        60.0));
    EXPECT_TRUE(warned);
    const sim::MissionStatus s = m.status();
    EXPECT_EQ(s.result->outcome, MissionState::Failed);
    EXPECT_NE(s.result->failureReason.find("Height"), std::string::npos) << s.result->failureReason;
}

TEST(MissionFlight, DeliveryAddsTheCargoMassToTheVehicle) {
    MissionWorld m(parseMission(R"({
        "schemaVersion": 1, "id": "parcel", "title": "Parcel",
        "terrain": { "source": "procedural", "seed": 1, "preset": "flatlands",
                     "origin": { "geo": [40.4, -3.7, 0] } },
        "environment": { "utc": "2026-06-21T10:00:00Z",
                         "weather": { "seed": 1, "preset": "clear" } },
        "vehicle": { "type": "aerial", "profile": "camera-250g" },
        "spawn": { "local": [0, 0, 0] },
        "rules": { "allowKeyboard": true },
        "items": [ { "type": "pickupZone", "id": "pick", "local": [0, 0, -15], "radiusM": 3 },
                   { "type": "dropZone", "id": "drop", "local": [15, 0, -15], "radiusM": 3 } ],
        "objectives": [ { "id": "deliver", "type": "deliver", "pickup": "pick", "drop": "drop",
                          "cargoKg": 0.1 } ]
    })"));
    sim::World& w = *m.world;
    const Autopilot pilot(w);
    const double emptyMass = w.vehicle().mass().massKg;
    const auto target = [&](const char* key) {
        for (const auto& o : m.status().objectives) {
            if (o.targetKey == key) {
                return *o.targetPosition;
            }
        }
        return glm::dvec3(0.0);
    };
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(w.vehicle().bodyState().position, 3.0, 0.0); },
        [&] { return m.status().state == MissionState::InProgress; }, 5.0));
    // Low and slow in the pickup zone: the cargo is attached and weighs on the drone.
    const glm::dvec3 pick = target("pick");
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(w.vehicle().bodyState().position, 3.0, 0.0); },
        [&] { return w.vehicle().bodyState().position.y > 2.5; }, 10.0));
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pick, 0.6, 3.0); },
        [&] { return m.status().payloadKg > 0.0; }, 40.0));
    EXPECT_NEAR(w.vehicle().mass().massKg, emptyMass + 0.1, 1e-9);
    // Then to the drop zone, where it is released.
    const glm::dvec3 drop = target("drop");
    EXPECT_NEAR(drop.x, 15.0, 0.5);
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(drop, 0.6, 3.0); },
        [&] { return m.status().result.has_value(); }, 60.0));
    EXPECT_EQ(m.status().result->outcome, MissionState::Succeeded)
        << m.status().result->failureReason;
    EXPECT_NEAR(w.vehicle().mass().massKg, emptyMass, 1e-9);
}

TEST(MissionFlight, FlyingIntoTheMastIsACrash) {
    MissionWorld m(loadMission(assets() / "missions" / "ops-02-mast-inspection.mission.json"));
    sim::World& w = *m.world;
    const Autopilot pilot(w);
    const glm::dvec3 pad = w.vehicle().bodyState().position;
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pad, 10.0, 0.0); },
        [&] { return w.vehicle().bodyState().position.y > 9.0; }, 20.0));
    // Stop 12 m south of the mast (100 m north), then straight at it at 10 m, fast.
    const glm::dvec3 mast(pad.x, 10.0, pad.z - 100.0);
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(mast + glm::dvec3(0, 0, 12), 10.0, 8.0); },
        [&] {
            const auto& b = w.vehicle().bodyState();
            return horizontalDistance(b.position, mast + glm::dvec3(0, 0, 12)) < 0.2 &&
                   glm::length(b.velocity) < 0.3;
        },
        60.0));
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(mast + glm::dvec3(0, 0, -30), 10.0, 6.0); },
        [&] { return m.status().result.has_value(); }, 20.0));
    EXPECT_EQ(m.status().result->outcome, MissionState::Failed);
    EXPECT_NE(m.status().result->failureReason.find("Mast"), std::string::npos)
        << m.status().result->failureReason;
    // It did not fly through: stopped at the mast's south side.
    EXPECT_GT(w.vehicle().bodyState().position.z, mast.z - 1.0);
}

TEST(MissionFlight, FreeFlightItemsAreSolidToo) {
    sim::WorldConfig c;
    c.profile = cameraProfile();
    c.seed = 5;
    sim::World w(std::move(c));
    items::Item tree;
    tree.type = items::ItemType::Obstacle;
    tree.key = "oak";
    tree.name = "Oak";
    tree.properties = common::Json{{"shape", "tree"}, {"radiusM", 2.0}, {"heightM", 12.0}};
    tree.shape = items::defaultShape(tree.type, tree.properties);
    tree.solid = true;
    std::string crash;
    w.events().subscribe<sim::CrashEvent>([&](const sim::CrashEvent& e) { crash = e.reason; });
    const Autopilot pilot(w);
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls({0.0, 0.0, 0.0}, 7.5, 0.0); },
        [&] { return w.vehicle().bodyState().position.y > 7.0; }, 20.0));
    // On the drone's track, 30 m north; it flies through the crown (centre 7.4 m up, 2 m
    // radius). Under the crown the air is free.
    const double x = w.vehicle().bodyState().position.x;
    tree.transform.position = {x, 0.0, -30.0};
    w.setFreeItems({tree});
    fly(
        w, [&] { return pilot.controls({x, 7.5, -60.0}, 7.5, 7.0); },
        [&] { return !crash.empty(); }, 20.0);
    EXPECT_NE(crash.find("Oak"), std::string::npos) << crash;
    EXPECT_GT(w.vehicle().bodyState().position.z, -30.0);
}

TEST(MissionFlight, CrashFailsTheMissionAndAbortEndsIt) {
    MissionWorld crash(loadMission(assets() / "default-missions" / "first-flight.mission.json"));
    sim::World& w = *crash.world;
    const Autopilot pilot(w);
    const glm::dvec3 pad = w.vehicle().bodyState().position;
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pad, 8.0, 0.0); },
        [&] { return w.vehicle().bodyState().position.y > 7.0; }, 20.0));
    // Disarmed in the air: the drone falls and breaks.
    ASSERT_TRUE(fly(
        w, [&] { return pilot.controls(pad, 8.0, 0.0, false); },
        [&] { return crash.status().result.has_value(); }, 10.0));
    EXPECT_EQ(crash.status().result->outcome, MissionState::Failed);

    MissionWorld aborted(loadMission(assets() / "default-missions" / "first-flight.mission.json"));
    sim::World& a = *aborted.world;
    const Autopilot pilot2(a);
    ASSERT_TRUE(fly(
        a, [&] { return pilot2.controls(pad, 3.0, 0.0); },
        [&] { return aborted.status().state == MissionState::InProgress; }, 3.0));
    a.mission()->abort(a);
    EXPECT_EQ(aborted.status().result->outcome, MissionState::Aborted);

    // Retry: a clean attempt from PreFlight with a new attempt number.
    a.resetAt(pad, 0.0);
    a.mission()->retry(a);
    const sim::MissionStatus retried = aborted.status();
    EXPECT_EQ(retried.state, MissionState::PreFlight);
    EXPECT_FALSE(retried.result.has_value());
    EXPECT_EQ(retried.attempt, 1U);
}

TEST(MissionFlight, FreeModeRulesWarnAndEndTheFlightOnlyWhenEnforced) {
    for (const bool enforce : {false, true}) {
        sim::WorldConfig c;
        c.profile = cameraProfile();
        c.seed = 3;
        c.startUtc = *astro::parseUtc("2026-06-21T10:00:00Z");
        c.geoOrigin = {40.4, -3.7, 0.0};
        sim::World w(std::move(c));
        w.setFreeFlightRules(
            sim::FreeFlightRules{.pilotPosition = {0.0, 0.0, 4.0}, .enforce = enforce});
        int ended = 0;
        w.events().subscribe<sim::FlightRuleBrokenEvent>(
            [&](const sim::FlightRuleBrokenEvent&) { ++ended; });
        const Autopilot pilot(w);
        bool warned = false;
        fly(
            w, [&] { return pilot.controls({0.0, 0.0, 0.0}, 135.0, 0.0); },
            [&] {
                sim::WorldSnapshot snap;
                w.fillSnapshot(snap);
                warned = warned || std::ranges::any_of(snap.ruleWarnings, [](const auto& r) {
                             return r.rule == missions::FlightRule::Height;
                         });
                return false;
            },
            70.0);
        EXPECT_TRUE(warned) << "enforce " << enforce;
        EXPECT_EQ(ended > 0, enforce);
    }
}

}  // namespace
}  // namespace ds
