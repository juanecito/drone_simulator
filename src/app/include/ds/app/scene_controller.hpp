// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "ds/app/scene_config.hpp"
#include "ds/common/error.hpp"
#include "ds/common/worker_pool.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/graphics/camera.hpp"
#include "ds/graphics/sky_model.hpp"
#include "ds/terrain/chunk.hpp"
#include "ds/terrain/ortho_streamer.hpp"
#include "ds/terrain/query.hpp"
#include "ds/terrain/streamer.hpp"

/// @file
/// @brief Adapter between the GUI and the Core scene: terrain, time, sky and the free-fly
/// camera (FR-UI-06).
/// @ingroup app

namespace ds::app {

class SettingsController;

/// @brief A terrain preset as listed in the Terrain tab.
struct PresetInfo {
    QString key;           ///< Stable key (`builtin.*` or `user.*`).
    QString name;          ///< Display name.
    bool builtIn = false;  ///< Built-in presets cannot be deleted or overwritten.
};

/// @brief Owns the free-mode scene and applies the Terrain and Environment tabs to it.
/// @details Lives on the GUI thread. Terrain chunks are generated on its worker pool.
/// The configuration is saved in the settings (keys `scene.terrain`, `scene.environment`)
/// and restored on the next start.
class SceneController final : public QObject {
    Q_OBJECT

public:
    /// @brief Creates the scene; @p settings may be null (nothing is saved, no user presets).
    /// @p assetDirectory holds `sky/bsc5.csv`. @p data gives online terrain its providers and
    /// cache (null ⇒ no online terrain); it must outlive the scene.
    SceneController(SettingsController* settings, std::filesystem::path assetDirectory,
                    DataServices* data = nullptr, QObject* parent = nullptr);
    ~SceneController() override;

    SceneController(const SceneController&) = delete;
    SceneController& operator=(const SceneController&) = delete;
    SceneController(SceneController&&) = delete;
    SceneController& operator=(SceneController&&) = delete;

    /// @brief Current terrain configuration.
    [[nodiscard]] const TerrainConfig& terrain() const noexcept { return terrain_; }
    /// @brief Validates @p config and regenerates the terrain with it.
    common::Result<void> setTerrain(const TerrainConfig& config);

    /// @brief Current environment.
    [[nodiscard]] const EnvironmentConfig& environment() const noexcept { return environment_; }
    /// @brief Applies and saves @p config.
    void setEnvironment(const EnvironmentConfig& config);

    /// @brief Shows a mission's terrain and environment (FR-MIS-02) without saving them: the
    /// free-mode scene is kept and restored by leaveMission(). Emits terrainChanged() and
    /// environmentChanged().
    common::Result<void> enterMission(const TerrainConfig& terrain,
                                      const EnvironmentConfig& environment);
    /// @brief Restores the free-mode scene after a mission.
    void leaveMission();
    /// @brief True between enterMission() and leaveMission().
    [[nodiscard]] bool inMission() const noexcept { return missionScene_; }

    /// @brief Built-in presets followed by the user's presets (sorted by name).
    [[nodiscard]] const QList<PresetInfo>& presets() const noexcept { return presets_; }
    /// @brief Configuration of the preset @p key.
    [[nodiscard]] common::Result<TerrainConfig> presetConfig(const QString& key) const;
    /// @brief Saves @p config as a user preset called @p name (overwrites a user preset with
    /// the same name). Returns the preset key.
    common::Result<QString> savePreset(const QString& name, const TerrainConfig& config);
    /// @brief Deletes the user preset @p key.
    common::Result<void> deletePreset(const QString& key);

    /// @brief Advances animated time by @p dtSeconds of real time and updates the sky.
    /// @details While the simulation drives the clock (followSimulationTime()), time does not
    /// animate on its own.
    void advance(double dtSeconds);
    /// @brief Lets the simulation clock drive the scene time (the sky follows the simulated
    /// UTC, FR-LOOP-04a); `std::nullopt` hands the clock back to the Environment tab. Not
    /// saved and no environmentChanged() signal: it is called every frame.
    void followSimulationTime(std::optional<astro::UtcTime> time);
    /// @brief True while the simulation drives the clock.
    [[nodiscard]] bool timeFollowsSimulation() const noexcept { return externalClock_; }

    /// @brief Requests the online blocks around world (@p x, @p z) (streaming online terrain;
    /// call it every frame with the camera position, FR-TER-O06). No-op otherwise.
    void focusTerrain(double x, double z);
    /// @brief The data services (null if online terrain is not available).
    [[nodiscard]] DataServices* dataServices() const noexcept { return data_; }
    /// @brief Provider metadata of online terrain (FR-TER-O12); empty otherwise.
    [[nodiscard]] const std::optional<terrain::OnlineMetadata>& onlineMetadata() const noexcept {
        return online_;
    }
    /// @brief State of block streaming (online terrain without extent); empty otherwise.
    [[nodiscard]] std::optional<terrain::OnlineStreamStatus> onlineStatus() const;
    /// @brief Hash of the online data in use (FR-TER-O14; bounded online terrain), else "".
    [[nodiscard]] const std::string& terrainDataHash() const noexcept { return dataHash_; }

