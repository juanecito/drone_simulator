// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/simulation_controller.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/common/log.hpp"
#include "ds/storage/input_profile_store.hpp"
#include "ds/storage/session_store.hpp"
#include "ds/storage/storage_service.hpp"
#include "ds/storage/vehicle_profile_store.hpp"

namespace ds::app {
namespace {

constexpr double kPilotEyeHeightM = 1.7;
constexpr double kPilotBehindHomeM = 4.0;
constexpr double kMaxExtrapolationS = 0.05;
constexpr double kMinRecordedFlightS = 1.0;
constexpr double kChaseArmLengths = 14.0;
constexpr double kMinChaseDistanceM = 1.5;
constexpr double kMaxChaseDistanceM = 6.0;
constexpr const char* kVehicleType = "aerial";

QString slug(const QString& name) {
    QString s = name.trimmed().toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    s.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return s.isEmpty() ? QStringLiteral("profile") : s;
}

const char* cameraModeKey(graphics::CameraMode mode) {
    switch (mode) {
        case graphics::CameraMode::Fpv: return "fpv";
        case graphics::CameraMode::Chase: return "chase";
        case graphics::CameraMode::LineOfSight: return "los";
        case graphics::CameraMode::Free: break;
    }
    return "free";
}

std::optional<graphics::CameraMode> cameraModeFromKey(const QString& key) {
    for (int i = 0; i < graphics::kCameraModeCount; ++i) {
        const auto mode = static_cast<graphics::CameraMode>(i);
        if (key == QLatin1String(cameraModeKey(mode))) {
            return mode;
        }
    }
    return std::nullopt;
}

// Validates a profile the same way a file or a database row is validated.
common::Result<physics::AerialProfile> validated(const physics::AerialProfile& profile) {
    return physics::aerialProfileFromJson(physics::aerialProfileToJson(profile));
}

// Nose direction (world) for a heading clockwise from north.
glm::dvec3 headingVector(double headingRad) {
    return {std::sin(headingRad), 0.0, -std::cos(headingRad)};
}

}  // namespace

SimulationController::SimulationController(SceneController& scene, SettingsController* settings,
                                           const std::filesystem::path& assetDirectory,
                                           bool joysticks, QObject* parent)
    : QObject(parent), scene_(&scene), settings_(settings) {
    loadWindTable(assetDirectory);
    loadBuiltInProfiles(assetDirectory);
    syncBuiltInProfiles();
    reloadProfiles();

    if (joysticks) {
        joysticks_ = std::make_unique<input::JoystickService>();
        if (auto started = joysticks_->start(); !started) {
            log::warn("Joysticks unavailable: {}", started.error().message);
            joysticks_.reset();
        }
    }
    selector_ = std::make_unique<input::InputSelector>(joysticks_.get());

    // Take-off point under the free camera, facing the same way.
    const graphics::Camera& free = scene.camera();
    home_ = glm::dvec3(free.position.x, 0.0, free.position.z);
    homeHeadingRad_ = free.yawRad;
    lastEnvironment_ = scene.environment();
    restoreSelections();
    createLoop();
    // Through the scene: its query object is replaced when the terrain changes.
    rig_.setGround([scene = &scene](double x, double z) -> std::optional<double> {
        return scene->query().height(x, z);
    });
    connect(&scene, &SceneController::terrainChanged, this,
            &SimulationController::onTerrainChanged);
    connect(&scene, &SceneController::environmentChanged, this,
            &SimulationController::onEnvironmentChanged);
}

SimulationController::~SimulationController() {
    loop_->shutdown();
    loop_->acquireSnapshot();
    recordSession(loop_->snapshot().stats, loop_->snapshot().profileKey);
    scene_->followSimulationTime(std::nullopt);
}

// --- Set-up ---------------------------------------------------------------------------------

void SimulationController::loadWindTable(const std::filesystem::path& assets) {
    const auto file = assets / "environment" / "wind_levels.json";
    if (auto table = weather::WindLevelTable::load(file)) {
        windTable_ = std::move(*table);
    } else {
        log::warn("Using the built-in wind levels: {}", table.error().message);
    }
}

void SimulationController::loadBuiltInProfiles(const std::filesystem::path& assets) {
    const auto dir = assets / "vehicles" / kVehicleType;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
        if (auto profile = physics::loadAerialProfile(file)) {
            builtIn_.push_back(std::move(*profile));
        } else {
            log::warn("Skipping vehicle profile {}: {}", file.string(), profile.error().message);
        }
    }
    if (builtIn_.empty()) {
        // Never run without a vehicle: fall back to the compiled-in defaults.
        log::error("No vehicle profiles in {}; using the generic defaults", dir.string());
        physics::AerialProfile generic;
        generic.key = "generic";
        generic.name = "Generic quadcopter";
        builtIn_.push_back(std::move(generic));
    }
}

