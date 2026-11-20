// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/ground_vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ds/common/math.hpp"
#include "ds/physics/atmosphere.hpp"

namespace ds::physics {
namespace {

// Tyres.
constexpr double kSlipReferenceMs = 0.2;     // Slip denominator floor: tyre relaxation at a crawl.
constexpr double kRollingSmoothing = 0.5;    // rad/s: regularised sign of the rolling resistance.
constexpr double kBrakeSmoothing = 1.0;      // rad/s: regularised holding brake.
constexpr double kFrictionSmoothing = 10.0;  // rad/s at the motor: gearbox friction.
// Suspension bump stop (relative to the spring) and the minimum strut angle to the ground.
constexpr double kBumpStopFactor = 20.0;
constexpr double kMinStrutCosine = 0.25;
// Body contacts with the terrain (penalty method, as for the aerial drone).
constexpr double kContactOmega = 120.0;
constexpr double kContactDamping = 0.7;
constexpr double kBodySlideSmoothing = 0.05;  // m/s.
constexpr double kBodyFriction = 0.5;         // Sliding body / surface peak friction.
// Integration.
constexpr double kMaxSubstepS = 0.001;
constexpr int kMaxSubsteps = 32;
// Events.
constexpr double kCollisionEventSpeed = 1.0;
constexpr double kRolloverTiltDeg = 70.0;
constexpr double kRolloverHoldS = 0.5;
constexpr double kStuckProgressM = 0.3;
constexpr double kStuckDrive = 0.25;
// Static hold on the brake.
constexpr double kHoldSpeed = 0.03;        // m/s: enter below this.
constexpr double kHoldStiffness = 3000.0;  // N/m per tyre, tangential.
constexpr double kHoldDamping = 60.0;      // N·s/m per tyre.
constexpr double kOdometryTauS = 0.05;

double kvRad(const DrivetrainParams& d) { return d.motorKv * common::kTwoPi / 60.0; }

double rotatingInertia(const GroundProfile& p) {
    return p.wheels.inertiaKgM2 +
           p.drivetrain.motorInertiaKgM2 * p.drivetrain.gearRatio * p.drivetrain.gearRatio;
}

}  // namespace

GroundVehicle::GroundVehicle(const GroundProfile& profile, std::uint64_t seed)
    : profile_(profile),
      wheels_(profile.wheelCount()),
      sensors_(profile.sensors, seed),
      controller_(profile.controller),
      battery_(profile.battery) {
    const ChassisParams& c = profile_.chassis;
    const double restExtension = profile_.suspension.travelM - profile_.staticCompressionM();
    for (const Vec3& hub : profile_.wheelHubsAtRest()) {
        mounts_.push_back(hub - Vec3(0.0, 0.0, restExtension));
    }
    // Collision shape: the body box (underside sampled against the terrain) and, for
    // obstacles, the front, back, outer side and top of each wheel.
    const double bottom = c.cogHeightM - c.clearanceM;
    const double top = bottom - c.heightM;
    const double hl = 0.5 * c.lengthM;
    const double hw = 0.5 * c.widthM;
    for (const double x : {hl, -hl}) {
        for (const double y : {-hw, hw}) {
            sampledPoints_.push_back(static_cast<int>(points_.size()));
            points_.push_back({Vec3(x, y, bottom), false});
        }
    }
    sampledPoints_.push_back(static_cast<int>(points_.size()));
    points_.push_back({Vec3(0.0, 0.0, bottom), false});
    for (const double x : {hl, -hl}) {
        for (const double y : {-hw, hw}) {
            points_.push_back({Vec3(x, y, top), false});
        }
    }
    const double r = profile_.wheels.radiusM;
    const auto hubs = profile_.wheelHubsAtRest();
    for (std::size_t i = 0; i < hubs.size(); ++i) {
        const Vec3& h = hubs[i];
        const double side = h.y < 0.0 ? -1.0 : 1.0;
        points_.push_back({h + Vec3(r, 0.0, 0.0), true});
        points_.push_back({h - Vec3(r, 0.0, 0.0), true});
        points_.push_back({h + Vec3(0.0, side * 0.5 * profile_.wheels.widthM, 0.0), true});
        points_.push_back({h - Vec3(0.0, 0.0, r), true});
    }
    pointPlanes_.resize(points_.size());
    preStepNormalSpeed_.assign(points_.size(), 0.0);
    pointContact_.assign(points_.size(), false);

    const double fullVolts =
        profile_.battery.cellsInSeries * openCircuitCellVoltage(profile_.battery.chemistry, 1.0);
    maxWheelOmega_ =
        std::max(1.0, fullVolts * kvRad(profile_.drivetrain) / profile_.drivetrain.gearRatio);
    updateMassProperties();
}

void GroundVehicle::updateMassProperties() {
    const ChassisParams& f = profile_.chassis;
    const double m0 = f.massKg;
    const double mp = payloadMassKg_;
    mass_.massKg = m0 + mp;
    const Vec3 c = mp > 0.0 ? payloadOffset_ * (mp / mass_.massKg) : Vec3(0.0);
    mass_.centreOfMassOffset = c;
    glm::dmat3 inertia(0.0);
    inertia[0][0] = f.inertiaXxKgM2;
    inertia[1][1] = f.inertiaYyKgM2;
    inertia[2][2] = f.inertiaZzKgM2;
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

void GroundVehicle::attachPayload(double massKg, const Vec3& offsetBody, double dragAreaM2) {
    const Vec3 oldC = mass_.centreOfMassOffset;
    payloadMassKg_ = std::max(0.0, massKg);
    payloadOffset_ = offsetBody;
    payloadDragArea_ = std::max(0.0, dragAreaM2);
    updateMassProperties();
    body_.position += body_.orientation * (mass_.centreOfMassOffset - oldC);
}

void GroundVehicle::releasePayload() { attachPayload(0.0, Vec3(0.0), 0.0); }

double GroundVehicle::restHeightM() const noexcept {
    return profile_.chassis.cogHeightM - mass_.centreOfMassOffset.z;
}

int GroundVehicle::substepsFor(double dt) const noexcept {
    // The stiffest modes: a wheel's spin against the tyre slip and the motor, and the body's
    // sideways slip on its tyres, both at the slip-denominator floor on the grippiest
    // surface. RK4 is stable well beyond h·λ = 1; stay below it.
    const GroundProfile& p = profile_;
    const double mu = surfaceProperties(SurfaceMaterial::Paved).peakFriction * p.tyre.gripScale;
    const double slope = mu * p.tyre.stiffnessB * p.tyre.shapeC / kSlipReferenceMs;
    const double load = mass_.massKg * kGravity / static_cast<double>(wheels_);
    const double r = p.wheels.radiusM;
    const DrivetrainParams& d = p.drivetrain;
    const double motorDamping = d.gearRatio * d.gearRatio *
                                (d.gearEfficiency / (kvRad(d) * kvRad(d) * d.motorResistanceOhm) +
                                 d.motorNoLoadCurrentA / (kvRad(d) * kFrictionSmoothing));
    const double wheelRate =
        (r * r * load * slope + motorDamping + d.brakeTorqueNm / kBrakeSmoothing) /
        rotatingInertia(p);
    const double bodyRate = kGravity * slope * 1.5;  // Load transfer: up to 1.5 × the mean.
    const double h = std::min(kMaxSubstepS, 1.0 / std::max(wheelRate, bodyRate));
    return std::clamp(static_cast<int>(std::ceil(dt / h - 1e-9)), 1, kMaxSubsteps);
}

void GroundVehicle::pushEvent(VehicleEvent::Kind kind, double value, std::string text) {
    events_.push_back({kind, value, std::move(text)});
}

void GroundVehicle::crash(double speed, std::string reason) {
    if (crashed_) {
        return;
    }
    crashed_ = true;
    controller_.forceDisarm();
    output_ = {.left = 0.0, .right = 0.0, .brake = true};
    pushEvent(VehicleEvent::Kind::Crash, speed, std::move(reason));
}

std::vector<VehicleEvent> GroundVehicle::takeEvents() { return std::exchange(events_, {}); }

Vec3 GroundVehicle::pointWorld(const Vec3& body) const noexcept {
    return body_.position + body_.orientation * (body - mass_.centreOfMassOffset);
}

DriveConditions GroundVehicle::conditions() const noexcept {
    DriveConditions c;
    c.batteryCritical = battery_.warning() == BatteryWarning::Critical || battery_.depleted();
    c.crashed = crashed_;
    c.rolledOver = rolledOver_;
    c.inWater = inWater_;
    c.odometrySpeedMs = odometrySpeed_;
    return c;
}

void GroundVehicle::reset(const Vec3& position, double headingRad, const EnvironmentSample& env) {
    body_ = {};
    // Sit on the terrain plane: body z along the ground normal, nose towards the heading.
    const Vec3 n = glm::normalize(env.ground.normal);
    const Quat level = orientationFromEuler({0.0, 0.0, headingRad});
    const Vec3 forward = level * Vec3(1.0, 0.0, 0.0);
    const Vec3 x = glm::normalize(forward - glm::dot(forward, n) * n);
    const Vec3 z = -n;
    const Vec3 y = glm::cross(z, x);
    body_.orientation = glm::normalize(glm::quat_cast(glm::dmat3(x, y, z)));
    const Vec3 ground(position.x, env.groundPoint.y, position.z);
    body_.position = ground + n * (restHeightM() + 0.002);
    omega_.fill(0.0);
    wheelAngle_.fill(0.0);
    encoderCounts_.fill(0);
    loads_ = {};
    output_ = {.left = 0.0, .right = 0.0, .brake = true};
    holding_ = false;
    battery_.reset(env.ambientC);
    sensors_.reset();
    estimator_.reset(headingRad, pressureAltitudeM(env.air.pressurePa, env.qnhHpa));
    controller_.reset();
    events_.clear();
    specificForce_ = Vec3(0.0, 0.0, -kGravity);
    wind_ = env.wind;
    airDensity_ = env.air.densityKgM3;
    waterLevel_ = env.waterLevelM;
    odometerM_ = 0.0;
    odometrySpeed_ = 0.0;
    rolloverTimer_ = 0.0;
    stuckTimer_ = 0.0;
    stuckAnchor_ = body_.position;
    stuckReported_ = false;
    waterDepth_ = 0.0;
    crashed_ = false;
    rolledOver_ = false;
    inWater_ = false;
    onGround_ = true;
    wasArmed_ = false;
    wasFailsafe_ = false;
    lastBlock_ = ArmBlock::None;
    lastWarning_ = BatteryWarning::None;
    std::fill(preStepNormalSpeed_.begin(), preStepNormalSpeed_.end(), 0.0);
    std::fill(pointContact_.begin(), pointContact_.end(), false);
    sampleTerrain(env);
}

void GroundVehicle::updateSensors(const EnvironmentSample& env, double dt) {
    SensorTruth t;
    t.body = body_;
    t.specificForceBody = specificForce_;
    t.staticPressurePa = env.air.pressurePa;
    t.qnhHpa = env.qnhHpa;
    t.heightAboveGroundM =
        glm::dot(env.ground.normal, body_.position - env.groundPoint) - restHeightM();
    double mean = 0.0;
    for (int i = 0; i < wheels_; ++i) {
        mean += std::abs(omega_[static_cast<std::size_t>(i)]);
    }
    mean /= static_cast<double>(wheels_);
    t.motorSpeedFraction = std::clamp(mean / maxWheelOmega_, 0.0, 1.0);
    t.motorFrequencyHz = mean * profile_.drivetrain.gearRatio / common::kTwoPi;
    const bool stationary = !controller_.armed() && glm::length(body_.velocity) < 0.02 &&
                            glm::length(body_.angularVelocity) < 0.01;
    estimator_.update(sensors_.update(t, dt), dt, stationary);

    // Wheel encoders: whole counts of each wheel's rotation; distance and speed from their
    // mean (FR-GND-10). Wheel slip fools the odometry, as on a real rover.
    const double countsPerRad = profile_.encoders.countsPerRev / common::kTwoPi;
    double delta = 0.0;
    for (int i = 0; i < wheels_; ++i) {
        const auto k = static_cast<std::size_t>(i);
        const auto counts = static_cast<std::int64_t>(std::floor(wheelAngle_[k] * countsPerRad));
        delta += static_cast<double>(counts - encoderCounts_[k]);
        encoderCounts_[k] = counts;
    }
    const double distance =
        delta / static_cast<double>(wheels_) / countsPerRad * profile_.wheels.radiusM;
    odometerM_ += std::abs(distance);
    if (dt > 0.0) {
        const double a = 1.0 - std::exp(-dt / kOdometryTauS);
        odometrySpeed_ += a * (distance / dt - odometrySpeed_);
    }
}

void GroundVehicle::updateController(const PilotCommand& command, double dt) {
    const bool armedBefore = controller_.armed();
    output_ = controller_.update(command, conditions(), dt);
    if (battery_.depleted()) {
        controller_.forceDisarm();
        output_ = {.left = 0.0, .right = 0.0, .brake = true};
    }
    if (driveOverride_ && controller_.armed()) {
        output_ = *driveOverride_;
    }
    if (controller_.armed() != wasArmed_) {
        pushEvent(controller_.armed() ? VehicleEvent::Kind::Armed : VehicleEvent::Kind::Disarmed);
        wasArmed_ = controller_.armed();
    }
    if (!armedBefore && !controller_.armed() && controller_.armBlock() != ArmBlock::None &&
        controller_.armBlock() != lastBlock_) {
        pushEvent(VehicleEvent::Kind::ArmRefused, 0.0,
                  std::string(armBlockText(controller_.armBlock())));
    }
    lastBlock_ = controller_.armed() ? ArmBlock::None : controller_.armBlock();
    if (!command.arm) {
        lastBlock_ = ArmBlock::None;
    }
    if (controller_.failsafe() != wasFailsafe_) {
        pushEvent(controller_.failsafe() ? VehicleEvent::Kind::FailsafeOn
                                         : VehicleEvent::Kind::FailsafeOff);
        wasFailsafe_ = controller_.failsafe();
    }
}

void GroundVehicle::sampleTerrain(const EnvironmentSample& env) {
    const auto plane = [&env](const Vec3& p) {
        ContactPlane c;
        if (env.surfaceQuery != nullptr && *env.surfaceQuery) {
            const GroundSample g = (*env.surfaceQuery)(p);
            c.point = Vec3(p.x, g.heightM, p.z);
            c.normal = glm::normalize(g.normal);
            c.material = g.material;
        } else if (env.groundQuery != nullptr && *env.groundQuery) {
            const GroundSample g = (*env.groundQuery)(p.x, p.z);
            c.point = Vec3(p.x, g.heightM, p.z);
            c.normal = glm::normalize(g.normal);
            c.material = g.material;
        } else {
            c.point = env.groundPoint;
            c.normal = glm::normalize(env.ground.normal);
            c.material = env.ground.material;
        }
        return c;
    };
    const Vec3 down = body_.orientation * Vec3(0.0, 0.0, 1.0);
    const double restExtension = profile_.suspension.travelM - profile_.staticCompressionM();
    for (int i = 0; i < wheels_; ++i) {
        const auto k = static_cast<std::size_t>(i);
        wheelPlanes_[k] = plane(pointWorld(mounts_[k]) + down * restExtension);
    }
    ContactPlane centre;
    centre.point = env.groundPoint;
    centre.normal = glm::normalize(env.ground.normal);
    centre.material = env.ground.material;
    std::fill(pointPlanes_.begin(), pointPlanes_.end(), centre);
    for (const int j : sampledPoints_) {
        const auto k = static_cast<std::size_t>(j);
        pointPlanes_[k] = plane(pointWorld(points_[k].body));
    }
    surface_ = env.ground.material;
    cover_ = env.groundCover;
    slopeDeg_ = common::radToDeg(std::acos(std::clamp(centre.normal.y, -1.0, 1.0)));
    waterLevel_ = env.waterLevelM;
    waterDepth_ = env.waterLevelM ? std::max(0.0, *env.waterLevelM - env.ground.heightM) : 0.0;
}

void GroundVehicle::setFaults(const VehicleFaults& faults) {
    faults_ = faults;
    sensors_.setFaults(faults.sensors);
}

double GroundVehicle::motorCurrent(int wheel, double omega) const noexcept {
    const DrivetrainParams& d = profile_.drivetrain;
    const bool right = profile_.isRightWheel(wheel);
    const double command = right ? output_.right : output_.left;
    double volts = 0.0;
    if (output_.brake) {
        volts = 0.0;  // Windings shorted by the ESC: dynamic braking.
    } else if (std::abs(command) < 0.01 || supplyVolts_ <= 0.0) {
        return 0.0;  // Neutral: the ESC lets the motor coast.
    } else {
        volts = command * supplyVolts_;
        if (wheel >= 0 && static_cast<std::size_t>(wheel) < kMaxFailureMotors) {
            volts *= faults_.motorEfficiency[static_cast<std::size_t>(wheel)];
        }
    }
    const double backEmf = d.gearRatio * omega / kvRad(d);
    return std::clamp((volts - backEmf) / d.motorResistanceOhm, -d.maxCurrentA, d.maxCurrentA);
}

double GroundVehicle::motorTorque(int wheel, double omega) const noexcept {
    const DrivetrainParams& d = profile_.drivetrain;
    const double kt = 1.0 / kvRad(d);
    const double motorOmega = d.gearRatio * omega;
    const double friction = kt * d.motorNoLoadCurrentA * std::tanh(motorOmega / kFrictionSmoothing);
    return d.gearRatio * (d.gearEfficiency * kt * motorCurrent(wheel, omega) - friction);
}

Wrench GroundVehicle::evaluate(const RigidBodyState& s, const WheelArray& omega,
                               WheelArray& omegaDot, WheelLoads* loads) const {
    Wrench w;
    const Vec3 c = mass_.centreOfMassOffset;
    const Quat& q = s.orientation;
    const Vec3 down = q * Vec3(0.0, 0.0, 1.0);
    const GroundProfile& p = profile_;
    const double r = p.wheels.radiusM;
    const double travel = p.suspension.travelM;
    const double k = p.suspension.stiffnessNPerM;
    const double cd = p.suspension.dampingNsPerM;
    const double inertia = rotatingInertia(p);
    const auto torqueAbout = [&](const Vec3& pointWorld, const Vec3& force) {
        return glm::cross(worldToBody(q, pointWorld - s.position), worldToBody(q, force));
    };

    // Aerodynamic drag of the body (C_d·A per axis, relative to the air).
    {
        const Vec3 air = worldToBody(q, s.velocity - wind_);
        const ChassisParams& ch = p.chassis;
        const Vec3 area(ch.dragAreaForwardM2 + payloadDragArea_,
                        ch.dragAreaSideM2 + payloadDragArea_,
                        ch.dragAreaVerticalM2 + payloadDragArea_);
        const Vec3 drag = -0.5 * airDensity_ * area * glm::abs(air) * air;
        w.forceWorld += q * drag;
    }

    // Wheels: suspension, tyre forces and spin.
    for (int i = 0; i < wheels_; ++i) {
        const auto ix = static_cast<std::size_t>(i);
        const ContactPlane& g = wheelPlanes_[ix];
        const Vec3& n = g.normal;
        const Vec3 mount = mounts_[ix] - c;
        const Vec3 mountWorld = s.position + q * mount;
        const double strut = glm::dot(n, down);  // < 0 when the wheel points at the ground.
        double longitudinal = 0.0;
        double rolling = 0.0;
        double normal = 0.0;
        double compression = 0.0;
        double slip = 0.0;
        bool contact = false;
        if (strut < -kMinStrutCosine) {
            // Hub extension (from the mount, along body down) at which the tyre touches.
            const double extension = (r - glm::dot(n, mountWorld - g.point)) / strut;
            if (extension < travel) {
                contact = true;
                const Vec3 mountVelocity = s.velocity + q * glm::cross(s.angularVelocity, mount);
                const double compressionRate = glm::dot(n, mountVelocity) / strut;
                compression = travel - extension;
                double force = k * compression + cd * compressionRate;
                if (extension < 0.0) {  // Bump stop.
                    force += kBumpStopFactor * (k * -extension + cd * compressionRate);
                }
                normal = std::max(0.0, force) / -strut;
                const Vec3 hub = mountWorld + down * std::clamp(extension, 0.0, travel);
                const Vec3 contactPoint = hub - n * r;
                const Vec3 rc = worldToBody(q, contactPoint - s.position);
                const Vec3 vc = s.velocity + q * glm::cross(s.angularVelocity, rc);
                const Vec3 nose = q * Vec3(1.0, 0.0, 0.0);
                Vec3 f = nose - glm::dot(nose, n) * n;
                const double fl = glm::length(f);
                f = fl > 1e-9 ? f / fl : glm::normalize(glm::cross(Vec3(0.0, 0.0, 1.0), n));
                const Vec3 l = glm::cross(n, f);  // Left.
                const double vx = glm::dot(vc, f);
                const double vy = glm::dot(vc, l);
                const double u = omega[ix] * r;
                const double sx = u - vx;
                const double sy = -vy;
                const double denom = std::max({std::abs(vx), std::abs(u), kSlipReferenceMs});
                const double kx = sx / denom;
                const double ky = sy / denom;
                slip = std::hypot(kx, ky);
                const SurfaceProperties sp = surfaceProperties(g.material, cover_);
                const double mu = sp.peakFriction * p.tyre.gripScale;
                double fx = 0.0;
                double fy = 0.0;
                bool slipping = false;
                if (holding_) {
                    // Static friction: a spring to the anchored contact point. A tyre pushed
                    // beyond its brake (it rolls) or its grip (it slides) gives way, and its
                    // anchor follows.
                    const Vec3 stretch = contactPoint - anchors_[ix];
                    Vec3 ft = -kHoldStiffness * stretch - kHoldDamping * vc;
                    ft -= glm::dot(ft, n) * n;
                    fx = glm::dot(ft, f);
                    fy = glm::dot(ft, l);
                    const double brakeLimit = p.drivetrain.brakeTorqueNm / r;
                    if (std::abs(fx) > brakeLimit) {
                        fx = std::copysign(brakeLimit, fx);
                        slipping = true;
                    }
                    const double magnitude = std::hypot(fx, fy);
                    if (magnitude > mu * normal) {
                        const double scale = mu * normal / std::max(magnitude, 1e-12);
                        fx *= scale;
                        fy *= scale;
                        slipping = true;
                    }
                    if (slipping && loads != nullptr) {
                        // The anchor whose spring alone gives the limited force.
                        loads->slidAnchor[ix] = contactPoint + (fx * f + fy * l) / kHoldStiffness;
                    }
                } else if (slip > 1e-12) {
                    const double magnitude =
                        mu * normal * std::sin(p.tyre.shapeC * std::atan(p.tyre.stiffnessB * slip));
                    fx = magnitude * kx / slip;
                    fy = magnitude * ky / slip;
                }
                longitudinal = holding_ ? 0.0 : fx;  // Held wheels do not turn.
                if (loads != nullptr) {
                    loads->contactPoint[ix] = contactPoint;
                    loads->slipping[ix] = slipping;
                }
                rolling = sp.rollingResistance * p.tyre.rollingResistanceScale * normal * r *
                          std::tanh(omega[ix] / kRollingSmoothing);
                const Vec3 force3 = normal * n + fx * f + fy * l;
                w.forceWorld += force3;
                w.torqueBody += torqueAbout(contactPoint, force3);
            }
        }
        double brake = 0.0;
        if (output_.brake) {
            brake = p.drivetrain.brakeTorqueNm * std::tanh(omega[ix] / kBrakeSmoothing);
        }
        omegaDot[ix] =
            holding_ ? -omega[ix] / 0.01
                     : (motorTorque(i, omega[ix]) - longitudinal * r - rolling - brake) / inertia;
        if (loads != nullptr) {
            loads->load[ix] = normal;
            loads->compression[ix] = std::min(compression, travel);
            loads->slip[ix] = slip;
            loads->longitudinal[ix] = longitudinal;
            loads->contact[ix] = contact;
        }
    }

    // Body against the terrain (crests, rollover onto the side or roof).
    const double m = mass_.massKg;
    const double kc = m * kContactOmega * kContactOmega / 4.0;
    const double damping = 2.0 * kContactDamping * m * kContactOmega / 4.0;
    for (std::size_t j = 0; j < points_.size(); ++j) {
        if (points_[j].gear) {
            continue;  // Wheel outlines are for obstacles; the suspension handles the ground.
        }
        const ContactPlane& g = pointPlanes_[j];
        const Vec3& n = g.normal;
        const Vec3 rb = points_[j].body - c;
        const Vec3 pw = s.position + q * rb;
        const double depth = -glm::dot(n, pw - g.point);
        if (depth <= 0.0) {
            continue;
        }
        const Vec3 vp = s.velocity + q * glm::cross(s.angularVelocity, rb);
        const double vn = glm::dot(vp, n);
        const double fn = std::max(0.0, kc * depth - damping * vn);
        const Vec3 vt = vp - vn * n;
        const double mu = kBodyFriction * surfaceProperties(g.material, cover_).peakFriction;
        const Vec3 ft =
            -mu * fn * vt / std::sqrt(glm::dot(vt, vt) + kBodySlideSmoothing * kBodySlideSmoothing);
        const Vec3 f = fn * n + ft;
        w.forceWorld += f;
        w.torqueBody += glm::cross(rb, worldToBody(q, f));
    }

    // Water: drag on the submerged part of the body.
    if (waterLevel_) {
        const double bottom = s.position.y - restHeightM();
        const double depth =
            std::clamp((*waterLevel_ - bottom) /
                           std::max(0.05, p.chassis.cogHeightM + 0.5 * p.chassis.heightM),
                       0.0, 1.0);
        if (depth > 0.0) {
            w.forceWorld -= 6.0 * m * depth * s.velocity;
            w.torqueBody -= 0.5 * depth * (mass_.inertia * s.angularVelocity);
        }
    }
    return w;
}

void GroundVehicle::step(double h) {
    const RigidBodyState start = body_;
    const WheelArray omega0 = omega_;
    std::array<WheelArray, 4> rates{};
    int stage = 0;
    WheelLoads loads;
    const Wrench first = integrateRk4(
        body_, mass_,
        [&](const RigidBodyState& s) {
            // Wheel speeds advance in step with the body: ω0, ω0 + k1·h/2, ω0 + k2·h/2,
            // ω0 + k3·h (the RK4 stages).
            WheelArray omega = omega0;
            if (stage > 0) {
                const double f = stage == 3 ? 1.0 : 0.5;
                for (int i = 0; i < wheels_; ++i) {
                    const auto k = static_cast<std::size_t>(i);
                    omega[k] += f * h * rates[static_cast<std::size_t>(stage - 1)][k];
                }
            }
            const Wrench result = evaluate(s, omega, rates[static_cast<std::size_t>(stage)],
                                           stage == 0 ? &loads : nullptr);
            ++stage;
            return result;
        },
        h);
    for (int i = 0; i < wheels_; ++i) {
        const auto k = static_cast<std::size_t>(i);
        omega_[k] = omega0[k] +
                    h / 6.0 * (rates[0][k] + 2.0 * rates[1][k] + 2.0 * rates[2][k] + rates[3][k]);
        wheelAngle_[k] += 0.5 * (omega0[k] + omega_[k]) * h;
    }
    loads_ = loads;
    specificForce_ = worldToBody(start.orientation, first.forceWorld / mass_.massKg);
}

void GroundVehicle::integrate(const EnvironmentSample& env, double dt) {
    wind_ = env.wind;
    airDensity_ = env.air.densityKgM3;
    sampleTerrain(env);
    // Motor supply for this tick: the pack voltage under last tick's load.
    supplyVolts_ = battery_.depleted() ? 0.0 : battery_.voltageAt(battery_.current());

    for (std::size_t j = 0; j < points_.size(); ++j) {
        const Vec3 rb = points_[j].body - mass_.centreOfMassOffset;
        const Vec3 vp = body_.velocity + body_.orientation * glm::cross(body_.angularVelocity, rb);
        preStepNormalSpeed_[j] = glm::dot(vp, pointPlanes_[j].normal);
    }

    const RigidBodyState before = body_;
    const int n = substepsFor(dt);
    const double h = dt / static_cast<double>(n);
    double charge = 0.0;  // Motor current × time, for the battery.
    for (int s = 0; s < n; ++s) {
        step(h);
        for (int i = 0; i < wheels_; ++i) {
            const double cmd = profile_.isRightWheel(i) ? output_.right : output_.left;
            const double current = motorCurrent(i, omega_[static_cast<std::size_t>(i)]);
            if (!output_.brake) {
                charge += std::max(0.0, cmd * current) * h;  // No regeneration (S).
            }
        }
    }

    // Numerical safety (FR-PHY-17).
    bool finite = isFinite(body_);
    for (int i = 0; i < wheels_; ++i) {
        finite = finite && std::isfinite(omega_[static_cast<std::size_t>(i)]);
    }
    if (!finite) {
        body_ = isFinite(before) ? before : RigidBodyState{};
        if (!isFinite(before)) {
            body_.position = env.groundPoint + Vec3(0.0, restHeightM(), 0.0);
        }
        body_.velocity = Vec3(0.0);
        body_.angularVelocity = Vec3(0.0);
        omega_.fill(0.0);
        controller_.forceDisarm();
        pushEvent(VehicleEvent::Kind::NumericalReset, 0.0, "invalid numbers in the vehicle state");
    }

    const double current =
        charge / dt / profile_.electrical.escEfficiency + profile_.electrical.avionicsCurrentA;
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

    // Standing on the brake: hold every tyre where it is (static friction); tyres pushed past
    // their brake or grip slide along. The hold ends when the brake is released or the
    // vehicle as a whole gets moving.
    bool canHold = output_.brake && std::abs(output_.left) < 1e-9 && std::abs(output_.right) < 1e-9;
    for (int i = 0; i < wheels_; ++i) {
        canHold = canHold && loads_.contact[static_cast<std::size_t>(i)];
    }
    const double speed = glm::length(body_.velocity);
    if (!canHold || (holding_ && speed > 2.0 * kHoldSpeed)) {
        holding_ = false;
    } else if (holding_) {
        for (int i = 0; i < wheels_; ++i) {
            const auto k = static_cast<std::size_t>(i);
            if (loads_.slipping[k]) {
                anchors_[k] = loads_.slidAnchor[k];
            }
        }
    } else if (speed < kHoldSpeed && glm::length(body_.angularVelocity) < 2.0 * kHoldSpeed) {
        holding_ = true;
        anchors_ = loads_.contactPoint;
        omega_.fill(0.0);
    }
}

void GroundVehicle::resolveCollisions(const EnvironmentSample& env, double dt) {
    // Body impacts with the terrain (falls, rolling down a slope).
    bool anyWheel = false;
    for (int i = 0; i < wheels_; ++i) {
        anyWheel = anyWheel || loads_.contact[static_cast<std::size_t>(i)];
    }
    bool bodyContact = false;
    for (std::size_t j = 0; j < points_.size(); ++j) {
        if (points_[j].gear) {
            continue;
        }
        const Vec3 p = pointWorld(points_[j].body);
        const bool contact = glm::dot(pointPlanes_[j].normal, p - pointPlanes_[j].point) < 0.0;
        if (contact && !pointContact_[j]) {
            const double impact = std::max(0.0, -preStepNormalSpeed_[j]);
            if (impact > kCollisionEventSpeed) {
                pushEvent(VehicleEvent::Kind::Collision, impact);
            }
            if (impact > profile_.chassis.crashSpeedMs) {
                crash(impact, "impact with the ground");
            }
        }
        pointContact_[j] = contact;
        bodyContact = bodyContact || contact;
    }
    onGround_ = anyWheel || bodyContact;

    // Rollover (FR-GND-07): tilted past the point of no return for a moment.
    const double tilt = common::radToDeg(tiltAngle(body_.orientation));
    rolloverTimer_ = tilt > kRolloverTiltDeg ? rolloverTimer_ + dt : 0.0;
    if (!rolledOver_ && rolloverTimer_ >= kRolloverHoldS) {
        rolledOver_ = true;
        controller_.forceDisarm();
        pushEvent(VehicleEvent::Kind::Rollover, tilt);
    }

    // Stuck (FR-GND-08): driving forwards or backwards without getting anywhere.
    const double drive = 0.5 * (output_.left + output_.right);
    const Vec3 moved = body_.position - stuckAnchor_;
    if (controller_.armed() && !output_.brake && std::abs(drive) > kStuckDrive) {
        if (std::hypot(moved.x, moved.z) > kStuckProgressM) {
            stuckAnchor_ = body_.position;
            stuckTimer_ = 0.0;
        } else {
            stuckTimer_ += dt;
        }
    } else {
        stuckAnchor_ = body_.position;
        stuckTimer_ = 0.0;
    }
    if (stuckTimer_ >= profile_.limits.stuckDetectS && !stuckReported_) {
        stuckReported_ = true;
        pushEvent(VehicleEvent::Kind::Stuck, stuckTimer_);
    } else if (stuckTimer_ == 0.0) {
        stuckReported_ = false;
    }

    // Water deeper than the wading depth drowns the electronics (FR-GND-09).
    if (env.waterLevelM && !inWater_ && waterDepth_ > profile_.limits.wadingDepthM) {
        inWater_ = true;
        controller_.forceDisarm();
        output_ = {.left = 0.0, .right = 0.0, .brake = true};
        pushEvent(VehicleEvent::Kind::WaterEntered, waterDepth_);
    }
}

void GroundVehicle::resolveObstacles(const ObstacleProbe& probe) {
    if (!probe) {
        return;
    }
    std::optional<ObstacleContact> deepest;
    bool wheel = false;
    for (const CollisionPoint& point : points_) {
        if (auto contact = probe(pointWorld(point.body));
            contact && (!deepest || contact->depthM > deepest->depthM)) {
            deepest = std::move(contact);
            wheel = point.gear;
        }
    }
    if (!deepest) {
        return;
    }
    const Vec3& n = deepest->normal;
    body_.position += n * deepest->depthM;
    const double normalSpeed = glm::dot(body_.velocity, n);
    if (normalSpeed < 0.0) {
        const double m = mass_.massKg;
        const double obstacleMass = deepest->massKg;
        const double share = std::isfinite(obstacleMass) ? obstacleMass / (m + obstacleMass) : 1.0;
        const double restitution = std::clamp(deepest->restitution, 0.0, 1.0);
        const double dvNormal = -(1.0 + restitution) * normalSpeed * share;
        body_.velocity += dvNormal * n;
        const Vec3 tangential = body_.velocity - glm::dot(body_.velocity, n) * n;
        const double slide = glm::length(tangential);
        if (slide > 1e-9 && deepest->friction > 0.0 && n.y < 0.7) {
            body_.velocity -= tangential * (std::min(slide, deepest->friction * dvNormal) / slide);
        }
        const double impact = -normalSpeed;
        if (impact > kCollisionEventSpeed) {
            pushEvent(VehicleEvent::Kind::Collision, impact);
        }
        if (!crashed_ && impact > profile_.chassis.crashSpeedMs) {
            crash(impact, "collision with " + deepest->name);
        }
    }
    // Driving on top of something counts as being on the ground.
    if (wheel && n.y > 0.7) {
        onGround_ = true;
    }
}

VehicleTelemetry GroundVehicle::telemetry() const {
    VehicleTelemetry t;
    t.type = VehicleType::Ground;
    t.armed = controller_.armed();
    t.crashed = crashed_;
    t.onGround = onGround_;
    t.failsafe = controller_.failsafe();
    t.armBlock = controller_.armBlock();
    t.throttle = 0.5 * (std::abs(output_.left) + std::abs(output_.right));
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
    t.wind = wind_;
    t.airspeedMs = glm::length(body_.velocity - wind_);
    t.armedTimeS = controller_.armedTime();
    double meanOmega = 0.0;
    GroundTelemetry& g = t.ground;
    g.mode = controller_.mode();
    g.wheelCount = wheels_;
    double left = 0.0;
    double right = 0.0;
    for (int i = 0; i < wheels_; ++i) {
        const auto k = static_cast<std::size_t>(i);
        g.wheelSpeed[k] = omega_[k];
        g.wheelAngle[k] = std::fmod(wheelAngle_[k], common::kTwoPi);
        g.compression[k] = loads_.compression[k];
        g.wheelLoad[k] = loads_.load[k];
        g.slip[k] = loads_.slip[k];
        g.contact[k] = loads_.contact[k];
        g.maxSlip = std::max(g.maxSlip, loads_.slip[k]);
        (profile_.isRightWheel(i) ? right : left) += loads_.load[k];
        meanOmega += std::abs(omega_[k]);
        if (k < t.motorRpm.size()) {
            t.motorRpm[k] = omega_[k] * profile_.drivetrain.gearRatio * 60.0 / common::kTwoPi;
        }
    }
    t.motorSpeedFraction = meanOmega / static_cast<double>(wheels_) / maxWheelOmega_;
    g.loadTransfer = left + right > 1e-9 ? (right - left) / (left + right) : 0.0;
    g.leftCommand = output_.left;
    g.rightCommand = output_.right;
    g.braking = output_.brake;
    const Vec3 nose = body_.orientation * Vec3(1.0, 0.0, 0.0);
    const glm::dvec2 heading(nose.x, nose.z);
    const double hl = glm::length(heading);
    g.speedMs =
        hl > 1e-9 ? glm::dot(glm::dvec2(body_.velocity.x, body_.velocity.z), heading / hl) : 0.0;
    g.odometrySpeedMs = odometrySpeed_;
    g.odometerM = odometerM_;
    g.cruiseSetpointMs = controller_.cruiseSetpoint();
    g.slopeDeg = slopeDeg_;
    g.surface = surface_;
    g.cover = cover_;
    g.rolledOver = rolledOver_;
    g.stuckS = stuckTimer_;
    g.inWater = inWater_;
    g.waterDepthM = waterDepth_;
    return t;
}

}  // namespace ds::physics
