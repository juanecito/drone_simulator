// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/world.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "ds/astro/sun_moon.hpp"
#include "ds/common/random.hpp"
#include "ds/physics/atmosphere.hpp"

namespace ds::sim {

GroundFunction flatGround(double heightM) {
    return [heightM](double, double) { return physics::GroundSample{.heightM = heightM}; };
}

missions::LineOfSightFunction terrainLineOfSight(GroundFunction ground) {
    if (!ground) {
        return {};
    }
    return [ground = std::move(ground)](const items::Vec3& from, const items::Vec3& to) {
        const double d = glm::length(glm::dvec2(to.x - from.x, to.z - from.z));
        const int n = std::clamp(static_cast<int>(std::ceil(d / 5.0)), 1, 400);
        // The end points themselves are on or above the ground: test the inside only.
        for (int i = 1; i < n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(n);
            const items::Vec3 p = from + (to - from) * t;
            if (ground(p.x, p.z).heightM > p.y) {
                return false;
            }
        }
        return true;
    };
}

physics::FlightMode toFlightMode(int index) noexcept {
    switch (index) {
        case 0: return physics::FlightMode::Acro;
        case 1: return physics::FlightMode::Angle;
        case 2: return physics::FlightMode::AltitudeHold;
        default: return physics::FlightMode::PositionHold;
    }
}

physics::DriveMode toDriveMode(int index) noexcept {
    // Low and middle switch positions (the default index is 1) drive manually.
    return index <= 1 ? physics::DriveMode::Manual : physics::DriveMode::SpeedControl;
}

physics::PilotCommand toPilotCommand(const input::ControlInputs& c) noexcept {
    physics::PilotCommand p;
    p.throttle = std::clamp(c.throttle, 0.0, 1.0);
    p.roll = std::clamp(c.roll, -1.0, 1.0);
    p.pitch = std::clamp(c.pitch, -1.0, 1.0);
    p.yaw = std::clamp(c.yaw, -1.0, 1.0);
    p.arm = c.arm;
    p.mode = toFlightMode(c.flightMode);
    p.returnToHome = c.returnToHome;
    p.drive = std::clamp(c.drive, -1.0, 1.0);
    p.steering = std::clamp(c.steering, -1.0, 1.0);
    p.brake = c.brake;
    p.driveMode = toDriveMode(c.flightMode);
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
                return e.active ? "Failsafe: RC link lost" : "RC link restored";
            } else if constexpr (std::is_same_v<T, NumericalResetEvent>) {
                return "Numerical error — vehicle state reset";
            } else if constexpr (std::is_same_v<T, FailureEvent>) {
                return (e.started ? "Failure: " : "Failure ended: ") + e.title;
            } else if constexpr (std::is_same_v<T, PayloadAttachedEvent>) {
                return std::format("Payload attached ({:.2f} kg)", e.massKg);
            } else if constexpr (std::is_same_v<T, PayloadReleasedEvent>) {
                return "Payload released";
            } else if constexpr (std::is_same_v<T, ObjectiveCompletedEvent>) {
                return "Objective completed: " + (e.title.empty() ? e.objectiveId : e.title);
            } else if constexpr (std::is_same_v<T, ObjectiveFailedEvent>) {
                return "Objective failed: " + (e.title.empty() ? e.objectiveId : e.title) +
                       (e.reason.empty() ? std::string() : " (" + e.reason + ")");
            } else if constexpr (std::is_same_v<T, MissionMessageEvent>) {
                return e.text;
            } else if constexpr (std::is_same_v<T, FlightRuleBrokenEvent>) {
                return "Flight ended — rule broken: " + e.message;
            } else if constexpr (std::is_same_v<T, MissionSucceededEvent>) {
                return "Mission succeeded";
            } else if constexpr (std::is_same_v<T, MissionFailedEvent>) {
                return "Mission failed: " + e.reason;
            } else if constexpr (std::is_same_v<T, RolloverEvent>) {
                return "Rolled over — reset the vehicle";
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

World::World(WorldConfig config)
    : config_(std::move(config)), failures_(common::mix64(config_.seed ^ 0x494E4A45ULL)) {
    if (!config_.ground) {
        config_.ground = flatGround();
    }
    // World is neither copied nor moved: `this` stays valid.
    surfaceQuery_ = [this](const physics::Vec3& p) { return surfaceAt(p); };
    vehicle_ = physics::makeVehicle(config_.profile, config_.seed);
    weatherSettings_ = config_.weather.value_or(weather::WeatherSettings::fromBasic(
        config_.air, config_.wind, config_.visibilityM, config_.seed));
    rebuildWeather();
    clockBase_ = config_.startUtc;
    clockBaseTime_ = 0.0;
    reset();
    updateSun();
}

World::~World() = default;

physics::AerialVehicle* World::aerialVehicle() noexcept {
    return dynamic_cast<physics::AerialVehicle*>(vehicle_.get());
}

const physics::AerialVehicle* World::aerialVehicle() const noexcept {
    return dynamic_cast<const physics::AerialVehicle*>(vehicle_.get());
}

physics::GroundVehicle* World::groundVehicle() noexcept {
    return dynamic_cast<physics::GroundVehicle*>(vehicle_.get());
}

const physics::GroundVehicle* World::groundVehicle() const noexcept {
    return dynamic_cast<const physics::GroundVehicle*>(vehicle_.get());
}

void World::setGeoOrigin(const geo::Geodetic& origin) noexcept {
    config_.geoOrigin = origin;
    sunUpdatedAtS_ = -1e9;
    updateSun();
}

void World::updateSun() {
    if (time_ - sunUpdatedAtS_ < 1.0 && time_ >= sunUpdatedAtS_) {
        return;
    }
    sunUpdatedAtS_ = time_;
    sunElevationDeg_ = astro::sunPosition(utc(), config_.geoOrigin).apparent.elevationDeg;
}

void World::setMission(std::unique_ptr<MissionSession> session) {
    mission_ = std::move(session);
    if (freeMonitor_) {
        freeMonitor_->reset();
    }
}

void World::setFreeFlightRules(std::optional<FreeFlightRules> rules) {
    freeRules_ = rules;
    freeMonitor_.reset();
    if (rules) {
        freeMonitor_ = std::make_unique<missions::FlightRulesMonitor>(
            missions::RulesSpec{}, vehicle_->type(), rules->pilotPosition, nullptr,
            terrainLineOfSight(config_.ground));
    }
}

void World::checkFreeRules() {
    if (!freeMonitor_ || mission_) {
        return;
    }
    const physics::Vehicle& v = *vehicle_;
    const physics::RigidBodyState& body = v.bodyState();
    missions::FlightRuleSample r;
    r.timeS = time_;
    r.position = body.position;
    r.aglM = body.position.y - env_.ground.heightM - v.restHeightM();
    r.armed = v.armed() && !v.telemetry().crashed;
    r.lightsOn = controls_.lights;
    r.sunElevationDeg = sunElevationDeg_;
    r.visibilityM = visibilityAtVehicle();
    if (const auto broken = freeMonitor_->update(r); broken && freeRules_->enforce) {
        events_.publish(FlightRuleBrokenEvent{broken->message});
        freeMonitor_->reset();
    }
}

astro::UtcTime World::utc() const noexcept {
    const auto ms = static_cast<std::int64_t>(std::llround((time_ - clockBaseTime_) * 1000.0));
    return clockBase_ + std::chrono::milliseconds(ms);
}

void World::setUtc(astro::UtcTime utc) noexcept {
    clockBase_ = utc;
    clockBaseTime_ = time_;
    sunUpdatedAtS_ = -1e9;
}

const items::ItemRegistry& World::activeItems() const noexcept {
    return mission_ ? mission_->runner().items() : freeItems_;
}

physics::GroundSample World::surfaceAt(const physics::Vec3& point) const {
    // A deck counts if its top is at most this far above the point (rounding at the joins).
    constexpr double kClimbM = 0.02;
    physics::GroundSample ground = config_.ground(point.x, point.z);
    const items::ItemRegistry& registry = activeItems();
    if (registry.size() == 0) {
        return ground;
    }
    const double maxHeight = point.y + kClimbM;
    for (const items::ItemId id : registry.queryBox(physics::Vec3(point.x, -1.0e7, point.z),
                                                    physics::Vec3(point.x, maxHeight, point.z))) {
        const items::Item* item = registry.get(id);
        if (item == nullptr || !items::isDrivableType(item->type)) {
            continue;
        }
        if (const auto deck = items::driveSurfaceAt(*item, point.x, point.z, maxHeight);
            deck && deck->heightM > ground.heightM) {
            ground.heightM = deck->heightM;
            ground.normal = deck->normal;
            ground.material = physics::SurfaceMaterial::Paved;  // Planks and concrete.
            ground.friction = physics::surfaceProperties(ground.material).peakFriction;
        }
    }
    return ground;
}

physics::EnvironmentSample World::sampleEnvironment(const physics::Vec3& position) const {
    physics::EnvironmentSample env;
    env.ground = surfaceAt(position);
    env.groundPoint = physics::Vec3(position.x, env.ground.heightM, position.z);
    env.waterLevelM = config_.waterLevelM;
    env.groundQuery = &config_.ground;
    env.surfaceQuery = &surfaceQuery_;
    const weather::AirConditions& ground = air();
    env.air = physics::airAt(config_.originAltitudeM + position.y, ground,
                             config_.originAltitudeM + env.ground.heightM);
    env.qnhHpa = ground.pressureHpa;
    env.ambientC = ground.temperatureC;
    env.wind = wind_.velocity();
    env.precipitation = weather_.state().conditions.precipitation;
    env.groundCover = weather_.state().ground;
    return env;
}

void World::restartWind() {
    const weather::WeatherConditions& c = weather_.state().conditions;
    auto params = physics::WindParameters::fromLevel(config_.windTable.interpolated(c.windLevel),
                                                     c.windFromDeg);
    params.roughnessM = config_.roughnessM;
    wind_ = physics::WindModel(params, config_.seed + resets_ * 0x9E3779B97F4A7C15ULL);
    windLevelApplied_ = c.windLevel;
    windFromApplied_ = c.windFromDeg;
}

void World::applyWeatherToWind() {
    const weather::WeatherConditions& c = weather_.state().conditions;
    if (std::abs(c.windLevel - windLevelApplied_) < 1e-4 &&
        std::abs(c.windFromDeg - windFromApplied_) < 1e-3) {
        return;
    }
    auto params = physics::WindParameters::fromLevel(config_.windTable.interpolated(c.windLevel),
                                                     c.windFromDeg);
    params.roughnessM = config_.roughnessM;
    wind_.setParameters(params);
    windLevelApplied_ = c.windLevel;
    windFromApplied_ = c.windFromDeg;
}

void World::rebuildWeather() {
    const double now = weather_.state().timeS;
    weather_ =
        weather::WeatherGenerator(weatherSettings_, config_.weatherPresets, config_.windTable);
    weather_.seek(now);
}

void World::setWeather(const weather::WeatherSettings& settings) {
    weatherSettings_ = settings;
    rebuildWeather();
    applyWeatherToWind();
}

void World::setAir(const weather::AirConditions& air) {
    config_.air = air;
    weather::WeatherOverrides& o = weatherSettings_.overrides;
    o.temperatureC = air.temperatureC;
    o.pressureHpa = air.pressureHpa;
    o.humidityPct = air.humidityPct;
    o.dewPointC.reset();
    rebuildWeather();
}

void World::setVisibility(double visibilityM) {
    config_.visibilityM = visibilityM;
    weatherSettings_.overrides.visibilityM = visibilityM;
    rebuildWeather();
}

double World::visibilityAtVehicle() const noexcept {
    const physics::RigidBodyState& body = vehicle_->bodyState();
    return weather_.state().conditions.visibilityAt(
        std::max(0.0, body.position.y - env_.ground.heightM));
}

void World::reset() {
    ++resets_;
    weather_.restart();
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
    failures_.clear();
    vehicle_->setFaults({});
    stats_.startedAt = utc();
    if (freeMonitor_) {
        freeMonitor_->reset();
    }
    events_.publish(SimResetEvent{});
}

void World::injectFailure(const physics::Failure& failure) {
    failures_.inject(failure);
    events_.publish(FailureEvent{std::string(physics::failureTitle(failure.type)), true});
}

void World::clearFailures() {
    for (const physics::FailureInjector::Active& a : failures_.active()) {
        events_.publish(FailureEvent{std::string(physics::failureTitle(a.failure.type)), false});
    }
    failures_.clear();
    vehicle_->setFaults({});
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
    if (freeRules_) {
        setFreeFlightRules(freeRules_);  // Its line-of-sight test uses the terrain.
    }
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
    weatherSettings_.overrides.windLevel = wind.level;
    weatherSettings_.overrides.windFromDeg = wind.fromDeg;
    weatherSettings_.randomWindDirection = wind.randomDirection;
    rebuildWeather();
    restartWind();
}

void World::setProfile(const physics::VehicleProfile& profile) {
    const bool typeChanged = physics::profileType(profile) != vehicle_->type();
    config_.profile = profile;
    vehicle_ = physics::makeVehicle(config_.profile, config_.seed);
    if (typeChanged && freeRules_) {
        setFreeFlightRules(freeRules_);  // The rules depend on the vehicle type.
    }
    reset();
}

void World::publishVehicleEvents() {
    using Kind = physics::VehicleEvent::Kind;
    for (const physics::VehicleEvent& e : vehicle_->takeEvents()) {
        switch (e.kind) {
            case Kind::Collision:
                incidents_.push_back({missions::VehicleIncident::Kind::Collision, e.value, {}});
                events_.publish(CollisionEvent{e.value});
                break;
            case Kind::Crash:
                ++stats_.crashes;
                incidents_.push_back({missions::VehicleIncident::Kind::Crash, e.value, e.text});
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
            case Kind::WaterEntered:
                incidents_.push_back({missions::VehicleIncident::Kind::WaterEntered, 0.0, {}});
                events_.publish(WaterEnteredEvent{});
                break;
            case Kind::Armed: events_.publish(ArmingEvent{true}); break;
            case Kind::Disarmed: events_.publish(ArmingEvent{false}); break;
            case Kind::ArmRefused: events_.publish(ArmRefusedEvent{e.text}); break;
            case Kind::FailsafeOn: events_.publish(FailsafeEvent{true}); break;
            case Kind::FailsafeOff: events_.publish(FailsafeEvent{false}); break;
            case Kind::Rollover:
                incidents_.push_back({missions::VehicleIncident::Kind::Rollover, 0.0, {}});
                events_.publish(RolloverEvent{});
                break;
            case Kind::Stuck: events_.publish(VehicleStuckEvent{}); break;
            case Kind::NumericalReset:
                // A mission attempt with invalid numbers counts as failed (NFR-REL-02).
                incidents_.push_back(
                    {missions::VehicleIncident::Kind::Crash, 0.0, "numerical error"});
                events_.publish(NumericalResetEvent{});
                break;
        }
    }
}

void World::setFreeItems(const std::vector<items::Item>& freeItems) {
    freeItems_.clear();
    for (const items::Item& item : freeItems) {
        (void)freeItems_.add(item);  // Keys are unique: the scene's items were checked.
    }
    freeItems_.clearHistory();
}

void World::resolveObstacles() {
    const items::ItemRegistry& registry = activeItems();
    if (registry.size() == 0) {
        return;
    }
    // Items near the vehicle that stop it; gate posts reach a few metres below the opening.
    constexpr double kQueryRadiusM = 4.0;
    physics::Vehicle& v = *vehicle_;
    struct Near {
        const items::Item* item;
        double massKg;
    };
    std::vector<Near> near;
    for (const items::ItemId id : registry.queryRadius(v.bodyState().position, kQueryRadiusM)) {
        const items::Item* item = registry.get(id);
        if (item != nullptr && items::blocksVehicle(*item)) {
            near.push_back({item, items::massKg(*item)});
        }
    }
    if (near.empty()) {
        return;
    }
    v.resolveObstacles([&near](const physics::Vec3& p) -> std::optional<physics::ObstacleContact> {
        std::optional<physics::ObstacleContact> best;
        for (const auto& [item, mass] : near) {
            const auto c = items::obstacleContact(*item, p);
            if (c && (!best || c->depthM > best->depthM)) {
                best = physics::ObstacleContact{
                    .normal = c->normal,
                    .depthM = c->depthM,
                    .name = !item->name.empty()  ? item->name
                            : !item->key.empty() ? item->key
                                                 : std::string(items::itemTypeName(item->type)),
                    .restitution = c->surface.restitution,
                    .friction = c->surface.friction,
                    .massKg = mass};
            }
        }
        return best;
    });
}

void World::tick(const input::ControlInputs& controls, double dt) {
    controls_ = controls;
    physics::Vehicle& v = *vehicle_;
    // Failure injection (FR-PHY-15): sensor errors, motor power, battery and RC link.
    for (const physics::Failure& f : failures_.advance(dt)) {
        events_.publish(FailureEvent{std::string(physics::failureTitle(f.type)), false});
    }
    v.setFaults(failures_.vehicleFaults());
    for (const double fraction : failures_.takeBatteryDrops()) {
        v.loseBatteryCapacity(fraction);
    }
    if (failures_.rcLost()) {
        controls_.linkValid = false;
    }
    // Environment at the vehicle (terrain queried once per tick).
    const physics::RigidBodyState& body = v.bodyState();
    const physics::GroundSample g = config_.ground(body.position.x, body.position.z);
    weather_.advance(dt);
    applyWeatherToWind();
    wind_.update(dt, std::max(0.0, body.position.y - g.heightM), body.velocity);
    env_ = sampleEnvironment(body.position);

    // FR-LOOP-05: sensors → controller → physics → collisions → items and missions.
    incidents_.clear();
    v.updateSensors(env_, dt);
    v.updateController(toPilotCommand(controls_), dt);
    v.integrate(env_, dt);
    v.resolveCollisions(env_, dt);
    resolveObstacles();
    publishVehicleEvents();

    time_ += dt;
    ++ticks_;
    updateSun();
    if (mission_) {
        mission_->tick(*this, incidents_);
    } else {
        checkFreeRules();
    }

    // Statistics.
    const physics::RigidBodyState& after = v.bodyState();
    stats_.durationS = time_;
    const bool armed = v.armed();
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
    const physics::Vehicle& v = *vehicle_;
    s.tick = ticks_;
    s.simTimeS = time_;
    s.utc = utc();
    s.hasVehicle = true;
    s.profileKey = physics::profileKey(config_.profile);
    s.profileName = physics::profileName(config_.profile);
    s.body = v.bodyState();
    s.controls = controls_;
    s.activeFailures.clear();
    for (const physics::FailureInjector::Active& a : failures_.active()) {
        s.activeFailures.emplace_back(physics::failureTitle(a.failure.type));
    }
    s.attitude = physics::eulerFromOrientation(s.body.orientation);
    s.telemetry = v.telemetry();
    const physics::GroundSample g = surfaceAt(s.body.position);
    s.groundHeightM = g.heightM;
    s.aglM = s.body.position.y - g.heightM - v.restHeightM();
    s.originAltitudeM = config_.originAltitudeM;
    s.home = home_;
    const weather::WeatherState& w = weather_.state();
    const weather::WindLevel level = config_.windTable.interpolated(w.conditions.windLevel);
    s.windLevel = static_cast<int>(std::lround(w.conditions.windLevel));
    s.windFromDeg = wind_.currentFromDeg();
    s.windMeanSpeedMs = level.meanSpeedMs;
    s.windGustMs = level.gustPeakMs;
    s.weatherTimeS = w.timeS;
    s.weather = w.conditions;
    s.cloudDrift = w.cloudDrift;
    s.groundCover = w.ground;
    s.visibilityM = visibilityAtVehicle();
    s.stats = stats_;
    s.resetCount = resets_;
    s.sunElevationDeg = sunElevationDeg_;
    if (mission_) {
        mission_->fill(s.mission);
        s.ruleWarnings = mission_->runner().warnings();
    } else {
        s.mission.active = false;
        s.mission.objectives.clear();
        s.mission.path.clear();
        s.mission.result.reset();
        if (freeMonitor_) {
            s.ruleWarnings = freeMonitor_->warnings();
        } else {
            s.ruleWarnings.clear();
        }
    }
}

}  // namespace ds::sim
