// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/mission_setup.hpp"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <utility>

namespace ds::app {
namespace {

constexpr std::string_view kDefaultPreset = "rolling-hills";

const BuiltInPreset* findPreset(std::string_view name) {
    const std::string key = "builtin." + std::string(name);
    for (const BuiltInPreset& p : builtInPresets()) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace

std::optional<std::string> unplayableReason(const missions::MissionDefinition& mission) {
    if (mission.vehicle.type == physics::VehicleType::Ground) {
        return "ground vehicles are not available yet (milestone M6)";
    }
    switch (mission.terrain.source) {
        case missions::TerrainSource::Procedural: break;
        case missions::TerrainSource::Online:
            return "online terrain needs the online elevation source, not available in this "
                   "build yet (milestone M4)";
        case missions::TerrainSource::File:
            return "elevation files need the raster terrain source, not available in this build "
                   "yet (milestone M4)";
    }
    return std::nullopt;
}

double weatherVisibilityM(const missions::WeatherSpec& weather) {
    if (weather.visibilityM) {
        return std::clamp(*weather.visibilityM, kMinVisibilityM, kMaxVisibilityM);
    }
    static constexpr std::array<std::pair<std::string_view, double>, 13> kVisibility{{
        {"clear", 50'000.0},
        {"few-clouds", 40'000.0},
        {"scattered-clouds", 30'000.0},
        {"broken-clouds", 25'000.0},
        {"overcast", 20'000.0},
        {"mist", 3'000.0},
        {"fog", 500.0},
        {"drizzle", 8'000.0},
        {"light-rain", 10'000.0},
        {"heavy-rain", 3'000.0},
        {"light-snow", 4'000.0},
        {"heavy-snow", 1'000.0},
        {"thunderstorm", 5'000.0},
    }};
    for (const auto& [name, metres] : kVisibility) {
        if (name == weather.preset) {
            return metres;
        }
    }
    return 50'000.0;
}

common::Result<TerrainConfig> missionTerrain(const missions::MissionDefinition& mission) {
    const missions::TerrainSpec& t = mission.terrain;
    if (t.source != missions::TerrainSource::Procedural) {
        return common::makeError(unplayableReason(mission).value_or("unsupported terrain source"));
    }
    const std::string_view presetName = t.preset.empty() ? kDefaultPreset : t.preset;
    const BuiltInPreset* preset = findPreset(presetName);
    if (preset == nullptr) {
        return common::makeError("unknown terrain preset \"" + std::string(presetName) + "\"");
    }
    TerrainConfig config = preset->config;
    config.presetKey.clear();  // Not a saved preset: the mission's own terrain.

    // Preset parameters, overridden member by member by the mission's `params`.
    QJsonObject params = paramsToJson(config.params);
    if (t.params.is_object()) {
        const QJsonObject overrides =
            QJsonDocument::fromJson(QByteArray::fromStdString(t.params.dump())).object();
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            if (!it.key().startsWith(QLatin1String("x-"))) {
                params.insert(it.key(), it.value());
            }
        }
    }
    params.insert(QStringLiteral("seed"), QString::number(t.seed));
    auto parsed = paramsFromJson(params);
    if (!parsed) {
        return common::makeError("terrain.params: " + parsed.error().message);
    }
    config.params = std::move(*parsed);
    if (t.origin) {
        config.origin.latitudeDeg = t.origin->latitudeDeg;
        config.origin.longitudeDeg = t.origin->longitudeDeg;
        // An AGL origin altitude keeps the preset's altitude for world height 0.
        if (t.originAltitudeRef == items::AltitudeRef::Amsl) {
            config.origin.heightM = t.originAltitudeM;
        }
    }
    if (t.extent) {
        config.extent = t.extent;
    }
    return config;
}

common::Result<MissionScene> missionScene(const missions::MissionDefinition& mission,
                                          const EnvironmentConfig& current) {
    auto terrain = missionTerrain(mission);
    if (!terrain) {
        return std::unexpected(terrain.error());
    }
    const missions::EnvironmentSpec& e = mission.environment;
    const missions::WeatherSpec& w = e.weather;
    EnvironmentConfig env = current;
    env.time = e.utc;
    env.timeZone = e.timeZone.empty() ? "UTC" : e.timeZone;
    env.animate = false;  // The simulation clock drives the sky during the mission.
    env.visibilityM = weatherVisibilityM(w);
    env.wind = {.level = w.windLevel, .fromDeg = w.windFromDeg, .randomDirection = false};
    env.air = weather::AirConditions{};
    if (w.temperatureC) {
        env.air.temperatureC = std::clamp(*w.temperatureC, kMinTemperatureC, kMaxTemperatureC);
    }
    if (w.pressureHpa) {
        env.air.pressureHpa = std::clamp(*w.pressureHpa, kMinQnhHpa, kMaxQnhHpa);
    }
    if (w.humidityPct) {
        env.air.humidityPct = std::clamp(*w.humidityPct, 0.0, 100.0);
    }
    return MissionScene{std::move(*terrain), env};
}

}  // namespace ds::app
