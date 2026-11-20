// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ds/astro/time.hpp"
#include "ds/common/error.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/graphics/exposure.hpp"
#include "ds/storage/terrain_preset_store.hpp"
#include "ds/terrain/online_terrain.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/terrain/raster_source.hpp"
#include "ds/weather/weather.hpp"
#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief Terrain and environment configuration of the free-mode scene, their JSON form
/// (settings and presets) and the built-in terrain presets (FR-TER-10).
/// @ingroup app

namespace ds::app {

/// @brief Kind of terrain source (FR-TER-02).
enum class TerrainKind {
    Procedural,  ///< Seeded noise (§4.5.2).
    File,        ///< Elevation rasters (§4.5.4).
    Online,      ///< Online elevation provider (§4.5.3).
};

/// @brief Online elevation data of a terrain (FR-TER-O01, O02). The geographic origin is
/// TerrainConfig::origin.
struct OnlineTerrainConfig {
    std::string provider;                   ///< Provider key, e.g. "opentopography".
    std::string dataset;                    ///< Dataset id; "" ⇒ the provider's default.
    std::optional<double> originAltitudeM;  ///< AMSL of world height 0; empty ⇒ the ground.
    terrain::EdgeMode edge = terrain::EdgeMode::Cliff;  ///< Cliff or Fade (FR-TER-O08).
    /// @brief Equality.
    bool operator==(const OnlineTerrainConfig&) const = default;
};

/// @brief Missions on online terrain without `extent` use this square, so their area can be
/// downloaded before PreFlight (FR-TER-O09), metres.
inline constexpr double kDefaultOnlineMissionExtentM = 10'000.0;

/// @brief Elevation files of a terrain (FR-TER-F02, FR-TER-F11).
struct FileTerrainConfig {
    std::vector<std::filesystem::path> files;  ///< Absolute paths of the rasters.
    int band = 1;                              ///< Elevation band.
    std::string crs;                           ///< CRS for files without one ("" ⇒ from the files).
    std::string verticalDatum;                 ///< Vertical datum ("" ⇒ from the files).
    std::string attribution;                   ///< Shown in the viewport (FR-TER-F10).
    std::optional<geo::LatLon> origin;         ///< Empty ⇒ centre of the data.
    std::optional<double> originAltitudeM;     ///< AMSL of world height 0; empty ⇒ the ground.
    terrain::EdgeMode edge = terrain::EdgeMode::Cliff;  ///< Cliff or Fade (FR-TER-O08).
    /// @brief Equality.
    bool operator==(const FileTerrainConfig&) const = default;
};

/// @brief Terrain of the scene: its source and placement.
struct TerrainConfig {
    TerrainKind kind = TerrainKind::Procedural;  ///< Source type.
    std::string presetKey;                  ///< Preset it came from (may be empty).
    terrain::ProceduralParams params;       ///< Procedural source parameters.
    FileTerrainConfig file;                 ///< Elevation files (kind File).
    OnlineTerrainConfig online;             ///< Online provider (kind Online).
    std::optional<terrain::Extent> extent;  ///< Dimensions; empty = unlimited (FR-TER-04a).
    /// Geographic origin; heightM is AMSL (FR-TER-09). For file and online terrain heightM
    /// (and for file terrain the position) is the origin in use, filled in when the terrain
    /// is built (buildTerrain()).
    geo::Geodetic origin{};
    /// @brief Equality.
    bool operator==(const TerrainConfig& other) const noexcept;
};

/// @brief Date, time, exposure, visibility, wind and air (Environment tab).
struct EnvironmentConfig {
    astro::UtcTime time{};                ///< Scene instant.
    std::string timeZone = "UTC";         ///< IANA zone used for display and the time slider.
    bool animate = false;                 ///< Let time run.
    double timeRate = 1.0;                ///< Speed of time when animating (1 = real time).
    graphics::ExposureSettings exposure;  ///< Exposure mode.
    double visibilityM = 60'000.0;        ///< Meteorological visibility (fog / haze).
    weather::WindSettings wind;           ///< Wind level and direction (FR-WIND-01).
    weather::AirConditions air;           ///< Temperature, QNH and humidity at the ground.
    /// Free-mode weather generator (FR-WX-12): seed, base preset, the clouds,
    /// precipitation, fog and thunderstorm as overrides, and the timeline. The air, wind and
    /// visibility above complete it (freeWeatherSettings()).
    weather::WeatherSettings freeWeather = defaultFreeWeather();
    /// Weather generator settings of a mission (FR-WX-01). Empty in free mode, where
    /// @ref freeWeather applies. In a mission the fields above hold the mission's weather at
    /// its start (for display).
    std::optional<weather::WeatherSettings> weather;

