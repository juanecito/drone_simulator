// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Playing missions (M5): mission HUD, item visuals and the mission → scene setup.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "ds/app/mission_setup.hpp"
#include "ds/app/mission_validation.hpp"
#include "ds/items/item.hpp"
#include "ds/items/registry.hpp"
#include "ds/missions/mission_loader.hpp"
#include "ds/sim/hud.hpp"
#include "ds/sim/item_visuals.hpp"
#include "ds/sim/mission_session.hpp"

namespace ds {
namespace {

using missions::MissionState;
using missions::ObjectiveState;

missions::MissionDefinition parse(const char* json) {
    auto validation = app::MissionValidation::create(DS_TEST_ASSETS_DIR, {});
    EXPECT_TRUE(validation.has_value());
    auto result = missions::validateMission(common::Json::parse(json), (*validation)->context);
    EXPECT_TRUE(result.valid()) << result.report();
    return *result.definition;
}

constexpr const char* kMission = R"({
    "schemaVersion": 1, "id": "m", "title": "Test mission",
    "terrain": { "source": "procedural", "seed": 42, "preset": "alpine",
                 "origin": { "geo": [46.0, 7.0, 0], "altRef": "AGL" },
                 "extent": { "widthM": 3000, "lengthM": 2000 },
                 "params": { "amplitudeM": 400 } },
    "environment": { "utc": "2026-06-21T10:00:00Z", "timeZone": "Europe/Zurich",
                     "weather": { "seed": 1, "preset": "fog", "temperatureC": 12,
                                  "wind": { "level": 3, "fromDeg": 90 } } },
    "vehicle": { "type": "aerial", "profile": "survey-1500g" },
    "spawn": { "local": [0, 0, 0] },
    "rules": { "allowKeyboard": true },
    "items": [ { "type": "waypoint", "id": "w", "local": [0, 10, -50] } ],
    "objectives": [ { "id": "w", "type": "reachWaypoint", "target": "w" } ]
})";

// --- HUD ---------------------------------------------------------------------------------

TEST(MissionHud, ClockShowsMinutesSecondsAndTenths) {
    EXPECT_EQ(sim::missionClock(0.0), "0:00.0");
    EXPECT_EQ(sim::missionClock(-3.0), "0:00.0");
    EXPECT_EQ(sim::missionClock(83.46), "1:23.4");
    EXPECT_EQ(sim::missionClock(600.0), "10:00.0");
}

sim::MissionStatus status(MissionState state) {
    sim::MissionStatus m;
    m.active = true;
    m.title = "Test";
    m.state = state;
    m.elapsedS = 95.0;
    m.timeLimitS = 120.0;
    missions::ObjectiveStatus done;
    done.title = "Climb";
    done.state = ObjectiveState::Completed;
    missions::ObjectiveStatus active;
    active.title = "Fly to the beacon";
    active.state = ObjectiveState::Active;
    active.targetPosition = glm::dvec3(0.0, 10.0, -100.0);
    active.targetName = "Beacon";
    missions::ObjectiveStatus locked;
    locked.title = "Land";
    locked.state = ObjectiveState::Locked;
    m.objectives = {done, active, locked};
    return m;
}

