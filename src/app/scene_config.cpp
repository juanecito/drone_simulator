// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/scene_config.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace ds::app {
namespace {

QString edgeName(terrain::EdgeMode edge) {
    switch (edge) {
        case terrain::EdgeMode::Cliff: return QStringLiteral("cliff");
        case terrain::EdgeMode::Water: return QStringLiteral("water");
        default: return QStringLiteral("fade");
    }
}

std::optional<terrain::EdgeMode> edgeFromName(const QString& name) {
    if (name == QLatin1String("cliff")) {
        return terrain::EdgeMode::Cliff;
    }
    if (name == QLatin1String("fade")) {
        return terrain::EdgeMode::Fade;
    }
    if (name == QLatin1String("water")) {
        return terrain::EdgeMode::Water;
    }
    return std::nullopt;
}

// Reads a number member into @p out; error for a non-number.
common::Result<void> readNumber(const QJsonObject& json, const char* key, double& out) {
    const QJsonValue v = json.value(QLatin1StringView(key));
    if (v.isUndefined() || v.isNull()) {
        return {};
    }
    if (!v.isDouble() || !std::isfinite(v.toDouble())) {
        return common::makeError(std::format("'{}' must be a number", key));
    }
    out = v.toDouble();
    return {};
}

common::Result<void> readOptional(const QJsonObject& json, const char* key,
                                  std::optional<double>& out) {
    const QJsonValue v = json.value(QLatin1StringView(key));
    if (v.isUndefined() || v.isNull()) {
        out.reset();
        return {};
    }
    double value = 0.0;
    if (auto r = readNumber(json, key, value); !r) {
        return r;
    }
    out = value;
    return {};
}

}  // namespace

bool TerrainConfig::operator==(const TerrainConfig& o) const noexcept {
    return presetKey == o.presetKey && params == o.params && extent == o.extent &&
           origin.latitudeDeg == o.origin.latitudeDeg &&
           origin.longitudeDeg == o.origin.longitudeDeg && origin.heightM == o.origin.heightM;
}

QJsonObject paramsToJson(const terrain::ProceduralParams& p) {
    QJsonObject json{
        {QStringLiteral("seed"), QString::number(p.seed)},
        {QStringLiteral("octaves"), p.octaves},
        {QStringLiteral("featureSizeM"), p.featureSizeM},
        {QStringLiteral("lacunarity"), p.lacunarity},
        {QStringLiteral("persistence"), p.persistence},
        {QStringLiteral("amplitudeM"), p.amplitudeM},
        {QStringLiteral("baseHeightM"), p.baseHeightM},
        {QStringLiteral("mountains"), p.mountains},
        {QStringLiteral("warp"), p.warp},
        {QStringLiteral("erosion"), p.erosion},
        {QStringLiteral("edge"), edgeName(p.edge)},
    };
    if (p.waterLevelM) {
        json.insert(QStringLiteral("waterLevelM"), *p.waterLevelM);
    }
    if (p.snowLineM) {
        json.insert(QStringLiteral("snowLineM"), *p.snowLineM);
    }
    if (!p.flatZones.empty()) {
        QJsonArray zones;
        for (const auto& z : p.flatZones) {
            zones.append(QJsonObject{{QStringLiteral("x"), z.x},
                                     {QStringLiteral("z"), z.z},
                                     {QStringLiteral("radiusM"), z.radiusM},
                                     {QStringLiteral("blendM"), z.blendM}});
        }
        json.insert(QStringLiteral("flatZones"), zones);
    }
    return json;
}

