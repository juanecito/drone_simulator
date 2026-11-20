// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Unit tests of the physical models (SPECIFICATION §4.6): frames, atmosphere, rigid body,
// powertrain, battery and aerodynamics. NFR-REAL-02 and NFR-REAL-03 are verified here.

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "ds/physics/aerodynamics.hpp"
#include "ds/physics/atmosphere.hpp"
#include "ds/physics/frames.hpp"
#include "ds/physics/powertrain.hpp"
#include "ds/physics/rigid_body.hpp"

#include "fixture_csv.hpp"

namespace {

using namespace ds::physics;

constexpr double kDeg = std::numbers::pi / 180.0;

// --- Frames ----------------------------------------------------------------------------------

TEST(Frames, NedConversionRoundTrips) {
    const Vec3 world{3.0, 5.0, -7.0};  // 3 m east, 5 m up, 7 m north.
    const Vec3 ned = worldToNed(world);
    EXPECT_DOUBLE_EQ(ned.x, 7.0);   // North.
    EXPECT_DOUBLE_EQ(ned.y, 3.0);   // East.
    EXPECT_DOUBLE_EQ(ned.z, -5.0);  // Down.
    const Vec3 back = nedToWorld(ned);
    EXPECT_DOUBLE_EQ(back.x, world.x);
    EXPECT_DOUBLE_EQ(back.y, world.y);
    EXPECT_DOUBLE_EQ(back.z, world.z);
}

TEST(Frames, EulerRoundTripAndAxes) {
    for (const Euler e : {Euler{0.1, -0.2, 0.3}, Euler{-0.7, 0.4, -2.5}, Euler{0.0, 0.0, 3.0}}) {
        const Euler r = eulerFromOrientation(orientationFromEuler(e));
        EXPECT_NEAR(r.roll, e.roll, 1e-12);
        EXPECT_NEAR(r.pitch, e.pitch, 1e-12);
        EXPECT_NEAR(r.yaw, e.yaw, 1e-12);
    }
    // Heading 90° (east): the body x axis (forward) points to world +x (east).
    const Quat east = orientationFromEuler({0.0, 0.0, 90.0 * kDeg});
    const Vec3 forward = bodyToWorld(east, {1.0, 0.0, 0.0});
    EXPECT_NEAR(forward.x, 1.0, 1e-12);
    EXPECT_NEAR(forward.z, 0.0, 1e-12);
    // Level: body z (down) points to world −y.
    const Vec3 down = bodyToWorld(orientationFromEuler({}), {0.0, 0.0, 1.0});
    EXPECT_NEAR(down.y, -1.0, 1e-12);
    // Nose up: forward gains height. Roll right: the right side (body +y) goes down.
    const Quat noseUp = orientationFromEuler({0.0, 10.0 * kDeg, 0.0});
    EXPECT_GT(bodyToWorld(noseUp, {1.0, 0.0, 0.0}).y, 0.0);
    const Quat rollRight = orientationFromEuler({10.0 * kDeg, 0.0, 0.0});
    EXPECT_LT(bodyToWorld(rollRight, {0.0, 1.0, 0.0}).y, 0.0);
    EXPECT_NEAR(tiltAngle(orientationFromEuler({30.0 * kDeg, 0.0, 1.0})), 30.0 * kDeg, 1e-12);
}

// --- Atmosphere ------------------------------------------------------------------------------

TEST(Atmosphere, IsaMatchesStandardTable) {
    const AirState sea = isaAir(0.0);
    EXPECT_NEAR(sea.temperatureK, 288.15, 1e-9);
    EXPECT_NEAR(sea.pressurePa, 101325.0, 1e-6);
    EXPECT_NEAR(sea.densityKgM3, 1.225, 1e-3);
    // ICAO standard atmosphere at 3 000 m: 268.65 K, 70 108 Pa, 0.9091 kg/m³.
    const AirState high = isaAir(3000.0);
    EXPECT_NEAR(high.temperatureK, 268.65, 1e-9);
    EXPECT_NEAR(high.pressurePa, 70108.5, 5.0);
    EXPECT_NEAR(high.densityKgM3, 0.9091, 5e-4);
    EXPECT_NEAR(pressureAltitudeM(isaAir(1234.0).pressurePa, 1013.25), 1234.0, 0.01);
}

TEST(Atmosphere, HumidityAndConditions) {
    // Buck: e_s(20 °C) = 2.3389 kPa.
    EXPECT_NEAR(saturationVapourPressurePa(20.0), 2338.9, 2.0);
    ds::weather::AirConditions dry{15.0, 1013.25, 0.0};
    ds::weather::AirConditions humid{15.0, 1013.25, 100.0};
    const AirState d = airAt(0.0, dry, 0.0);
    EXPECT_NEAR(d.densityKgM3, 1.225, 2e-3);
    EXPECT_NEAR(d.pressurePa, 101325.0, 1.0);
    EXPECT_LT(airAt(0.0, humid, 0.0).densityKgM3, d.densityKgM3);  // Moist air is lighter.
    // Hot day: lower density; higher QNH: higher pressure.
    EXPECT_LT(airAt(0.0, {35.0, 1013.25, 0.0}, 0.0).densityKgM3, d.densityKgM3);
    EXPECT_GT(airAt(0.0, {15.0, 1030.0, 0.0}, 0.0).pressurePa, d.pressurePa);
    // Temperature is given at the ground: 1 000 m above a 1 000 m site is 6.5 K colder.
    const AirState site = airAt(1000.0, dry, 1000.0);
    const AirState above = airAt(2000.0, dry, 1000.0);
    EXPECT_NEAR(site.temperatureK, 288.15, 1e-6);
    EXPECT_NEAR(site.temperatureK - above.temperatureK, 6.5, 1e-6);
    EXPECT_LT(above.pressurePa, site.pressurePa);
}

// --- Rigid body ------------------------------------------------------------------------------

TEST(RigidBody, FreeFallMatchesClosedForm) {
    // NFR-REAL-02: motors off, no drag: h = ½·g·t² within 0.1 % over 5 s.
    RigidBodyState s;
    s.position = Vec3(0.0, 1000.0, 0.0);
    MassProperties m;
    m.massKg = 0.25;
    const WrenchFunction none = [](const RigidBodyState&) { return Wrench{}; };
    const double dt = 1.0 / 500.0;
    for (int i = 0; i < 2500; ++i) {
        (void)integrateRk4(s, m, none, dt);
    }
    const double expected = 0.5 * kGravity * 25.0;
    EXPECT_NEAR(1000.0 - s.position.y, expected, expected * 1e-3);
    EXPECT_NEAR(s.velocity.y, -kGravity * 5.0, kGravity * 5.0 * 1e-3);
    EXPECT_TRUE(isFinite(s));
}

TEST(RigidBody, ConstantForceAndTorque) {
    MassProperties m;
    m.massKg = 2.0;
    m.inertia = glm::dmat3(1.0);
    m.inertia[0][0] = 0.01;
    m.inertia[1][1] = 0.02;
    m.inertia[2][2] = 0.03;
    RigidBodyState s;
    s.orientation = orientationFromEuler({});  // Level, heading north.
    const WrenchFunction push = [](const RigidBodyState&) {
        Wrench w;
        w.forceWorld = Vec3(4.0, 2.0 * kGravity, 0.0);  // Cancels gravity; 2 m/s² east.
        w.torqueBody = Vec3(0.0, 0.0, 0.003);           // 0.1 rad/s² about body z.
        return w;
    };
    for (int i = 0; i < 1000; ++i) {
        (void)integrateRk4(s, m, push, 0.001);
    }
    EXPECT_NEAR(s.position.x, 1.0, 1e-9);  // ½·2·1².
    EXPECT_NEAR(s.position.y, 0.0, 1e-9);
    EXPECT_NEAR(s.angularVelocity.z, 0.1, 1e-9);
    // Yaw right (FRD z down) turns the heading clockwise.
    EXPECT_NEAR(eulerFromOrientation(s.orientation).yaw, 0.05, 1e-6);
    EXPECT_NEAR(glm::length(s.orientation), 1.0, 1e-12);
}

TEST(RigidBody, TorqueFreeRotationConservesMomentumAndEnergy) {
    MassProperties m;
    m.inertia = glm::dmat3(1.0);
    m.inertia[0][0] = 0.01;
    m.inertia[1][1] = 0.015;
    m.inertia[2][2] = 0.022;
    RigidBodyState s;
    s.angularVelocity = Vec3(3.0, 0.5, -1.0);
    const auto momentumWorld = [&](const RigidBodyState& st) {
        return st.orientation * (m.inertia * st.angularVelocity);
    };
    const auto energy = [&](const RigidBodyState& st) {
        return 0.5 * glm::dot(st.angularVelocity, m.inertia * st.angularVelocity);
    };
    const Vec3 l0 = momentumWorld(s);
    const double e0 = energy(s);
    const WrenchFunction gravityFree = [&](const RigidBodyState&) {
        Wrench w;
        w.forceWorld = Vec3(0.0, kGravity * m.massKg, 0.0);
        return w;
    };
    for (int i = 0; i < 5000; ++i) {
        (void)integrateRk4(s, m, gravityFree, 0.002);
    }
    EXPECT_LT(glm::length(momentumWorld(s) - l0), 1e-5 * glm::length(l0));
    EXPECT_NEAR(energy(s), e0, 1e-5 * e0);
}

TEST(RigidBody, DetectsNonFiniteState) {
    RigidBodyState s;
    EXPECT_TRUE(isFinite(s));
    s.velocity.x = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(isFinite(s));
    s = {};
    s.orientation.w = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(isFinite(s));
}

// --- Powertrain ------------------------------------------------------------------------------

TEST(Powertrain, StaticThrustMatchesHandComputation) {
    PropellerParams prop;
    prop.diameterM = 0.127;
    prop.thrustCoefficient = 0.11;
    prop.torqueCoefficient = 0.0045;
    const double omega = 2000.0;  // rad/s.
    const double n = omega / (2.0 * std::numbers::pi);
    const RotorOutput out = propellerLoads(prop, omega, 1.225, 0.0);
    EXPECT_NEAR(out.thrustN, 0.11 * 1.225 * n * n * std::pow(0.127, 4), 1e-9);
    EXPECT_NEAR(out.torqueNm, 0.0045 * 1.225 * n * n * std::pow(0.127, 5), 1e-9);
    // Climbing reduces thrust (advance ratio), zero thrust at J0.
    EXPECT_LT(propellerLoads(prop, omega, 1.225, 5.0).thrustN, out.thrustN);
    const double vZero = prop.zeroThrustAdvanceRatio * n * prop.diameterM;
    EXPECT_NEAR(propCoefficients(prop, prop.zeroThrustAdvanceRatio).ct, 0.0, 1e-12);
    EXPECT_LE(propellerLoads(prop, omega, 1.225, vZero * 1.1).thrustN, 1e-9);
}

TEST(Powertrain, ThrustScalesWithDensityRatioAtAltitude) {
    // NFR-REAL-03: at 3 000 m (ISA) thrust drops by the density ratio within 1 %.
    PropellerParams prop;
    MotorParams motor;
    const double rho0 = isaAir(0.0).densityKgM3;
    const double rho3 = isaAir(3000.0).densityKgM3;
    const double omega = 2500.0;
    const double ratio = propellerLoads(prop, omega, rho3, 0.0).thrustN /
                         propellerLoads(prop, omega, rho0, 0.0).thrustN;
    EXPECT_NEAR(ratio, rho3 / rho0, 0.01 * rho3 / rho0);
    // At a fixed voltage the motor spins faster in thin air but thrust still drops.
    const double w0 = steadyStateOmega(motor, prop, 16.0, rho0, 0.0);
    const double w3 = steadyStateOmega(motor, prop, 16.0, rho3, 0.0);
    EXPECT_GT(w3, w0);
    EXPECT_LT(propellerLoads(prop, w3, rho3, 0.0).thrustN,
              propellerLoads(prop, w0, rho0, 0.0).thrustN);
}

TEST(Powertrain, SteadyStateSpeedBalancesMotorAndPropeller) {
    MotorParams motor;
    PropellerParams prop;
    const double voltage = 20.0;
    const double omega = steadyStateOmega(motor, prop, voltage, 1.225, 0.0);
    ASSERT_GT(omega, 0.0);
    EXPECT_LT(omega, voltage * kvRadPerVolt(motor));  // Below the no-load speed.
    // Electrical balance: V = ω/Kv + I·R with I from the propeller torque.
    const double torque = propellerLoads(prop, omega, 1.225, 0.0).torqueNm;
    const double current = motorCurrent(motor, omega, torque, 0.0);
    EXPECT_NEAR(omega / kvRadPerVolt(motor) + current * motor.resistanceOhm, voltage,
                0.01 * voltage);
    EXPECT_GT(current, motor.noLoadCurrentA);
    EXPECT_DOUBLE_EQ(steadyStateOmega(motor, prop, 0.0, 1.225, 0.0), 0.0);
    EXPECT_NEAR(kvRadPerVolt(motor), 1900.0 * 2.0 * std::numbers::pi / 60.0, 1e-9);
}

// --- Battery ---------------------------------------------------------------------------------

TEST(Battery, OpenCircuitCurvesAreMonotonic) {
    for (const Chemistry c : {Chemistry::LiPo, Chemistry::LiIon}) {
        double previous = 0.0;
        for (int i = 0; i <= 100; ++i) {
            const double v = openCircuitCellVoltage(c, i / 100.0);
            EXPECT_GE(v, previous) << chemistryName(c) << " soc " << i;
            previous = v;
        }
        EXPECT_NEAR(openCircuitCellVoltage(c, 1.0), 4.2, 0.05);
        EXPECT_LT(openCircuitCellVoltage(c, 0.0), 3.4);
    }
}

TEST(Battery, InstantSagIsCurrentTimesResistance) {
    BatteryParams p;
    Battery b(p, 25.0);
    const double r = b.packResistance();
    EXPECT_NEAR(r, p.cellResistanceOhm * p.cellsInSeries, 1e-12);
    EXPECT_NEAR(b.voltageAt(0.0) - b.voltageAt(20.0), 20.0 * r, 1e-9);
    EXPECT_NEAR(b.voltageAt(0.0), b.openCircuitVoltage(), 1e-12);
    // The polarisation (RC) branch makes the sag deepen under a sustained load.
    b.update(20.0, 25.0, 0.01);
    const double early = b.voltage();
    for (int i = 0; i < 3000; ++i) {
        b.update(20.0, 25.0, 0.01);
    }
    EXPECT_LT(b.voltage(), early - 0.5 * 20.0 * 0.5 * r);
}

TEST(Battery, CoulombCountingAndTemperature) {
    BatteryParams p;
    p.capacityAh = 2.0;
    Battery b(p, 25.0);
    for (int i = 0; i < 3600; ++i) {
        b.update(1.0, 25.0, 1.0);  // 1 A for one hour.
    }
    EXPECT_NEAR(b.consumedMah(), 1000.0, 1.0);
    EXPECT_NEAR(b.stateOfCharge(), 0.5, 0.01);
    EXPECT_GT(b.consumedWh(), 0.0);
    // Cold packs hold less charge and have higher resistance (NFR-REAL-11).
    EXPECT_LT(capacityFactor(Chemistry::LiPo, -10.0), 0.9);
    EXPECT_NEAR(capacityFactor(Chemistry::LiPo, 25.0), 1.0, 1e-9);
    EXPECT_GT(resistanceFactor(0.0), 1.5);
    EXPECT_NEAR(resistanceFactor(25.0), 1.0, 1e-9);
    EXPECT_LT(resistanceFactor(45.0), 1.0);
    // A high current heats the pack.
    Battery hot(BatteryParams{}, 20.0);
    for (int i = 0; i < 6000; ++i) {
        hot.update(60.0, 20.0, 0.01);
    }
    EXPECT_GT(hot.temperatureC(), 21.0);
}

// NFR-REAL-11: delivered capacity and loaded voltage in the cold follow the datasheet-type
// curves of tests/fixtures/battery/temperature_curves.csv (0.5 C to 3.0 V per cell).
TEST(Battery, ColdDischargeFollowsTheDatasheetCurves) {
    const ds::test::CsvTable table("battery/temperature_curves.csv");
    ASSERT_GE(table.size(), 10U);
    for (std::size_t row = 0; row < table.size(); ++row) {
        const bool lipo = table.text(row, "chemistry") == "lipo";
        const double ambient = table.number(row, "temperatureC");
        BatteryParams p;
        p.chemistry = lipo ? Chemistry::LiPo : Chemistry::LiIon;
        p.cellsInSeries = 1;
        // A 5 Ah pouch cell and a 2 Ah 18650 cell, as in the reference profiles.
        p.capacityAh = lipo ? 5.0 : 2.0;
        p.cellResistanceOhm = lipo ? 0.005 : 0.030;
        p.heatCapacityJPerK = lipo ? 100.0 : 45.0;
        p.coolingWPerK = lipo ? 0.5 : 0.25;
        Battery b(p, ambient);
        const double current = 0.5 * p.capacityAh;
        double midVoltage = 0.0;
        for (int s = 0; s < 4 * 3600 && !b.depleted(); ++s) {
            b.update(current, ambient, 1.0);
            if (midVoltage == 0.0 && b.consumedMah() >= 500.0 * p.capacityAh) {
                midVoltage = b.voltage();
            }
        }
        const double delivered = b.consumedMah() / (1000.0 * p.capacityAh);
        const double expected = table.number(row, "capacityFraction");
        const std::string where =
            table.text(row, "chemistry") + " at " + table.text(row, "temperatureC") + " °C";
        EXPECT_NEAR(delivered, expected, 0.10 * expected) << where;
        // The voltage must be within 10 % (NFR-REAL-11); 3 % is checked so that the sag in the
        // cold actually follows the curve.
        EXPECT_NEAR(midVoltage, table.number(row, "midVoltageV"),
                    0.03 * table.number(row, "midVoltageV"))
            << where;
    }
}

TEST(Battery, WarningsAreHeldThenLatchedAndDepletionStopsOutput) {
    BatteryParams p;
    p.cellsInSeries = 4;
    p.capacityAh = 0.1;
    Battery b(p, 25.0);
    EXPECT_EQ(b.warning(), BatteryWarning::None);
    // A short spike below the low threshold does not warn.
    b.update(80.0, 25.0, 0.1);
    b.update(0.0, 25.0, 0.1);
    EXPECT_EQ(b.warning(), BatteryWarning::None);
    bool sawLow = false;
    bool sawCritical = false;
    for (int i = 0; i < 100000 && !b.depleted(); ++i) {
        b.update(2.0, 25.0, 0.1);
        sawLow = sawLow || b.warning() == BatteryWarning::Low;
        sawCritical = sawCritical || b.warning() == BatteryWarning::Critical;
    }
    EXPECT_TRUE(sawLow);
    EXPECT_TRUE(sawCritical);
    ASSERT_TRUE(b.depleted());
    EXPECT_DOUBLE_EQ(b.voltage(), 0.0);
    EXPECT_DOUBLE_EQ(b.voltageAt(1.0), 0.0);
    b.reset(25.0);
    EXPECT_EQ(b.warning(), BatteryWarning::None);
    EXPECT_NEAR(b.stateOfCharge(), 1.0, 1e-12);
}

// --- Aerodynamics ----------------------------------------------------------------------------

TEST(Aerodynamics, GroundEffect) {
    const double radius = 0.0635;
    EXPECT_NEAR(groundEffectGain(10.0, radius), 1.0, 1e-4);
    EXPECT_NEAR(groundEffectGain(2.0 * radius, radius), 1.0 / (1.0 - 1.0 / 64.0), 1e-12);
    double previous = 1.5;
    for (double h = 0.0; h < 1.0; h += 0.01) {
        const double g = groundEffectGain(h, radius);
        EXPECT_LE(g, 1.4);
        EXPECT_GE(g, 1.0);
        EXPECT_LE(g, previous);
        previous = g;
    }
}

TEST(Aerodynamics, InducedVelocityVortexRingAndDrag) {
    // Momentum theory: v_i = sqrt(T / (2 ρ A)).
    EXPECT_NEAR(hoverInducedVelocity(2.0, 1.225, 0.01), std::sqrt(2.0 / (2.0 * 1.225 * 0.01)),
                1e-12);
    EXPECT_DOUBLE_EQ(vortexRingFactor(0.0, 5.0), 1.0);
    EXPECT_DOUBLE_EQ(vortexRingFactor(-3.0, 5.0), 1.0);    // Climbing.
    EXPECT_NEAR(vortexRingFactor(5.0, 5.0), 0.75, 1e-12);  // Worst at v_d ≈ v_i.
    EXPECT_DOUBLE_EQ(vortexRingFactor(20.0, 5.0), 1.0);    // Windmill state.

    PropellerParams prop;
    const Vec3 h = rotorDrag(prop, 5.0, 2000.0, Vec3(10.0, 0.0, 0.0));
    EXPECT_LT(h.x, 0.0);  // Opposes the in-plane air velocity.
    EXPECT_NEAR(h.y, 0.0, 1e-12);
    EXPECT_NEAR(glm::length(rotorDrag(prop, 5.0, 2000.0, Vec3(0.0, 0.0, 3.0))), 0.0, 1e-12);

    FrameParams frame;
    const Vec3 d1 = bodyDrag(frame, Vec3(10.0, 0.0, 0.0), 1.225);
    const Vec3 d2 = bodyDrag(frame, Vec3(20.0, 0.0, 0.0), 1.225);
    EXPECT_LT(d1.x, 0.0);
    EXPECT_NEAR(d2.x / d1.x, 4.0, 1e-9);  // Quadratic.
}

}  // namespace