TEST(MissionHud, PanelShowsStateTimerAndObjectives) {
    sim::HudModel hud;
    sim::buildMissionHud(status(MissionState::InProgress), {0.0, 10.0, 0.0}, {}, hud);
    ASSERT_TRUE(hud.mission.has_value());
    EXPECT_EQ(hud.mission->title, "Test");
    EXPECT_EQ(hud.mission->state.text, "IN PROGRESS");
    EXPECT_EQ(hud.mission->timer.text, "T 1:35.0 / 2:00.0");
    EXPECT_EQ(hud.mission->timer.level, sim::HudLevel::Caution);  // 25 s left.
    ASSERT_EQ(hud.mission->objectives.size(), 3U);
    EXPECT_EQ(hud.mission->objectives[0].level, sim::HudLevel::Good);
    EXPECT_NE(hud.mission->objectives[1].text.find("100 m"), std::string::npos)
        << hud.mission->objectives[1].text;
    EXPECT_EQ(hud.mission->objectives[2].level, sim::HudLevel::Muted);
    // One guidance marker, on the active objective's target.
    ASSERT_EQ(hud.markers.size(), 1U);
    EXPECT_EQ(hud.markers[0].label, "Beacon 100 m");

    // No markers when the mission hides them, nor before the start.
    sim::HudOptions hidden;
    hidden.showMarkers = false;
    sim::buildMissionHud(status(MissionState::InProgress), {}, hidden, hud);
    EXPECT_TRUE(hud.markers.empty());
    sim::buildMissionHud(status(MissionState::PreFlight), {}, {}, hud);
    EXPECT_TRUE(hud.markers.empty());
    EXPECT_EQ(hud.mission->state.text, "PRE-FLIGHT — ARM TO START");

    // Inactive: nothing.
    sim::buildMissionHud(sim::MissionStatus{}, {}, {}, hud);
    EXPECT_FALSE(hud.mission.has_value());
}

TEST(MissionHud, EndStatesAreColoured) {
    sim::HudModel hud;
    auto failed = status(MissionState::Failed);
    failed.result = missions::MissionResult{};
    failed.result->outcome = MissionState::Failed;
    failed.result->failureReason = "crashed";
    sim::buildMissionHud(failed, {}, {}, hud);
    EXPECT_EQ(hud.mission->state.text, "MISSION FAILED: crashed");
    EXPECT_EQ(hud.mission->state.level, sim::HudLevel::Warning);
    sim::buildMissionHud(status(MissionState::Succeeded), {}, {}, hud);
    EXPECT_EQ(hud.mission->state.level, sim::HudLevel::Good);
}

// --- Item visuals --------------------------------------------------------------------------

items::Item item(items::ItemType type, const char* properties = "{}") {
    items::Item it;
    it.type = type;
    it.key = std::string(items::itemTypeName(type));
    it.properties = common::Json::parse(properties);
    it.shape = items::defaultShape(type, it.properties);
    return it;
}

TEST(ItemVisuals, PhysicalItemsAlwaysDrawnMarkersOnlyWhenShown) {
    using items::ItemType;
    for (const ItemType t :
         {ItemType::Gate, ItemType::LandingPad, ItemType::Obstacle, ItemType::NoFlyZone,
          ItemType::Geofence, ItemType::InspectionTarget, ItemType::Payload}) {
        const auto mesh = sim::ItemVisuals::meshFor(item(t), false);
        ASSERT_NE(mesh, nullptr) << items::itemTypeName(t);
        EXPECT_FALSE(mesh->indices.empty());
        EXPECT_EQ(mesh->indices.size() % 3, 0U);
    }
    for (const ItemType t :
         {ItemType::Waypoint, ItemType::PickupZone, ItemType::DropZone, ItemType::SurveyArea}) {
        EXPECT_EQ(sim::ItemVisuals::meshFor(item(t), false), nullptr) << items::itemTypeName(t);
        EXPECT_NE(sim::ItemVisuals::meshFor(item(t), true), nullptr) << items::itemTypeName(t);
    }
    // Zones are see-through; obstacles are not.
    EXPECT_TRUE(sim::ItemVisuals::meshFor(item(ItemType::NoFlyZone), true)->transparent);
    EXPECT_FALSE(sim::ItemVisuals::meshFor(item(ItemType::Obstacle, R"({"shape": "tree"})"), true)
                     ->transparent);
}

