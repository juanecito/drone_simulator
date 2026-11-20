// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/mission_setup.hpp"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "ds/app/data_services.hpp"

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

namespace {

std::optional<std::string> onlineUnplayable(const missions::MissionDefinition& mission,
                                            DataServices* data) {
    if (data == nullptr) {
        return "online elevation data is not available in this session";
    }
    auto config = missionTerrain(mission);
    if (!config) {
        return config.error().message;
    }
    auto request = onlineRequest(*config, *data);
    if (!request) {
        return request.error().message;
    }
    auto area = terrain::onlineRequiredArea(*request);
    if (!area) {
        return area.error().message;
    }
    const net::ProviderConfig& p = request->provider;
    if (p.requiresKey && !data->hasApiKey(p.key)) {
        // Without a key only an area downloaded earlier can be flown.
        auto dataset = terrain::onlineDataset(*request);
        auto estimate =
            data->client().estimateDownload(p, dataset ? (*dataset)->id : std::string(), *area);
        if (!estimate || estimate->uncachedRequests > 0) {
            return std::format("{} needs an API key ({}) to download the mission area", p.name,
                               DataServices::keyVariable(p.key));
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> unplayableReason(const missions::MissionDefinition& mission,
                                            DataServices* data) {
    switch (mission.terrain.source) {
        case missions::TerrainSource::Procedural: break;
        case missions::TerrainSource::Online: return onlineUnplayable(mission, data);
        case missions::TerrainSource::File:
            if (!mission.terrain.file) {
                return "the mission has no elevation files";
            }
            if (mission.terrain.file->format == terrain::ElevationFormat::PointCloud) {
                return "LAS/LAZ elevation files need the lidar rasterizer, not available in this "
                       "build yet (milestone M4)";
            }
            if (mission.terrain.file->resolvedElevationFiles.size() !=
                mission.terrain.file->elevationFiles.size()) {
                return "elevation files not found";
            }
            break;
    }
    return std::nullopt;
}

namespace {

common::Result<TerrainConfig> fileMissionTerrain(const missions::MissionDefinition& mission) {
    const missions::TerrainSpec& t = mission.terrain;
    if (auto reason = unplayableReason(mission)) {
        return common::makeError(*reason);
    }
    const terrain::FileTerrainSpec& spec = *t.file;
    TerrainConfig config;
    config.kind = TerrainKind::File;
    FileTerrainConfig& f = config.file;
    f.files = spec.resolvedElevationFiles;
    f.band = spec.band;
    f.crs = spec.crs;
    f.verticalDatum = spec.verticalDatum;
    f.attribution = spec.attribution;
    f.edge = terrain::EdgeMode::Cliff;
    if (t.origin) {
        f.origin = t.origin;
    } else if (spec.originLatDeg && spec.originLonDeg) {
        f.origin = geo::LatLon{*spec.originLatDeg, *spec.originLonDeg};
    }
    if (t.originAltitudeRef == items::AltitudeRef::Amsl && f.origin) {
        f.originAltitudeM = t.originAltitudeM;
    }
    config.extent = t.extent ? t.extent : spec.extent;
    return config;
}

}  // namespace

weather::WeatherConditions missionStartWeather(const missions::WeatherSpec& weather) {
    weather::WeatherConditions c = weather::WeatherGenerator(weather.settings).plannedAt(0.0);
    c.visibilityM = std::clamp(c.visibilityM, kMinVisibilityM, kMaxVisibilityM);
    c.air.temperatureC = std::clamp(c.air.temperatureC, kMinTemperatureC, kMaxTemperatureC);
    c.air.pressureHpa = std::clamp(c.air.pressureHpa, kMinQnhHpa, kMaxQnhHpa);
    c.air.humidityPct = std::clamp(c.air.humidityPct, 0.0, 100.0);
    return c;
}

common::Result<TerrainConfig> missionTerrain(const missions::MissionDefinition& mission) {
    const missions::TerrainSpec& t = mission.terrain;
    if (t.source == missions::TerrainSource::File) {
        return fileMissionTerrain(mission);
    }
    if (t.source == missions::TerrainSource::Online) {
        if (!t.origin) {
            return common::makeError("online terrain needs a geographic origin");
        }
        TerrainConfig config;
        config.kind = TerrainKind::Online;
        config.online.provider = t.elevationProvider;
        config.online.dataset = t.dataset;
        config.online.edge = terrain::EdgeMode::Cliff;
        config.origin = {t.origin->latitudeDeg, t.origin->longitudeDeg, 0.0};
        if (t.originAltitudeRef == items::AltitudeRef::Amsl) {
            config.online.originAltitudeM = t.originAltitudeM;
            config.origin.heightM = t.originAltitudeM;
        }
        config.extent = t.extent.value_or(
            terrain::Extent{kDefaultOnlineMissionExtentM, kDefaultOnlineMissionExtentM});
        return config;
    }
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
    const weather::WeatherConditions start = missionStartWeather(w);
    env.visibilityM = start.visibilityM;
    env.wind = {.level = static_cast<int>(std::lround(start.windLevel)),
                .fromDeg = start.windFromDeg,
                .randomDirection = false};
    env.air = start.air;
    env.weather = w.settings;
    return MissionScene{std::move(*terrain), env};
}

}  // namespace ds::app
