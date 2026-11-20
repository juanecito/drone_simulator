// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the ground drone (SPECIFICATION §4.2.2): profile, derived figures, drive
// controller, and the vehicle driven headless through the world (suspension, tyres,
// drivetrain, slopes, rollover, stuck, water, failsafe, determinism).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>

#include "ds/common/json.hpp"
#include "ds/common/math.hpp"
#include "ds/items/item.hpp"
#include "ds/physics/drive_controller.hpp"
#include "ds/physics/ground_profile.hpp"
#include "ds/physics/ground_vehicle.hpp"
#include "ds/physics/vehicle_factory.hpp"
#include "ds/sim/world.hpp"

namespace {

using namespace ds;
using physics::GroundProfile;
using physics::SurfaceMaterial;
using physics::Vec3;

GroundProfile rover() {
    auto p = physics::loadGroundProfile(std::filesystem::path(DS_TEST_ASSETS_DIR) / "vehicles" /
                                        "ground" / "rover-4wd-3kg.json");
    EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message);
    return p ? *p : GroundProfile{};
}

// Inclined plane rising by @p slopeDeg towards the north (−z), or towards the east (+x) when
// @p eastward; all of it made of @p material.
sim::GroundFunction plane(double slopeDeg, SurfaceMaterial material, bool eastward = false) {
    const double t = std::tan(common::degToRad(slopeDeg));
    return [t, material, eastward](double x, double z) {
        physics::GroundSample g;
        g.heightM = eastward ? t * x : -t * z;
        const Vec3 n = eastward ? Vec3(-t, 1.0, 0.0) : Vec3(0.0, 1.0, t);
        g.normal = glm::normalize(n);
        g.material = material;
        return g;
    };
}

sim::WorldConfig roverWorld(sim::GroundFunction ground = plane(0.0, SurfaceMaterial::Paved)) {
    sim::WorldConfig c;
    c.profile = rover();
    c.seed = 7;
    c.ground = std::move(ground);
    c.startHeadingRad = 0.0;  // North (−z).
    return c;
}

// Drives a world with fixed controls; the arm switch is cycled first.
struct Driver {
    sim::World& world;
    input::ControlInputs controls{};
    double dt = 0.002;

    void run(double seconds) {
        const int n = static_cast<int>(std::lround(seconds / dt));
        for (int i = 0; i < n; ++i) {
            world.tick(controls, dt);
        }
    }
    void arm() {
        controls.arm = false;
        run(0.1);
        controls.arm = true;
        run(0.1);
    }
};

double headingDeg(const sim::World& w) {
    return common::radToDeg(physics::eulerFromOrientation(w.vehicle().bodyState().orientation).yaw);
}

// --- Profile ---------------------------------------------------------------------------------

TEST(GroundProfile, ReferenceProfileLoadsAndRoundTrips) {
    const GroundProfile p = rover();
    EXPECT_EQ(p.key, "rover-4wd-3kg");
    EXPECT_EQ(p.wheelCount(), 4);
    EXPECT_DOUBLE_EQ(p.chassis.massKg, 3.0);
    const auto again = physics::groundProfileFromJson(physics::groundProfileToJson(p));
    ASSERT_TRUE(again.has_value()) << again.error().message;
    EXPECT_EQ(physics::groundProfileToJson(*again), physics::groundProfileToJson(p));
    // Through the type-dispatching reader as well.
    const auto any = physics::vehicleProfileFromJson(physics::groundProfileToJson(p));
    ASSERT_TRUE(any.has_value());
    EXPECT_EQ(physics::profileType(*any), physics::VehicleType::Ground);
    EXPECT_EQ(physics::profileKey(*any), "rover-4wd-3kg");
}

TEST(GroundProfile, WheelGeometry) {
    GroundProfile p = rover();
    const auto hubs = p.wheelHubsAtRest();
    ASSERT_EQ(hubs.size(), 4U);
    EXPECT_DOUBLE_EQ(hubs[0].x, 0.13);  // Left front.
    EXPECT_DOUBLE_EQ(hubs[0].y, -0.15);
    EXPECT_DOUBLE_EQ(hubs[1].x, -0.13);  // Left rear.
    EXPECT_DOUBLE_EQ(hubs[2].y, 0.15);   // Right front.
    EXPECT_NEAR(hubs[0].z, 0.11 - 0.065, 1e-12);
    EXPECT_FALSE(p.isRightWheel(1));
    EXPECT_TRUE(p.isRightWheel(2));
    p.layout = physics::DriveLayout::SkidSteer6;
    const auto six = p.wheelHubsAtRest();
    ASSERT_EQ(six.size(), 6U);
    EXPECT_DOUBLE_EQ(six[1].x, 0.0);  // Middle axle.
    EXPECT_NEAR(p.staticCompressionM(), 3.0 * physics::kGravity / (6 * 600.0), 1e-12);
}

TEST(GroundProfile, RejectsBadValuesWithTheirPaths) {
    common::Json j = physics::groundProfileToJson(rover());
    j["suspension"]["stiffnessNPerM"] = 100;  // 49 mm of sag for 30 mm of travel.
    j["chassis"]["cogHeightM"] = 0.3;         // Above the body.
    j["wheels"]["radiusM"] = -1;
    j["layout"] = "tank";
    const auto r = physics::groundProfileFromJson(j);
    ASSERT_FALSE(r.has_value());
    const std::string& e = r.error().message;
    EXPECT_NE(e.find("suspension/stiffnessNPerM"), std::string::npos) << e;
    EXPECT_NE(e.find("chassis/cogHeightM"), std::string::npos) << e;
    EXPECT_NE(e.find("wheels/radiusM"), std::string::npos) << e;
    EXPECT_NE(e.find("layout"), std::string::npos) << e;
    // An aerial file is not a ground profile.
    common::Json aerial = j;
    aerial["vehicleType"] = "aerial";
    EXPECT_FALSE(physics::groundProfileFromJson(aerial).has_value());
}