TEST(ItemVisuals, RampsAndBridgesAreDrawnAsTheyCollide) {
    // FR-ITM-13: every face of the wedge faces out of the solid, so lighting and culling see
    // it from outside; the top of a ramp faces up and towards its low end.
    using items::ItemType;
    const auto ramp = sim::ItemVisuals::meshFor(item(ItemType::Ramp), false);
    ASSERT_NE(ramp, nullptr);
    EXPECT_EQ(ramp->indices.size(), 3U * (2 + 2 + 2 + 1 + 1));  // 3 quads, 2 triangles.
    const glm::vec3 inside(0.0F, 0.05F, 0.2F);                  // Low part of the wedge.
    for (std::size_t i = 0; i < ramp->indices.size(); i += 3) {
        const auto& v = ramp->vertices[ramp->indices[i]];
        EXPECT_GE(glm::dot(v.normal, v.position - inside), 0.0F) << "triangle " << i / 3;
    }
    bool slope = false;
    for (const auto& v : ramp->vertices) {
        slope = slope || (v.normal.y > 0.9F && v.normal.y < 0.999F && v.normal.z > 0.1F);
    }
    EXPECT_TRUE(slope);
    // Bridge: deck, two ramps and two kerbs.
    const auto bridge = sim::ItemVisuals::meshFor(item(ItemType::Bridge), false);
    ASSERT_NE(bridge, nullptr);
    EXPECT_EQ(bridge->indices.size(), 3U * (12 + 2 * 8 + 2 * 12));
}

TEST(ItemVisuals, HighlightedTargetsGlow) {
    items::ItemRegistry registry;
    ASSERT_TRUE(registry.add(item(items::ItemType::Waypoint)).has_value());
    ASSERT_TRUE(registry.add(item(items::ItemType::Gate)).has_value());
    const sim::ItemVisuals visuals(registry, true);
    EXPECT_EQ(visuals.size(), 2U);
    std::vector<graphics::DrawItem> plain;
    visuals.appendDrawItems(0.0, {}, plain);
    std::vector<graphics::DrawItem> lit;
    const std::vector<std::string> highlighted{"gate"};
    visuals.appendDrawItems(0.0, highlighted, lit);
    ASSERT_EQ(plain.size(), lit.size());
    bool brighter = false;
    for (std::size_t i = 0; i < lit.size(); ++i) {
        brighter = brighter || lit[i].emissiveScale > plain[i].emissiveScale ||
                   lit[i].tint != plain[i].tint;
    }
    EXPECT_TRUE(brighter);
}

// --- Mission setup -------------------------------------------------------------------------

TEST(MissionSetup, TerrainFromThePresetWithTheMissionsOverrides) {
    const auto m = parse(kMission);
    auto t = app::missionTerrain(m);
    ASSERT_TRUE(t.has_value()) << t.error().message;
    EXPECT_EQ(t->params.seed, 42U);
    EXPECT_DOUBLE_EQ(t->params.amplitudeM, 400.0);     // Mission override.
    EXPECT_DOUBLE_EQ(t->params.featureSizeM, 4000.0);  // From the alpine preset.
    EXPECT_DOUBLE_EQ(t->origin.latitudeDeg, 46.0);
    EXPECT_DOUBLE_EQ(t->origin.longitudeDeg, 7.0);
    EXPECT_DOUBLE_EQ(t->origin.heightM, 1200.0);  // AGL origin: the preset's altitude.
    ASSERT_TRUE(t->extent.has_value());
    EXPECT_DOUBLE_EQ(t->extent->widthM, 3000.0);
    EXPECT_TRUE(t->presetKey.empty());
}

TEST(MissionSetup, EnvironmentFromTheMissionsWeather) {
    const auto m = parse(kMission);
    auto scene = app::missionScene(m, app::EnvironmentConfig{});
    ASSERT_TRUE(scene.has_value());
    EXPECT_EQ(scene->environment.timeZone, "Europe/Zurich");
    EXPECT_FALSE(scene->environment.animate);
    EXPECT_DOUBLE_EQ(scene->environment.visibilityM, 500.0);  // Fog.
    EXPECT_EQ(scene->environment.wind.level, 3);
    EXPECT_DOUBLE_EQ(scene->environment.wind.fromDeg, 90.0);
    EXPECT_DOUBLE_EQ(scene->environment.air.temperatureC, 12.0);
    EXPECT_FALSE(app::unplayableReason(m).has_value());
}