common::Result<terrain::ProceduralParams> paramsFromJson(const QJsonObject& json) {
    terrain::ProceduralParams p;
    const QJsonValue seed = json.value(QStringLiteral("seed"));
    if (seed.isString()) {
        bool ok = false;
        p.seed = seed.toString().toULongLong(&ok);
        if (!ok) {
            return common::makeError("'seed' must be a decimal 64-bit number");
        }
    } else if (seed.isDouble() && seed.toDouble() >= 0.0 && seed.toDouble() < 9.0e15) {
        p.seed = static_cast<std::uint64_t>(seed.toDouble());
    } else if (!seed.isUndefined()) {
        return common::makeError("'seed' must be a decimal 64-bit number");
    }
    double octaves = p.octaves;
    for (const auto& [key, out] :
         {std::pair{"octaves", &octaves}, std::pair{"featureSizeM", &p.featureSizeM},
          std::pair{"lacunarity", &p.lacunarity}, std::pair{"persistence", &p.persistence},
          std::pair{"amplitudeM", &p.amplitudeM}, std::pair{"baseHeightM", &p.baseHeightM},
          std::pair{"mountains", &p.mountains}, std::pair{"warp", &p.warp},
          std::pair{"erosion", &p.erosion}}) {
        if (auto r = readNumber(json, key, *out); !r) {
            return std::unexpected(r.error());
        }
    }
    if (octaves != std::floor(octaves) || octaves < 0 || octaves > 64) {
        return common::makeError("'octaves' must be an integer");
    }
    p.octaves = static_cast<int>(octaves);
    if (auto r = readOptional(json, "waterLevelM", p.waterLevelM); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = readOptional(json, "snowLineM", p.snowLineM); !r) {
        return std::unexpected(r.error());
    }
    if (const QJsonValue edge = json.value(QStringLiteral("edge")); !edge.isUndefined()) {
        const auto mode = edgeFromName(edge.toString());
        if (!mode) {
            return common::makeError("'edge' must be \"cliff\", \"fade\" or \"water\"");
        }
        p.edge = *mode;
    }
    if (const QJsonValue zones = json.value(QStringLiteral("flatZones")); zones.isArray()) {
        for (const QJsonValue& z : zones.toArray()) {
            const QJsonObject o = z.toObject();
            terrain::FlatZone zone;
            for (const auto& [key, out] :
                 {std::pair{"x", &zone.x}, std::pair{"z", &zone.z},
                  std::pair{"radiusM", &zone.radiusM}, std::pair{"blendM", &zone.blendM}}) {
                if (auto r = readNumber(o, key, *out); !r) {
                    return std::unexpected(r.error());
                }
            }
            p.flatZones.push_back(zone);
        }
    }
    if (auto valid = terrain::validate(p); !valid) {
        return std::unexpected(valid.error());
    }
    return p;
}

QJsonObject terrainToJson(const TerrainConfig& c) {
    QJsonObject json{
        {QStringLiteral("preset"), QString::fromStdString(c.presetKey)},
        {QStringLiteral("source"), QStringLiteral("procedural")},
        {QStringLiteral("params"), paramsToJson(c.params)},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("lat"), c.origin.latitudeDeg},
                                               {QStringLiteral("lon"), c.origin.longitudeDeg},
                                               {QStringLiteral("alt"), c.origin.heightM}}},
    };
    if (c.extent) {
        json.insert(QStringLiteral("extent"),
                    QJsonObject{{QStringLiteral("widthM"), c.extent->widthM},
                                {QStringLiteral("lengthM"), c.extent->lengthM}});
    }
    return json;
}

common::Result<TerrainConfig> terrainFromJson(const QJsonObject& json) {
    TerrainConfig c;
    c.presetKey = json.value(QStringLiteral("preset")).toString().toStdString();
    const QString source =
        json.value(QStringLiteral("source")).toString(QStringLiteral("procedural"));
    if (source != QLatin1String("procedural")) {
        return common::makeError(
            std::format("terrain source '{}' is not available yet", source.toStdString()));
    }
    auto params = paramsFromJson(json.value(QStringLiteral("params")).toObject());
    if (!params) {
        return std::unexpected(params.error());
    }
    c.params = std::move(*params);
    const QJsonObject origin = json.value(QStringLiteral("origin")).toObject();
    for (const auto& [key, out] :
         {std::pair{"lat", &c.origin.latitudeDeg}, std::pair{"lon", &c.origin.longitudeDeg},
          std::pair{"alt", &c.origin.heightM}}) {
        if (auto r = readNumber(origin, key, *out); !r) {
            return std::unexpected(r.error());
        }
    }
    if (std::abs(c.origin.latitudeDeg) > 89.0 || std::abs(c.origin.longitudeDeg) > 180.0 ||
        std::abs(c.origin.heightM) > 9000.0) {
        return common::makeError("the geographic origin is out of range");
    }
    if (const QJsonValue e = json.value(QStringLiteral("extent")); e.isObject()) {
        terrain::Extent extent;
        if (auto r = readNumber(e.toObject(), "widthM", extent.widthM); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = readNumber(e.toObject(), "lengthM", extent.lengthM); !r) {
            return std::unexpected(r.error());
        }
        if (auto valid = terrain::validate(extent); !valid) {
            return std::unexpected(valid.error());
        }
        c.extent = extent;
    }
    return c;
}

