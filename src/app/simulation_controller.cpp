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

#include "ds/app/mission_setup.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/common/log.hpp"
#include "ds/common/math.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/storage/data_location.hpp"
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
constexpr double kChaseRoverLengths = 4.0;

const char* typeName(physics::VehicleType type) {
    return type == physics::VehicleType::Ground ? "ground" : "aerial";
}

const char* profileSettingKey(physics::VehicleType type) {
    return type == physics::VehicleType::Ground ? sim_settings_keys::kGroundProfile
                                                : sim_settings_keys::kProfile;
}

const char* joystickSettingKey(physics::VehicleType type) {
    return type == physics::VehicleType::Ground ? sim_settings_keys::kGroundJoystick
                                                : sim_settings_keys::kJoystick;
}

const char* defaultProfileKey(physics::VehicleType type) {
    return type == physics::VehicleType::Ground ? SimulationController::kDefaultGroundProfile
                                                : SimulationController::kDefaultProfile;
}

// A JSON string: the array's only element.
QByteArray jsonString(const QString& text) {
    return QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
}

void setProfileKey(physics::VehicleProfile& profile, std::string key) {
    std::visit([&](auto& p) { p.key = std::move(key); }, profile);
}

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
common::Result<physics::VehicleProfile> validated(const physics::VehicleProfile& profile) {
    return physics::vehicleProfileFromJson(physics::vehicleProfileToJson(profile));
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
    std::vector<std::filesystem::path> modelDirs;
    if (settings_ != nullptr) {
        modelDirs.push_back(storage::homeDirectory() / storage::kDataDirectoryName / "models");
    }
    modelDirs.push_back(assetDirectory / "models");
    models_ = std::make_unique<graphics::ModelLibrary>(std::move(modelDirs));
    loadWindTable(assetDirectory);
    loadWeatherPresets(assetDirectory);
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
    if (settings_ != nullptr) {
        connect(settings_, &SettingsController::enforceFreeFlightRulesChanged, this,
                &SimulationController::applyFreeFlightRules);
    }
    applyFreeFlightRules();
}