TEST(GroundProfile, DerivedFigures) {
    const physics::GroundStats s = physics::computeGroundStats(rover());
    // 3S on 30:1 gear motors of 850 rpm/V: ≈ 310 wheel rpm with no load, 2 m/s on 130 mm
    // wheels.
    EXPECT_GT(s.topSpeedMs, 1.6);
    EXPECT_LT(s.topSpeedMs, 2.3);
    EXPECT_GT(s.accelerationTimeS, 0.05);
    EXPECT_LT(s.accelerationTimeS, 1.0);
    EXPECT_NEAR(s.rolloverSideDeg, common::radToDeg(std::atan(0.15 / 0.11)), 1e-9);
    EXPECT_NEAR(s.rolloverPitchDeg, common::radToDeg(std::atan(0.13 / 0.11)), 1e-9);
    ASSERT_EQ(s.slopes.size(), physics::allSurfaceMaterials().size());
    for (const physics::SlopeLimit& l : s.slopes) {
        const double mu = physics::surfaceProperties(l.material).peakFriction;
        EXPECT_LE(l.maxSlopeDeg, common::radToDeg(std::atan(mu)) + 1e-9);
        EXPECT_GT(l.maxSlopeDeg, 5.0);
    }
    // Snow is slipperier than paved ground.
    EXPECT_LT(s.slopes[4].maxSlopeDeg, s.slopes[0].maxSlopeDeg);
    EXPECT_GT(s.rangeKm, 1.0);
    EXPECT_LT(s.rangeKm, 20.0);
    EXPECT_GT(s.enduranceMin, 15.0);
}

TEST(SurfaceMaterial, NamesRoundTrip) {
    for (const SurfaceMaterial m : physics::allSurfaceMaterials()) {
        EXPECT_EQ(physics::parseSurfaceMaterial(physics::surfaceMaterialName(m)), m);
        const auto p = physics::surfaceProperties(m);
        EXPECT_GT(p.peakFriction, 0.0);
        EXPECT_GT(p.rollingResistance, 0.0);
    }
    EXPECT_FALSE(physics::parseSurfaceMaterial("lava").has_value());
}

TEST(SurfaceMaterial, WetnessAndSnowChangeTheGrip) {
    const weather::GroundCover dry;
    const weather::GroundCover wet{.wetness = 1.0, .snowDepthM = 0.0};
    const weather::GroundCover snow{.wetness = 0.0, .snowDepthM = 0.05};
    EXPECT_EQ(surfaceProperties(SurfaceMaterial::Paved, dry).peakFriction,
              surfaceProperties(SurfaceMaterial::Paved).peakFriction);
    EXPECT_DOUBLE_EQ(surfaceProperties(SurfaceMaterial::Paved, wet).peakFriction, 0.6);
    EXPECT_NEAR(surfaceProperties(SurfaceMaterial::Paved, {.wetness = 0.5}).peakFriction, 0.75,
                1e-12);
    EXPECT_GT(surfaceProperties(SurfaceMaterial::Dirt, wet).rollingResistance,
              surfaceProperties(SurfaceMaterial::Dirt).rollingResistance);  // Mud.
    EXPECT_GT(surfaceProperties(SurfaceMaterial::Sand, wet).peakFriction,
              surfaceProperties(SurfaceMaterial::Sand).peakFriction);  // Firmer.
    for (const SurfaceMaterial m : physics::allSurfaceMaterials()) {
        EXPECT_DOUBLE_EQ(surfaceProperties(m, snow).peakFriction, 0.3);
        EXPECT_NEAR(surfaceProperties(m, snow).rollingResistance, 0.08 + 0.10, 1e-12);
    }
}

TEST(SurfaceMaterial, FollowsTheTerrainTexture) {
    using terrain::dominantMaterial;
    const auto at = [](double height, double normalY) {
        return physics::surfaceFromTerrain(dominantMaterial(height, normalY, 0.0, 1500.0));
    };
    EXPECT_EQ(at(50.0, 1.0), SurfaceMaterial::Grass);
    EXPECT_EQ(at(50.0, 0.6), SurfaceMaterial::Paved);  // ≈ 53°: bare rock.
    EXPECT_EQ(at(0.5, 1.0), SurfaceMaterial::Sand);    // Beach.
    EXPECT_EQ(at(2000.0, 1.0), SurfaceMaterial::Snow);
}

// --- Drive controller ------------------------------------------------------------------------

TEST(DriveController, ArcadeMixKeepsTheRatio) {
    const auto straight = physics::mixArcade(0.5, 0.0, 0.8);
    EXPECT_DOUBLE_EQ(straight.left, 0.5);
    EXPECT_DOUBLE_EQ(straight.right, 0.5);
    const auto spin = physics::mixArcade(0.0, 1.0, 0.8);
    EXPECT_DOUBLE_EQ(spin.left, 0.8);
    EXPECT_DOUBLE_EQ(spin.right, -0.8);
    const auto both = physics::mixArcade(1.0, 1.0, 1.0);  // 2 and 0 → 1 and 0.
    EXPECT_DOUBLE_EQ(both.left, 1.0);
    EXPECT_DOUBLE_EQ(both.right, 0.0);
}