void SimulationController::syncBuiltInProfiles() {
    if (settings_ == nullptr) {
        return;
    }
    std::vector<storage::VehicleProfileRecord> records;
    for (const auto& p : builtIn_) {
        records.push_back({.key = p.key,
                           .vehicleType = kVehicleType,
                           .name = p.name,
                           .origin = "builtin",
                           .paramsJson = physics::aerialProfileToJson(p).dump()});
    }
    auto report = settings_->storage()
                      .submit([records](storage::DbConnection& c) {
                          storage::VehicleProfileStore store(c);
                          return store.syncBuiltIn(records);
                      })
                      .get();
    if (!report) {
        log::warn("Cannot store the built-in vehicle profiles: {}", report.error().message);
    } else if (report->inserted + report->updated > 0 || report->skipped > 0) {
        log::info("Vehicle profiles: {} added, {} updated, {} keys taken by user profiles",
                  report->inserted, report->updated, report->skipped);
    }
}

void SimulationController::reloadProfiles() {
    profiles_.clear();
    for (const auto& p : builtIn_) {
        profiles_.append({QString::fromStdString(p.key), QString::fromStdString(p.name), true});
    }
    if (settings_ == nullptr) {
        return;
    }
    auto records = settings_->storage()
                       .submit([](storage::DbConnection& c) {
                           storage::VehicleProfileStore store(c);
                           return store.list(kVehicleType);
                       })
                       .get();
    if (!records) {
        log::warn("Cannot list vehicle profiles: {}", records.error().message);
        return;
    }
    for (const auto& r : *records) {
        if (r.origin == "user") {
            profiles_.append(
                {QString::fromStdString(r.key), QString::fromStdString(r.name), false});
        }
    }
}