    /// @brief Metadata of file and online terrain (FR-TER-O12); empty for procedural terrain.
    [[nodiscard]] const std::optional<terrain::SourceMetadata>& terrainMetadata() const noexcept {
        return metadata_;
    }
    /// @brief Attribution of the terrain data and imagery, shown in the viewport (FR-TER-O13,
    /// F10, I09).
    [[nodiscard]] const std::string& attribution() const noexcept { return attribution_; }
    /// @brief Imagery over the terrain: orthophotos (FR-TER-I01) or an online provider
    /// (FR-TER-O10), null if none.
    [[nodiscard]] const std::shared_ptr<const terrain::OrthoImagery>& orthophotos() const noexcept {
        return ortho_;
    }
    /// @brief Streamer of the orthophoto tiles, null without imagery.
    [[nodiscard]] terrain::OrthoStreamer* orthoStreamer() noexcept { return orthoStreamer_.get(); }
    /// @brief Why the imagery is disabled or a caution about it (FR-TER-I08), "" if none.
    [[nodiscard]] const std::string& orthoWarning() const noexcept { return orthoWarning_; }
    /// @brief Height source of the current terrain.
    [[nodiscard]] const std::shared_ptr<const terrain::HeightSource>& source() const noexcept {
        return source_;
    }
    /// @brief Chunk streamer of the current terrain.
    [[nodiscard]] terrain::TerrainStreamer& streamer() noexcept { return *streamer_; }
    /// @brief Height queries of the current terrain.
    [[nodiscard]] const terrain::TerrainQuery& query() const noexcept { return *query_; }
    /// @brief Chunk layout.
    [[nodiscard]] const terrain::ChunkLayout& layout() const noexcept { return layout_; }
    /// @brief Local frame at the terrain's geographic origin.
    [[nodiscard]] const geo::LocalFrame& frame() const noexcept { return frame_; }
    /// @brief Sky for the current time at the camera position.
    [[nodiscard]] const graphics::SkyModel& sky() const noexcept { return sky_; }
    /// @brief Free-fly camera.
    [[nodiscard]] graphics::Camera& camera() noexcept { return camera_; }
    /// @brief Free-fly controller (keeps the camera above the terrain).
    [[nodiscard]] graphics::FreeFlyController& freeFly() noexcept { return freeFly_; }
    /// @brief Geodetic position of a world point (origin altitude taken as AMSL).
    [[nodiscard]] geo::Geodetic toGeodetic(const glm::dvec3& world) const;
    /// @brief Incremented on every terrain change.
    [[nodiscard]] std::uint64_t terrainVersion() const noexcept { return terrainVersion_; }
    /// @brief True once after a large time jump: the renderer should reset its exposure.
    [[nodiscard]] bool takeExposureReset() noexcept { return std::exchange(exposureReset_, false); }

signals:
    /// @brief The terrain was regenerated.
    void terrainChanged();
    /// @brief The environment settings changed (not emitted for animated time).
    void environmentChanged();
    /// @brief The preset list changed.
    void presetsChanged();

private:
    void loadSaved();
    // Builds @p config and makes it the scene's terrain; on error nothing changes.
    common::Result<void> applyTerrain(const TerrainConfig& config);
    // applyTerrain(), falling back to the default preset on error.
    void applyTerrainOrDefault(const TerrainConfig& config);
    void reloadPresets();
    void saveTerrain();
    void saveEnvironment();
    void updateSky();

    SettingsController* settings_;
    std::filesystem::path assetDirectory_;
    DataServices* data_;
    common::WorkerPool pool_;
    terrain::ChunkLayout layout_;
    TerrainConfig terrain_;
    EnvironmentConfig environment_;
    std::shared_ptr<const terrain::HeightSource> source_;
    std::optional<terrain::SourceMetadata> metadata_;
    std::optional<terrain::OnlineMetadata> online_;
    std::shared_ptr<terrain::OnlineStreamSource> stream_;
    std::string dataHash_;
    std::string attribution_;
    std::unique_ptr<terrain::TerrainStreamer> streamer_;
    std::shared_ptr<const terrain::OrthoImagery> ortho_;
    std::unique_ptr<terrain::OrthoStreamer> orthoStreamer_;
    std::string orthoWarning_;
    std::unique_ptr<terrain::TerrainQuery> query_;
    geo::LocalFrame frame_{geo::Geodetic{}};
    graphics::SkyModel sky_;
    graphics::Camera camera_;
    graphics::FreeFlyController freeFly_;
    QList<PresetInfo> presets_;
    std::uint64_t terrainVersion_ = 0;
    bool exposureReset_ = true;
    bool externalClock_ = false;
    bool missionScene_ = false;
    TerrainConfig freeTerrain_;
    EnvironmentConfig freeEnvironment_;
    graphics::Camera freeCamera_;
};

}  // namespace ds::app