TEST(DriveController, ArmingChecksAndFailsafe) {
    physics::DriveController fc(rover().controller);
    physics::PilotCommand c;
    physics::DriveConditions ok;
    c.arm = true;
    (void)fc.update(c, ok, 0.002);
    EXPECT_FALSE(fc.armed());
    EXPECT_EQ(fc.armBlock(), physics::ArmBlock::SwitchNotCycled);
    c.arm = false;
    (void)fc.update(c, ok, 0.002);
    c.arm = true;
    c.drive = 0.5;
    (void)fc.update(c, ok, 0.002);
    EXPECT_EQ(fc.armBlock(), physics::ArmBlock::ThrottleNotCentred);
    c.arm = false;
    c.drive = 0.0;
    (void)fc.update(c, ok, 0.002);
    EXPECT_EQ(fc.preArmCheck(c, {.rolledOver = true}), physics::ArmBlock::RolledOver);
    EXPECT_EQ(fc.preArmCheck(c, {.inWater = true}), physics::ArmBlock::InWater);
    c.arm = true;
    (void)fc.update(c, ok, 0.002);
    ASSERT_TRUE(fc.armed());
    c.drive = 1.0;
    auto out = fc.update(c, ok, 0.002);
    EXPECT_DOUBLE_EQ(out.left, 1.0);
    EXPECT_FALSE(out.brake);
    // Link lost: keeps going for the failsafe delay, then stops and brakes.
    c.linkValid = false;
    out = fc.update(c, ok, 0.1);
    EXPECT_FALSE(fc.failsafe());
    out = fc.update(c, ok, 0.5);
    EXPECT_TRUE(fc.failsafe());
    EXPECT_DOUBLE_EQ(out.left, 0.0);
    EXPECT_TRUE(out.brake);
    c.linkValid = true;
    out = fc.update(c, ok, 0.002);
    EXPECT_FALSE(fc.failsafe());
    // Rollover disarms.
    (void)fc.update(c, {.rolledOver = true}, 0.002);
    EXPECT_FALSE(fc.armed());
}

// --- Driving -----------------------------------------------------------------------------------

TEST(GroundVehicle, RestsOnItsSuspension) {
    sim::World world(roverWorld());
    Driver d{world};
    d.run(2.0);
    const auto& v = world.vehicle();
    EXPECT_EQ(v.type(), physics::VehicleType::Ground);
    EXPECT_NEAR(v.bodyState().position.y, v.restHeightM(), 0.004);
    EXPECT_LT(glm::length(v.bodyState().velocity), 1e-3);
    const physics::VehicleTelemetry t = v.telemetry();
    double load = 0.0;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(t.ground.contact[static_cast<std::size_t>(i)]);
        load += t.ground.wheelLoad[static_cast<std::size_t>(i)];
        EXPECT_NEAR(t.ground.compression[static_cast<std::size_t>(i)], rover().staticCompressionM(),
                    0.003);
    }
    EXPECT_NEAR(load, 3.0 * physics::kGravity, 0.5);
    EXPECT_TRUE(t.onGround);
    EXPECT_NEAR(t.ground.loadTransfer, 0.0, 0.02);
}

TEST(GroundVehicle, ReachesTheTopSpeedOfItsProfileAndDrivesStraight) {
    sim::World world(roverWorld());
    Driver d{world};
    d.arm();
    ASSERT_TRUE(world.vehicle().armed());
    d.controls.drive = 1.0;
    d.run(4.0);
    const physics::VehicleTelemetry t = world.vehicle().telemetry();
    const double expected = physics::computeGroundStats(rover()).topSpeedMs;
    EXPECT_NEAR(t.ground.speedMs, expected, 0.1 * expected);
    EXPECT_NEAR(t.ground.odometrySpeedMs, t.ground.speedMs, 0.1);
    // Northwards (−z), still on its heading.
    EXPECT_LT(world.vehicle().bodyState().position.z, -4.0);
    EXPECT_NEAR(world.vehicle().bodyState().position.x, 0.0, 0.05);
    EXPECT_NEAR(headingDeg(world), 0.0, 1.0);
    EXPECT_LT(t.ground.maxSlip, 0.2);
    // Battery: four motors cruising plus the electronics.
    EXPECT_GT(t.battery.current, 1.0);
    EXPECT_LT(t.battery.current, 15.0);
    EXPECT_GT(t.ground.odometerM, 4.0);
}

TEST(GroundVehicle, AFailedWheelMotorPullsToItsSide) {
    // FR-PHY-15: motor 1 (front left) dead — the left side drives on one motor and drags, so
    // the rover is slower and veers left.
    sim::World world(roverWorld());
    Driver d{world};
    d.arm();
    physics::Failure f = physics::defaultFailure(physics::FailureType::MotorFailure);
    f.motor = 1;
    world.injectFailure(f);
    d.controls.drive = 1.0;
    d.run(4.0);
    const double expected = physics::computeGroundStats(rover()).topSpeedMs;
    EXPECT_LT(world.vehicle().telemetry().ground.speedMs, 0.95 * expected);
    EXPECT_LT(headingDeg(world), -2.0);
    EXPECT_FALSE(world.vehicle().telemetry().ground.rolledOver);
}

TEST(GroundVehicle, AcceleratesAsTheDerivedFiguresSay) {
    sim::World world(roverWorld());
    Driver d{world};
    d.arm();
    d.controls.drive = 1.0;
    const physics::GroundStats s = physics::computeGroundStats(rover());
    double t = 0.0;
    while (world.vehicle().telemetry().ground.speedMs < 0.9 * s.topSpeedMs && t < 5.0) {
        d.run(0.01);
        t += 0.01;
    }
    EXPECT_NEAR(t, s.accelerationTimeS, std::max(0.1, 0.3 * s.accelerationTimeS));
}