void SimulationController::restoreSelections() {
    QString key = QString::fromLatin1(kDefaultProfile);
    if (settings_ != nullptr) {
        if (const auto json = settings_->readJson(sim_settings_keys::kProfile)) {
            const QJsonDocument doc = QJsonDocument::fromJson("[" + *json + "]");
            if (const QString saved = doc.array().at(0).toString(); !saved.isEmpty()) {
                key = saved;
            }
        }
        if (const auto json = settings_->readJson(sim_settings_keys::kCameraMode)) {
            const QJsonDocument doc = QJsonDocument::fromJson("[" + *json + "]");
            cameraMode_ = cameraModeFromKey(doc.array().at(0).toString())
                              .value_or(graphics::CameraMode::Free);
        }
    }
    auto profile = profileFor(key);
    if (!profile) {
        log::warn("Vehicle profile {} not available: {}", key.toStdString(),
                  profile.error().message);
        profile = profileFor(QString::fromLatin1(kDefaultProfile));
    }
    profile_ = profile ? *profile : builtIn_.front();
    model_.reset();

    // Joystick profile chosen last time (applied even if the device is not plugged in yet:
    // the selector falls back to the keyboard until it appears).
    if (settings_ == nullptr) {
        return;
    }
    const auto json = settings_->readJson(sim_settings_keys::kJoystick);
    if (!json) {
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(*json).object();
    const std::string guid = o.value(QStringLiteral("guid")).toString().toStdString();
    const std::string name = o.value(QStringLiteral("name")).toString().toStdString();
    if (guid.empty()) {
        return;
    }
    auto record = settings_->storage()
                      .submit([guid, name](storage::DbConnection& c) {
                          storage::InputProfileStore store(c);
                          return store.find(guid, kVehicleType, name);
                      })
                      .get();
    if (!record || !*record) {
        return;
    }
    auto parsed = common::parseJson((*record)->mappingJson);
    if (!parsed) {
        return;
    }
    if (auto mapping = input::InputMapping::fromJson(*parsed)) {
        mapping_ = std::move(*mapping);
        selector_->setJoystick(mapping_);
    } else {
        log::warn("Ignoring the saved joystick mapping: {}", mapping.error().message);
    }
}

sim::GroundFunction SimulationController::groundFunction() const {
    // A private query object: the world reads it from the simulation thread.
    auto query = std::make_shared<const terrain::TerrainQuery>(scene_->source(), scene_->layout(),
                                                               scene_->terrain().origin.heightM);
    return [query](double x, double z) {
        physics::GroundSample g;
        g.heightM = query->height(x, z);
        const terrain::Normal n = query->normal(x, z);
        g.normal = physics::Vec3(n.x, n.y, n.z);
        return g;
    };
}

void SimulationController::createLoop() {
    sim::WorldConfig c;
    c.profile = profile_;
    c.seed = std::random_device{}();
    c.ground = groundFunction();
    c.waterLevelM = scene_->source()->waterLevel();
    c.originAltitudeM = scene_->terrain().origin.heightM;
    c.air = lastEnvironment_.air;
    c.wind = lastEnvironment_.wind;
    c.windTable = windTable_;
    c.startPosition = home_;
    c.startHeadingRad = homeHeadingRad_;
    c.startUtc = lastEnvironment_.time;
    input::InputSelector* selector = selector_.get();
    loop_ =
        std::make_unique<sim::SimulationLoop>(std::make_unique<sim::World>(std::move(c)),
                                              [selector](double dt) { return selector->poll(dt); });
    loop_->acquireSnapshot();
    lastResetCount_ = loop_->snapshot().resetCount;
    lastProfileKey_ = profile_.key;
    updatePilotEye();
    rig_.reset({.position = loop_->snapshot().body.position,
                .orientation = loop_->snapshot().body.orientation});
}

void SimulationController::updatePilotEye() {
    const glm::dvec3 behind = home_ - kPilotBehindHomeM * headingVector(homeHeadingRad_);
    const double ground = scene_->query().height(behind.x, behind.z);
    rig_.setPilotEye({behind.x, ground + kPilotEyeHeightM, behind.z});
}

// --- Profiles -------------------------------------------------------------------------------

common::Result<physics::AerialProfile> SimulationController::profileFor(const QString& key) const {
    const std::string k = key.toStdString();
    for (const auto& p : builtIn_) {
        if (p.key == k) {
            return p;
        }
    }
    if (settings_ == nullptr) {
        return common::makeError("Unknown vehicle profile.");
    }
    auto record = settings_->storage()
                      .submit([k](storage::DbConnection& c) {
                          storage::VehicleProfileStore store(c);
                          return store.find(k);
                      })
                      .get();
    if (!record) {
        return common::makeError(record.error().message);
    }
    if (!*record) {
        return common::makeError("Unknown vehicle profile.");
    }
    auto json = common::parseJson((*record)->paramsJson);
    if (!json) {
        return common::makeError("Vehicle profile " + k + ": " + json.error().message);
    }
    auto profile = physics::aerialProfileFromJson(*json);
    if (!profile) {
        return common::makeError("Vehicle profile " + k + ": " + profile.error().message);
    }
    profile->key = k;
    return profile;
}

void SimulationController::setFlownProfile(const physics::AerialProfile& profile, bool modified) {
    profile_ = profile;
    modified_ = modified;
    model_.reset();
    loop_->post([profile](sim::World& w) { w.setProfile(profile); });
    selector_->keyboardAction(input::KeyboardAction::Reset);
    emit profileChanged();
}

common::Result<void> SimulationController::selectProfile(const QString& key) {
    auto profile = profileFor(key);
    if (!profile) {
        return common::makeError(profile.error().message);
    }
    setFlownProfile(*profile, false);
    if (settings_ != nullptr) {
        // A JSON string: the array's only element.
        settings_->writeJson(
            sim_settings_keys::kProfile,
            QJsonDocument(QJsonArray{key}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
    }
    return {};
}

common::Result<void> SimulationController::applyProfile(const physics::AerialProfile& profile) {
    auto checked = validated(profile);
    if (!checked) {
        return common::makeError(checked.error().message);
    }
    checked->key = profile_.key;
    setFlownProfile(*checked, true);
    return {};
}

common::Result<QString> SimulationController::saveProfileAs(const QString& name) {
    if (settings_ == nullptr) {
        return common::makeError("Profiles cannot be saved without a database.");
    }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 80) {
        return common::makeError("A profile name must have 1 to 80 characters.");
    }
    physics::AerialProfile p = profile_;
    p.key = (QStringLiteral("user.") + slug(trimmed)).toStdString();
    p.name = trimmed.toStdString();
    p.reference.reset();  // Reference figures belong to the built-in profiles.
    auto checked = validated(p);
    if (!checked) {
        return common::makeError(checked.error().message);
    }
    const storage::VehicleProfileRecord record{
        .key = p.key,
        .vehicleType = kVehicleType,
        .name = p.name,
        .origin = "user",
        .paramsJson = physics::aerialProfileToJson(p).dump()};
    auto saved = settings_->storage()
                     .submit([record](storage::DbConnection& c) {
                         storage::VehicleProfileStore store(c);
                         return store.saveUser(record);
                     })
                     .get();
    if (!saved) {
        return common::makeError(saved.error().message);
    }
    reloadProfiles();
    emit profilesChanged();
    const QString key = QString::fromStdString(p.key);
    if (auto selected = selectProfile(key); !selected) {
        return common::makeError(selected.error().message);
    }
    return key;
}

common::Result<void> SimulationController::deleteProfile(const QString& key) {
    if (settings_ == nullptr || !key.startsWith(QLatin1String("user."))) {
        return common::makeError("Built-in profiles cannot be deleted.");
    }
    auto removed = settings_->storage()
                       .submit([k = key.toStdString()](storage::DbConnection& c) {
                           storage::VehicleProfileStore store(c);
                           return store.remove(k);
                       })
                       .get();
    if (!removed) {
        return common::makeError(removed.error().message);
    }
    reloadProfiles();
    emit profilesChanged();
    if (profile_.key == key.toStdString()) {
        return selectProfile(QString::fromLatin1(kDefaultProfile));
    }
    return {};
}

// --- Loop -----------------------------------------------------------------------------------

void SimulationController::start() {
    if (!started_) {
        // From now on the simulated clock drives the sky; start from the scene's time.
        started_ = true;
        const astro::UtcTime now = scene_->environment().time;
        loop_->post([now](sim::World& w) { w.setUtc(now); });
    }
    if (loop_->state() == sim::RunState::Paused) {
        loop_->resume();
    } else {
        loop_->start();
    }
    if (cameraMode_ == graphics::CameraMode::Free) {
        setCameraMode(graphics::CameraMode::Chase);
    }
}

void SimulationController::pause() { loop_->pause(); }

void SimulationController::togglePause() {
    if (loop_->state() == sim::RunState::Running) {
        pause();
    } else {
        start();
    }
}

void SimulationController::step() {
    if (loop_->state() == sim::RunState::Running) {
        loop_->pause();
    }
    loop_->step();
}

void SimulationController::reset() {
    selector_->keyboardAction(input::KeyboardAction::Reset);
    loop_->post([](sim::World& w) { w.reset(); });
}

void SimulationController::moveHomeToCamera() {
    const graphics::Camera& free = scene_->camera();
    home_ = glm::dvec3(free.position.x, 0.0, free.position.z);
    homeHeadingRad_ = free.yawRad;
    updatePilotEye();
    selector_->keyboardAction(input::KeyboardAction::Reset);
    loop_->post(
        [home = home_, heading = homeHeadingRad_](sim::World& w) { w.resetAt(home, heading); });
}

common::Result<void> SimulationController::setTimeScale(double scale) {
    return loop_->setTimeScale(scale);
}

// --- Cameras and input ---------------------------------------------------------------------

void SimulationController::setCameraMode(graphics::CameraMode mode) {
    if (mode == cameraMode_) {
        return;
    }
    if (cameraMode_ == graphics::CameraMode::Free) {
        rig_.reset(pose_);
    }
    cameraMode_ = mode;
    if (settings_ != nullptr) {
        settings_->writeJson(sim_settings_keys::kCameraMode,
                             QByteArray("\"") + cameraModeKey(mode) + "\"");
    }
    emit cameraModeChanged();
}

void SimulationController::cycleCameraMode() {
    setCameraMode(graphics::nextCameraMode(cameraMode_));
}

void SimulationController::setFlightKeys(std::uint32_t keys) { selector_->setKeys(keys); }

void SimulationController::keyboardAction(input::KeyboardAction action) {
    selector_->keyboardAction(action);
}

void SimulationController::setJoystickMapping(std::optional<input::InputMapping> mapping) {
    mapping_ = std::move(mapping);
    selector_->setJoystick(mapping_);
    if (settings_ != nullptr) {
        const QJsonObject o =
            mapping_ ? QJsonObject{{QStringLiteral("guid"),
                                    QString::fromStdString(mapping_->deviceGuid)},
                                   {QStringLiteral("name"), QString::fromStdString(mapping_->name)}}
                     : QJsonObject{};
        settings_->writeJson(sim_settings_keys::kJoystick,
                             QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
}

input::InputSource SimulationController::activeInputSource() const noexcept {
    return selector_->activeSource();
}

// --- Scene changes --------------------------------------------------------------------------

void SimulationController::onTerrainChanged() {
    loop_->post([ground = groundFunction(), water = scene_->source()->waterLevel(),
                 origin = scene_->terrain().origin.heightM](sim::World& w) {
        w.setGround(ground, water, origin);
    });
    updatePilotEye();
}

void SimulationController::onEnvironmentChanged() {
    const EnvironmentConfig& env = scene_->environment();
    if (env.wind != lastEnvironment_.wind) {
        loop_->post([wind = env.wind](sim::World& w) { w.setWind(wind); });
    }
    if (env.air != lastEnvironment_.air) {
        loop_->post([air = env.air](sim::World& w) { w.setAir(air); });
    }
    // A time edited in the Environment tab moves the simulated clock; before the start the
    // clock simply follows the scene.
    using namespace std::chrono;
    if (!started_ || abs(env.time - loop_->snapshot().utc) > seconds(2)) {
        loop_->post([t = env.time](sim::World& w) { w.setUtc(t); });
    }
    lastEnvironment_ = env;
}

// --- Frames ---------------------------------------------------------------------------------

graphics::FpvCameraParams SimulationController::fpvParams() const {
    graphics::FpvCameraParams p;
    p.uptiltDeg = profile_.camera.uptiltDeg;
    p.horizontalFovDeg = profile_.camera.fovDeg;
    if (model_) {
        const glm::dvec3& body = model_->shape().bodySize;
        p.offsetBody = {0.5 * body.x + 0.15 * body.y, 0.0, -0.1 * body.z};
    }
    return p;
}

void SimulationController::handleEvents() {
    for (const sim::TimedEvent& e : loop_->takeEvents()) {
        if (std::holds_alternative<sim::SimResetEvent>(e.event)) {
            continue;  // Handled through the snapshot's reset counter.
        }
        emit simulationEvent(QString::fromStdString(sim::describe(e.event)));
    }
}

void SimulationController::recordSession(const sim::SessionStats& stats,
                                         const std::string& profileKey) {
    if (settings_ == nullptr || stats.flightTimeS < kMinRecordedFlightS) {
        return;
    }
    storage::SessionRecord r;
    r.vehicleProfileKey = profileKey;
    r.vehicleType = kVehicleType;
    r.startedAt = astro::formatUtc(stats.startedAt);
    r.durationS = stats.durationS;
    r.maxAltitudeAglM = stats.maxAglM;
    r.maxSpeedMs = stats.maxSpeedMs;
    r.distanceM = stats.distanceM;
    r.energyUsedWh = stats.energyWh;
    r.crashes = stats.crashes;
    // Fire and forget: failures are logged on the storage thread's result.
    auto pending = settings_->storage().submit([r](storage::DbConnection& c) {
        storage::SessionStore store(c);
        auto id = store.insert(r);
        if (!id) {
            log::warn("Cannot record the flight: {}", id.error().message);
        }
        return id.has_value();
    });
    (void)pending;
}

void SimulationController::updateFrame(double dt, double aspect) {
    if (loop_->acquireSnapshot()) {
        const sim::WorldSnapshot& s = loop_->snapshot();
        if (s.resetCount != lastResetCount_) {
            recordSession(lastStats_, lastProfileKey_);
            lastResetCount_ = s.resetCount;
            rig_.reset({.position = s.body.position, .orientation = s.body.orientation});
        }
        lastStats_ = s.stats;
        lastProfileKey_ = s.profileKey;
    }
    handleEvents();
    const sim::WorldSnapshot& s = loop_->snapshot();
    if (s.state != state_) {
        state_ = s.state;
        emit stateChanged();
    }
    if (started_) {
        scene_->followSimulationTime(s.utc);
    }

    // Pose, extrapolated from the last tick to now.
    pose_.position = s.body.position;
    pose_.orientation = s.body.orientation;
    pose_.velocity = s.body.velocity;
    if (s.state == sim::RunState::Running) {
        const double ahead = std::clamp(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s.publishedAt)
                    .count() *
                s.timeScale,
            0.0, kMaxExtrapolationS);
        pose_.position += s.body.velocity * ahead;
        const double angle = glm::length(s.body.angularVelocity) * ahead;
        if (angle > 1e-9) {
            pose_.orientation = glm::normalize(
                pose_.orientation * glm::angleAxis(angle, glm::normalize(s.body.angularVelocity)));
        }
    }

    if (!model_) {
        const auto motors = profile_.motorPositions();
        model_ = std::make_unique<graphics::DroneModel>(graphics::droneShapeFor(
            {motors[0], motors[1], motors[2], motors[3]}, profile_.propeller.diameterM,
            profile_.propeller.blades, profile_.frame.gearHeightM, profile_.camera.uptiltDeg));
        // Close enough to see a small quad, far enough to see a large one.
        rig_.setChaseDistance(std::clamp(kChaseArmLengths * profile_.frame.armLengthM,
                                         kMinChaseDistanceM, kMaxChaseDistanceM));
    }
    if (cameraMode_ != graphics::CameraMode::Free) {
        camera_ = rig_.update(cameraMode_, pose_, fpvParams(), aspect, dt);
    }

    items_.clear();
    if (cameraMode_ != graphics::CameraMode::Fpv) {
        graphics::DroneVisualState v;
        v.position = pose_.position;
        v.orientation = pose_.orientation;
        v.propAngleRad = s.telemetry.propAngleRad;
        double meanRpm = 0.0;
        for (const double rpm : s.telemetry.motorRpm) {
            meanRpm += 0.25 * std::abs(rpm);
        }
        for (std::size_t i = 0; i < 4; ++i) {
            v.rotorSpeedFraction[i] =
                meanRpm > 1e-6
                    ? s.telemetry.motorSpeedFraction * std::abs(s.telemetry.motorRpm[i]) / meanRpm
                    : 0.0;
        }
        v.lights = s.controls.lights;
        v.timeS = s.simTimeS;
        model_->appendDrawItems(v, items_);
    }

    sim::HudOptions options;
    if (settings_ != nullptr) {
        options.imperial = settings_->units() == UnitSystem::Imperial;
        options.showLatLon = settings_->coordinateDisplay() == CoordinateDisplay::UtmAndLatLon;
    }
    options.timeZone = scene_->environment().timeZone;
    if (cameraMode_ == graphics::CameraMode::LineOfSight) {
        options.pilotPosition = rig_.pilotEye();
    }
    sim::WorldSnapshot shown = s;
    shown.body.position = pose_.position;
    if (!started_) {
        shown.utc = scene_->environment().time;  // The scene's (possibly animated) clock.
    }
    hud_ = sim::buildHud(shown, scene_->frame(), options);

    telemetryClock_ += dt;
    if (telemetryClock_ >= 0.25) {
        telemetryClock_ = 0.0;
        emit telemetryUpdated();
    }
}

}  // namespace ds::app