TEST(MissionSetup, ProfileIsTheMissionsOwn) {
    const auto m = parse(kMission);
    std::string asked;
    auto profile = app::missionProfile(m, [&](const std::string& key) {
        asked = key;
        return physics::loadAerialProfile(std::filesystem::path(DS_TEST_ASSETS_DIR) / "vehicles" /
                                          "aerial" / (key + ".json"));
    });
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(asked, "survey-1500g");
}

TEST(MissionSetup, OnlineTerrainAndGroundVehiclesAreNotPlayableYet) {
    auto online = parse(kMission);
    online.terrain.source = missions::TerrainSource::Online;
    EXPECT_NE(app::unplayableReason(online).value_or("").find("M4"), std::string::npos);
    EXPECT_FALSE(app::missionTerrain(online).has_value());
    auto ground = parse(kMission);
    ground.vehicle.type = physics::VehicleType::Ground;
    EXPECT_NE(app::unplayableReason(ground).value_or("").find("M6"), std::string::npos);
}

}  // namespace
}  // namespace ds

namespace ds {
namespace {

// --- Obstacle contacts ---------------------------------------------------------------------

TEST(ObstacleContact, BoxCylinderAndGateFrame) {
    // A 4 × 6 × 4 m building at x = 10: a point 0.5 m inside its west wall is pushed west.
    items::Item building = item(items::ItemType::Obstacle, R"({"sizeM": [4, 6, 4]})");
    building.solid = true;
    building.name = "Shed";
    building.transform.position = {10.0, 0.0, 0.0};
    auto c = items::obstacleContact(building, {8.5, 2.0, 0.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.x, -1.0, 1e-9);
    EXPECT_NEAR(c->depthM, 0.5, 1e-9);
    // On the roof: pushed up.
    c = items::obstacleContact(building, {10.0, 5.9, 0.5});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.y, 1.0, 1e-9);
    EXPECT_FALSE(items::obstacleContact(building, {7.9, 2.0, 0.0}).has_value());

    // An 8 m tree with a 2 m crown: trunk and crown collide, the air under the crown does not.
    items::Item tree = item(items::ItemType::Obstacle, R"({"shape": "tree", "radiusM": 2})");
    tree.solid = true;
    EXPECT_FALSE(items::obstacleContact(tree, {0.0, 3.0, 1.5}).has_value());
    c = items::obstacleContact(tree, {0.0, 2.0, 0.2});  // Trunk, 0.24 m radius.
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.z, 1.0, 1e-9);
    EXPECT_NEAR(c->depthM, 0.04, 1e-9);
    EXPECT_EQ(c->material, items::Material::Wood);
    c = items::obstacleContact(tree, {0.0, 4.96, 1.5});  // Crown sphere at 0.62 · 8 m.
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.z, 1.0, 1e-9);
    EXPECT_NEAR(c->depthM, 0.5, 1e-9);
    EXPECT_EQ(c->material, items::Material::Foliage);
    EXPECT_DOUBLE_EQ(c->surface.restitution, 0.05);

    // Not solid: no contact.
    tree.solid = false;
    EXPECT_FALSE(items::obstacleContact(tree, {0.0, 4.96, 1.5}).has_value());

    // A 3 × 2 m gate at 3 m, facing east: the opening is free, the frame is not.
    items::Item gate = item(items::ItemType::Gate, R"({"widthM": 3, "heightM": 2})");
    gate.transform.position = {0.0, 3.0, 0.0};
    gate.transform.rotation = items::headingRotation(90.0);
    gate.altitudeM = 3.0;
    EXPECT_FALSE(items::obstacleContact(gate, {0.0, 3.0, 0.0}).has_value());
    EXPECT_FALSE(items::obstacleContact(gate, {0.0, 3.9, 1.4}).has_value());
    const double t = items::gateFrameThicknessM(1.5, 1.0);
    EXPECT_TRUE(items::obstacleContact(gate, {0.0, 4.0 + 0.5 * t, 0.0}).has_value());   // Top bar.
    EXPECT_TRUE(items::obstacleContact(gate, {0.0, 0.5, 1.5 + 0.5 * t}).has_value());   // Post.
    EXPECT_FALSE(items::obstacleContact(gate, {1.0, 4.0 + 0.5 * t, 0.0}).has_value());  // Behind.
}