TEST(GroundVehicle, BrakesToAStopAndHolds) {
    sim::World world(roverWorld());
    Driver d{world};
    d.arm();
    d.controls.drive = 1.0;
    d.run(3.0);
    const double z0 = world.vehicle().bodyState().position.z;
    d.controls.drive = 0.0;
    d.controls.brake = true;
    d.run(2.0);
    EXPECT_LT(std::abs(world.vehicle().telemetry().ground.speedMs), 0.01);
    const double stop = z0 - world.vehicle().bodyState().position.z;
    EXPECT_GT(stop, 0.0);
    EXPECT_LT(stop, 0.6);  // ≈ v² / (2·μ·g) plus the reaction of the motors.
}

TEST(GroundVehicle, SkidSteersInPlaceAndTurnsRightWithRightSteering) {
    sim::World world(roverWorld(plane(0.0, SurfaceMaterial::Grass)));
    Driver d{world};
    d.arm();
    d.controls.steering = 1.0;
    d.run(1.0);
    // Body rate r > 0: clockwise seen from above, to the right; a few turns a second at most.
    const double rate = common::radToDeg(world.vehicle().bodyState().angularVelocity.z);
    EXPECT_GT(rate, 45.0);
    EXPECT_LT(rate, 720.0);
    const Vec3 p = world.vehicle().bodyState().position;
    EXPECT_LT(std::hypot(p.x, p.z), 0.1);  // Spins about its centre.
    // Arcing forward and right: heading east of north after a short while.
    sim::World arc(roverWorld(plane(0.0, SurfaceMaterial::Grass)));
    Driver a{arc};
    a.arm();
    a.controls.steering = 0.3;
    a.controls.drive = 0.8;
    a.run(0.5);
    EXPECT_GT(headingDeg(arc), 5.0);
    EXPECT_GT(arc.vehicle().bodyState().position.x, 0.0);
}

TEST(GroundVehicle, SpeedControlHoldsTheCruiseSpeed) {
    sim::World world(roverWorld(plane(0.0, SurfaceMaterial::Grass)));
    Driver d{world};
    d.controls.flightMode = 2;  // Speed control.
    d.arm();
    d.controls.drive = 0.5;  // Half of 1.8 m/s.
    d.run(4.0);
    const physics::VehicleTelemetry t = world.vehicle().telemetry();
    EXPECT_EQ(t.ground.mode, physics::DriveMode::SpeedControl);
    EXPECT_DOUBLE_EQ(t.ground.cruiseSetpointMs, 0.9);
    EXPECT_NEAR(t.ground.speedMs, 0.9, 0.05);
    // Uphill the controller adds power to keep the speed.
    d.controls.drive = 0.0;
    d.run(2.0);
    EXPECT_NEAR(world.vehicle().telemetry().ground.speedMs, 0.0, 0.02);
}

TEST(GroundVehicle, ClimbsWithinItsLimitAndSlidesBackBeyondIt) {
    const physics::GroundStats s = physics::computeGroundStats(rover());
    {
        // 20° of paved ground: well within the limit.
        sim::World world(roverWorld(plane(20.0, SurfaceMaterial::Paved)));
        Driver d{world};
        d.arm();
        d.controls.drive = 1.0;
        d.run(3.0);
        EXPECT_LT(world.vehicle().bodyState().position.z, -2.0);
        EXPECT_GT(world.vehicle().telemetry().ground.speedMs, 0.5);
        EXPECT_NEAR(world.vehicle().telemetry().ground.slopeDeg, 20.0, 0.5);
    }
    {
        // Snow is good for ≈ 16°; at 25° the wheels spin and it slides back.
        ASSERT_LT(s.slopes[4].maxSlopeDeg, 20.0);
        sim::World world(roverWorld(plane(25.0, SurfaceMaterial::Snow)));
        Driver d{world};
        d.arm();
        d.controls.drive = 1.0;
        d.run(3.0);
        EXPECT_GT(world.vehicle().bodyState().position.z, 0.0);  // Lost ground.
        EXPECT_GT(world.vehicle().telemetry().ground.maxSlip, 0.3);
    }
}

TEST(GroundVehicle, ParksOnASlopeWithTheBrake) {
    sim::World world(roverWorld(plane(15.0, SurfaceMaterial::Grass)));
    Driver d{world};
    d.run(0.5);  // Disarmed: the ESC brakes.
    const Vec3 p0 = world.vehicle().bodyState().position;
    d.run(10.0);
    EXPECT_LT(glm::length(world.vehicle().bodyState().position - p0), 0.002);
    // Facing uphill (north): the rear wheels are downhill and carry more.
    const auto t = world.vehicle().telemetry();
    EXPECT_GT(t.ground.wheelLoad[1], t.ground.wheelLoad[0]);
    // Without a holding brake on 25°, gearbox friction and the shorted motors only slow it
    // down: it rolls back.
    GroundProfile weak = rover();
    weak.drivetrain.brakeTorqueNm = 0.0;
    sim::WorldConfig c = roverWorld(plane(25.0, SurfaceMaterial::Paved));
    c.profile = weak;
    sim::World rolling(c);
    Driver r{rolling};
    r.run(3.0);
    EXPECT_GT(rolling.vehicle().bodyState().position.z, 0.2);
}

