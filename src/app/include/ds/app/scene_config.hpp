// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>

#include <optional>
#include <string>
#include <vector>

#include "ds/astro/time.hpp"
#include "ds/common/error.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/graphics/exposure.hpp"
#include "ds/storage/terrain_preset_store.hpp"
#include "ds/terrain/procedural_source.hpp"
#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief Terrain and environment configuration of the free-mode scene, their JSON form
/// (settings and presets) and the built-in terrain presets (FR-TER-10).
/// @ingroup app

namespace ds::app {

/// @brief The terrain the scene uses (Terrain tab).
struct TerrainConfig {
    std::string presetKey;                  ///< Preset it came from (may be empty).
    terrain::ProceduralParams params;       ///< Procedural source parameters.
    std::optional<terrain::Extent> extent;  ///< Dimensions; empty = unlimited (FR-TER-04a).
    geo::Geodetic origin{};                 ///< Geographic origin; heightM is AMSL (FR-TER-09).
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
    /// @brief Equality.
    bool operator==(const EnvironmentConfig&) const = default;
};

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

/// @brief A built-in terrain preset.
struct BuiltInPreset {
    std::string key;       ///< Stable key, prefixed `builtin.`.
    std::string name;      ///< Display name (English).
    TerrainConfig config;  ///< Configuration.
};

/// @brief The built-in procedural presets; the first one is the default terrain.
[[nodiscard]] const std::vector<BuiltInPreset>& builtInPresets();

}  // namespace ds::app