    /// @brief Clear sky, seed 1, no timeline.
    [[nodiscard]] static weather::WeatherSettings defaultFreeWeather();
    /// @brief Equality.
    bool operator==(const EnvironmentConfig&) const = default;
};

/// @brief The weather generator settings of free mode: @ref EnvironmentConfig::freeWeather
/// with the air, wind and visibility of @p config as overrides.
[[nodiscard]] weather::WeatherSettings freeWeatherSettings(const EnvironmentConfig& config);

/// @brief The conditions free mode starts with (the generator's plan at time 0).
[[nodiscard]] weather::WeatherConditions freeWeatherStart(const EnvironmentConfig& config);

/// @brief Largest seed the Environment tab offers (it is typed in and must survive JSON).
inline constexpr std::uint64_t kMaxWeatherSeed = 999'999'999;

/// @brief Valid visibility range, metres.
inline constexpr double kMinVisibilityM = 50.0;
/// @brief Valid visibility range, metres.
inline constexpr double kMaxVisibilityM = 300'000.0;
/// @brief Valid ground temperatures, °C.
inline constexpr double kMinTemperatureC = -40.0;
/// @brief Valid ground temperatures, °C.
inline constexpr double kMaxTemperatureC = 50.0;
/// @brief Valid QNH, hPa.
inline constexpr double kMinQnhHpa = 900.0;
/// @brief Valid QNH, hPa.
inline constexpr double kMaxQnhHpa = 1080.0;

/// @brief Procedural parameters as a JSON object (preset `paramsJson`).
[[nodiscard]] QJsonObject paramsToJson(const terrain::ProceduralParams& params);
/// @brief Reads parameters; missing members keep their defaults. The seed is a decimal string
/// (64-bit). Errors for wrong types and invalid values (terrain::validate()).
[[nodiscard]] common::Result<terrain::ProceduralParams> paramsFromJson(const QJsonObject& json);

/// @brief Whole terrain configuration as JSON (setting `scene.terrain`).
[[nodiscard]] QJsonObject terrainToJson(const TerrainConfig& config);
/// @brief Reads a terrain configuration, validating it.
[[nodiscard]] common::Result<TerrainConfig> terrainFromJson(const QJsonObject& json);

/// @brief Environment as JSON (setting `scene.environment`).
[[nodiscard]] QJsonObject environmentToJson(const EnvironmentConfig& config);
/// @brief Reads an environment configuration; invalid members keep their defaults.
[[nodiscard]] EnvironmentConfig environmentFromJson(const QJsonObject& json,
                                                    const EnvironmentConfig& defaults);

/// @brief Preset record for the database.
[[nodiscard]] storage::TerrainPresetRecord toPresetRecord(const TerrainConfig& config,
                                                          const std::string& key,
                                                          const std::string& name);
/// @brief Terrain configuration from a stored procedural preset.
[[nodiscard]] common::Result<TerrainConfig> fromPresetRecord(
    const storage::TerrainPresetRecord& record);

/// @brief A terrain source built from a TerrainConfig.
struct BuiltTerrain {
    std::shared_ptr<const terrain::HeightSource> source;  ///< Heights.
    geo::Geodetic origin{};                               ///< Origin in use (AMSL altitude).
    std::optional<terrain::SourceMetadata> metadata;      ///< File and online terrain: the data.
    std::optional<terrain::OnlineMetadata> online;        ///< Online terrain: the provider.
    std::shared_ptr<terrain::OnlineStreamSource> stream;  ///< Online without extent: streaming.
    std::string attribution;                              ///< Data attribution ("" if none).
    std::string dataHash;  ///< Online terrain with extent: hash of the data (FR-TER-O14).
};

class DataServices;

/// @brief The online request of @p config (kind Online): its provider from @p data.
[[nodiscard]] common::Result<terrain::OnlineTerrainRequest> onlineRequest(
    const TerrainConfig& config, const DataServices& data);

/// @brief Builds the height source of @p config: procedural, the elevation files, or online
/// data from the cache of @p data (never from the network: download it first,
/// ensureOnlineData()). Online terrain needs @p data.
[[nodiscard]] common::Result<BuiltTerrain> buildTerrain(const TerrainConfig& config,
                                                        DataServices* data = nullptr);

/// @brief A built-in terrain preset.
struct BuiltInPreset {
    std::string key;       ///< Stable key, prefixed `builtin.`.
    std::string name;      ///< Display name (English).
    TerrainConfig config;  ///< Configuration.
};

/// @brief The built-in procedural presets; the first one is the default terrain.
[[nodiscard]] const std::vector<BuiltInPreset>& builtInPresets();

}  // namespace ds::app