TEST(GroundVehicle, LeansOnASideSlopeAndRollsOverWhenTippedPastItsLimit) {
    {
        // 30° side slope (rising east, the rover facing north): it leans but stays up, held
        // by its tyres.
        sim::World world(roverWorld(plane(30.0, SurfaceMaterial::Paved, true)));
        Driver d{world};
        d.run(0.5);
        const Vec3 p0 = world.vehicle().bodyState().position;
        d.run(2.0);
        EXPECT_LT(glm::length(world.vehicle().bodyState().position - p0), 0.002);
        const auto t = world.vehicle().telemetry();
        EXPECT_FALSE(t.ground.rolledOver);
        // Downhill (left) wheels carry more of the weight: statically the transfer is
        // 2·h·tan θ / track = 0.42, a little more as the body rolls on its springs.
        EXPECT_NEAR(t.ground.loadTransfer, -0.45, 0.05);
    }
    {
        // Lying on its side: a rollover.
        sim::World world(roverWorld());
        int rollovers = 0;
        (void)world.events().subscribe<sim::RolloverEvent>(
            [&](const sim::RolloverEvent&) { ++rollovers; });
        physics::RigidBodyState s = world.vehicle().bodyState();
        s.orientation = physics::orientationFromEuler({std::numbers::pi / 2.0, 0.0, 0.0});
        s.position.y = 0.2;
        world.vehicle().setBodyState(s);
        Driver d{world};
        d.run(1.5);
        EXPECT_EQ(rollovers, 1);
        const auto t = world.vehicle().telemetry();
        EXPECT_TRUE(t.ground.rolledOver);
        // It cannot be armed again until it is reset.
        d.arm();
        EXPECT_FALSE(world.vehicle().armed());
        EXPECT_EQ(world.vehicle().telemetry().armBlock, physics::ArmBlock::RolledOver);
        world.reset();
        EXPECT_FALSE(world.vehicle().telemetry().ground.rolledOver);
    }
}

TEST(GroundVehicle, StaticRolloverAngleMatchesTheAnalyticalValue) {
    // NFR-REAL-09, tilt-table test: standing across a side slope of θ (tyres that cannot slide,
    // so the slope is not limited by the grip), the rover stays on its wheels below its static
    // rollover angle and rolls over beyond it; the tipping angle is found by bisection.
    // - With near-rigid springs it matches the rigid-body value atan(track / (2 · CoG height))
    //   within ±2°.
    // - With the profile's springs the body leans on the loaded downhill springs (all the
    //   weight on them at the tipping point) and it tips earlier by that lean, as real
    //   vehicles do on a tilt table: within ±2° of the rigid value minus the lean
    //   atan((m g cos θ / 2) / k / track).
    const GroundProfile base = rover();
    const double rigidDeg = physics::computeGroundStats(base).rolloverSideDeg;
    const auto tippingAngle = [&](const GroundProfile& p) {
        const auto rollsOver = [&](double slopeDeg) {
            sim::WorldConfig config = roverWorld(plane(slopeDeg, SurfaceMaterial::Paved, true));
            config.profile = p;
            sim::World world(std::move(config));
            Driver d{world};
            d.run(3.0);
            return world.vehicle().telemetry().ground.rolledOver;
        };
        double lo = rigidDeg - 10.0;
        double hi = rigidDeg + 6.0;
        EXPECT_FALSE(rollsOver(lo));
        EXPECT_TRUE(rollsOver(hi));
        for (int i = 0; i < 7; ++i) {  // 0.13° resolution.
            const double mid = 0.5 * (lo + hi);
            (rollsOver(mid) ? hi : lo) = mid;
        }
        return 0.5 * (lo + hi);
    };
    GroundProfile p = base;
    p.tyre.gripScale = 4.0;
    GroundProfile stiff = p;
    stiff.suspension.stiffnessNPerM *= 25.0;
    stiff.suspension.dampingNsPerM *= 5.0;
    EXPECT_NEAR(tippingAngle(stiff), rigidDeg, 2.0);

    double compliantDeg = rigidDeg;
    for (int i = 0; i < 10; ++i) {
        const double perWheel =
            p.chassis.massKg * physics::kGravity * std::cos(common::degToRad(compliantDeg)) / 2.0;
        const double lean =
            std::atan(perWheel / p.suspension.stiffnessNPerM / p.wheels.trackWidthM);
        compliantDeg = rigidDeg - common::radToDeg(lean);
    }
    EXPECT_NEAR(tippingAngle(p), compliantDeg, 2.0);
}

TEST(GroundVehicle, BrakingDistanceOnEachMaterial) {
    // NFR-REAL-09: from full speed, the brake locks the wheels (its torque exceeds the grip on
    // every material) and the rover slides to a stop within ±10 % of v² / (2 μ g), μ the
    // surface's peak friction. The locked tyre slides at the Magic Formula's value at full
    // slip, sin(C · atan B) ≈ 0.91 of the peak, so the distance is a little longer: within
    // 5 % of v² / (2 μ_slide g).
    const GroundProfile p = rover();
    for (const SurfaceMaterial m : physics::allSurfaceMaterials()) {
        sim::World world(roverWorld(plane(0.0, m)));
        Driver d{world};
        d.arm();
        d.controls.drive = 1.0;
        d.run(4.0);
        const double v0 = world.vehicle().telemetry().ground.speedMs;
        const Vec3 p0 = world.vehicle().bodyState().position;
        d.controls.drive = 0.0;
        d.controls.brake = true;
        d.run(3.0);
        const std::string name(physics::surfaceMaterialName(m));
        EXPECT_LT(std::abs(world.vehicle().telemetry().ground.speedMs), 0.01) << name;
        const double distance = glm::length(world.vehicle().bodyState().position - p0);
        const double mu = physics::surfaceProperties(m).peakFriction * p.tyre.gripScale;
        const double muSlide = mu * std::sin(p.tyre.shapeC * std::atan(p.tyre.stiffnessB));
        const double peakDistance = v0 * v0 / (2.0 * mu * physics::kGravity);
        const double slideDistance = v0 * v0 / (2.0 * muSlide * physics::kGravity);
        EXPECT_NEAR(distance, peakDistance, 0.1 * peakDistance) << name;
        EXPECT_NEAR(distance, slideDistance, 0.05 * slideDistance) << name;
    }
}

