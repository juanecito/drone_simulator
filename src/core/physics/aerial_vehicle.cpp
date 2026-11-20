// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/aerial_vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "ds/common/math.hpp"
#include "ds/physics/aerodynamics.hpp"

namespace ds::physics {
namespace {

constexpr double kContactOmega = 120.0;      // Natural frequency of the ground contact, rad/s.
constexpr double kContactDamping = 0.7;      // Damping ratio.
constexpr double kFrictionSmoothing = 0.05;  // m/s: regularised Coulomb friction.
constexpr double kCollisionEventSpeed = 1.0;
constexpr double kPropStrikeSpeedFraction = 0.3;
constexpr double kVrsTimeConstantS = 0.3;
constexpr double kSleepSpeed = 0.05;

}  // namespace

std::string_view vehicleTypeName(VehicleType type) noexcept {
    return type == VehicleType::Aerial ? "aerial" : "ground";
}

AerialVehicle::AerialVehicle(const AerialProfile& profile, std::uint64_t seed)
    : profile_(profile),
      seed_(seed),
      sensors_(profile.sensors, seed),
      controller_(profile),
      battery_(profile.battery) {
    // Collision shape: four feet, four propeller tips (outboard of each motor) and the top.
    const auto motors = profile_.motorPositions();
    const double tip = 0.5 * profile_.propeller.diameterM;
    for (const Vec3& m : motors) {
        points_.push_back({Vec3(m.x, m.y, profile_.frame.gearHeightM), true});
    }
    for (const Vec3& m : motors) {
        const Vec3 radial = glm::normalize(Vec3(m.x, m.y, 0.0));
        points_.push_back({Vec3(m.x + radial.x * tip, m.y + radial.y * tip, m.z), false});
    }
    points_.push_back({Vec3(0.0, 0.0, -profile_.frame.propHeightM - 0.03), false});
    inContact_.assign(points_.size(), false);
    preStepNormalSpeed_.assign(points_.size(), 0.0);

    // Motor speed at full command on a full pack (vibration scale).
    const double fullVoltage =
        profile_.battery.cellsInSeries * openCircuitCellVoltage(profile_.battery.chemistry, 1.0);
    maxOmega_ = std::max(
        1.0, steadyStateOmega(profile_.motor, profile_.propeller, fullVoltage, 1.225, 0.0));
    updateMassProperties();
}

void AerialVehicle::updateMassProperties() {
    const FrameParams& f = profile_.frame;
    const double m0 = f.massKg + waterInMassKg_;  // Water spread over the frame.
    const double mp = payloadMassKg_;
    mass_.massKg = m0 + mp;
    const Vec3 c = mp > 0.0 ? payloadOffset_ * (mp / mass_.massKg) : Vec3(0.0);
    mass_.centreOfMassOffset = c;
    glm::dmat3 inertia(0.0);
    inertia[0][0] = f.inertiaXxKgM2;
    inertia[1][1] = f.inertiaYyKgM2;
    inertia[2][2] = f.inertiaZzKgM2;
    // Parallel-axis theorem for the frame (CoG moved by −c) and the point-mass payload.
    auto pointInertia = [](double m, const Vec3& r) {
        glm::dmat3 i(m * glm::dot(r, r));
        return i - m * glm::outerProduct(r, r);
    };
    inertia += pointInertia(m0, -c);
    if (mp > 0.0) {
        inertia += pointInertia(mp, payloadOffset_ - c);
    }
    mass_.inertia = inertia;
}

void AerialVehicle::attachPayload(double massKg, const Vec3& offsetBody, double dragAreaM2) {
    const Vec3 oldC = mass_.centreOfMassOffset;
    payloadMassKg_ = std::max(0.0, massKg);
    payloadOffset_ = offsetBody;
    payloadDragArea_ = std::max(0.0, dragAreaM2);
    updateMassProperties();
    // Keep the frame where it is: the tracked point (CoM) moves with the new CoG.
    body_.position += body_.orientation * (mass_.centreOfMassOffset - oldC);
}

void AerialVehicle::releasePayload() { attachPayload(0.0, Vec3(0.0), 0.0); }

namespace {

// Rain on the airframe (FR-WX-06): a film of about 0.1 mm over the wetted surface (taken as
// twice the sum of the drag areas), thinner in light rain; it builds up in about a minute and
// is blown off in about five once the rain stops.
constexpr double kFilmThicknessM = 1e-4;
constexpr double kWaterDensity = 1000.0;
constexpr double kWettingTimeS = 60.0;
constexpr double kDryingTimeS = 300.0;
constexpr double kFilmReferenceRateMmH = 4.0;

bool liquidPrecipitation(const weather::Precipitation& p) noexcept {
    return p.rateMmH > 0.0 && (p.type == weather::PrecipitationType::Drizzle ||
                               p.type == weather::PrecipitationType::Rain ||
                               p.type == weather::PrecipitationType::Sleet);
}

}  // namespace

Vec3 AerialVehicle::precipitationForce(const RigidBodyState& s,
                                       const EnvironmentSample& env) const noexcept {
    const weather::Precipitation& p = env.precipitation;
    const double fall = weather::fallSpeedMs(p);
    if (p.rateMmH <= 0.0 || fall <= 0.0) {
        return Vec3(0.0);
    }
    const double content = p.rateMmH / 3600.0 / fall;  // kg of water per m³ of air.
    const Vec3 drops = env.wind + Vec3(0.0, -fall, 0.0);
    const Vec3 relative = drops - s.velocity;
    const double speed = glm::length(relative);
    if (speed < 1e-9) {
        return Vec3(0.0);
    }
    const Vec3 dir = worldToBody(s.orientation, relative / speed);
    const FrameParams& f = profile_.frame;
    const double area = std::abs(dir.x) * f.dragAreaForwardM2 + std::abs(dir.y) * f.dragAreaSideM2 +
                        std::abs(dir.z) * f.dragAreaVerticalM2;
    return content * speed * area * relative;
}

double AerialVehicle::restHeightM() const noexcept {
    return profile_.frame.gearHeightM - mass_.centreOfMassOffset.z;
}

void AerialVehicle::pushEvent(VehicleEvent::Kind kind, double value, std::string text) {
    events_.push_back({kind, value, std::move(text)});
}

void AerialVehicle::crash(double speed, std::string reason) {
    if (crashed_) {
        return;
    }
    crashed_ = true;
    controller_.forceDisarm();
    commands_.fill(0.0);
    pushEvent(VehicleEvent::Kind::Crash, speed, std::move(reason));
}

std::vector<VehicleEvent> AerialVehicle::takeEvents() { return std::exchange(events_, {}); }

void AerialVehicle::reset(const Vec3& position, double headingRad, const EnvironmentSample& env) {
    body_ = {};
    body_.orientation = orientationFromEuler({0.0, 0.0, headingRad});
    const double ny = std::max(env.ground.normal.y, 0.3);
    body_.position = Vec3(position.x, env.groundPoint.y + restHeightM() / ny + 0.002, position.z);
    omega_.fill(0.0);
    omegaDot_.fill(0.0);
    commands_.fill(0.0);
    propAngle_.fill(0.0);
    loads_ = {};
    battery_.reset(env.ambientC);
    sensors_.reset();
    waterMassKg_ = 0.0;
    if (waterInMassKg_ != 0.0) {
        waterInMassKg_ = 0.0;
        updateMassProperties();
    }
    estimator_.reset(headingRad, pressureAltitudeM(env.air.pressurePa, env.qnhHpa));
    controller_.reset();
    random_ = common::GaussianRandom(common::mix64(seed_ ^ 0x56454849434C45ULL));  // "VEHICLE"
    crashed_ = false;
    inWater_ = false;
    onGround_ = true;
    wasArmed_ = false;
    wasFailsafe_ = false;
    lastBlock_ = ArmBlock::None;
    lastWarning_ = BatteryWarning::None;
    vrsFluctuation_ = 0.0;
    throttle_ = 0.0;
    events_.clear();
    for (std::size_t i = 0; i < points_.size(); ++i) {
        inContact_[i] = points_[i].gear;
        preStepNormalSpeed_[i] = 0.0;
    }
    specificForce_ = Vec3(0.0, 0.0, -kGravity);
    lastWind_ = env.wind;
}

void AerialVehicle::updateSensors(const EnvironmentSample& env, double dt) {
    SensorTruth t;
    t.body = body_;
    t.specificForceBody = specificForce_;
    t.staticPressurePa = env.air.pressurePa;
    t.qnhHpa = env.qnhHpa;
    t.heightAboveGroundM =
        glm::dot(env.ground.normal, body_.position - env.groundPoint) - restHeightM();
    const double meanOmega =
        std::accumulate(omega_.begin(), omega_.end(), 0.0) / static_cast<double>(omega_.size());
    t.motorSpeedFraction = std::clamp(meanOmega / maxOmega_, 0.0, 1.0);
    t.motorFrequencyHz = meanOmega / common::kTwoPi;
    const bool stationary =
        !controller_.armed() && onGround_ && glm::length(body_.angularVelocity) < 0.01;
    estimator_.update(sensors_.update(t, dt), dt, stationary);
}

void AerialVehicle::updateController(const PilotCommand& command, double dt) {
    throttle_ = command.throttle;
    const bool armInputBefore = controller_.armed();
    VehicleConditions conditions;
    conditions.batteryCritical =
        battery_.warning() == BatteryWarning::Critical || battery_.depleted();
    conditions.crashed = crashed_;
    conditions.landed = onGround_;
    const ControllerOutput out = controller_.update(command, estimator_.state(), conditions, dt);
    commands_ = out.motors;
    if (battery_.depleted()) {
        controller_.forceDisarm();
        commands_.fill(0.0);
    }
    if (motorOverride_ && (controller_.armed() || overrideDisarmed_)) {
        commands_ = *motorOverride_;
    }

    if (controller_.armed() != wasArmed_) {
        pushEvent(controller_.armed() ? VehicleEvent::Kind::Armed : VehicleEvent::Kind::Disarmed);
        wasArmed_ = controller_.armed();
    }
    if (!armInputBefore && !controller_.armed() && controller_.armBlock() != ArmBlock::None &&
        controller_.armBlock() != lastBlock_) {
        pushEvent(VehicleEvent::Kind::ArmRefused, 0.0,
                  std::string(armBlockText(controller_.armBlock())));
    }
    lastBlock_ = controller_.armed() ? ArmBlock::None : controller_.armBlock();
    if (!command.arm) {
        lastBlock_ = ArmBlock::None;  // Report the same reason again on the next attempt.
    }
    if (controller_.failsafe() != wasFailsafe_) {
        pushEvent(controller_.failsafe() ? VehicleEvent::Kind::FailsafeOn
                                         : VehicleEvent::Kind::FailsafeOff);
        wasFailsafe_ = controller_.failsafe();
    }
}

Wrench AerialVehicle::computeWrench(const RigidBodyState& s, const EnvironmentSample& env,
                                    RotorLoads* loads) const {
    Wrench w;
    if (gravityOnly_) {
        return w;
    }
    const double m = mass_.massKg;
    const Vec3 c = mass_.centreOfMassOffset;
    const double rho = env.air.densityKgM3;
    const Vec3 airBody = worldToBody(s.orientation, s.velocity - env.wind);
    const auto motors = profile_.motorPositions();
    const auto spins = AerialProfile::spinDirections();
    const PropellerParams& prop = profile_.propeller;
    const double radius = 0.5 * prop.diameterM;
    const double area = common::kPi * radius * radius;
    const Vec3& n = env.ground.normal;
    const Vec3 thrustDirWorld = s.orientation * Vec3(0.0, 0.0, -1.0);
    const double facingGround = std::max(0.0, glm::dot(thrustDirWorld, n));

    Vec3 forceBody(0.0);
    Vec3 torque(0.0);
    double spinMomentum = 0.0;
    for (std::size_t i = 0; i < motors.size(); ++i) {
        const Vec3 r = motors[i] - c;
        const Vec3 airRotor = airBody + glm::cross(s.angularVelocity, r);
        const double axial = -airRotor.z;  // Air entering the disc from above (climbing) > 0.
        const RotorOutput out = propellerLoads(prop, omega_[i], rho, axial);
        double thrust = out.thrustN;
        // Ground effect (only when the rotor faces the ground).
        const double height = glm::dot(n, s.position + s.orientation * r - env.groundPoint);
        thrust *= 1.0 + (groundEffectGain(height, radius) - 1.0) * facingGround;
        // Vortex-ring state in fast vertical descent, with seeded fluctuations.
        const double vrs = vortexRingFactor(-axial, hoverInducedVelocity(thrust, rho, area));
        thrust *= std::max(0.0, vrs - (1.0 - vrs) * vrsFluctuation_);

        forceBody += Vec3(0.0, 0.0, -thrust) + rotorDrag(prop, thrust, omega_[i], airRotor);
        torque += glm::cross(r, Vec3(0.0, 0.0, -thrust));
        // Reaction of the propeller drag and of the rotor acceleration (yaw).
        torque.z -= spins[i] * (out.torqueNm + profile_.motor.rotorInertiaKgM2 * omegaDot_[i]);
        spinMomentum += spins[i] * profile_.motor.rotorInertiaKgM2 * omega_[i];
        if (loads != nullptr) {
            loads->thrust[i] = thrust;
            loads->torque[i] = out.torqueNm;
        }
    }
    // Gyroscopic torque of the spinning rotors (angular momentum along body z).
    torque -= glm::cross(s.angularVelocity, Vec3(0.0, 0.0, spinMomentum));
    FrameParams drag = profile_.frame;
    drag.dragAreaForwardM2 += payloadDragArea_;
    drag.dragAreaSideM2 += payloadDragArea_;
    drag.dragAreaVerticalM2 += payloadDragArea_;
    forceBody += bodyDrag(drag, airBody, rho);
    w.forceWorld = s.orientation * forceBody + precipitationForce(s, env);
    w.torqueBody = torque;

    // Ground contact: penalty spring–damper with regularised Coulomb friction (FR-PHY-09).
    const double k = m * kContactOmega * kContactOmega / 4.0;
    const double damping = 2.0 * kContactDamping * m * kContactOmega / 4.0;
    for (const CollisionPoint& point : points_) {
        const Vec3 r = point.body - c;
        const Vec3 p = s.position + s.orientation * r;
        const double depth = -glm::dot(n, p - env.groundPoint);
        if (depth <= 0.0) {
            continue;
        }
        const Vec3 vp = s.velocity + s.orientation * glm::cross(s.angularVelocity, r);
        const double vn = glm::dot(vp, n);
        const double fn = std::max(0.0, k * depth - damping * vn);
        const Vec3 vt = vp - vn * n;
        const Vec3 ft = -env.ground.friction * fn * vt /
                        std::sqrt(glm::dot(vt, vt) + kFrictionSmoothing * kFrictionSmoothing);
        const Vec3 f = fn * n + ft;
        w.forceWorld += f;
        w.torqueBody += glm::cross(r, worldToBody(s.orientation, f));
    }

    // Water: buoyancy and heavy damping once the vehicle is in it.
    if (env.waterLevelM && s.position.y < *env.waterLevelM + 0.05) {
        const double depth = std::clamp((*env.waterLevelM + 0.05 - s.position.y) / 0.1, 0.0, 1.0);
        w.forceWorld += Vec3(0.0, 1.2 * m * kGravity * depth, 0.0) - 8.0 * m * s.velocity;
        w.torqueBody -= 0.05 * m * s.angularVelocity;
    }
    return w;
}

void AerialVehicle::integrate(const EnvironmentSample& env, double dt) {
    lastWind_ = env.wind;
    {
        const FrameParams& f = profile_.frame;
        const bool raining = liquidPrecipitation(env.precipitation);
        const double wetted = 2.0 * (f.dragAreaForwardM2 + f.dragAreaSideM2 + f.dragAreaVerticalM2);
        const double film =
            raining
                ? kWaterDensity * kFilmThicknessM * wetted *
                      std::min(1.0, std::sqrt(env.precipitation.rateMmH / kFilmReferenceRateMmH))
                : 0.0;
        const double tau = raining && film > waterMassKg_ ? kWettingTimeS : kDryingTimeS;
        waterMassKg_ += (film - waterMassKg_) * (1.0 - std::exp(-dt / tau));
        if (std::abs(waterMassKg_ - waterInMassKg_) > 1e-4) {
            waterInMassKg_ = waterMassKg_;
            updateMassProperties();
        }
    }
    const Vec3 c = mass_.centreOfMassOffset;
    const Vec3& n = env.ground.normal;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const Vec3 r = points_[i].body - c;
        const Vec3 vp = body_.velocity + body_.orientation * glm::cross(body_.angularVelocity, r);
        preStepNormalSpeed_[i] = glm::dot(vp, n);
    }

