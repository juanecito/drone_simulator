// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "ds/items/item.hpp"
#include "ds/items/registry.hpp"
#include "ds/items/triggers.hpp"

namespace {

using namespace ds::items;
using ds::common::Json;

Item makeItem(ItemType type, std::string key, Vec3 position, const Json& props = Json::object(),
              double headingDeg = 0.0) {
    Item item;
    item.type = type;
    item.key = std::move(key);
    item.name = item.key;
    item.transform.position = position;
    item.transform.rotation = headingRotation(headingDeg);
    item.shape = defaultShape(type, props);
    item.solid = isSolidType(type);
    item.properties = props;
    return item;
}

TEST(Items, TypeNamesRoundTrip) {
    for (const ItemType t : allItemTypes()) {
        EXPECT_EQ(parseItemType(itemTypeName(t)), t);
    }
    EXPECT_FALSE(parseItemType("spaceship"));
}

TEST(Items, HeadingRotationTurnsForwardClockwise) {
    const Vec3 east = headingRotation(90.0) * Vec3(0.0, 0.0, -1.0);
    EXPECT_NEAR(east.x, 1.0, 1e-12);
    EXPECT_NEAR(east.z, 0.0, 1e-12);
    EXPECT_NEAR(headingOf(headingRotation(225.0)), 225.0, 1e-9);
    EXPECT_NEAR(headingOf(headingRotation(0.0)), 0.0, 1e-9);
}

TEST(Items, ShapesContainPoints) {
    const Item pad = makeItem(ItemType::LandingPad, "pad", Vec3(10, 5, 0), Json{{"radiusM", 1.5}});
    EXPECT_TRUE(pad.contains(Vec3(11.4, 5.1, 0)));
    EXPECT_FALSE(pad.contains(Vec3(11.6, 5.1, 0)));
    // Obstacle box stands on its origin.
    const Item box = makeItem(ItemType::Obstacle, "b", Vec3(0), Json{{"sizeM", {4, 10, 2}}});
    EXPECT_TRUE(box.contains(Vec3(1.9, 9.9, 0.9)));
    EXPECT_FALSE(box.contains(Vec3(0, -0.1, 0)));
    // Prism from a polygon, rotated by its heading.
    const Item area = makeItem(ItemType::SurveyArea, "a", Vec3(100, 0, 100),
                               Json{{"polygon", {{0, 0}, {50, 0}, {50, 10}, {0, 10}}}}, 90.0);
    // Heading 90°: local +x (east) becomes south (+z), local +z becomes west (−x).
    EXPECT_TRUE(area.contains(Vec3(95, 300, 120)));
    EXPECT_FALSE(area.contains(Vec3(120, 0, 95)));
    // Parking zone footprint follows the heading too.
    const Item bay = makeItem(ItemType::ParkingZone, "bay", Vec3(0), Json{{"sizeM", {2, 6}}}, 90.0);
    EXPECT_TRUE(bay.contains(Vec3(2.5, 0.2, 0.5)));
    EXPECT_FALSE(bay.contains(Vec3(0.5, 0.2, 2.5)));
}

TEST(Items, RampIsADrivableWedgeRisingTowardsItsHeading) {
    // FR-ITM-13: 1 m wide, 0.25 m high, 1.5 m long, rising towards the east (heading 90°).
    const Item ramp =
        makeItem(ItemType::Ramp, "ramp", {10.0, 2.0, 0.0}, {{"sizeM", {1.0, 0.25, 1.5}}}, 90.0);
    EXPECT_TRUE(ramp.solid);
    EXPECT_TRUE(isDrivableType(ItemType::Ramp));
    EXPECT_FALSE(isDrivableType(ItemType::Obstacle));
    ASSERT_EQ(ramp.shape.parts.size(), 1U);
    EXPECT_EQ(ramp.shape.parts[0].kind, SolidPart::Kind::Wedge);
    EXPECT_NEAR(ramp.shape.parts[0].volumeM3(), 0.5 * 1.0 * 0.25 * 1.5, 1e-12);

    // Top: 0 at the west (low) end, 0.125 m in the middle, 0.25 m at the east end.
    const double slope = 0.25 / 1.5;
    for (const double x : {9.26, 10.0, 10.74}) {
        const auto top = driveSurfaceAt(ramp, x, 0.3, 10.0);
        ASSERT_TRUE(top.has_value()) << x;
        EXPECT_NEAR(top->heightM, 2.0 + (x - 9.25) * slope, 1e-9) << x;
        EXPECT_NEAR(top->normal.y, 1.0 / std::hypot(1.0, slope), 1e-9);
        EXPECT_NEAR(top->normal.x, -slope / std::hypot(1.0, slope), 1e-9);  // Faces downhill.
        EXPECT_NEAR(top->normal.z, 0.0, 1e-9);
        EXPECT_EQ(top->material, Material::Wood);
    }
    EXPECT_FALSE(driveSurfaceAt(ramp, 10.0, 0.6, 10.0).has_value());  // Beside it.
    EXPECT_FALSE(driveSurfaceAt(ramp, 10.8, 0.0, 10.0).has_value());  // Past the high end.
    EXPECT_FALSE(driveSurfaceAt(ramp, 10.0, 0.0, 2.1).has_value());   // Above the point.

    // Solid: a point just under the slope is pushed out through it; one at the high end's
    // foot is pushed back east out of the vertical face.
    auto c = obstacleContact(ramp, {10.0, 2.12, 0.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_GT(c->normal.y, 0.95);
    EXPECT_LT(c->normal.x, 0.0);
    EXPECT_NEAR(c->depthM, 0.005 / std::hypot(1.0, slope), 1e-9);
    c = obstacleContact(ramp, {10.74, 2.05, 0.0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->normal.x, 1.0, 1e-9);
    EXPECT_FALSE(obstacleContact(ramp, {10.0, 2.13, 0.0}).has_value());
}

TEST(Items, BridgeHasADeckApproachRampsAndKerbs) {
    // Deck top 1 m up over 4 m along z (heading 0), 1.2 m wide; ramps 2 m long.
    const Item bridge = makeItem(ItemType::Bridge, "bridge", {0.0, 0.0, 0.0},
                                 {{"sizeM", {1.2, 1.0, 4.0}}, {"rampLengthM", 2.0}});
    ASSERT_EQ(bridge.shape.parts.size(), 5U);  // Deck, two ramps, two kerbs.
    Vec3 lo;
    Vec3 hi;
    bridge.worldBounds(lo, hi);
    EXPECT_NEAR(lo.z, -4.0, 1e-9);
    EXPECT_NEAR(hi.z, 4.0, 1e-9);

    // On the deck and half-way up either ramp.
    EXPECT_NEAR(driveSurfaceAt(bridge, 0.0, 1.5, 5.0)->heightM, 1.0, 1e-9);
    EXPECT_NEAR(driveSurfaceAt(bridge, 0.0, 3.0, 5.0)->heightM, 0.5, 1e-9);
    EXPECT_NEAR(driveSurfaceAt(bridge, 0.0, -3.0, 5.0)->heightM, 0.5, 1e-9);
    EXPECT_GT(driveSurfaceAt(bridge, 0.0, 3.0, 5.0)->normal.z, 0.0);  // Downhill: south.
    EXPECT_LT(driveSurfaceAt(bridge, 0.0, -3.0, 5.0)->normal.z, 0.0);
    // Under the deck there is nothing to stand on (the deck is 0.3 m thick) and room to pass.
    EXPECT_FALSE(driveSurfaceAt(bridge, 0.0, 0.0, 0.5).has_value());
    EXPECT_FALSE(obstacleContact(bridge, {0.0, 0.5, 0.0}).has_value());
    EXPECT_TRUE(obstacleContact(bridge, {0.0, 0.8, 0.0}).has_value());
    // The kerbs stand above the deck and stop a wheel at the edge.
    const auto kerb = obstacleContact(bridge, {0.58, 1.05, 0.0});
    ASSERT_TRUE(kerb.has_value());
    EXPECT_NEAR(std::abs(kerb->normal.x), 1.0, 1e-9);
    EXPECT_NEAR(driveSurfaceAt(bridge, 0.58, 0.0, 1.05)->heightM, 1.0, 1e-9);  // Not on it.

    const Item plain = makeItem(ItemType::Bridge, "plain", {0.0, 0.0, 0.0},
                                {{"sizeM", {1.0, 0.5, 3.0}}, {"kerbs", false}});
    ASSERT_EQ(plain.shape.parts.size(), 3U);
    // Default approach ramps: 12° slopes.
    Vec3 plo;
    Vec3 phi;
    plain.worldBounds(plo, phi);
    EXPECT_NEAR(phi.z, 1.5 + 0.5 / std::tan(kBridgeRampSlopeDeg * std::numbers::pi / 180.0), 1e-9);
    Item off = plain;
    off.enabled = false;
    EXPECT_FALSE(driveSurfaceAt(off, 0.0, 0.0, 5.0).has_value());
}

TEST(ItemRegistry, GenerationalIdsGoStale) {
    ItemRegistry reg;
    const auto a = reg.add(makeItem(ItemType::Waypoint, "w1", Vec3(0)));
    ASSERT_TRUE(a);
    EXPECT_TRUE(reg.remove(*a));
    EXPECT_EQ(reg.get(*a), nullptr);
    const auto b = reg.add(makeItem(ItemType::Waypoint, "w2", Vec3(0)));
    ASSERT_TRUE(b);
    EXPECT_EQ(b->index, a->index);  // Slot reused…
    EXPECT_NE(b->generation, a->generation);  // …with a new generation.
    EXPECT_EQ(reg.get(*a), nullptr);
    EXPECT_EQ(ItemId::fromValue(b->value()), *b);
    EXPECT_FALSE(ItemId{}.valid());
}

TEST(ItemRegistry, CrudKeysAndNotifications) {
    ItemRegistry reg;
    std::vector<ItemChange::Kind> changes;
    const auto token = reg.subscribe([&](const ItemChange& c) { changes.push_back(c.kind); });
    const auto id = reg.add(makeItem(ItemType::Gate, "g1", Vec3(0, 3, 0)));
    ASSERT_TRUE(id);
    EXPECT_FALSE(reg.add(makeItem(ItemType::Gate, "g1", Vec3(0))));  // Duplicate key.
    EXPECT_EQ(reg.find("g1")->id, *id);
    EXPECT_TRUE(reg.rename(*id, "Start gate"));
    EXPECT_EQ(reg.get(*id)->name, "Start gate");
    const auto copy = reg.duplicate(*id);
    ASSERT_TRUE(copy);
    EXPECT_EQ(reg.get(*copy)->key, "");
    EXPECT_EQ(reg.get(*copy)->name, "Start gate (copy)");
    EXPECT_TRUE(reg.setEnabled(*copy, false));
    EXPECT_EQ(reg.size(), 2U);
    reg.unsubscribe(token);
    reg.remove(*copy);
    EXPECT_EQ(changes, (std::vector{ItemChange::Kind::Added, ItemChange::Kind::Modified,
                                    ItemChange::Kind::Added, ItemChange::Kind::Modified}));
}

TEST(ItemRegistry, UndoRedo) {
    ItemRegistry reg;
    const auto id = reg.add(makeItem(ItemType::Waypoint, "w", Vec3(0)));
    Transform moved;
    moved.position = Vec3(10, 0, 0);
    reg.setTransform(*id, moved);
    reg.remove(*id);
    EXPECT_EQ(reg.size(), 0U);
    ASSERT_TRUE(reg.undo());  // Remove undone: same id is valid again.
    ASSERT_NE(reg.get(*id), nullptr);
    EXPECT_EQ(reg.get(*id)->transform.position.x, 10.0);
    ASSERT_TRUE(reg.undo());  // Move undone.
    EXPECT_EQ(reg.get(*id)->transform.position.x, 0.0);
    ASSERT_TRUE(reg.undo());  // Add undone.
    EXPECT_EQ(reg.size(), 0U);
    EXPECT_FALSE(reg.undo());
    ASSERT_TRUE(reg.redo());
    ASSERT_TRUE(reg.redo());
    EXPECT_EQ(reg.get(*id)->transform.position.x, 10.0);
    EXPECT_EQ(reg.find("w")->id, *id);
    // A new edit drops the redo branch.
    reg.rename(*id, "x");
    EXPECT_FALSE(reg.canRedo());
}

TEST(ItemRegistry, UndoNeverReissuesALaterGeneration) {
    ItemRegistry reg;
    const auto a = reg.add(makeItem(ItemType::Waypoint, "a", Vec3(0)));
    reg.remove(*a);
    const auto b = reg.add(makeItem(ItemType::Waypoint, "b", Vec3(0)));  // Reuses the slot.
    ASSERT_TRUE(reg.undo());  // Remove b.
    ASSERT_TRUE(reg.undo());  // Restore a.
    EXPECT_NE(reg.get(*a), nullptr);
    reg.clearHistory();
    reg.remove(*a);
    const auto c = reg.add(makeItem(ItemType::Waypoint, "c", Vec3(0)));
    EXPECT_NE(c->value(), b->value());
    EXPECT_EQ(reg.get(*b), nullptr);
}

TEST(ItemRegistry, SpatialQueriesAndRaycast) {
    ItemRegistry reg(10.0);
    const auto near = reg.add(makeItem(ItemType::Obstacle, "near", Vec3(20, 0, 0),
                                       Json{{"sizeM", {2, 10, 2}}}));
    const auto far = reg.add(makeItem(ItemType::Obstacle, "far", Vec3(500, 0, 0),
                                      Json{{"shape", "tree"}}));
    const auto zone = reg.add(makeItem(ItemType::NoFlyZone, "nfz", Vec3(0),
                                       Json{{"radiusM", 5000}}));
    EXPECT_EQ(reg.queryRadius(Vec3(0, 5, 0), 25.0).size(), 2U);  // near + huge zone.
    EXPECT_EQ(reg.containing(Vec3(500, 3, 1)), (std::vector{*far, *zone}));
    const auto hit = reg.raycast(Vec3(0, 5, 0), Vec3(1, 0, 0), 1000.0);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->id, *near);
    EXPECT_NEAR(hit->distanceM, 19.0, 1e-9);
    EXPECT_FALSE(reg.raycast(Vec3(0, 15, 0), Vec3(1, 0, 0), 1000.0));  // Over the box…
    const auto tree = reg.raycast(Vec3(0, 5, 0), Vec3(1, 0, 0) * 1.0, 1000.0);
    EXPECT_EQ(tree->id, *near);
    reg.setEnabled(*near, false);
    const auto next = reg.raycast(Vec3(0, 5, 0), Vec3(1, 0, 0), 1000.0);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->id, *far);  // …and past it when disabled: the tree.
    EXPECT_NEAR(next->distanceM, 498.0, 1e-9);
}

TEST(ItemRegistry, PlaceOnTerrainFollowsAltitudeReference) {
    ItemRegistry reg;
    Item agl = makeItem(ItemType::Waypoint, "agl", Vec3(10, 0, 0));
    agl.altitudeM = 5.0;
    Item amsl = makeItem(ItemType::Waypoint, "amsl", Vec3(10, 0, 0));
    amsl.altitudeRef = AltitudeRef::Amsl;
    amsl.altitudeM = 700.0;
    const auto a = reg.add(agl);
    const auto b = reg.add(amsl);
    reg.placeOnTerrain([](double x, double) { return x * 2.0; }, 650.0);
    EXPECT_DOUBLE_EQ(reg.get(*a)->transform.position.y, 25.0);
    EXPECT_DOUBLE_EQ(reg.get(*b)->transform.position.y, 50.0);
    EXPECT_TRUE(reg.snapToTerrain(*b, [](double, double) { return 3.0; }));
    EXPECT_DOUBLE_EQ(reg.get(*b)->transform.position.y, 3.0);
    EXPECT_EQ(reg.get(*b)->altitudeRef, AltitudeRef::Agl);
}

TEST(Triggers, GateIsDirectionAware) {
    const Item gate = makeItem(ItemType::Gate, "g", Vec3(0, 5, 0),
                               Json{{"widthM", 4}, {"heightM", 2}}, 90.0);  // Faces east.
    const auto forward = crossGate(gate, Vec3(-1, 5, 0.5), Vec3(1, 5.5, 0.5));
    ASSERT_TRUE(forward);
    EXPECT_FALSE(forward->reversed);
    EXPECT_NEAR(forward->t, 0.5, 1e-12);
    const auto back = crossGate(gate, Vec3(1, 5, 0), Vec3(-1, 5, 0));
    ASSERT_TRUE(back);
    EXPECT_TRUE(back->reversed);
    EXPECT_FALSE(crossGate(gate, Vec3(-1, 7, 0), Vec3(1, 7, 0)));  // Above the opening.
    EXPECT_FALSE(crossGate(gate, Vec3(-1, 5, 3), Vec3(1, 5, 3)));  // Beside it.
}

TEST(Triggers, ZonesWaypointsAndFastGates) {
    ItemRegistry reg;
    const auto gate = reg.add(makeItem(ItemType::Gate, "g", Vec3(0, 5, -50)));  // Faces north.
    const auto wp = reg.add(makeItem(ItemType::Waypoint, "w", Vec3(0, 5, -100)));
    TriggerDetector det;
    det.reset(reg, Vec3(0, 5, 0));
    // One huge step straight through the gate and into the waypoint.
    auto events = det.update(reg, Vec3(0, 5, -100));
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].kind, TriggerEvent::Kind::GatePassed);
    EXPECT_EQ(events[0].id, *gate);
    EXPECT_NEAR(events[0].point.z, -50.0, 1e-9);
    EXPECT_EQ(events[1].kind, TriggerEvent::Kind::ZoneEntered);
    EXPECT_EQ(events[2].kind, TriggerEvent::Kind::WaypointReached);
    EXPECT_TRUE(det.isInside(*wp));
    events = det.update(reg, Vec3(0, 5, -110));
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, TriggerEvent::Kind::ZoneExited);
}

}  // namespace