items::Item wallAhead(double distanceM) {
    items::Item wall;
    wall.type = items::ItemType::Obstacle;
    wall.key = "wall";
    wall.name = "Wall";
    wall.solid = true;
    wall.properties = {{"kind", "box"}, {"sizeM", {4.0, 1.0, 0.4}}};
    wall.transform.position = Vec3(0.0, 0.0, -distanceM);
    wall.shape = items::defaultShape(wall.type, wall.properties);
    return wall;
}

TEST(GroundVehicle, ReportsBeingStuck) {
    // Pushing against a wall: the wheels turn, the rover goes nowhere.
    sim::World world(roverWorld());
    world.setFreeItems({wallAhead(1.0)});
    int stuck = 0;
    (void)world.events().subscribe<sim::VehicleStuckEvent>(
        [&](const sim::VehicleStuckEvent&) { ++stuck; });
    Driver d{world};
    d.arm();
    d.controls.drive = 0.6;
    d.run(4.0);
    EXPECT_EQ(stuck, 0);
    EXPECT_GT(world.vehicle().telemetry().ground.stuckS, 2.0);
    d.run(4.0);
    EXPECT_EQ(stuck, 1);  // Once per episode.
    EXPECT_GT(world.vehicle().telemetry().ground.stuckS, rover().limits.stuckDetectS);
    // Backing away is progress.
    d.controls.drive = -0.6;
    d.run(2.0);
    EXPECT_LT(world.vehicle().telemetry().ground.stuckS, 0.5);  // Restarts every 0.3 m.
}

TEST(GroundVehicle, StopsInWaterDeeperThanItsWadingDepth) {
    // A beach sloping 10° down to the south into a lake at height −0.3 m.
    sim::WorldConfig c = roverWorld(plane(10.0, SurfaceMaterial::Sand));
    c.waterLevelM = -0.3;
    c.startHeadingRad = std::numbers::pi;  // South, downhill.
    sim::World world(c);
    int water = 0;
    (void)world.events().subscribe<sim::WaterEnteredEvent>(
        [&](const sim::WaterEnteredEvent&) { ++water; });
    Driver d{world};
    d.arm();
    d.controls.drive = 0.6;
    d.run(6.0);
    EXPECT_EQ(water, 1);
    const auto t = world.vehicle().telemetry();
    EXPECT_TRUE(t.ground.inWater);
    EXPECT_FALSE(world.vehicle().armed());
    EXPECT_GT(t.ground.waterDepthM, rover().limits.wadingDepthM);
}

TEST(GroundVehicle, RainAndSnowTakeTheGripAway) {
    // FR-WX-07: dry grass is good for a 22° climb, wet grass and snow-covered ground are not.
    const auto climb = [](const char* weatherJson) {
        sim::WorldConfig config = roverWorld(plane(22.0, SurfaceMaterial::Grass));
        if (weatherJson != nullptr) {
            config.weather = *weather::WeatherSettings::fromJson(*common::parseJson(weatherJson));
        }
        sim::World world(std::move(config));
        Driver d{world};
        d.arm();
        d.controls.drive = 1.0;
        d.run(3.0);
        return std::pair(-world.vehicle().bodyState().position.z,
                         world.vehicle().telemetry().ground.cover);
    };
    const auto [dry, dryCover] = climb(nullptr);
    EXPECT_GT(dry, 2.0);
    EXPECT_DOUBLE_EQ(dryCover.wetness, 0.0);
    const auto [wet, wetCover] = climb(R"({"seed": 1, "preset": "light-rain", "temperatureC": 9})");
    EXPECT_DOUBLE_EQ(wetCover.wetness, 1.0);
    EXPECT_LT(wet, 0.5);
    const auto [snowy, snowCover] = climb(R"({"seed": 1, "preset": "heavy-snow"})");
    EXPECT_GT(snowCover.snowDepthM, 0.02);
    EXPECT_LT(snowy, 0.5);
    EXPECT_EQ(physics::surfaceDescription(SurfaceMaterial::Grass, wetCover), "wet grass");
    EXPECT_EQ(physics::surfaceDescription(SurfaceMaterial::Grass, snowCover), "snow");
}

TEST(GroundVehicle, SofterGroundIsSlower) {
    const auto topSpeed = [](SurfaceMaterial m) {
        sim::World world(roverWorld(plane(0.0, m)));
        Driver d{world};
        d.arm();
        d.controls.drive = 1.0;
        d.run(3.0);
        return world.vehicle().telemetry().ground.speedMs;
    };
    const double paved = topSpeed(SurfaceMaterial::Paved);
    const double sand = topSpeed(SurfaceMaterial::Sand);
    EXPECT_LT(sand, paved - 0.1);
}