    // Motors: first-order response towards the steady-state speed at the applied voltage.
    const double vbatt = battery_.depleted() ? 0.0 : battery_.voltage();
    const Vec3 airBody = worldToBody(body_.orientation, body_.velocity - env.wind);
    const auto motors = profile_.motorPositions();
    const double decay = std::exp(-dt / profile_.motor.timeConstantS);
    for (std::size_t i = 0; i < omega_.size(); ++i) {
        const Vec3 airRotor = airBody + glm::cross(body_.angularVelocity, motors[i] - c);
        const double target =
            steadyStateOmega(profile_.motor, profile_.propeller, commands_[i] * vbatt,
                             env.air.densityKgM3, -airRotor.z);
        const double previous = omega_[i];
        omega_[i] = target + (omega_[i] - target) * decay;
        omegaDot_[i] = (omega_[i] - previous) / dt;
        propAngle_[i] = std::fmod(propAngle_[i] + omega_[i] * dt, common::kTwoPi);
    }
    {
        const double a = std::exp(-dt / kVrsTimeConstantS);
        vrsFluctuation_ = a * vrsFluctuation_ + std::sqrt(1.0 - a * a) * random_.next(0.6);
    }

    // Rigid body (RK4); the first evaluation also records the rotor loads.
    const RigidBodyState before = body_;
    bool first = true;
    const Wrench w1 = integrateRk4(
        body_, mass_,
        [&](const RigidBodyState& s) {
            if (first) {
                first = false;
                return computeWrench(s, env, &loads_);
            }
            return computeWrench(s, env, nullptr);
        },
        dt);
    specificForce_ = worldToBody(before.orientation, w1.forceWorld / mass_.massKg);

