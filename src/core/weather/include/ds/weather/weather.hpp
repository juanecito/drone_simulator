// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief Seeded weather generator (SPECIFICATION §4.2.3, FR-WX-01…03): presets, parameter
/// overrides, a timeline of smooth changes, and slow seeded fluctuations of cloud cover and
/// precipitation around the set values.
/// @ingroup weather

namespace ds::weather {

/// @brief Cloud genus of a layer (FR-WX-02).
enum class CloudType { Cumulus, Stratocumulus, Stratus, Cirrus, Cumulonimbus };
/// @brief Mission JSON name ("cumulus", …).
[[nodiscard]] std::string_view cloudTypeName(CloudType type) noexcept;
/// @brief Parses a cloud type name.
[[nodiscard]] std::optional<CloudType> parseCloudType(std::string_view name) noexcept;

/// @brief One cloud layer. Heights are above the ground at the scene origin.
struct CloudLayer {
    CloudType type = CloudType::Cumulus;  ///< Genus.
    double coverOktas = 0.0;  ///< Sky cover 0–8 (fractional while blending or fluctuating).
    double baseM = 1000.0;    ///< Cloud base, m AGL.
    double topM = 1500.0;     ///< Cloud top, m AGL.
    /// @brief Equality.
    bool operator==(const CloudLayer&) const = default;
};

/// @brief Kind of precipitation.
enum class PrecipitationType { None, Drizzle, Rain, Snow, Sleet };
/// @brief Mission JSON name ("none", "rain", …).
[[nodiscard]] std::string_view precipitationTypeName(PrecipitationType type) noexcept;
/// @brief Parses a precipitation type name.
[[nodiscard]] std::optional<PrecipitationType> parsePrecipitationType(
    std::string_view name) noexcept;

/// @brief Descriptive intensity.
enum class Intensity { Light, Moderate, Heavy };
/// @brief Mission JSON name ("light", "moderate", "heavy").
[[nodiscard]] std::string_view intensityName(Intensity intensity) noexcept;
/// @brief Parses an intensity name.
[[nodiscard]] std::optional<Intensity> parseIntensity(std::string_view name) noexcept;
/// @brief Representative rate of @p intensity for @p type, mm/h of water (rain: 1 / 4 / 12;
/// drizzle: 0.1 / 0.3 / 0.6; snow and sleet as water equivalent: 0.5 / 1.5 / 4).
[[nodiscard]] double intensityRateMmH(PrecipitationType type, Intensity intensity) noexcept;
/// @brief Intensity of a rate (thresholds halfway, in log scale, between the representative
/// rates).
[[nodiscard]] Intensity intensityOf(PrecipitationType type, double rateMmH) noexcept;

/// @brief Precipitation.
struct Precipitation {
    PrecipitationType type = PrecipitationType::None;  ///< Kind.
    double rateMmH = 0.0;                              ///< Rate, mm/h of water.
    /// @brief Equality.
    bool operator==(const Precipitation&) const = default;
};

/// @brief Fog and haze.
struct Fog {
    double topM = 0.0;  ///< Top of a ground fog layer, m AGL (0 = the visibility holds everywhere).
    bool haze = false;  ///< Dry haze rather than fog (rendering tint).
    /// @brief Equality.
    bool operator==(const Fog&) const = default;
};

/// @brief At most three cloud layers (FR-WX-02).
inline constexpr std::size_t kMaxCloudLayers = 3;
/// @brief Visibility inside a cloud layer of at least 4 oktas, m.
inline constexpr double kInCloudVisibilityM = 60.0;
/// @brief Visibility above a ground fog layer when the settings give none, m.
inline constexpr double kAboveFogVisibilityM = 30'000.0;

/// @brief A complete set of weather parameters at one moment (FR-WX-02).
struct WeatherConditions {
    AirConditions air;               ///< Temperature, QNH and humidity at the ground.
    double windLevel = 0.0;          ///< Wind level 0–10 (fractional while blending).
    double windFromDeg = 270.0;      ///< Mean direction the wind blows from, degrees true.
    std::vector<CloudLayer> clouds;  ///< Up to 3 layers, lowest base first.
    Precipitation precipitation;     ///< Precipitation.
    double visibilityM = 50'000.0;   ///< Meteorological visibility at the ground.
    Fog fog;                         ///< Ground fog or haze.
    bool thunderstorm = false;       ///< Thunderstorm (lightning in the rendering, C).