items::Item solid(const char* properties) {
    items::Item it = item(items::ItemType::Obstacle, properties);
    it.solid = true;
    return it;
}

TEST(ObstacleContact, HouseWallsRoofAndChimney) {
    // 8 × 7 × 10 m: walls to 4.34 m, 0.3 m eaves, roof ridge along z at 7 m.
    const items::Item house = solid(R"({"shape": "house"})");
    ASSERT_EQ(house.shape.parts.size(), 3U);
    items::Vec3 lo;
    items::Vec3 hi;
    house.shape.localBounds(lo, hi);
    EXPECT_NEAR(hi.x - lo.x, 8.0, 1e-9);
    EXPECT_NEAR(hi.y, 7.0, 1e-9);
    EXPECT_NEAR(hi.z - lo.z, 10.0, 1e-9);
    EXPECT_DOUBLE_EQ(items::massKg(house), 250.0 * 8.0 * 7.0 * 10.0);

    EXPECT_FALSE(items::obstacleContact(house, {3.85, 4.0, 0.0}).has_value());  // Under the eaves.
    auto c = items::obstacleContact(house, {3.5, 2.0, 0.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.x, 1.0, 1e-9);
    EXPECT_NEAR(c->depthM, 0.2, 1e-9);
    EXPECT_EQ(c->material, items::Material::Masonry);
    // West roof slope: pushed out perpendicular to it.
    c = items::obstacleContact(house, {-1.0, 5.5, 0.0});
    ASSERT_TRUE(c.has_value());
    const double len = std::hypot(2.66, 4.0);
    EXPECT_NEAR(c->normal.x, -2.66 / len, 1e-9);
    EXPECT_NEAR(c->normal.y, 4.0 / len, 1e-9);
    EXPECT_NEAR(c->depthM, (4.0 * 2.66 - 2.66 * 1.0 - 4.0 * 1.16) / len, 1e-9);
    EXPECT_EQ(c->material, items::Material::RoofTiles);
    c = items::obstacleContact(house, {2.0, 6.5, 2.0});  // Chimney, above the roof.
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.y, 1.0, 1e-9);
    EXPECT_EQ(c->material, items::Material::Masonry);
}