    // Numerical safety (FR-PHY-17).
    if (!isFinite(body_)) {
        body_ = before;
        if (!isFinite(body_)) {
            body_ = {};
            body_.position =
                Vec3(env.groundPoint.x, env.groundPoint.y + restHeightM(), env.groundPoint.z);
        }
        body_.velocity = Vec3(0.0);
        body_.angularVelocity = Vec3(0.0);
        omega_.fill(0.0);
        controller_.forceDisarm();
        pushEvent(VehicleEvent::Kind::NumericalReset, 0.0, "invalid numbers in the vehicle state");
    }

    // Battery: motor currents through the ESCs plus the avionics. Each motor draws what its
    // load demands, but at most what the winding resistance lets through, (d·V − back-EMF)/R;
    // the terminal voltage V depends on that current, so V is solved for (bisection on a
    // monotonic function) instead of using the previous tick's value, which would oscillate.
    double current = 0.0;
    if (!battery_.depleted()) {
        std::array<double, AerialProfile::kMotorCount> demand{};
        std::array<double, AerialProfile::kMotorCount> backEmf{};
        for (std::size_t i = 0; i < omega_.size(); ++i) {
            demand[i] = motorCurrent(profile_.motor, omega_[i], loads_.torque[i], omegaDot_[i]);
            backEmf[i] = omega_[i] / kvRadPerVolt(profile_.motor);
        }
        const auto drawAt = [&](double volts) {
            double total = profile_.electrical.avionicsCurrentA;
            for (std::size_t i = 0; i < omega_.size(); ++i) {
                const double limit = std::max(0.0, (commands_[i] * volts - backEmf[i]) /
                                                       profile_.motor.resistanceOhm) +
                                     profile_.motor.noLoadCurrentA;
                total +=
                    commands_[i] * std::min(demand[i], limit) / profile_.electrical.escEfficiency;
            }
            return total;
        };
        double lo = 0.0;
        double hi = battery_.voltageAt(0.0);
        if (hi - battery_.voltageAt(drawAt(hi)) <= 0.0) {
            lo = hi;  // Light load: no limit active at the open-circuit voltage.
        }
        for (int k = 0; k < 40 && hi - lo > 1e-4; ++k) {
            const double mid = 0.5 * (lo + hi);
            (mid > battery_.voltageAt(drawAt(mid)) ? hi : lo) = mid;
        }
        current = drawAt(0.5 * (lo + hi));
    }
    battery_.update(current, env.ambientC, dt);
    if (battery_.warning() != lastWarning_) {
        switch (battery_.warning()) {
            case BatteryWarning::Low: pushEvent(VehicleEvent::Kind::BatteryLow); break;
            case BatteryWarning::Critical: pushEvent(VehicleEvent::Kind::BatteryCritical); break;
            case BatteryWarning::Depleted: pushEvent(VehicleEvent::Kind::BatteryDepleted); break;
            case BatteryWarning::None: break;
        }
        lastWarning_ = battery_.warning();
    }

