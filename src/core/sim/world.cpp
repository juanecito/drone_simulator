// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/world.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "ds/common/random.hpp"
#include "ds/physics/atmosphere.hpp"

namespace ds::sim {

GroundFunction flatGround(double heightM) {
    return [heightM](double, double) { return physics::GroundSample{.heightM = heightM}; };
}

physics::FlightMode toFlightMode(int index) noexcept {
    switch (index) {
        case 0: return physics::FlightMode::Acro;
        case 1: return physics::FlightMode::Angle;
        default: return physics::FlightMode::AltitudeHold;  // Position hold arrives in M7.
    }
}

physics::PilotCommand toPilotCommand(const input::ControlInputs& c) noexcept {
    physics::PilotCommand p;
    p.throttle = std::clamp(c.throttle, 0.0, 1.0);
    p.roll = std::clamp(c.roll, -1.0, 1.0);
    p.pitch = std::clamp(c.pitch, -1.0, 1.0);
    p.yaw = std::clamp(c.yaw, -1.0, 1.0);
    p.arm = c.arm;
    p.mode = toFlightMode(c.flightMode);
    p.linkValid = c.linkValid;
    return p;
}

std::string describe(const Event& event) {
    return std::visit(
        [](const auto& e) -> std::string {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, CollisionEvent>) {
                return std::format("Collision at {:.1f} m/s", e.speedMs);
            } else if constexpr (std::is_same_v<T, CrashEvent>) {
                return std::format("Crash: {} ({:.1f} m/s)", e.reason, e.speedMs);
            } else if constexpr (std::is_same_v<T, BatteryWarningEvent>) {
                switch (e.level) {
                    case BatteryWarningEvent::Level::Low: return "Battery low";
                    case BatteryWarningEvent::Level::Critical: return "Battery critical — land now";
                    case BatteryWarningEvent::Level::Depleted:
                        return "Battery empty — motors stopped";
                }
                return "Battery";
            } else if constexpr (std::is_same_v<T, WaterEnteredEvent>) {
                return "Fell into water";
            } else if constexpr (std::is_same_v<T, SimResetEvent>) {
                return "Reset";
            } else if constexpr (std::is_same_v<T, ArmingEvent>) {
                return e.armed ? "Armed" : "Disarmed";
            } else if constexpr (std::is_same_v<T, ArmRefusedEvent>) {
                return "Cannot arm: " + e.reason;
            } else if constexpr (std::is_same_v<T, FailsafeEvent>) {
                return e.active ? "Failsafe: RC link lost — landing" : "RC link restored";
            } else if constexpr (std::is_same_v<T, NumericalResetEvent>) {
                return "Numerical error — vehicle state reset";
            } else if constexpr (std::is_same_v<T, PayloadAttachedEvent>) {
                return std::format("Payload attached ({:.2f} kg)", e.massKg);
            } else if constexpr (std::is_same_v<T, PayloadReleasedEvent>) {
                return "Payload released";
            } else if constexpr (std::is_same_v<T, ObjectiveCompletedEvent>) {
                return "Objective completed: " + e.objectiveId;
            } else if constexpr (std::is_same_v<T, ObjectiveFailedEvent>) {
                return "Objective failed: " + e.objectiveId;
            } else if constexpr (std::is_same_v<T, MissionSucceededEvent>) {
                return "Mission succeeded";
            } else if constexpr (std::is_same_v<T, MissionFailedEvent>) {
                return "Mission failed: " + e.reason;
            } else if constexpr (std::is_same_v<T, RolloverEvent>) {
                return "Rollover";
            } else if constexpr (std::is_same_v<T, VehicleStuckEvent>) {
                return "Vehicle stuck";
            } else if constexpr (std::is_same_v<T, GatePassedEvent>) {
                return "Gate passed";
            } else if constexpr (std::is_same_v<T, ZoneEnteredEvent>) {
                return "Zone entered";
            } else {
                static_assert(std::is_same_v<T, ZoneExitedEvent>);
                return "Zone left";
            }
        },
        event);
}

World::World(WorldConfig config) : config_(std::move(config)) {
    if (!config_.ground) {
        config_.ground = flatGround();
    }
    vehicle_ = std::make_unique<physics::AerialVehicle>(config_.profile, config_.seed);
    clockBase_ = config_.startUtc;
    clockBaseTime_ = 0.0;
    reset();
}

astro::UtcTime World::utc() const noexcept {
    const auto ms = static_cast<std::int64_t>(std::llround((time_ - clockBaseTime_) * 1000.0));
    return clockBase_ + std::chrono::milliseconds(ms);
}

void World::setUtc(astro::UtcTime utc) noexcept {
    clockBase_ = utc;
    clockBaseTime_ = time_;
}

physics::EnvironmentSample World::sampleEnvironment(const physics::Vec3& position) const {
    physics::EnvironmentSample env;
    env.ground = config_.ground(position.x, position.z);
    env.groundPoint = physics::Vec3(position.x, env.ground.heightM, position.z);
    env.waterLevelM = config_.waterLevelM;
    env.air = physics::airAt(config_.originAltitudeM + position.y, config_.air,
                             config_.originAltitudeM + env.ground.heightM);
    env.qnhHpa = config_.air.pressureHpa;
    env.ambientC = config_.air.temperatureC;
    env.wind = wind_.velocity();
    return env;
}

void World::restartWind() {
    auto params = physics::WindParameters::fromLevel(config_.windTable, config_.wind, config_.seed);
    params.roughnessM = config_.roughnessM;
    wind_ = physics::WindModel(params, config_.seed + resets_ * 0x9E3779B97F4A7C15ULL);
}

