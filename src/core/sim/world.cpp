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
    recordCommand({{"kind", "geoOrigin"},
                   {"value", {origin.latitudeDeg, origin.longitudeDeg, origin.heightM}}});
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
    if (recording_ && recording_->encoder.ticks() == 0) {
        recording_->recording.replay.header["mission"] =
            mission_ ? mission_->runner().mission().id : std::string();
    } else {
        finishRecording();  // The session ends with its mission; the next reset starts one.
    }
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
    recordCommand({{"kind", "utc"}, {"ms", utc.time_since_epoch().count()}});
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
    if (platform_ && platform_->covers(point.x, point.z) &&
        platform_->centre.y <= point.y + kClimbM && platform_->centre.y > ground.heightM) {
        ground.heightM = platform_->centre.y;
        ground.normal = physics::Vec3(0.0, 1.0, 0.0);
        ground.material = physics::SurfaceMaterial::Paved;
        ground.friction = physics::surfaceProperties(ground.material).peakFriction;
    }
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
    recordCommand({{"kind", "weather"}, {"value", settings.toJson()}});
    weatherSettings_ = settings;
    rebuildWeather();
    applyWeatherToWind();
}

void World::setAir(const weather::AirConditions& air) {
    recordCommand(
        {{"kind", "air"}, {"value", {air.temperatureC, air.pressureHpa, air.humidityPct}}});
    config_.air = air;
    weather::WeatherOverrides& o = weatherSettings_.overrides;
    o.temperatureC = air.temperatureC;
    o.pressureHpa = air.pressureHpa;
    o.humidityPct = air.humidityPct;
    o.dewPointC.reset();
    rebuildWeather();
}

void World::setVisibility(double visibilityM) {
    recordCommand({{"kind", "visibility"}, {"value", visibilityM}});
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
    finishRecording();
    ++resets_;
    sessionStartS_ = time_;
    sessionStartTick_ = ticks_;
    weather_.restart();
    restartWind();
    const physics::Vec3 start = config_.startPosition;
    placeLaunchPlatform();
    // Air where the vehicle will rest (the barometer starts from it).
    double ground = config_.ground(start.x, start.z).heightM;
    if (platform_) {
        ground = std::max(ground, platform_->centre.y);
    }
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
    sunUpdatedAtS_ = -1e9;  // Every session starts from the same sun (FR-X-05).
    updateSun();
    beginRecording();
    events_.publish(SimResetEvent{});
}

void World::injectFailure(const physics::Failure& failure) {
    recordCommand({{"kind", "failure"}, {"value", failureToJson(failure)}});
    failures_.inject(failure);
    events_.publish(FailureEvent{std::string(physics::failureTitle(failure.type)), true});
}

void World::clearFailures() {
    recordCommand({{"kind", "clearFailures"}});
    for (const physics::FailureInjector::Active& a : failures_.active()) {
        events_.publish(FailureEvent{std::string(physics::failureTitle(a.failure.type)), false});
    }
    failures_.clear();
    vehicle_->setFaults({});
}

void World::placeLaunchPlatform() {
    platform_.reset();
    if (const auto* aerial = std::get_if<physics::AerialProfile>(&config_.profile)) {
        const physics::Vec3 start = config_.startPosition;
        platform_ =
            launchPlatformFor(config_.ground, start.x, start.z,
                              launchPlatformHalfSizeM(footprintRadiusM(vehicle_->collisionPoints()),
                                                      aerial->sensors.gpsHorizontalNoiseM));
    }
}

void World::resetAt(const physics::Vec3& position, double headingRad) {
    config_.startPosition = position;
    config_.startHeadingRad = headingRad;
    reset();
}

void World::setGround(GroundFunction ground, std::optional<double> waterLevelM,
                      double originAltitudeM) {
    const bool restart = sceneChangedMidSession();
    config_.ground = ground ? std::move(ground) : flatGround();
    config_.waterLevelM = waterLevelM;
    config_.originAltitudeM = originAltitudeM;
    if (freeRules_) {
        setFreeFlightRules(freeRules_);  // Its line-of-sight test uses the terrain.
    }
    placeLaunchPlatform();  // On the new ground.
    // Lift the vehicle onto the new surface if it is now underground.
    physics::RigidBodyState body = vehicle_->bodyState();
    double surface = config_.ground(body.position.x, body.position.z).heightM;
    if (platform_ && platform_->covers(body.position.x, body.position.z)) {
        surface = std::max(surface, platform_->centre.y);
    }
    const double minimum = surface + vehicle_->restHeightM();
    if (body.position.y < minimum) {
        body.position.y = minimum + 0.002;
        body.velocity = physics::Vec3(0.0);
        vehicle_->setBodyState(body);
    }
    if (restart) {
        reset();
    }
}