    // Resting on the ground while disarmed: stop the residual creep of the penalty contact.
    if (!controller_.armed() && onGround_ && glm::length(body_.velocity) < kSleepSpeed &&
        glm::length(body_.angularVelocity) < 2.0 * kSleepSpeed) {
        body_.velocity = Vec3(0.0);
        body_.angularVelocity = Vec3(0.0);
    }
}

void AerialVehicle::resolveCollisions(const EnvironmentSample& env, double /*dt*/) {
    const Vec3 c = mass_.centreOfMassOffset;
    const Vec3& n = env.ground.normal;
    const double meanOmega =
        std::accumulate(omega_.begin(), omega_.end(), 0.0) / static_cast<double>(omega_.size());
    const bool spinning = meanOmega > kPropStrikeSpeedFraction * maxOmega_;
    bool gearContact = false;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const Vec3 p = body_.position + body_.orientation * (points_[i].body - c);
        const bool contact = glm::dot(n, p - env.groundPoint) < 0.0;
        if (contact && !inContact_[i]) {
            const double impact = std::max(0.0, -preStepNormalSpeed_[i]);
            if (impact > kCollisionEventSpeed) {
                pushEvent(VehicleEvent::Kind::Collision, impact);
            }
            if (points_[i].gear) {
                if (impact > profile_.frame.crashSpeedMs) {
                    crash(impact, "hard landing");
                }
            } else if (impact > 0.5 * profile_.frame.crashSpeedMs) {
                crash(impact, "impact with the ground");
            } else if (spinning) {
                crash(impact, "propeller strike");
            }
        }
        inContact_[i] = contact;
        gearContact = gearContact || (contact && points_[i].gear);
    }
    onGround_ = gearContact;
    if (env.waterLevelM && body_.position.y < *env.waterLevelM && !inWater_) {
        inWater_ = true;
        pushEvent(VehicleEvent::Kind::WaterEntered);
        crash(glm::length(body_.velocity), "fell into water");
    }
}