QJsonObject environmentToJson(const EnvironmentConfig& c) {
    return QJsonObject{
        {QStringLiteral("time"), QString::fromStdString(astro::formatUtc(c.time))},
        {QStringLiteral("timeZone"), QString::fromStdString(c.timeZone)},
        {QStringLiteral("animate"), c.animate},
        {QStringLiteral("timeRate"), c.timeRate},
        {QStringLiteral("exposureMode"), c.exposure.mode == graphics::ExposureMode::Manual
                                             ? QStringLiteral("manual")
                                             : QStringLiteral("auto")},
        {QStringLiteral("manualEv100"), c.exposure.manualEv100},
        {QStringLiteral("compensationEv"), c.exposure.compensationEv},
        {QStringLiteral("visibilityM"), c.visibilityM},
        {QStringLiteral("windLevel"), c.wind.level},
        {QStringLiteral("windFromDeg"), c.wind.fromDeg},
        {QStringLiteral("windRandomDirection"), c.wind.randomDirection},
        {QStringLiteral("temperatureC"), c.air.temperatureC},
        {QStringLiteral("qnhHpa"), c.air.pressureHpa},
        {QStringLiteral("humidityPct"), c.air.humidityPct},
    };
}

EnvironmentConfig environmentFromJson(const QJsonObject& json, const EnvironmentConfig& defaults) {
    EnvironmentConfig c = defaults;
    if (auto t = astro::parseUtc(json.value(QStringLiteral("time")).toString().toStdString());
        t && astro::isSupported(*t)) {
        c.time = *t;
    }
    if (const QString zone = json.value(QStringLiteral("timeZone")).toString(); !zone.isEmpty()) {
        if (astro::toLocalTime(c.time, zone.toStdString())) {
            c.timeZone = zone.toStdString();
        }
    }
    c.animate = json.value(QStringLiteral("animate")).toBool(defaults.animate);
    c.timeRate = std::clamp(json.value(QStringLiteral("timeRate")).toDouble(defaults.timeRate), 0.0,
                            86'400.0);
    c.exposure.mode =
        json.value(QStringLiteral("exposureMode")).toString() == QLatin1String("manual")
            ? graphics::ExposureMode::Manual
            : graphics::ExposureMode::Auto;
    c.exposure.manualEv100 = std::clamp(
        json.value(QStringLiteral("manualEv100")).toDouble(defaults.exposure.manualEv100),
        graphics::kMinEv100, graphics::kMaxEv100);
    c.exposure.compensationEv = std::clamp(
        json.value(QStringLiteral("compensationEv")).toDouble(defaults.exposure.compensationEv),
        -6.0, 6.0);
    c.visibilityM =
        std::clamp(json.value(QStringLiteral("visibilityM")).toDouble(defaults.visibilityM),
                   kMinVisibilityM, kMaxVisibilityM);
    c.wind.level = std::clamp(json.value(QStringLiteral("windLevel")).toInt(defaults.wind.level), 0,
                              weather::WindLevelTable::kLevelCount - 1);
    const double from = json.value(QStringLiteral("windFromDeg")).toDouble(defaults.wind.fromDeg);
    c.wind.fromDeg = std::isfinite(from) ? std::fmod(std::fmod(from, 360.0) + 360.0, 360.0)
                                         : defaults.wind.fromDeg;
    c.wind.randomDirection =
        json.value(QStringLiteral("windRandomDirection")).toBool(defaults.wind.randomDirection);
    c.air.temperatureC =
        std::clamp(json.value(QStringLiteral("temperatureC")).toDouble(defaults.air.temperatureC),
                   kMinTemperatureC, kMaxTemperatureC);
    c.air.pressureHpa =
        std::clamp(json.value(QStringLiteral("qnhHpa")).toDouble(defaults.air.pressureHpa),
                   kMinQnhHpa, kMaxQnhHpa);
    c.air.humidityPct = std::clamp(
        json.value(QStringLiteral("humidityPct")).toDouble(defaults.air.humidityPct), 0.0, 100.0);
    return c;
}

storage::TerrainPresetRecord toPresetRecord(const TerrainConfig& c, const std::string& key,
                                            const std::string& name) {
    storage::TerrainPresetRecord r;
    r.key = key;
    r.name = name;
    r.sourceType = "procedural";
    r.seed = c.params.seed;
    r.originLatDeg = c.origin.latitudeDeg;
    r.originLonDeg = c.origin.longitudeDeg;
    r.originAltM = c.origin.heightM;
    if (c.extent) {
        r.extentWidthM = c.extent->widthM;
        r.extentLengthM = c.extent->lengthM;
    }
    r.paramsJson =
        QJsonDocument(paramsToJson(c.params)).toJson(QJsonDocument::Compact).toStdString();
    return r;
}

common::Result<TerrainConfig> fromPresetRecord(const storage::TerrainPresetRecord& r) {
    if (r.sourceType != "procedural") {
        return common::makeError(std::format(
            "preset '{}' uses the '{}' source, which is not available yet", r.name, r.sourceType));
    }
    QJsonParseError error{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(r.paramsJson), &error);
    if (!doc.isObject()) {
        return common::makeError(std::format("preset '{}' has invalid parameters", r.name));
    }
    auto params = paramsFromJson(doc.object());
    if (!params) {
        return std::unexpected(params.error());
    }
    TerrainConfig c;
    c.presetKey = r.key;
    c.params = std::move(*params);
    if (r.seed) {
        c.params.seed = *r.seed;
    }
    c.origin = {r.originLatDeg, r.originLonDeg, r.originAltM};
    if (r.extentWidthM && r.extentLengthM) {
        c.extent = terrain::Extent{*r.extentWidthM, *r.extentLengthM};
    }
    return c;
}

const std::vector<BuiltInPreset>& builtInPresets() {
    static const std::vector<BuiltInPreset> presets = [] {
        std::vector<BuiltInPreset> list;
        const auto add = [&list](const char* key, const char* name, auto configure) {
            TerrainConfig c;
            c.presetKey = key;
            c.origin = {40.4168, -3.7038, 650.0};  // Central Spain.
            configure(c);
            list.push_back({key, name, std::move(c)});
        };
        add("builtin.rolling-hills", "Rolling hills", [](TerrainConfig& c) {
            c.params.seed = 1;
            c.params.amplitudeM = 180.0;
            c.params.mountains = 0.2;
        });
        add("builtin.alpine", "Alpine valley", [](TerrainConfig& c) {
            c.params.seed = 7;
            c.params.featureSizeM = 4000.0;
            c.params.amplitudeM = 1200.0;
            c.params.mountains = 0.7;
            c.params.erosion = 0.55;
            c.params.baseHeightM = 300.0;
            c.origin = {46.55, 8.0, 1200.0};
        });
        add("builtin.flatlands", "Flat farmland", [](TerrainConfig& c) {
            c.params.seed = 3;
            c.params.amplitudeM = 25.0;
            c.params.mountains = 0.0;
            c.params.octaves = 6;
            c.params.flatZones.push_back({.x = 0.0, .z = 0.0, .radiusM = 150.0, .blendM = 100.0});
        });
        add("builtin.island", "Island (10 × 10 km)", [](TerrainConfig& c) {
            c.params.seed = 11;
            c.params.amplitudeM = 300.0;
            c.params.baseHeightM = 20.0;
            c.params.waterLevelM = 0.0;
            c.params.edge = terrain::EdgeMode::Water;
            c.extent = terrain::Extent{10'000.0, 10'000.0};
            c.origin = {28.3, -16.6, 0.0};
        });
        add("builtin.coast", "Coast with lakes", [](TerrainConfig& c) {
            c.params.seed = 5;
            c.params.amplitudeM = 220.0;
            c.params.warp = 0.4;
            c.params.waterLevelM = -30.0;
            c.origin = {43.36, -8.4, 30.0};
        });
        return list;
    }();
    return presets;
}

}  // namespace ds::app