SimulationController::~SimulationController() {
    loop_->shutdown();
    loop_->acquireSnapshot();
    recordSession(loop_->snapshot().stats, loop_->snapshot().profileKey,
                  loop_->snapshot().telemetry.type);
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

void SimulationController::loadWeatherPresets(const std::filesystem::path& assets) {
    const auto file = assets / "environment" / "weather_presets.json";
    if (auto table = weather::WeatherPresetTable::load(file)) {
        weatherPresets_ = std::move(*table);
    } else {
        log::warn("Using the built-in weather presets: {}", table.error().message);
    }
}

void SimulationController::loadBuiltInProfiles(const std::filesystem::path& assets) {
    for (const physics::VehicleType type :
         {physics::VehicleType::Aerial, physics::VehicleType::Ground}) {
        const auto dir = assets / "vehicles" / typeName(type);
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        bool found = false;
        for (const auto& file : files) {
            auto profile = physics::loadVehicleProfile(file);
            if (profile && physics::profileType(*profile) != type) {
                profile = common::makeError(std::string("not a ") + typeName(type) + " profile");
            }
            if (profile) {
                builtIn_.push_back(std::move(*profile));
                found = true;
            } else {
                log::warn("Skipping vehicle profile {}: {}", file.string(),
                          profile.error().message);
            }
        }
        if (!found) {
            // Never run without a vehicle: fall back to the compiled-in defaults.
            log::error("No vehicle profiles in {}; using the generic defaults", dir.string());
            if (type == physics::VehicleType::Aerial) {
                physics::AerialProfile generic;
                generic.key = "generic";
                generic.name = "Generic quadcopter";
                builtIn_.emplace_back(std::move(generic));
            } else {
                physics::GroundProfile generic;
                generic.key = "generic-rover";
                generic.name = "Generic rover";
                builtIn_.emplace_back(std::move(generic));
            }
        }
    }
}

void SimulationController::syncBuiltInProfiles() {
    if (settings_ == nullptr) {
        return;
    }
    std::vector<storage::VehicleProfileRecord> records;
    for (const auto& p : builtIn_) {
        records.push_back({.key = physics::profileKey(p),
                           .vehicleType = typeName(physics::profileType(p)),
                           .name = physics::profileName(p),
                           .origin = "builtin",
                           .paramsJson = physics::vehicleProfileToJson(p).dump()});
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
        profiles_.append({QString::fromStdString(physics::profileKey(p)),
                          QString::fromStdString(physics::profileName(p)), true,
                          physics::profileType(p)});
    }
    if (settings_ == nullptr) {
        return;
    }
    auto records = settings_->storage()
                       .submit([](storage::DbConnection& c) {
                           storage::VehicleProfileStore store(c);
                           return store.list("");
                       })
                       .get();
    if (!records) {
        log::warn("Cannot list vehicle profiles: {}", records.error().message);
        return;
    }
    for (const auto& r : *records) {
        if (r.origin == "user") {
            profiles_.append({QString::fromStdString(r.key), QString::fromStdString(r.name), false,
                              r.vehicleType == "ground" ? physics::VehicleType::Ground
                                                        : physics::VehicleType::Aerial});
        }
    }
}

QList<VehicleProfileInfo> SimulationController::profiles(physics::VehicleType type) const {
    QList<VehicleProfileInfo> out;
    for (const VehicleProfileInfo& p : profiles_) {
        if (p.type == type) {
            out.append(p);
        }
    }
    return out;
}

void SimulationController::restoreSelections() {
    physics::VehicleType type = physics::VehicleType::Aerial;
    QString key = QString::fromLatin1(kDefaultProfile);
    if (settings_ != nullptr) {
        auto savedString = [this](const char* settingKey) -> QString {
            const auto json = settings_->readJson(settingKey);
            if (!json) {
                return {};
            }
            const QJsonDocument doc = QJsonDocument::fromJson("[" + *json + "]");
            return doc.array().at(0).toString();
        };
        if (savedString(sim_settings_keys::kVehicleType) == QLatin1String("ground")) {
            type = physics::VehicleType::Ground;
        }
        key = savedString(profileSettingKey(type));
        if (key.isEmpty()) {
            key = QString::fromLatin1(defaultProfileKey(type));
        }
        cameraMode_ = cameraModeFromKey(savedString(sim_settings_keys::kCameraMode))
                          .value_or(graphics::CameraMode::Free);
    }
    auto profile = profileFor(key);
    if (profile && physics::profileType(*profile) != type) {
        profile = common::makeError("the profile is not of the saved vehicle type");
    }
    if (!profile) {
        log::warn("Vehicle profile {} not available: {}", key.toStdString(),
                  profile.error().message);
        profile = profileFor(QString::fromLatin1(defaultProfileKey(type)));
    }
    profile_ = profile ? *profile : builtIn_.front();
    model_.reset();
    rover_.reset();

    // Joystick profiles chosen last time (applied even if the device is not plugged in yet:
    // the selector falls back to the keyboard until it appears).
    restoreJoystick(physics::VehicleType::Aerial);
    restoreJoystick(physics::VehicleType::Ground);
    activateJoystick();
}

void SimulationController::restoreJoystick(physics::VehicleType type) {
    if (settings_ == nullptr) {
        return;
    }
    const auto json = settings_->readJson(joystickSettingKey(type));
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
                      .submit([guid, name, type](storage::DbConnection& c) {
                          storage::InputProfileStore store(c);
                          return store.find(guid, typeName(type), name);
                      })
                      .get();
    if (!record || !*record) {
        return;
    }
    auto parsed = common::parseJson((*record)->mappingJson);
    if (!parsed) {
        return;
    }
    auto mapping = input::InputMapping::fromJson(*parsed);
    if (!mapping) {
        log::warn("Ignoring the saved joystick mapping: {}", mapping.error().message);
        return;
    }
    // The table's type is authoritative (older aerial rows carry no type in their JSON).
    mapping->vehicle = type == physics::VehicleType::Ground ? input::VehicleControls::Ground
                                                            : input::VehicleControls::Aerial;
    mappings_[typeIndex(type)] = std::move(*mapping);
}

void SimulationController::activateJoystick() { selector_->setJoystick(joystickMapping()); }

sim::GroundFunction SimulationController::groundFunction() const {
    // A private query object: the world reads it from the simulation thread.
    auto query = std::make_shared<const terrain::TerrainQuery>(scene_->source(), scene_->layout(),
                                                               scene_->terrain().origin.heightM);
    const std::optional<double> water = scene_->source()->waterLevel();
    const double snowLine = scene_->source()->snowLine();
    return [query, water, snowLine](double x, double z) {
        physics::GroundSample g;
        g.heightM = query->height(x, z);
        const terrain::Normal n = query->normal(x, z);
        g.normal = physics::Vec3(n.x, n.y, n.z);
        // The material drawn on the terrain sets the tyre grip of ground drones.
        g.material =
            physics::surfaceFromTerrain(terrain::dominantMaterial(g.heightM, n.y, water, snowLine));
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
    c.weather = lastEnvironment_.weather.value_or(freeWeatherSettings(lastEnvironment_));
    c.weatherPresets = weatherPresets_;
    c.startPosition = home_;
    c.startHeadingRad = homeHeadingRad_;
    c.startUtc = lastEnvironment_.time;
    c.geoOrigin = scene_->terrain().origin;
    c.visibilityM = lastEnvironment_.visibilityM;
    input::InputSelector* selector = selector_.get();
    loop_ =
        std::make_unique<sim::SimulationLoop>(std::make_unique<sim::World>(std::move(c)),
                                              [selector](double dt) { return selector->poll(dt); });
    loop_->acquireSnapshot();
    lastResetCount_ = loop_->snapshot().resetCount;
    lastProfileKey_ = physics::profileKey(profile_);
    lastVehicleType_ = vehicleType();
    updatePilotEye();
    rig_.reset({.position = loop_->snapshot().body.position,
                .orientation = loop_->snapshot().body.orientation});
}

void SimulationController::updatePilotEye() {
    if (mission_) {
        // The mission's pilot position (its pilot item, or the spawn point).
        const double ground = scene_->query().height(missionPilot_.x, missionPilot_.z);
        rig_.setPilotEye({missionPilot_.x, ground + kPilotEyeHeightM, missionPilot_.z});
        return;
    }
    const glm::dvec3 behind = home_ - kPilotBehindHomeM * headingVector(homeHeadingRad_);
    const double ground = scene_->query().height(behind.x, behind.z);
    rig_.setPilotEye({behind.x, ground + kPilotEyeHeightM, behind.z});
    applyFreeFlightRules();
}

void SimulationController::applyFreeFlightRules() {
    // Free mode checks the default rules from the pilot's position behind the take-off point
    // (FR-MIS-06d); while a mission runs the world checks the mission's rules instead.
    const glm::dvec3 behind = home_ - kPilotBehindHomeM * headingVector(homeHeadingRad_);
    const double ground = scene_->query().height(behind.x, behind.z);
    const sim::FreeFlightRules rules{
        .pilotPosition = {behind.x, ground, behind.z},
        .enforce = settings_ != nullptr && settings_->enforceFreeFlightRules()};
    loop_->post([rules](sim::World& w) { w.setFreeFlightRules(rules); });
}

void SimulationController::refuseInMission(const QString& what) {
    emit simulationEvent(tr("%1 is not allowed during a mission").arg(what));
}

bool SimulationController::keyboardAllowed() const noexcept {
    return !mission_ || mission_->rules.allowKeyboard;
}

// --- Profiles -------------------------------------------------------------------------------

common::Result<physics::VehicleProfile> SimulationController::profileFor(const QString& key) const {
    const std::string k = key.toStdString();
    for (const auto& p : builtIn_) {
        if (physics::profileKey(p) == k) {
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
    auto profile = physics::vehicleProfileFromJson(*json);
    if (!profile) {
        return common::makeError("Vehicle profile " + k + ": " + profile.error().message);
    }
    setProfileKey(*profile, k);
    return profile;
}

void SimulationController::setFlownProfile(const physics::VehicleProfile& profile, bool modified) {
    const bool typeChanged = physics::profileType(profile) != vehicleType();
    profile_ = profile;
    modified_ = modified;
    model_.reset();
    rover_.reset();
    loop_->post([profile](sim::World& w) { w.setProfile(profile); });
    selector_->keyboardAction(input::KeyboardAction::Reset);
    if (typeChanged) {
        activateJoystick();
    }
    emit profileChanged();
}

common::Result<void> SimulationController::selectProfile(const QString& key) {
    if (mission_) {
        return common::makeError("The vehicle profile is locked during a mission.");
    }
    auto profile = profileFor(key);
    if (!profile) {
        return common::makeError(profile.error().message);
    }
    setFlownProfile(*profile, false);
    if (settings_ != nullptr) {
        settings_->writeJson(profileSettingKey(vehicleType()), jsonString(key));
        settings_->writeJson(sim_settings_keys::kVehicleType,
                             jsonString(QString::fromLatin1(typeName(vehicleType()))));
    }
    return {};
}

common::Result<void> SimulationController::selectVehicleType(physics::VehicleType type) {
    if (type == vehicleType()) {
        return {};
    }
    if (mission_) {
        return common::makeError("The vehicle is locked during a mission.");
    }
    QString key = QString::fromLatin1(defaultProfileKey(type));
    if (settings_ != nullptr) {
        if (const auto json = settings_->readJson(profileSettingKey(type))) {
            const QJsonDocument doc = QJsonDocument::fromJson("[" + *json + "]");
            if (const QString saved = doc.array().at(0).toString(); !saved.isEmpty()) {
                key = saved;
            }
        }
    }
    auto profile = profileFor(key);
    if (!profile || physics::profileType(*profile) != type) {
        key = QString::fromLatin1(defaultProfileKey(type));
        profile = profileFor(key);
    }
    if (!profile) {
        // The compiled-in fallback of that type.
        for (const auto& p : builtIn_) {
            if (physics::profileType(p) == type) {
                key = QString::fromStdString(physics::profileKey(p));
                break;
            }
        }
    }
    return selectProfile(key);
}

common::Result<void> SimulationController::applyProfile(const physics::VehicleProfile& profile) {
    if (mission_) {
        return common::makeError("The vehicle profile is locked during a mission.");
    }
    if (physics::profileType(profile) != vehicleType()) {
        return common::makeError("The edited profile is of another vehicle type.");
    }
    auto checked = validated(profile);
    if (!checked) {
        return common::makeError(checked.error().message);
    }
    setProfileKey(*checked, physics::profileKey(profile_));
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
    physics::VehicleProfile p = profile_;
    const std::string key = (QStringLiteral("user.") + slug(trimmed)).toStdString();
    std::visit(
        [&](auto& v) {
            v.key = key;
            v.name = trimmed.toStdString();
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, physics::AerialProfile>) {
                v.reference.reset();  // Reference figures belong to the built-in profiles.
            }
        },
        p);
    auto checked = validated(p);
    if (!checked) {
        return common::makeError(checked.error().message);
    }
    const storage::VehicleProfileRecord record{
        .key = key,
        .vehicleType = typeName(physics::profileType(p)),
        .name = physics::profileName(p),
        .origin = "user",
        .paramsJson = physics::vehicleProfileToJson(p).dump()};
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
    const QString savedKey = QString::fromStdString(key);
    if (auto selected = selectProfile(savedKey); !selected) {
        return common::makeError(selected.error().message);
    }
    return savedKey;
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
    if (physics::profileKey(profile_) == key.toStdString()) {
        return selectProfile(QString::fromLatin1(defaultProfileKey(vehicleType())));
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
    if (mission_) {
        refuseInMission(tr("Reset (use Missions › Retry)"));
        return;
    }
    selector_->keyboardAction(input::KeyboardAction::Reset);
    loop_->post([](sim::World& w) { w.reset(); });
}

void SimulationController::moveHomeToCamera() {
    if (mission_) {
        refuseInMission(tr("Moving the take-off point"));
        return;
    }
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

// --- Missions -------------------------------------------------------------------------------

common::Result<void> SimulationController::startMission(const missions::MissionDefinition& m,
                                                        const physics::VehicleProfile& profile,
                                                        const std::filesystem::path& missionDir) {
    if (auto reason = unplayableReason(m)) {
        return common::makeError(*reason);
    }
    if (physics::profileType(profile) != m.vehicle.type) {
        return common::makeError(std::string("the mission needs a ") + typeName(m.vehicle.type) +
                                 " vehicle");
    }
    auto checked = validated(profile);
    if (!checked) {
        return common::makeError("vehicle profile: " + checked.error().message);
    }
    setProfileKey(*checked, physics::profileKey(profile));
    auto scene = missionScene(m, scene_->environment());
    if (!scene) {
        return common::makeError(scene.error().message);
    }
    if (!mission_) {
        freeProfile_ = profile_;
        freeProfileModified_ = modified_;
        freeHome_ = home_;
        freeHeadingRad_ = homeHeadingRad_;
    }
    // The mission's terrain and environment first: the world receives the new ground before
    // the vehicle is placed on it.
    if (auto entered = scene_->enterMission(scene->terrain, scene->environment); !entered) {
        return common::makeError(entered.error().message);
    }
    missions::MissionPlacement placement;
    placement.origin = geo::LatLon{scene_->terrain().origin.latitudeDeg,
                                   scene_->terrain().origin.longitudeDeg};
    auto query = std::make_shared<const terrain::TerrainQuery>(
        scene_->source(), scene_->layout(), scene_->terrain().origin.heightM);
    placement.ground = [query](double x, double z) { return query->height(x, z); };
    placement.originAltitudeM = scene_->terrain().origin.heightM;
    auto runner =
        missions::MissionRunner::create(m, placement, sim::terrainLineOfSight(groundFunction()));
    if (!runner) {
        if (!mission_) {
            scene_->leaveMission();
        }
        return common::makeError(runner.error().message);
    }
    itemVisuals_ = std::make_unique<sim::ItemVisuals>((*runner)->items(), m.rules.showMarkers,
                                                      models_.get(), missionDir);
    highlighted_.clear();
    missionSpawn_ = (*runner)->spawnPosition();
    missionHeadingRad_ = common::degToRad((*runner)->spawnHeadingDeg());
    missionPilot_ = (*runner)->pilotPosition();
    mission_ = m;
    const bool typeChanged = physics::profileType(*checked) != vehicleType();
    profile_ = *checked;
    modified_ = false;
    model_.reset();
    rover_.reset();
    if (typeChanged) {
        activateJoystick();
    }
    home_ = missionSpawn_;
    homeHeadingRad_ = missionHeadingRad_;
    updatePilotEye();

    // Mission rules (FR-MIS-09).
    loop_->setMissionActive(true);
    selector_->setKeyboardAllowed(m.rules.allowKeyboard);
    selector_->keyboardAction(input::KeyboardAction::Reset);
    auto session = std::make_shared<std::unique_ptr<sim::MissionSession>>(
        std::make_unique<sim::MissionSession>(std::move(*runner)));
    loop_->post([profile = profile_, home = home_, heading = homeHeadingRad_, session,
                 utc = m.environment.utc](sim::World& w) {
        w.setProfile(profile);
        w.setUtc(utc);
        w.resetAt(home, heading);
        w.setMission(std::move(*session));
    });
    started_ = true;  // The simulated clock drives the sky from the mission's start time.
    lastMissionState_.reset();
    reportedAttempt_ = ~std::uint64_t{0};
    if (cameraMode_ == graphics::CameraMode::Free) {
        setCameraMode(graphics::CameraMode::Chase);
    }
    rig_.reset({.position = home_, .orientation = glm::dquat(1.0, 0.0, 0.0, 0.0)});
    if (loop_->state() == sim::RunState::Paused) {
        loop_->resume();
    } else {
        loop_->start();
    }
    log::info("Mission {} started with {}", m.id, physics::profileKey(profile_));
    emit profileChanged();
    emit missionActiveChanged();
    return {};
}

void SimulationController::retryMission() {
    if (!mission_) {
        return;
    }
    selector_->keyboardAction(input::KeyboardAction::Reset);
    loop_->post([home = home_, heading = homeHeadingRad_,
                 utc = mission_->environment.utc](sim::World& w) {
        w.setUtc(utc);
        w.resetAt(home, heading);
        if (sim::MissionSession* m = w.mission()) {
            m->retry(w);
        }
    });
    lastMissionState_.reset();
    if (loop_->state() == sim::RunState::Paused) {
        loop_->resume();
    } else {
        loop_->start();
    }
}

void SimulationController::abortMission() {
    if (!mission_) {
        return;
    }
    loop_->post([](sim::World& w) {
        if (sim::MissionSession* m = w.mission()) {
            m->abort(w);
        }
    });
}

void SimulationController::setFreeItems(const std::vector<items::Item>& freeItems) {
    items::ItemRegistry registry;
    for (const items::Item& item : freeItems) {
        (void)registry.add(item);
    }
    freeVisuals_ = registry.size() > 0
                       ? std::make_unique<sim::ItemVisuals>(registry, true, models_.get())
                       : nullptr;
    loop_->post([freeItems](sim::World& w) { w.setFreeItems(freeItems); });
}

void SimulationController::endMission() {
    if (!mission_) {
        return;
    }
    loop_->post([](sim::World& w) { w.setMission(nullptr); });
    mission_.reset();
    itemVisuals_.reset();
    highlighted_.clear();
    lastMissionState_.reset();
    loop_->setMissionActive(false);
    selector_->setKeyboardAllowed(true);
    selector_->keyboardAction(input::KeyboardAction::Reset);
    const bool typeChanged = physics::profileType(freeProfile_) != vehicleType();
    profile_ = freeProfile_;
    modified_ = freeProfileModified_;
    model_.reset();
    rover_.reset();
    if (typeChanged) {
        activateJoystick();
    }
    home_ = freeHome_;
    homeHeadingRad_ = freeHeadingRad_;
    // The free-mode terrain first, then the vehicle back at its take-off point on it.
    scene_->leaveMission();
    loop_->post([profile = profile_, home = home_, heading = homeHeadingRad_](sim::World& w) {
        w.setProfile(profile);
        w.resetAt(home, heading);
    });
    updatePilotEye();
    log::info("Mission ended");
    emit profileChanged();
    emit missionActiveChanged();
}

// --- Cameras and input ---------------------------------------------------------------------

void SimulationController::setCameraMode(graphics::CameraMode mode) {
    if (mode == cameraMode_) {
        return;
    }
    if (mission_ && mode == graphics::CameraMode::Free) {
        refuseInMission(tr("The free camera"));
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
    graphics::CameraMode next = graphics::nextCameraMode(cameraMode_);
    if (mission_ && next == graphics::CameraMode::Free) {
        next = graphics::nextCameraMode(next);  // No free camera in missions (FR-MIS-09).
    }
    setCameraMode(next);
}

void SimulationController::setFlightKeys(std::uint32_t keys) { selector_->setKeys(keys); }

void SimulationController::keyboardAction(input::KeyboardAction action) {
    selector_->keyboardAction(action);
}

common::Result<void> SimulationController::setJoystickMapping(
    std::optional<input::InputMapping> mapping) {
    if (mapping && mapping->vehicle != vehicleControls()) {
        return common::makeError(std::string("the mapping is for ") +
                                 input::vehicleControlsName(mapping->vehicle) + " drones");
    }
    std::optional<input::InputMapping>& slot = mappings_[typeIndex(vehicleType())];
    slot = std::move(mapping);
    activateJoystick();
    if (settings_ != nullptr) {
        const QJsonObject o =
            slot ? QJsonObject{{QStringLiteral("guid"), QString::fromStdString(slot->deviceGuid)},
                               {QStringLiteral("name"), QString::fromStdString(slot->name)}}
                 : QJsonObject{};
        settings_->writeJson(joystickSettingKey(vehicleType()),
                             QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    return {};
}

input::InputSource SimulationController::activeInputSource() const noexcept {
    return selector_->activeSource();
}

// --- Scene changes --------------------------------------------------------------------------

void SimulationController::onTerrainChanged() {
    weatherGroundAt_.reset();
    loop_->post([ground = groundFunction(), water = scene_->source()->waterLevel(),
                 origin = scene_->terrain().origin](sim::World& w) {
        w.setGround(ground, water, origin.heightM);
        w.setGeoOrigin(origin);
    });
    updatePilotEye();
}

void SimulationController::onEnvironmentChanged() {
    const EnvironmentConfig& env = scene_->environment();
    if (env.weather != lastEnvironment_.weather ||
        (!env.weather && env.freeWeather != lastEnvironment_.freeWeather)) {
        // Entering or leaving a mission, or the free-mode generator changed (FR-WX-12): the
        // whole weather changes; its clock keeps running.
        loop_->post([settings = env.weather.value_or(freeWeatherSettings(env))](sim::World& w) {
            w.setWeather(settings);
        });
    } else {
        if (env.wind != lastEnvironment_.wind) {
            loop_->post([wind = env.wind](sim::World& w) { w.setWind(wind); });
        }
        if (env.air != lastEnvironment_.air) {
            loop_->post([air = env.air](sim::World& w) { w.setAir(air); });
        }
        if (env.visibilityM != lastEnvironment_.visibilityM) {
            loop_->post([v = env.visibilityM](sim::World& w) { w.setVisibility(v); });
        }
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
    const physics::CameraParams& camera = physics::profileCamera(profile_);
    p.uptiltDeg = camera.uptiltDeg;
    p.horizontalFovDeg = camera.fovDeg;
    if (model_) {
        const glm::dvec3& body = model_->shape().bodySize;
        p.offsetBody = {0.5 * body.x + 0.15 * body.y, 0.0, -0.1 * body.z};
    } else if (rover_) {
        // On top of the shell, near the front.
        const graphics::RoverShape& shape = rover_->shape();
        p.offsetBody = {0.4 * shape.bodySize.x, 0.0,
                        shape.bodyBottomM - 1.1 * shape.bodySize.z - 0.02};
    }
    return p;
}

void SimulationController::handleEvents() {
    for (const sim::TimedEvent& e : loop_->takeEvents()) {
        if (std::holds_alternative<sim::SimResetEvent>(e.event)) {
            continue;  // Handled through the snapshot's reset counter.
        }
        if (std::holds_alternative<sim::FlightRuleBrokenEvent>(e.event) && !mission_) {
            loop_->pause();  // Enforced free-mode rules end the flight (FR-MIS-06d).
        }
        emit simulationEvent(QString::fromStdString(sim::describe(e.event)));
    }
}

void SimulationController::recordSession(const sim::SessionStats& stats,
                                         const std::string& profileKey, physics::VehicleType type) {
    if (settings_ == nullptr || stats.flightTimeS < kMinRecordedFlightS) {
        return;
    }
    storage::SessionRecord r;
    r.vehicleProfileKey = profileKey;
    r.vehicleType = typeName(type);
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

void SimulationController::updateLens() {
    // Weather time: the lens neither wets nor dries while paused; a restart dries it.
    const double now = weather_.timeS;
    const double dt = lensTimeS_ ? now - *lensTimeS_ : 0.0;
    lensTimeS_ = now;
    if (dt < 0.0) {
        lensWetness_ = 0.0;
        return;
    }
    double target = 0.0;
    switch (weather_.precipitation) {
        case weather::PrecipitationType::Drizzle: target = 0.4; break;
        case weather::PrecipitationType::Rain:
        case weather::PrecipitationType::Sleet:
            target = std::clamp(0.3 + weather_.rateMmH / 6.0, 0.0, 1.0);
            break;
        default: break;
    }
    const double tau = target > lensWetness_ ? 8.0 : 60.0;
    lensWetness_ += (target - lensWetness_) * (1.0 - std::exp(-std::min(dt, 1.0) / tau));
}

glm::dvec3 SimulationController::cameraVelocity() const noexcept {
    return cameraMode_ == graphics::CameraMode::Fpv || cameraMode_ == graphics::CameraMode::Chase
               ? pose_.velocity
               : glm::dvec3(0.0);
}

void SimulationController::updateFrame(double dt, double aspect) {
    if (loop_->acquireSnapshot()) {
        const sim::WorldSnapshot& s = loop_->snapshot();
        if (s.resetCount != lastResetCount_) {
            recordSession(lastStats_, lastProfileKey_, lastVehicleType_);
            lastResetCount_ = s.resetCount;
            rig_.reset({.position = s.body.position, .orientation = s.body.orientation});
        }
        lastStats_ = s.stats;
        lastProfileKey_ = s.profileKey;
        lastVehicleType_ = s.telemetry.type;
    }
    handleEvents();
    const sim::WorldSnapshot& s = loop_->snapshot();
    if (s.state != state_) {
        state_ = s.state;
        emit stateChanged();
    }
    if (mission_ && s.mission.active) {
        if (lastMissionState_ != s.mission.state) {
            lastMissionState_ = s.mission.state;
            emit missionStateChanged();
        }
        if (s.mission.result && s.mission.attempt != reportedAttempt_) {
            reportedAttempt_ = s.mission.attempt;
            emit missionFinished();
        }
        highlighted_.clear();
        for (const missions::ObjectiveStatus& o : s.mission.objectives) {
            if (o.state == missions::ObjectiveState::Active && !o.targetKey.empty()) {
                highlighted_.push_back(o.targetKey);
            }
        }
    }
    if (started_) {
        scene_->followSimulationTime(s.utc);
    }

    // Pose, extrapolated from the last tick to now.
    pose_.position = s.body.position;
    pose_.orientation = s.body.orientation;
    pose_.velocity = s.body.velocity;
    double ahead = 0.0;
    if (s.state == sim::RunState::Running) {
        ahead = std::clamp(
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

    // Weather (FR-WX-09), heights from the ground at the take-off point.
    const glm::dvec2 homeXz(s.home.x, s.home.z);
    if (!weatherGroundAt_ || *weatherGroundAt_ != homeXz) {
        weatherGroundAt_ = homeXz;
        weatherGroundY_ = scene_->query().height(s.home.x, s.home.z);
    }
    weather_ = graphics::makeWeatherView(s.weather, s.cloudDrift, s.groundCover, weatherGroundY_,
                                         s.windMeanSpeedMs, s.weatherTimeS + ahead);
    updateLens();

    if (!model_ && !rover_) {
        buildModel();
    }
    if (cameraMode_ != graphics::CameraMode::Free) {
        camera_ = rig_.update(cameraMode_, pose_, fpvParams(), aspect, dt);
    }

    items_.clear();
    if (itemVisuals_) {
        itemVisuals_->appendDrawItems(s.simTimeS, highlighted_, items_);
    } else if (freeVisuals_ && !mission_) {
        freeVisuals_->appendDrawItems(s.simTimeS, {}, items_);
    }
    if (cameraMode_ != graphics::CameraMode::Fpv) {
        appendVehicleItems(s);
    }
    // Headlights light the scene from every camera, the vehicle's own included.
    lights_.clear();
    if (rover_ && s.telemetry.type == physics::VehicleType::Ground) {
        rover_->appendLights(roverState(s), lights_);
    }

    sim::HudOptions options;
    if (settings_ != nullptr) {
        options.imperial = settings_->units() == UnitSystem::Imperial;
        options.showLatLon = settings_->coordinateDisplay() == CoordinateDisplay::UtmAndLatLon;
    }
    options.timeZone = scene_->environment().timeZone;
    if (cameraMode_ == graphics::CameraMode::LineOfSight || !s.ruleWarnings.empty() ||
        mission_) {
        options.pilotPosition = rig_.pilotEye();
    }
    options.showMarkers = !mission_ || mission_->rules.showMarkers;
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

void SimulationController::buildModel() {
    if (const physics::AerialProfile* a = aerialProfile()) {
        const auto motors = a->motorPositions();
        model_ = std::make_unique<graphics::DroneModel>(graphics::droneShapeFor(
            {motors[0], motors[1], motors[2], motors[3]}, a->propeller.diameterM,
            a->propeller.blades, a->frame.gearHeightM, a->camera.uptiltDeg));
        // Close enough to see a small quad, far enough to see a large one.
        rig_.setChaseDistance(std::clamp(kChaseArmLengths * a->frame.armLengthM, kMinChaseDistanceM,
                                         kMaxChaseDistanceM));
    } else if (const physics::GroundProfile* g = groundProfile()) {
        graphics::RoverShape shape;
        for (const physics::Vec3& hub : g->wheelHubsAtRest()) {
            shape.wheelHubs.emplace_back(hub.x, hub.y, hub.z);
        }
        shape.wheelRadiusM = g->wheels.radiusM;
        shape.wheelWidthM = g->wheels.widthM;
        shape.bodySize = {g->chassis.lengthM, g->chassis.widthM, g->chassis.heightM};
        shape.bodyBottomM = g->chassis.cogHeightM - g->chassis.clearanceM;
        shape.staticCompressionM = g->staticCompressionM();
        shape.cameraUptiltDeg = g->camera.uptiltDeg;
        rover_ = std::make_unique<graphics::RoverModel>(std::move(shape));
        rig_.setChaseDistance(std::clamp(kChaseRoverLengths * g->chassis.lengthM,
                                         kMinChaseDistanceM, kMaxChaseDistanceM));
    }
    applyModelFile();
}

void SimulationController::applyModelFile() {
    const physics::AerialProfile* a = aerialProfile();
    const physics::GroundProfile* g = groundProfile();
    const std::string& file = a != nullptr ? a->modelFile : g != nullptr ? g->modelFile : "";
    if (file.empty()) {
        return;
    }
    auto loaded = models_->load(file);
    if (!loaded) {
        const QString text = tr("Vehicle model: %1. The built-in model is drawn.")
                                 .arg(QString::fromStdString(loaded.error().message));
        log::warn("{}", text.toStdString());
        emit simulationEvent(text);
        return;
    }
    // Fitted to the procedural body, resting on its underside (body z points down).
    auto mesh = std::make_shared<graphics::Mesh>(**loaded);
    graphics::gltfToBodyFrame(*mesh);
    if (model_) {
        graphics::fitMesh(*mesh, model_->frameBounds(), 2, 1.0F);
        model_->setFrameMesh(std::move(mesh));
    } else if (rover_) {
        graphics::fitMesh(*mesh, rover_->bodyBounds(), 2, 1.0F);
        rover_->setBodyMesh(std::move(mesh));
    }
}

graphics::RoverVisualState SimulationController::roverState(const sim::WorldSnapshot& s) const {
    const physics::GroundTelemetry& g = s.telemetry.ground;
    graphics::RoverVisualState v;
    v.position = pose_.position;
    v.orientation = pose_.orientation;
    const std::size_t wheels =
        std::min<std::size_t>(static_cast<std::size_t>(g.wheelCount), graphics::kRoverMaxWheels);
    for (std::size_t i = 0; i < wheels; ++i) {
        v.wheelAngle[i] = g.wheelAngle[i];
        v.compression[i] = g.compression[i];
    }
    v.lights = s.controls.lights;
    v.braking = g.braking && s.telemetry.armed;
    return v;
}

void SimulationController::appendVehicleItems(const sim::WorldSnapshot& s) {
    if (rover_ && s.telemetry.type == physics::VehicleType::Ground) {
        rover_->appendDrawItems(roverState(s), items_);
        return;
    }
    if (!model_ || s.telemetry.type != physics::VehicleType::Aerial) {
        return;  // The snapshot is still of the previous vehicle.
    }
    graphics::DroneVisualState v;
    v.position = pose_.position;
    v.orientation = pose_.orientation;
    v.propAngleRad = s.telemetry.propAngleRad;
    double meanRpm = 0.0;
    for (const double rpm : s.telemetry.motorRpm) {
        meanRpm += 0.25 * std::abs(rpm);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        v.rotorSpeedFraction[i] = meanRpm > 1e-6 ? s.telemetry.motorSpeedFraction *
                                                       std::abs(s.telemetry.motorRpm[i]) / meanRpm
                                                 : 0.0;
    }
    v.lights = s.controls.lights;
    v.timeS = s.simTimeS;
    model_->appendDrawItems(v, items_);
}

}  // namespace ds::app