void AerialVehicle::resolveObstacles(const ObstacleProbe& probe) {
    if (!probe) {
        return;
    }
    // The deepest contact of the collision points, resolved for the whole body.
    const Vec3 c = mass_.centreOfMassOffset;
    std::optional<ObstacleContact> deepest;
    bool gear = false;
    for (const CollisionPoint& point : points_) {
        const Vec3 p = body_.position + body_.orientation * (point.body - c);
        if (auto contact = probe(p); contact && (!deepest || contact->depthM > deepest->depthM)) {
            deepest = std::move(contact);
            gear = point.gear;
        }
    }
    if (!deepest) {
        return;
    }
    const Vec3& n = deepest->normal;
    body_.position += n * deepest->depthM;
    const double normalSpeed = glm::dot(body_.velocity, n);
    if (normalSpeed < 0.0) {
        // Impulse against an obstacle that stays put: a light one (a person) takes its share
        // of the momentum, M / (m + M).
        const double m = mass_.massKg;
        const double obstacleMass = deepest->massKg;
        const double share = std::isfinite(obstacleMass) ? obstacleMass / (m + obstacleMass) : 1.0;
        const double restitution = std::clamp(deepest->restitution, 0.0, 1.0);
        const double dvNormal = -(1.0 + restitution) * normalSpeed * share;
        body_.velocity += dvNormal * n;
        // Coulomb friction: the sliding speed drops by at most μ times the normal change.
        const Vec3 tangential = body_.velocity - glm::dot(body_.velocity, n) * n;
        const double slide = glm::length(tangential);
        if (slide > 1e-9 && deepest->friction > 0.0) {
            body_.velocity -= tangential * (std::min(slide, deepest->friction * dvNormal) / slide);
        }
        const double impact = -normalSpeed;
        const double meanOmega =
            std::accumulate(omega_.begin(), omega_.end(), 0.0) / static_cast<double>(omega_.size());
        const bool spinning = meanOmega > kPropStrikeSpeedFraction * maxOmega_;
        if (impact > kCollisionEventSpeed) {
            pushEvent(VehicleEvent::Kind::Collision, impact);
        }
        if (!crashed_) {
            if (impact > (gear ? 1.0 : 0.5) * profile_.frame.crashSpeedMs) {
                crash(impact, "collision with " + deepest->name);
            } else if (!gear && spinning && impact > kCollisionEventSpeed) {
                crash(impact, "propeller strike on " + deepest->name);
            }
        }
    }
    // Standing on top of something (a roof) counts as being on the ground.
    if (gear && n.y > 0.7) {
        onGround_ = true;
    }
}