    /// @brief Total sky cover 0–1 (random-overlap combination of the layers).
    [[nodiscard]] double totalCover() const noexcept;
    /// @brief Visibility at @p heightAglM: inside a layer of ≥ 4 oktas it is
    /// @ref kInCloudVisibilityM; above a ground fog layer it is the clear-air value.
    [[nodiscard]] double visibilityAt(double heightAglM) const noexcept;
    /// @brief The lowest layer of at least 5 oktas (a ceiling), if any.
    [[nodiscard]] std::optional<double> ceilingM() const noexcept;
    /// @brief Equality.
    bool operator==(const WeatherConditions&) const = default;
};

/// @brief The conditions before any preset: 15 °C, 1013.25 hPa, 50 %, calm, no clouds, no
/// precipitation, 50 km visibility.
[[nodiscard]] WeatherConditions defaultConditions();

/// @brief Relative humidity from the temperature and the dew point (Magnus formula), %.
[[nodiscard]] double humidityFromDewPoint(double temperatureC, double dewPointC) noexcept;
/// @brief Dew point from the temperature and the relative humidity (Magnus formula), °C.
[[nodiscard]] double dewPointFromHumidity(double temperatureC, double humidityPct) noexcept;

/// @brief A partial set of parameters: a preset, the settings' overrides or a timeline entry.
/// Unset members leave the conditions unchanged.
struct WeatherOverrides {
    std::optional<double> temperatureC;             ///< Ground temperature.
    std::optional<double> humidityPct;              ///< Relative humidity.
    std::optional<double> dewPointC;                ///< Dew point (used when humidity is unset).
    std::optional<double> pressureHpa;              ///< QNH.
    std::optional<int> windLevel;                   ///< Wind level 0–10.
    std::optional<double> windFromDeg;              ///< Wind direction (from).
    std::optional<std::vector<CloudLayer>> clouds;  ///< Replaces every layer.
    std::optional<Precipitation> precipitation;     ///< Precipitation.
    std::optional<double> visibilityM;              ///< Visibility.
    std::optional<Fog> fog;                         ///< Fog.
    std::optional<bool> thunderstorm;               ///< Thunderstorm.

    /// @brief Applies the set members to @p conditions (the dew point after the temperature).
    void applyTo(WeatherConditions& conditions) const;
    /// @brief Reads the parameter members of a mission weather object or timeline entry
    /// (other members are ignored; ranges as in the mission schema).
    void read(common::JsonReader& reader);
    /// @brief Writes the set members into @p object.
    void write(common::Json& object) const;
    /// @brief True if nothing is set.
    [[nodiscard]] bool empty() const noexcept;
    /// @brief Equality.
    bool operator==(const WeatherOverrides&) const = default;
};

/// @brief A named preset: the parameters it sets (FR-WX-01).
struct WeatherPreset {
    std::string key;          ///< "broken-clouds".
    std::string name;         ///< "Broken clouds".
    WeatherOverrides values;  ///< What the preset sets.
    /// @brief Equality.
    bool operator==(const WeatherPreset&) const = default;
};

/// @brief The presets of `assets/environment/weather_presets.json`.
class WeatherPresetTable {
public:
    /// @brief The 13 presets of FR-WX-01 (identical to the shipped data file).
    [[nodiscard]] static const WeatherPresetTable& builtIn();
    /// @brief Parses the data file format (unique keys; "clear" must exist).
    [[nodiscard]] static common::Result<WeatherPresetTable> fromJson(const common::Json& json);
    /// @brief Reads `weather_presets.json`.
    [[nodiscard]] static common::Result<WeatherPresetTable> load(const std::filesystem::path& file);

    /// @brief The preset @p key, or null.
    [[nodiscard]] const WeatherPreset* find(std::string_view key) const noexcept;
    /// @brief All presets in file order.
    [[nodiscard]] const std::vector<WeatherPreset>& presets() const noexcept { return presets_; }
    /// @brief Equality (tests).
    bool operator==(const WeatherPresetTable&) const = default;

private:
    std::vector<WeatherPreset> presets_;
};

/// @brief One timeline entry (FR-WX-03): from @ref atS the weather blends over
/// @ref transitionS into the previous target changed by @ref preset and @ref overrides.
struct WeatherChange {
    double atS = 0.0;            ///< Start, seconds after the start of the session.
    double transitionS = 0.0;    ///< Length of the blend (0 = sudden).
    std::string preset;          ///< Preset applied first (empty = none).
    WeatherOverrides overrides;  ///< Then these.
    /// @brief Equality.
    bool operator==(const WeatherChange&) const = default;
};

/// @brief Generator input (FR-WX-01): seed, base preset, overrides and timeline. The JSON form
/// is the mission's `environment.weather` object.
struct WeatherSettings {
    std::uint64_t seed = 0;               ///< Same seed + settings ⇒ same weather.
    std::string preset = "clear";         ///< Base preset.
    WeatherOverrides overrides;           ///< Changes to the preset.
    bool randomWindDirection = false;     ///< Draw the wind direction from the seed.
    std::vector<WeatherChange> timeline;  ///< Changes over time, in time order.
    bool timelineEnabled = true;          ///< Free mode can switch the timeline off (FR-WX-12).
    bool forecast = false;                ///< The briefing may reveal the timeline (FR-WX-10).