void World::reset() {
    ++resets_;
    restartWind();
    const physics::Vec3 start = config_.startPosition;
    // Air where the vehicle will rest (the barometer starts from it).
    const double ground = config_.ground(start.x, start.z).heightM;
    env_ = sampleEnvironment(physics::Vec3(start.x, ground + vehicle_->restHeightM(), start.z));
    vehicle_->reset(start, config_.startHeadingRad, env_);
    home_ = vehicle_->bodyState().position;
    lastPosition_ = home_;
    stats_ = {};
    controls_ = {};
    stats_.startedAt = utc();
    events_.publish(SimResetEvent{});
}

void World::resetAt(const physics::Vec3& position, double headingRad) {
    config_.startPosition = position;
    config_.startHeadingRad = headingRad;
    reset();
}

void World::setGround(GroundFunction ground, std::optional<double> waterLevelM,
                      double originAltitudeM) {
    config_.ground = ground ? std::move(ground) : flatGround();
    config_.waterLevelM = waterLevelM;
    config_.originAltitudeM = originAltitudeM;
    // Lift the vehicle onto the new surface if it is now underground.
    physics::RigidBodyState body = vehicle_->bodyState();
    const physics::GroundSample g = config_.ground(body.position.x, body.position.z);
    const double minimum = g.heightM + vehicle_->restHeightM();
    if (body.position.y < minimum) {
        body.position.y = minimum + 0.002;
        body.velocity = physics::Vec3(0.0);
        vehicle_->setBodyState(body);
    }
}

void World::setWind(const weather::WindSettings& wind) {
    config_.wind = wind;
    restartWind();
}

void World::setProfile(const physics::AerialProfile& profile) {
    config_.profile = profile;
    vehicle_ = std::make_unique<physics::AerialVehicle>(config_.profile, config_.seed);
    reset();
}

void World::publishVehicleEvents() {
    using Kind = physics::VehicleEvent::Kind;
    for (const physics::VehicleEvent& e : vehicle_->takeEvents()) {
        switch (e.kind) {
            case Kind::Collision: events_.publish(CollisionEvent{e.value}); break;
            case Kind::Crash:
                ++stats_.crashes;
                events_.publish(CrashEvent{e.text, e.value});
                break;
            case Kind::BatteryLow:
                events_.publish(BatteryWarningEvent{BatteryWarningEvent::Level::Low});
                break;
            case Kind::BatteryCritical:
                events_.publish(BatteryWarningEvent{BatteryWarningEvent::Level::Critical});
                break;
            case Kind::BatteryDepleted:
                events_.publish(BatteryWarningEvent{BatteryWarningEvent::Level::Depleted});
                break;
            case Kind::WaterEntered: events_.publish(WaterEnteredEvent{}); break;
            case Kind::Armed: events_.publish(ArmingEvent{true}); break;
            case Kind::Disarmed: events_.publish(ArmingEvent{false}); break;
            case Kind::ArmRefused: events_.publish(ArmRefusedEvent{e.text}); break;
            case Kind::FailsafeOn: events_.publish(FailsafeEvent{true}); break;
            case Kind::FailsafeOff: events_.publish(FailsafeEvent{false}); break;
            case Kind::NumericalReset: events_.publish(NumericalResetEvent{}); break;
        }
    }
}

void World::tick(const input::ControlInputs& controls, double dt) {
    controls_ = controls;
    physics::AerialVehicle& v = *vehicle_;
    // Environment at the vehicle (terrain queried once per tick).
    const physics::RigidBodyState& body = v.bodyState();
    const physics::GroundSample g = config_.ground(body.position.x, body.position.z);
    wind_.update(dt, std::max(0.0, body.position.y - g.heightM), body.velocity);
    env_ = sampleEnvironment(body.position);

    // FR-LOOP-05: sensors → controller → physics → collisions → (items, missions: M5).
    v.updateSensors(env_, dt);
    v.updateController(toPilotCommand(controls), dt);
    v.integrate(env_, dt);
    v.resolveCollisions(env_, dt);
    publishVehicleEvents();

    time_ += dt;
    ++ticks_;

    // Statistics.
    const physics::RigidBodyState& after = v.bodyState();
    stats_.durationS = time_;
    const bool armed = v.controller().armed();
    if (armed) {
        stats_.flightTimeS += dt;
        stats_.distanceM += glm::length(after.position - lastPosition_);
    }
    lastPosition_ = after.position;
    stats_.maxAglM =
        std::max(stats_.maxAglM, after.position.y - env_.ground.heightM - v.restHeightM());
    stats_.maxSpeedMs = std::max(stats_.maxSpeedMs, glm::length(after.velocity));
    stats_.energyWh = v.battery().consumedWh();
}

void World::fillSnapshot(WorldSnapshot& s) const {
    const physics::AerialVehicle& v = *vehicle_;
    s.tick = ticks_;
    s.simTimeS = time_;
    s.utc = utc();
    s.hasVehicle = true;
    s.profileKey = config_.profile.key;
    s.profileName = config_.profile.name;
    s.body = v.bodyState();
    s.controls = controls_;
    s.attitude = physics::eulerFromOrientation(s.body.orientation);
    s.telemetry = v.telemetry();
    const physics::GroundSample g = config_.ground(s.body.position.x, s.body.position.z);
    s.groundHeightM = g.heightM;
    s.aglM = s.body.position.y - g.heightM - v.restHeightM();
    s.originAltitudeM = config_.originAltitudeM;
    s.home = home_;
    const weather::WindLevel& level = config_.windTable.level(config_.wind.level);
    s.windLevel = level.level;
    s.windFromDeg = wind_.currentFromDeg();
    s.windMeanSpeedMs = level.meanSpeedMs;
    s.windGustMs = level.gustPeakMs;
    s.stats = stats_;
    s.resetCount = resets_;
}

}  // namespace ds::sim