VehicleTelemetry AerialVehicle::telemetry() const {
    VehicleTelemetry t;
    t.type = VehicleType::Aerial;
    t.armed = controller_.armed();
    t.crashed = crashed_;
    t.onGround = onGround_;
    t.failsafe = controller_.failsafe();
    t.mode = controller_.mode();
    t.rthStage = controller_.rthStage();
    t.gpsModeUnavailable = controller_.gpsModeUnavailable();
    t.armBlock = controller_.armBlock();
    t.throttle = throttle_;
    double sum = 0.0;
    for (std::size_t i = 0; i < omega_.size(); ++i) {
        t.motorCommand[i] = commands_[i];
        t.motorRpm[i] = omega_[i] * 60.0 / common::kTwoPi;
        t.propAngleRad[i] = propAngle_[i];
        t.totalThrustN += loads_.thrust[i];
        sum += omega_[i];
    }
    t.motorSpeedFraction = sum / static_cast<double>(omega_.size()) / maxOmega_;
    t.battery.voltage = battery_.voltage();
    t.battery.current = battery_.current();
    t.battery.stateOfCharge = battery_.stateOfCharge();
    t.battery.temperatureC = battery_.temperatureC();
    t.battery.consumedMah = battery_.consumedMah();
    t.battery.consumedWh = battery_.consumedWh();
    t.battery.cells = profile_.battery.cellsInSeries;
    t.battery.warning = battery_.warning();
    t.estimate = estimator_.state();
    t.gps = sensors_.readings().gps;
    t.wind = lastWind_;
    t.airspeedMs = glm::length(body_.velocity - lastWind_);
    t.armedTimeS = controller_.armedTime();
    return t;
}

}  // namespace ds::physics