    /// @brief Reads the mission JSON form (the mission schema has already been checked; this
    /// only reports values it cannot use). Unknown presets are errors.
    [[nodiscard]] static common::Result<WeatherSettings> fromJson(
        const common::Json& json,
        const WeatherPresetTable& presets = WeatherPresetTable::builtIn());
    /// @brief Writes the mission JSON form (plus `randomWindDirection` and `timelineEnabled`
    /// when they differ from the defaults).
    [[nodiscard]] common::Json toJson() const;
    /// @brief Clear sky with the given air, wind and visibility (the M3 Environment tab).
    [[nodiscard]] static WeatherSettings fromBasic(const AirConditions& air,
                                                   const WindSettings& wind, double visibilityM,
                                                   std::uint64_t seed);
    /// @brief Equality.
    bool operator==(const WeatherSettings&) const = default;
};

/// @brief Blends @p a towards @p b by @p s (0 … 1): numbers linearly (visibility in log scale,
/// wind direction along the shorter arc), layers pairwise (a layer missing on one side has no
/// cover there), types and flags switching at the half-way point.
[[nodiscard]] WeatherConditions blend(const WeatherConditions& a, const WeatherConditions& b,
                                      double s);

/// @brief One-line summary for the briefing (FR-WX-10), e.g. "Wind 270° level 4 (24 km/h,
/// gusts 34) · broken clouds at 900 m · light rain · visibility 6 km · 12 °C · QNH 1009".
[[nodiscard]] std::string describe(const WeatherConditions& conditions,
                                   const WindLevelTable& windTable = WindLevelTable::builtIn());
/// @brief Sky cover name of a layer: "few" (≤ 2 oktas), "scattered" (≤ 4), "broken" (≤ 7) or
/// "overcast".
[[nodiscard]] std::string_view coverName(double oktas) noexcept;

/// @brief Horizontal displacement, metres (east, north).
struct Drift {
    double eastM = 0.0;   ///< Towards east.
    double northM = 0.0;  ///< Towards north.
    /// @brief Equality.
    bool operator==(const Drift&) const = default;
};

/// @brief The weather at one moment of a session.
struct WeatherState {
    double timeS = 0.0;            ///< Seconds since the start.
    WeatherConditions conditions;  ///< Current conditions, fluctuations included.
    std::array<Drift, kMaxCloudLayers> cloudDrift{};  ///< How far each layer has moved.
};

/// @brief The weather generator (FR-WX-01…03).
/// @details `plannedAt()` and `conditionsAt()` are pure functions of time; `advance()` keeps
/// the session clock and moves the cloud layers with the wind at their height.
class WeatherGenerator {
public:
    /// @brief Clear, calm weather.
    WeatherGenerator();
    /// @brief Generator for @p settings. Unknown preset names count as "clear".
    explicit WeatherGenerator(WeatherSettings settings,
                              const WeatherPresetTable& presets = WeatherPresetTable::builtIn(),
                              const WindLevelTable& windTable = WindLevelTable::builtIn());

    /// @brief The settings.
    [[nodiscard]] const WeatherSettings& settings() const noexcept { return settings_; }
    /// @brief The conditions the settings ask for at @p timeS (timeline applied, no
    /// fluctuations).
    [[nodiscard]] WeatherConditions plannedAt(double timeS) const;
    /// @brief The conditions at @p timeS with the seeded fluctuations: cloud cover wanders by
    /// up to ±0.75 okta around partial covers (not around 0 or 8), precipitation by ±40 %.
    [[nodiscard]] WeatherConditions conditionsAt(double timeS) const;

    /// @brief Back to time 0 (clouds at their starting place).
    void restart();
    /// @brief Advances the clock by @p dt seconds and moves the clouds.
    const WeatherState& advance(double dt);
    /// @brief Jumps to @p timeS (clouds moved in steps of at most 10 s).
    const WeatherState& seek(double timeS);
    /// @brief The current state.
    [[nodiscard]] const WeatherState& state() const noexcept { return state_; }
    /// @brief Mean wind speed at @p heightAglM for the (fractional) level @p level, m/s
    /// (logarithmic profile over short grass, constant above 300 m).
    [[nodiscard]] double windSpeedAt(double level, double heightAglM) const noexcept;

private:
    [[nodiscard]] double noise(std::uint64_t channel, double timeS, double periodS) const noexcept;
    void moveClouds(double dt);

    WeatherSettings settings_;
    WindLevelTable windTable_;
    WeatherConditions base_;
    std::vector<WeatherConditions> targets_;  // One per timeline entry.
    WeatherState state_;
};

}  // namespace ds::weather