TEST(GroundVehicle, FailsafeStopsTheRover) {
    sim::World world(roverWorld());
    Driver d{world};
    d.arm();
    d.controls.drive = 1.0;
    d.run(2.0);
    d.controls.linkValid = false;
    d.run(3.0);
    const auto t = world.vehicle().telemetry();
    EXPECT_TRUE(t.failsafe);
    EXPECT_LT(std::abs(t.ground.speedMs), 0.02);
}

TEST(GroundVehicle, IsDeterministicAndStableAtAnyTickRate) {
    const auto run = [](double dt) {
        sim::World world(roverWorld(plane(5.0, SurfaceMaterial::Dirt)));
        Driver d{world, {}, dt};
        d.arm();
        d.controls.drive = 0.8;
        d.controls.steering = 0.3;
        d.run(3.0);
        return world.vehicle().bodyState().position;
    };
    const Vec3 a = run(0.002);
    EXPECT_EQ(a, run(0.002));
    // 240 Hz and 2 kHz end up within a few centimetres of 500 Hz.
    EXPECT_LT(glm::length(run(1.0 / 240.0) - a), 0.05);
    EXPECT_LT(glm::length(run(0.0005) - a), 0.05);
}

TEST(GroundVehicle, HitsAnObstacle) {
    sim::World world(roverWorld());
    world.setFreeItems({wallAhead(2.0)});
    int collisions = 0;
    (void)world.events().subscribe<sim::CollisionEvent>(
        [&](const sim::CollisionEvent&) { ++collisions; });
    Driver d{world};
    d.arm();
    d.controls.drive = 1.0;
    d.run(3.0);
    EXPECT_GE(collisions, 1);
    EXPECT_GT(world.vehicle().bodyState().position.z, -2.0 + 0.2);  // Stopped by the wall.
    EXPECT_FALSE(world.vehicle().telemetry().crashed);              // 2 m/s < 4 m/s.
}

items::Item drivable(items::ItemType type, const Vec3& position, double headingDeg,
                     const common::Json& properties) {
    items::Item item;
    item.type = type;
    item.key = std::string(items::itemTypeName(type));
    item.solid = true;
    item.properties = properties;
    item.transform.position = position;
    item.transform.rotation = items::headingRotation(headingDeg);
    item.shape = items::defaultShape(type, properties);
    return item;
}

TEST(GroundVehicle, DrivesUpARampAndOffAKicker) {
    // FR-GND-03, FR-ITM-13: a 0.25 m high, 1.5 m long (9.5°) ramp rising north, starting 1 m
    // ahead of the rover. It drives up and parks near the top with the brake.
    sim::World world(roverWorld(plane(0.0, SurfaceMaterial::Grass)));
    world.setFreeItems({drivable(items::ItemType::Ramp, Vec3(0.0, 0.0, -1.75), 0.0,
                                 {{"sizeM", {1.0, 0.25, 1.5}}})});
    const double rest = world.vehicle().bodyState().position.y;
    EXPECT_NEAR(world.surfaceAt(Vec3(0.0, 1.0, -2.5)).heightM, 0.25, 1e-3);
    EXPECT_EQ(world.surfaceAt(Vec3(0.0, 1.0, -2.0)).material, SurfaceMaterial::Paved);
    EXPECT_NEAR(world.surfaceAt(Vec3(0.0, 0.1, -2.4)).heightM, 0.0, 1e-9);  // Below its top.
    Driver d{world};
    d.arm();
    d.controls.drive = 0.7;
    for (int i = 0; i < 400 && world.vehicle().bodyState().position.z > -2.1; ++i) {
        d.run(0.01);
    }
    d.controls.drive = 0.0;
    d.controls.brake = true;
    d.run(0.5);
    const Vec3 parked = world.vehicle().bodyState().position;
    const auto t = world.vehicle().telemetry();
    EXPECT_NEAR(t.ground.slopeDeg, common::radToDeg(std::atan(0.25 / 1.5)), 1.0);
    EXPECT_EQ(t.ground.surface, SurfaceMaterial::Paved);
    EXPECT_GT(parked.y - rest, 0.15) << "on the ramp";
    EXPECT_NEAR(std::abs(parked.x), 0.0, 0.1);
    d.run(2.0);
    EXPECT_NEAR(world.vehicle().bodyState().position.z, parked.z, 0.01) << "held by the brake";

    // A 10 cm kicker: the rover drives over it and lands on its wheels. (Driving slowly off
    // a 25 cm ledge makes this short rover nose-dive, as it would for real.)
    sim::World kicker(roverWorld(plane(0.0, SurfaceMaterial::Grass)));
    kicker.setFreeItems(
        {drivable(items::ItemType::Ramp, Vec3(0.0, 0.0, -1.5), 0.0, {{"sizeM", {1.0, 0.1, 1.0}}})});
    Driver k{kicker};
    k.arm();
    k.controls.drive = 0.7;
    double highest = 0.0;
    for (int i = 0; i < 100 && kicker.vehicle().bodyState().position.z > -4.0; ++i) {
        k.run(0.05);
        highest = std::max(highest, kicker.vehicle().bodyState().position.y - rest);
    }
    k.controls.drive = 0.0;
    k.run(1.0);
    EXPECT_GT(highest, 0.07);
    EXPECT_LT(kicker.vehicle().bodyState().position.z, -3.0);
    EXPECT_NEAR(kicker.vehicle().bodyState().position.y, rest, 0.03) << "back on the ground";
    EXPECT_FALSE(kicker.vehicle().telemetry().ground.rolledOver);
    EXPECT_FALSE(kicker.vehicle().telemetry().crashed);
}