TEST(ObstacleContact, CowAndManArePartsWithTypicalMasses) {
    const items::Item cow = solid(R"({"shape": "cow"})");
    EXPECT_NEAR(items::massKg(cow), 650.0, 1e-6);
    EXPECT_NEAR(items::massKg(solid(R"({"shape": "cow", "sizeM": [1.6, 3, 4.8]})")), 8.0 * 650.0,
                1e-6);
    EXPECT_FALSE(items::obstacleContact(cow, {0.0, 0.4, 0.0}).has_value());  // Between the legs.
    auto c = items::obstacleContact(cow, {0.24, 0.3, -0.478});               // Front leg.
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.z, 1.0, 1e-9);
    EXPECT_EQ(c->material, items::Material::Hide);
    c = items::obstacleContact(cow, {0.0, 1.3, 0.2});  // Back.
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.y, 1.0, 1e-9);
    EXPECT_NEAR(c->depthM, 0.05, 1e-9);
    EXPECT_TRUE(items::obstacleContact(cow, {0.0, 1.23, -1.1}).has_value());  // Head, at −z.

    const items::Item man = solid(R"({"shape": "man"})");
    EXPECT_NEAR(items::massKg(man), 75.0, 1e-6);
    EXPECT_NEAR(items::massKg(solid(R"({"shape": "man", "heightM": 3.5})")), 600.0, 1e-6);
    EXPECT_DOUBLE_EQ(items::massKg(solid(R"({"shape": "man", "massKg": 90})")), 90.0);
    EXPECT_FALSE(items::obstacleContact(man, {0.0, 0.3, 0.0}).has_value());  // Between the legs.
    c = items::obstacleContact(man, {0.0, 1.74, 0.0});                       // Top of the head.
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.y, 1.0, 1e-9);
    EXPECT_NEAR(c->depthM, 0.01, 1e-9);
    EXPECT_EQ(c->material, items::Material::Skin);
    EXPECT_DOUBLE_EQ(c->surface.restitution, 0.1);

    // Item properties override the materials.
    const items::Item stiff = solid(R"({"shape": "man", "restitution": 0.5, "friction": 0})");
    c = items::obstacleContact(stiff, {0.0, 1.74, 0.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_DOUBLE_EQ(c->surface.restitution, 0.5);
    EXPECT_DOUBLE_EQ(c->surface.friction, 0.0);
    // Plain shapes keep their old geometry, with a material.
    c = items::obstacleContact(solid(R"({"shape": "building", "sizeM": [4, 6, 4]})"),
                               {1.5, 2.0, 0.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->material, items::Material::Masonry);
    EXPECT_FALSE(std::isfinite(items::massKg(item(items::ItemType::Gate))));
}

TEST(ItemVisuals, FiguresAreDrawnWithinTheirBounds) {
    for (const char* shape : {"house", "tree", "cow", "man"}) {
        const items::Item it = solid(common::Json{{"shape", shape}}.dump().c_str());
        const auto mesh = sim::ItemVisuals::meshFor(it, false);
        ASSERT_NE(mesh, nullptr) << shape;
        EXPECT_GT(mesh->indices.size(), 100U) << shape;
        EXPECT_EQ(mesh->indices.size() % 3, 0U);
        items::Vec3 lo;
        items::Vec3 hi;
        it.shape.localBounds(lo, hi);
        const glm::vec3 margin(0.2F);
        for (const auto& v : mesh->vertices) {
            EXPECT_TRUE(glm::all(glm::greaterThanEqual(v.position, glm::vec3(lo) - margin)) &&
                        glm::all(glm::lessThanEqual(v.position, glm::vec3(hi) + margin)))
                << shape << " vertex " << v.position.x << ", " << v.position.y << ", "
                << v.position.z;
        }
    }
}

TEST(MissionItems, ObstacleFiguresAndPhysicalProperties) {
    auto validation = app::MissionValidation::create(DS_TEST_ASSETS_DIR, {});
    ASSERT_TRUE(validation.has_value());
    common::Json m = common::Json::parse(kMission);
    m["items"].push_back({{"type", "obstacle"},
                          {"id", "barn"},
                          {"shape", "house"},
                          {"local", {20, 0, -20}},
                          {"sizeM", {10, 8, 14}}});
    m["items"].push_back({{"type", "obstacle"},
                          {"id", "daisy"},
                          {"shape", "cow"},
                          {"local", {-10, 0, -20}},
                          {"massKg", 700},
                          {"friction", 0.8}});
    m["items"].push_back({{"type", "obstacle"},
                          {"id", "farmer"},
                          {"shape", "man"},
                          {"local", {-12, 0, -20}},
                          {"heightM", 1.8},
                          {"restitution", 0.15}});
    auto result = missions::validateMission(m, (*validation)->context);
    EXPECT_TRUE(result.valid()) << result.report();

    m["items"][3]["restitution"] = 1.5;  // Out of range.
    EXPECT_FALSE(missions::validateMission(m, (*validation)->context).valid());
    m["items"][3].erase("restitution");
    m["items"][0]["friction"] = 0.5;  // Not for waypoints.
    EXPECT_FALSE(missions::validateMission(m, (*validation)->context).valid());
}

}  // namespace
}  // namespace ds
