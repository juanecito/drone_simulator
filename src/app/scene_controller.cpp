// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/scene_controller.hpp"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "ds/app/settings_controller.hpp"
#include "ds/astro/stars.hpp"
#include "ds/common/log.hpp"
#include "ds/storage/storage_service.hpp"
#include "ds/storage/terrain_preset_store.hpp"
#include "ds/terrain/procedural_source.hpp"

namespace ds::app {
namespace {

constexpr double kInitialHeightAglM = 60.0;
constexpr double kViewRadiusM = 40'000.0;

astro::UtcTime nowUtc() {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

QString slug(const QString& name) {
    QString s = name.trimmed().toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    s.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return s.isEmpty() ? QStringLiteral("preset") : s;
}

common::Result<void> validateTerrain(const TerrainConfig& config) {
    if (config.kind == TerrainKind::Procedural) {
        if (auto valid = terrain::validate(config.params); !valid) {
            return valid;
        }
    } else if (config.file.files.empty()) {
        return common::makeError("choose at least one elevation file");
    }
    if (config.extent) {
        if (auto valid = terrain::validate(*config.extent); !valid) {
            return valid;
        }
    }
    return {};
}

}  // namespace

SceneController::SceneController(SettingsController* settings, std::filesystem::path assetDirectory,
                                 QObject* parent)
    : QObject(parent), settings_(settings), assetDirectory_(std::move(assetDirectory)) {
    terrain_ = builtInPresets().front().config;
    environment_.time = nowUtc();
    // The system zone, if the time-zone database knows it (it can be "UTC" in containers).
    if (const std::string zone = QTimeZone::systemTimeZoneId().toStdString();
        !zone.empty() && astro::toLocalTime(nowUtc(), zone)) {
        environment_.timeZone = zone;
    }
    if (auto catalog = astro::loadStarCatalog(assetDirectory_ / "sky" / "bsc5.csv")) {
        sky_.setCatalog(std::move(*catalog));
    } else {
        log::warn("Star catalogue not loaded: {}", catalog.error().message);
    }
    loadSaved();
    reloadPresets();
    applyTerrainOrDefault(terrain_);
    camera_.pitchRad = -0.12;
    camera_.position.y = query_->height(0.0, 0.0) + kInitialHeightAglM;
    updateSky();
}

SceneController::~SceneController() {
    saveEnvironment();  // Keeps the animated time.
}

void SceneController::loadSaved() {
    if (settings_ == nullptr) {
        return;
    }
    if (const auto json = settings_->readJson(settings_keys::kSceneTerrain)) {
        const QJsonDocument doc = QJsonDocument::fromJson(*json);
        if (auto config = terrainFromJson(doc.object()); config) {
            terrain_ = std::move(*config);
        } else {
            log::warn("Ignoring saved terrain: {}", config.error().message);
        }
    }
    if (const auto json = settings_->readJson(settings_keys::kSceneEnvironment)) {
        environment_ = environmentFromJson(QJsonDocument::fromJson(*json).object(), environment_);
    }
}

common::Result<void> SceneController::applyTerrain(const TerrainConfig& config) {
    auto built = buildTerrain(config);
    if (!built) {
        return std::unexpected(built.error());
    }
    terrain_ = config;
    terrain_.origin = built->origin;  // File terrain: the origin found in the data.
    source_ = built->source;
    metadata_ = std::move(built->metadata);
    attribution_ = std::move(built->attribution);
    streamer_.reset();
    terrain::StreamerSettings settings;
    settings.selection.viewRadiusM = kViewRadiusM;
    streamer_ = std::make_unique<terrain::TerrainStreamer>(source_, layout_, pool_, settings);
    query_ = std::make_unique<terrain::TerrainQuery>(source_, layout_, terrain_.origin.heightM);
    frame_ = geo::LocalFrame(terrain_.origin);
    freeFly_.setGround([query = query_.get()](double x, double z) -> std::optional<double> {
        return query->height(x, z);
    });
    // Keep the camera inside a bounded terrain and above the ground.
    if (const auto extent = source_->extent()) {
        camera_.position.x =
            std::clamp(camera_.position.x, -0.5 * extent->widthM, 0.5 * extent->widthM);
        camera_.position.z =
            std::clamp(camera_.position.z, -0.5 * extent->lengthM, 0.5 * extent->lengthM);
    }
    const double ground = query_->height(camera_.position.x, camera_.position.z);
    if (camera_.position.y < ground + graphics::FreeFlyController::kClearanceM) {
        camera_.position.y = ground + kInitialHeightAglM;
    }
    ++terrainVersion_;
    exposureReset_ = true;
    log::info("Terrain: {}", source_->describe());
    return {};
}

void SceneController::applyTerrainOrDefault(const TerrainConfig& config) {
    if (auto applied = applyTerrain(config); !applied) {
        // E.g. elevation files deleted since: run on the default preset rather than without
        // terrain.
        log::error("Cannot create terrain: {}", applied.error().message);
        (void)applyTerrain(builtInPresets().front().config);
    }
}

common::Result<void> SceneController::setTerrain(const TerrainConfig& config) {
    if (auto valid = validateTerrain(config); !valid) {
        return valid;
    }
    if (auto applied = applyTerrain(config); !applied) {
        return applied;
    }
    updateSky();
    saveTerrain();
    emit terrainChanged();
    return {};
}

common::Result<void> SceneController::enterMission(const TerrainConfig& terrain,
                                                  const EnvironmentConfig& environment) {
    if (auto valid = validateTerrain(terrain); !valid) {
        return valid;
    }
    const TerrainConfig before = terrain_;
    const graphics::Camera camera = camera_;
    if (auto applied = applyTerrain(terrain); !applied) {
        return applied;  // Nothing changed.
    }
    if (!missionScene_) {
        freeTerrain_ = before;
        freeEnvironment_ = environment_;
        freeCamera_ = camera;
    }
    missionScene_ = true;
    environment_ = environment;
    environment_.visibilityM =
        std::clamp(environment.visibilityM, kMinVisibilityM, kMaxVisibilityM);
    exposureReset_ = true;
    updateSky();
    emit terrainChanged();
    emit environmentChanged();
    return {};
}

void SceneController::leaveMission() {
    if (!missionScene_) {
        return;
    }
    missionScene_ = false;
    terrain_ = freeTerrain_;
    environment_ = freeEnvironment_;
    camera_ = freeCamera_;
    applyTerrainOrDefault(freeTerrain_);
    exposureReset_ = true;
    updateSky();
    emit terrainChanged();
    emit environmentChanged();
}

void SceneController::setEnvironment(const EnvironmentConfig& config) {
    using namespace std::chrono;
    if (abs(config.time - environment_.time) > minutes(30)) {
        exposureReset_ = true;
    }
    environment_ = config;
    environment_.visibilityM = std::clamp(config.visibilityM, kMinVisibilityM, kMaxVisibilityM);
    updateSky();
    saveEnvironment();
    emit environmentChanged();
}

void SceneController::followSimulationTime(std::optional<astro::UtcTime> time) {
    externalClock_ = time.has_value();
    if (time && astro::isSupported(*time)) {
        using namespace std::chrono;
        if (abs(*time - environment_.time) > minutes(30)) {
            exposureReset_ = true;
        }
        environment_.time = *time;
    }
}

void SceneController::advance(double dtSeconds) {
    if (!externalClock_ && environment_.animate && dtSeconds > 0.0) {
        const auto step = std::chrono::milliseconds(
            static_cast<std::int64_t>(std::llround(dtSeconds * environment_.timeRate * 1000.0)));
        const astro::UtcTime next = environment_.time + step;
        if (astro::isSupported(next)) {
            environment_.time = next;
        }
    }
    updateSky();
}

void SceneController::updateSky() { sky_.update(environment_.time, toGeodetic(camera_.position)); }

geo::Geodetic SceneController::toGeodetic(const glm::dvec3& world) const {
    return frame_.toGeodetic(geo::toEnu(geo::WorldPosition{world.x, world.y, world.z}));
}

void SceneController::saveTerrain() {
    if (settings_ != nullptr && !missionScene_) {
        settings_->writeJson(settings_keys::kSceneTerrain,
                             QJsonDocument(terrainToJson(terrain_)).toJson(QJsonDocument::Compact));
    }
}

void SceneController::saveEnvironment() {
    if (settings_ != nullptr && !missionScene_) {
        settings_->writeJson(
            settings_keys::kSceneEnvironment,
            QJsonDocument(environmentToJson(environment_)).toJson(QJsonDocument::Compact));
    }
}

void SceneController::reloadPresets() {
    presets_.clear();
    for (const BuiltInPreset& preset : builtInPresets()) {
        presets_.append(
            {QString::fromStdString(preset.key), QString::fromStdString(preset.name), true});
    }
    if (settings_ == nullptr) {
        return;
    }
    auto records = settings_->storage()
                       .submit([](storage::DbConnection& c) {
                           storage::TerrainPresetStore store(c);
                           return store.list();
                       })
                       .get();
    if (!records) {
        log::warn("Cannot list terrain presets: {}", records.error().message);
        return;
    }
    for (const auto& record : *records) {
        presets_.append(
            {QString::fromStdString(record.key), QString::fromStdString(record.name), false});
    }
}

common::Result<TerrainConfig> SceneController::presetConfig(const QString& key) const {
    for (const BuiltInPreset& preset : builtInPresets()) {
        if (QString::fromStdString(preset.key) == key) {
            return preset.config;
        }
    }
    if (settings_ == nullptr) {
        return common::makeError("Unknown preset.");
    }
    auto record = settings_->storage()
                      .submit([key = key.toStdString()](storage::DbConnection& c) {
                          storage::TerrainPresetStore store(c);
                          return store.find(key);
                      })
                      .get();
    if (!record) {
        return common::makeError(record.error().message);
    }
    if (!*record) {
        return common::makeError("Unknown preset.");
    }
    return fromPresetRecord(**record);
}

common::Result<QString> SceneController::savePreset(const QString& name,
                                                    const TerrainConfig& config) {
    if (settings_ == nullptr) {
        return common::makeError("Presets cannot be saved without a database.");
    }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 80) {
        return common::makeError("A preset name must have 1 to 80 characters.");
    }
    const QString key = QStringLiteral("user.") + slug(trimmed);
    const auto record = toPresetRecord(config, key.toStdString(), trimmed.toStdString());
    auto saved = settings_->storage()
                     .submit([record](storage::DbConnection& c) {
                         storage::TerrainPresetStore store(c);
                         return store.save(record);
                     })
                     .get();
    if (!saved) {
        return common::makeError(saved.error().message);
    }
    terrain_.presetKey = key.toStdString();
    saveTerrain();
    reloadPresets();
    emit presetsChanged();
    return key;
}

common::Result<void> SceneController::deletePreset(const QString& key) {
    if (settings_ == nullptr || !key.startsWith(QLatin1String("user."))) {
        return common::makeError("Built-in presets cannot be deleted.");
    }
    auto removed = settings_->storage()
                       .submit([key = key.toStdString()](storage::DbConnection& c) {
                           storage::TerrainPresetStore store(c);
                           return store.remove(key);
                       })
                       .get();
    if (!removed) {
        return common::makeError(removed.error().message);
    }
    reloadPresets();
    emit presetsChanged();
    return {};
}

}  // namespace ds::app