TEST(GroundVehicle, CrossesABridgeOverWaterAndPassesUnderAHighOne) {
    // A 3 m wide channel 1 m deep with water up to −0.5 m, crossing the rover's way north, and
    // a 0.3 m high bridge over it (deck 4 m long, 1 m wide, 1.4 m approach ramps).
    const sim::GroundFunction channel = [](double, double z) {
        physics::GroundSample g;
        g.heightM = z < -3.0 && z > -6.0 ? -1.0 : 0.0;
        g.material = SurfaceMaterial::Grass;
        return g;
    };
    sim::WorldConfig c = roverWorld(channel);
    c.waterLevelM = -0.5;
    sim::World world(c);
    world.setFreeItems({drivable(items::ItemType::Bridge, Vec3(0.0, 0.0, -4.5), 0.0,
                                 {{"sizeM", {1.0, 0.3, 4.0}}})});
    EXPECT_NEAR(world.surfaceAt(Vec3(0.0, 1.0, -4.5)).heightM, 0.3, 1e-9);
    EXPECT_NEAR(world.surfaceAt(Vec3(0.0, -0.8, -4.5)).heightM, -1.0, 1e-9);  // Under it.
    int water = 0;
    (void)world.events().subscribe<sim::WaterEnteredEvent>(
        [&](const sim::WaterEnteredEvent&) { ++water; });
    Driver d{world};
    d.arm();
    d.controls.drive = 0.7;
    double highest = -1.0;
    for (int i = 0; i < 200 && world.vehicle().bodyState().position.z > -10.0; ++i) {
        d.run(0.1);
        highest = std::max(highest, world.vehicle().bodyState().position.y);
        EXPECT_FALSE(world.vehicle().telemetry().ground.inWater);
    }
    EXPECT_EQ(water, 0);
    EXPECT_LT(world.vehicle().bodyState().position.z, -9.0) << "across the bridge";
    EXPECT_GT(highest, 0.3) << "over the deck";
    EXPECT_TRUE(world.vehicle().armed());

    // A footbridge 1 m up (deck 0.7–1.0 m) across the way: the rover drives under it.
    sim::World under(roverWorld(plane(0.0, SurfaceMaterial::Paved)));
    under.setFreeItems({drivable(items::ItemType::Bridge, Vec3(0.0, 0.0, -3.0), 90.0,
                                 {{"sizeM", {1.5, 1.0, 3.0}}})});
    const double rest = under.vehicle().bodyState().position.y;
    EXPECT_NEAR(under.surfaceAt(Vec3(0.0, rest, -3.0)).heightM, 0.0, 1e-9);
    EXPECT_NEAR(under.surfaceAt(Vec3(0.0, 1.2, -3.0)).heightM, 1.0, 1e-9);
    Driver u{under};
    u.arm();
    u.controls.drive = 0.7;
    double top = 0.0;
    for (int i = 0; i < 120 && under.vehicle().bodyState().position.z > -6.0; ++i) {
        u.run(0.1);
        top = std::max(top, under.vehicle().bodyState().position.y);
    }
    EXPECT_LT(under.vehicle().bodyState().position.z, -5.0);
    EXPECT_LT(top, rest + 0.03);
}

// --- World integration -----------------------------------------------------------------------

TEST(GroundWorld, SwitchesVehicleTypeAndFillsTheSnapshot) {
    sim::World world(roverWorld());
    EXPECT_NE(world.groundVehicle(), nullptr);
    EXPECT_EQ(world.aerialVehicle(), nullptr);
    sim::WorldSnapshot s;
    world.fillSnapshot(s);
    EXPECT_EQ(s.profileKey, "rover-4wd-3kg");
    EXPECT_EQ(s.telemetry.type, physics::VehicleType::Ground);
    EXPECT_EQ(s.telemetry.ground.wheelCount, 4);
    EXPECT_NEAR(s.aglM, 0.0, 0.01);

    auto aerial = physics::loadAerialProfile(std::filesystem::path(DS_TEST_ASSETS_DIR) /
                                             "vehicles" / "aerial" / "camera-250g.json");
    ASSERT_TRUE(aerial.has_value());
    world.setProfile(*aerial);
    EXPECT_NE(world.aerialVehicle(), nullptr);
    EXPECT_EQ(world.vehicle().type(), physics::VehicleType::Aerial);
    world.setProfile(rover());
    EXPECT_NE(world.groundVehicle(), nullptr);
}

TEST(GroundWorld, ToPilotCommandCarriesTheDriveControls) {
    input::ControlInputs c;
    c.drive = 2.0;
    c.steering = -0.5;
    c.brake = true;
    const physics::PilotCommand p = sim::toPilotCommand(c);  // Default mode index (1).
    EXPECT_DOUBLE_EQ(p.drive, 1.0);
    EXPECT_DOUBLE_EQ(p.steering, -0.5);
    EXPECT_TRUE(p.brake);
    EXPECT_EQ(p.driveMode, physics::DriveMode::Manual);
    c.flightMode = 2;
    EXPECT_EQ(sim::toPilotCommand(c).driveMode, physics::DriveMode::SpeedControl);
}

TEST(GroundWorld, PayloadChangesTheMass) {
    sim::World world(roverWorld());
    const double empty = world.vehicle().mass().massKg;
    world.vehicle().attachPayload(1.0, Vec3(0.0, 0.0, -0.1), 0.01);
    EXPECT_NEAR(world.vehicle().mass().massKg, empty + 1.0, 1e-12);
    world.vehicle().releasePayload();
    EXPECT_NEAR(world.vehicle().mass().massKg, empty, 1e-12);
}

}  // namespace