void World::setWind(const weather::WindSettings& wind) {
    recordCommand({{"kind", "wind"},
                   {"level", wind.level},
                   {"fromDeg", wind.fromDeg},
                   {"randomDirection", wind.randomDirection}});
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
    const bool restart = sceneChangedMidSession();
    freeItems_.clear();
    for (const items::Item& item : freeItems) {
        (void)freeItems_.add(item);  // Keys are unique: the scene's items were checked.
    }
    freeItems_.clearHistory();
    if (restart) {
        reset();
    }
}

void World::resolveObstacles() {
    const items::ItemRegistry& registry = activeItems();
    // Items near the vehicle that stop it; gate posts reach a few metres below the opening.
    constexpr double kQueryRadiusM = 4.0;
    physics::Vehicle& v = *vehicle_;
    const physics::Vec3 position = v.bodyState().position;
    struct Near {
        const items::Item* item;
        double massKg;
    };
    std::vector<Near> near;
    if (registry.size() != 0) {
        for (const items::ItemId id : registry.queryRadius(position, kQueryRadiusM)) {
            const items::Item* item = registry.get(id);
            if (item != nullptr && items::blocksVehicle(*item)) {
                near.push_back({item, items::massKg(*item)});
            }
        }
    }
    // The launch platform's sides (and its top where it is not the vehicle's ground).
    const LaunchPlatform* platform = nullptr;
    bool topIsGround = false;
    if (platform_ &&
        std::abs(position.x - platform_->centre.x) < platform_->halfSizeM + kQueryRadiusM &&
        std::abs(position.z - platform_->centre.z) < platform_->halfSizeM + kQueryRadiusM) {
        platform = &*platform_;
        topIsGround =
            env_.ground.heightM == platform_->centre.y && platform_->covers(position.x, position.z);
    }
    if (near.empty() && platform == nullptr) {
        return;
    }
    v.resolveObstacles([&near, platform, topIsGround](
                           const physics::Vec3& p) -> std::optional<physics::ObstacleContact> {
        std::optional<physics::ObstacleContact> best;
        if (platform != nullptr) {
            best = launchPlatformContact(*platform, p, topIsGround);
        }
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

void World::tick(const input::ControlInputs& liveControls, double liveDt) {
    input::ControlInputs controls = liveControls;
    double dt = liveDt;
    if (replay_) {
        if (replay_->finished) {
            return;  // The last state stays.
        }
        applyReplayCommands();
        const auto next = replay_->decoder->next();
        if (!next) {
            replay_->finished = true;
            return;
        }
        controls = *next;
        dt = replay_->dt;
        ++replay_->tick;
        replay_->timeS += dt;
    } else if (recording_) {
        controls = quantized(controls);
        recordTick(controls, dt);
    }
    inTick_ = true;
    tickBody(controls, dt);
    inTick_ = false;
    if (recording_) {
        sampleTelemetry();
    }
}

void World::tickBody(const input::ControlInputs& controls, double dt) {
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
    stats_.durationS = time_ - sessionStartS_;
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
    s.launchPlatform = platform_;
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
    s.replay = replayStatus();
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

// --- Missions ---------------------------------------------------------------------------------

void World::retryMission() {
    recordCommand({{"kind", "retryMission"}});
    if (mission_) {
        mission_->retry(*this);
    }
}

void World::abortMission() {
    recordCommand({{"kind", "abortMission"}});
    if (mission_) {
        mission_->abort(*this);
    }
}

// --- Recording (FR-X-06) --------------------------------------------------------------------

void World::setRecording(bool enabled) {
    recordingEnabled_ = enabled;
    if (!enabled) {
        recording_.reset();
    } else if (!recording_ && ticks_ == sessionStartTick_) {
        beginRecording();  // Nothing happened since the reset: this session is recorded.
    }
}

void World::setSceneTag(common::Json scene) {
    sceneTag_ = std::move(scene);
    if (recording_ && recording_->encoder.ticks() == 0) {
        recording_->recording.replay.header["scene"] = sceneTag_;
    }
}

void World::beginRecording() {
    if (!recordingEnabled_ || replay_) {
        return;
    }
    recording_.emplace();
    FlightRecording& r = recording_->recording;
    r.profileKey = physics::profileKey(config_.profile);
    r.type = vehicle_->type();
    const physics::Vec3& start = config_.startPosition;
    r.replay.header = {
        {"format", ReplayLog::kFormat},
        {"seed", config_.seed},
        {"resets", resets_},
        {"timeS", time_},
        {"ticks", ticks_},
        {"clockBaseMs", clockBase_.time_since_epoch().count()},
        {"clockBaseTimeS", clockBaseTime_},
        {"profile", physics::vehicleProfileToJson(config_.profile)},
        {"weather", weatherSettings_.toJson()},
        {"air", {config_.air.temperatureC, config_.air.pressureHpa, config_.air.humidityPct}},
        {"wind",
         {{"level", config_.wind.level},
          {"fromDeg", config_.wind.fromDeg},
          {"randomDirection", config_.wind.randomDirection}}},
        {"visibilityM", config_.visibilityM},
        {"geoOrigin",
         {config_.geoOrigin.latitudeDeg, config_.geoOrigin.longitudeDeg,
          config_.geoOrigin.heightM}},
        {"start", {start.x, start.y, start.z}},
        {"headingRad", config_.startHeadingRad},
        {"scene", sceneTag_},
        {"mission", mission_ ? mission_->runner().mission().id : std::string()},
    };
}

void World::recordCommand(common::Json command) {
    if (!recording_ || inTick_) {
        return;  // Changes the world makes itself (missions) replay by themselves.
    }
    recording_->recording.replay.commands.push_back(
        {recording_->encoder.ticks(), std::move(command)});
}

void World::recordTick(const input::ControlInputs& controls, double dt) {
    ActiveRecording& r = *recording_;
    ReplayLog& log = r.recording.replay;
    if (log.truncated) {
        return;
    }
    if (time_ - sessionStartS_ >= kMaxRecordedS) {
        log.truncated = true;
        return;
    }
    if (r.encoder.ticks() == 0) {
        log.header["dt"] = dt;
    } else if (dt != r.lastDt) {
        log.commands.push_back({r.encoder.ticks(), {{"kind", "dt"}, {"value", dt}}});
    }
    r.lastDt = dt;
    r.encoder.add(controls);
}

void World::sampleTelemetry() {
    ActiveRecording& r = *recording_;
    const double t = time_ - sessionStartS_;
    if (r.recording.replay.truncated ||
        t + 1e-9 < static_cast<double>(r.samples) / kTelemetryRateHz) {
        return;
    }
    ++r.samples;
    const physics::Vehicle& v = *vehicle_;
    const physics::RigidBodyState& body = v.bodyState();
    const physics::Euler e = physics::eulerFromOrientation(body.orientation);
    const bool aerial = v.type() == physics::VehicleType::Aerial;
    r.recording.samples.push_back({
        .tS = t,
        .position = body.position,
        .rollRad = e.roll,
        .pitchRad = e.pitch,
        .yawRad = e.yaw,
        .speedMs = glm::length(body.velocity),
        .aglM = body.position.y - env_.ground.heightM - v.restHeightM(),
        .throttle = aerial ? controls_.throttle : controls_.drive,
        .steering = aerial ? controls_.roll : controls_.steering,
        .batteryV = v.battery().voltage(),
        .batteryA = v.battery().current(),
    });
}

bool World::sceneChangedMidSession() {
    if (!recording_ || recording_->encoder.ticks() == 0) {
        return false;
    }
    if (stats_.flightTimeS == 0.0) {
        // Not flown yet: a new session starts on the new scene, still replayable. The vehicle
        // stands at its take-off point; the reset puts it on the new surface.
        return true;
    }
    recording_->recording.replay.replayable = false;
    return false;
}

void World::finishRecording() {
    if (!recording_) {
        return;
    }
    ActiveRecording& r = *recording_;
    if (r.encoder.ticks() > 0) {
        FlightRecording& out = r.recording;
        out.stats = stats_;
        out.missionId = out.replay.header.value("mission", std::string());
        out.replay.ticks = r.encoder.ticks();
        out.replay.inputs = r.encoder.take();
        finished_.push_back(std::move(out));
    }
    recording_.reset();
}

std::vector<FlightRecording> World::takeRecordings() { return std::exchange(finished_, {}); }

// --- Replay (FR-X-06) -----------------------------------------------------------------------

common::Result<void> World::startReplay(ReplayLog log, std::unique_ptr<MissionSession> mission) {
    auto replay = std::make_unique<ActiveReplay>();
    replay->log = std::move(log);
    const common::Json& h = replay->log.header;
    if (!h.is_object() || h.value("format", 0) != ReplayLog::kFormat || !h.contains("dt")) {
        return common::makeError("the replay is damaged or from another version");
    }
    auto profile = physics::vehicleProfileFromJson(h.value("profile", common::Json()));
    if (!profile) {
        return common::makeError("replay vehicle: " + profile.error().message);
    }
    auto weather = weather::WeatherSettings::fromJson(h.value("weather", common::Json()),
                                                      config_.weatherPresets);
    if (!weather) {
        return common::makeError("replay weather: " + weather.error().message);
    }
    const common::Json start = h.value("start", common::Json::array());
    const common::Json geo = h.value("geoOrigin", common::Json::array());
    const common::Json air = h.value("air", common::Json::array());
    if (start.size() != 3 || geo.size() != 3 || air.size() != 3) {
        return common::makeError("the replay is damaged");
    }
    auto decoder = InputDecoder::open(replay->log.inputs);
    if (!decoder) {
        return common::makeError(decoder.error().message);
    }
    replay->decoder = std::move(*decoder);
    replay->dt = h.value("dt", 0.002);
    replay->durationS = replay->log.durationS();

    // The world as it was when the session started.
    finishRecording();
    config_.seed = h.value("seed", std::uint64_t{1});
    config_.profile = std::move(*profile);
    vehicle_ = physics::makeVehicle(config_.profile, config_.seed);
    failures_ = physics::FailureInjector(common::mix64(config_.seed ^ 0x494E4A45ULL));
    config_.air = {air[0].get<double>(), air[1].get<double>(), air[2].get<double>()};
    const common::Json wind = h.value("wind", common::Json::object());
    config_.wind = {wind.value("level", 0), wind.value("fromDeg", 270.0),
                    wind.value("randomDirection", false)};
    config_.visibilityM = h.value("visibilityM", config_.visibilityM);
    config_.geoOrigin = {geo[0].get<double>(), geo[1].get<double>(), geo[2].get<double>()};
    config_.startPosition = {start[0].get<double>(), start[1].get<double>(),
                             start[2].get<double>()};
    config_.startHeadingRad = h.value("headingRad", 0.0);
    weatherSettings_ = *weather;
    rebuildWeather();
    time_ = h.value("timeS", 0.0);
    ticks_ = h.value("ticks", std::uint64_t{0});
    clockBase_ = astro::UtcTime(std::chrono::milliseconds(h.value("clockBaseMs", std::int64_t{0})));
    clockBaseTime_ = h.value("clockBaseTimeS", 0.0);
    resets_ = h.value("resets", std::uint64_t{1}) - 1;
    mission_ = std::move(mission);
    if (freeRules_) {
        setFreeFlightRules(freeRules_);  // For the replayed vehicle type.
    }
    replay_ = std::move(replay);
    reset();
    return {};
}

void World::stopReplay() { replay_.reset(); }

ReplayStatus World::replayStatus() const noexcept {
    if (!replay_) {
        return {};
    }
    return {.active = true,
            .timeS = replay_->timeS,
            .durationS = replay_->durationS,
            .finished = replay_->finished};
}

void World::applyReplayCommands() {
    ActiveReplay& r = *replay_;
    const std::vector<ReplayCommand>& commands = r.log.commands;
    while (r.nextCommand < commands.size() && commands[r.nextCommand].tick <= r.tick) {
        const common::Json& c = commands[r.nextCommand++].data;
        const std::string kind = c.value("kind", std::string());
        const common::Json& v = c.contains("value") ? c["value"] : c;
        if (kind == "dt") {
            r.dt = v.get<double>();
        } else if (kind == "failure") {
            if (const auto f = failureFromJson(v)) {
                injectFailure(*f);
            }
        } else if (kind == "clearFailures") {
            clearFailures();
        } else if (kind == "weather") {
            if (auto w = weather::WeatherSettings::fromJson(v, config_.weatherPresets)) {
                setWeather(*w);
            }
        } else if (kind == "air" && v.size() == 3) {
            setAir({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
        } else if (kind == "wind") {
            setWind({c.value("level", 0), c.value("fromDeg", 270.0),
                     c.value("randomDirection", false)});
        } else if (kind == "visibility") {
            setVisibility(v.get<double>());
        } else if (kind == "utc") {
            setUtc(astro::UtcTime(std::chrono::milliseconds(c.value("ms", std::int64_t{0}))));
        } else if (kind == "geoOrigin" && v.size() == 3) {
            setGeoOrigin({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
        } else if (kind == "retryMission") {
            retryMission();
        } else if (kind == "abortMission") {
            abortMission();
        }
    }
}

}  // namespace ds::sim
