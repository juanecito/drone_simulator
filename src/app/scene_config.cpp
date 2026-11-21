// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/scene_config.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>

#include "ds/app/data_services.hpp"
#include "ds/common/json.hpp"
#include "ds/common/log.hpp"

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

QJsonObject fileToJson(const FileTerrainConfig& f) {
    QJsonArray files;
    for (const auto& file : f.files) {
        files.append(QString::fromStdString(file.string()));
    }
    QJsonObject json{
        {QStringLiteral("files"), files},
        {QStringLiteral("band"), f.band},
        {QStringLiteral("crs"), QString::fromStdString(f.crs)},
        {QStringLiteral("verticalDatum"), QString::fromStdString(f.verticalDatum)},
        {QStringLiteral("attribution"), QString::fromStdString(f.attribution)},
        {QStringLiteral("edge"), edgeName(f.edge)},
    };
    if (f.origin) {
        json.insert(QStringLiteral("origin"),
                    QJsonObject{{QStringLiteral("lat"), f.origin->latitudeDeg},
                                {QStringLiteral("lon"), f.origin->longitudeDeg}});
    }
    if (f.originAltitudeM) {
        json.insert(QStringLiteral("originAltM"), *f.originAltitudeM);
    }
    if (isPointCloud(f)) {
        if (f.resolutionM) {
            json.insert(QStringLiteral("resolutionM"), *f.resolutionM);
        }
        json.insert(QStringLiteral("interpolation"),
                    f.interpolation == terrain::LidarInterpolation::Idw ? QStringLiteral("idw")
                                                                        : QStringLiteral("tin"));
        json.insert(QStringLiteral("cellHeight"), f.cellHeight == terrain::LidarCellHeight::Lowest
                                                      ? QStringLiteral("lowest")
                                                      : QStringLiteral("mean"));
        json.insert(QStringLiteral("classifyGround"), f.classifyGround);
    }
    return json;
}

common::Result<FileTerrainConfig> fileFromJson(const QJsonObject& json) {
    FileTerrainConfig f;
    for (const QJsonValue& v : json.value(QStringLiteral("files")).toArray()) {
        if (!v.isString() || v.toString().isEmpty()) {
            return common::makeError("'files' must list file paths");
        }
        f.files.emplace_back(v.toString().toStdString());
    }
    if (f.files.empty()) {
        return common::makeError("a file terrain needs at least one elevation file");
    }
    f.band = json.value(QStringLiteral("band")).toInt(1);
    if (f.band < 1 || f.band > 1000) {
        return common::makeError("'band' must be 1 or more");
    }
    f.crs = json.value(QStringLiteral("crs")).toString().toStdString();
    f.verticalDatum = json.value(QStringLiteral("verticalDatum")).toString().toStdString();
    f.attribution = json.value(QStringLiteral("attribution")).toString().toStdString();
    const auto edge =
        edgeFromName(json.value(QStringLiteral("edge")).toString(QStringLiteral("cliff")));
    if (!edge || *edge == terrain::EdgeMode::Water) {
        return common::makeError("'edge' must be \"cliff\" or \"fade\" for file terrain");
    }
    f.edge = *edge;
    if (const QJsonValue o = json.value(QStringLiteral("origin")); o.isObject()) {
        geo::LatLon origin;
        if (auto r = readNumber(o.toObject(), "lat", origin.latitudeDeg); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = readNumber(o.toObject(), "lon", origin.longitudeDeg); !r) {
            return std::unexpected(r.error());
        }
        if (std::abs(origin.latitudeDeg) > 89.0 || std::abs(origin.longitudeDeg) > 180.0) {
            return common::makeError("the geographic origin is out of range");
        }
        f.origin = origin;
    }
    if (auto r = readOptional(json, "originAltM", f.originAltitudeM); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = readOptional(json, "resolutionM", f.resolutionM); !r) {
        return std::unexpected(r.error());
    }
    if (f.resolutionM && !(*f.resolutionM >= 0.25 && *f.resolutionM <= 100.0)) {
        return common::makeError("'resolutionM' must be between 0.25 and 100 m");
    }
    const QString interpolation =
        json.value(QStringLiteral("interpolation")).toString(QStringLiteral("tin"));
    if (interpolation != QLatin1String("tin") && interpolation != QLatin1String("idw")) {
        return common::makeError("'interpolation' must be \"tin\" or \"idw\"");
    }
    f.interpolation = interpolation == QLatin1String("idw") ? terrain::LidarInterpolation::Idw
                                                            : terrain::LidarInterpolation::Tin;
    const QString cell = json.value(QStringLiteral("cellHeight")).toString(QStringLiteral("mean"));
    if (cell != QLatin1String("mean") && cell != QLatin1String("lowest")) {
        return common::makeError("'cellHeight' must be \"mean\" or \"lowest\"");
    }
    f.cellHeight = cell == QLatin1String("lowest") ? terrain::LidarCellHeight::Lowest
                                                   : terrain::LidarCellHeight::Mean;
    f.classifyGround = json.value(QStringLiteral("classifyGround")).toBool(false);
    return f;
}

QJsonObject orthoToJson(const OrthoConfig& o) {
    if (!o.provider.empty()) {
        return QJsonObject{
            {QStringLiteral("provider"), QString::fromStdString(o.provider)},
            {QStringLiteral("dataset"), QString::fromStdString(o.dataset)},
            {QStringLiteral("brightness"), o.color.brightness},
            {QStringLiteral("contrast"), o.color.contrast},
            {QStringLiteral("gamma"), o.color.gamma},
        };
    }
    QJsonArray files;
    for (const auto& file : o.files) {
        files.append(QString::fromStdString(file.string()));
    }
    QJsonArray bands;
    for (const int b : o.bands) {
        bands.append(b);
    }
    return QJsonObject{
        {QStringLiteral("files"), files},
        {QStringLiteral("crs"), QString::fromStdString(o.crs)},
        {QStringLiteral("bands"), bands},
        {QStringLiteral("attribution"), QString::fromStdString(o.attribution)},
        {QStringLiteral("brightness"), o.color.brightness},
        {QStringLiteral("contrast"), o.color.contrast},
        {QStringLiteral("gamma"), o.color.gamma},
    };
}

common::Result<OrthoConfig> orthoFromJson(const QJsonObject& json) {
    OrthoConfig o;
    o.provider = json.value(QStringLiteral("provider")).toString().toStdString();
    o.dataset = json.value(QStringLiteral("dataset")).toString().toStdString();
    for (const QJsonValue& v : json.value(QStringLiteral("files")).toArray()) {
        if (!v.isString() || v.toString().isEmpty()) {
            return common::makeError("'ortho.files' must list file paths");
        }
        o.files.emplace_back(v.toString().toStdString());
    }
    o.crs = json.value(QStringLiteral("crs")).toString().toStdString();
    for (const QJsonValue& v : json.value(QStringLiteral("bands")).toArray()) {
        const int b = v.toInt(0);
        if (b < 1 || b > 1000) {
            return common::makeError("'ortho.bands' must list band numbers");
        }
        o.bands.push_back(b);
    }
    if (!o.bands.empty() && o.bands.size() != 3 && o.bands.size() != 4) {
        return common::makeError("'ortho.bands' must list 3 (RGB) or 4 (RGBA) bands");
    }
    o.attribution = json.value(QStringLiteral("attribution")).toString().toStdString();
    for (const auto& [key, out] :
         {std::pair{"brightness", &o.color.brightness}, std::pair{"contrast", &o.color.contrast},
          std::pair{"gamma", &o.color.gamma}}) {
        if (auto r = readNumber(json, key, *out); !r) {
            return std::unexpected(r.error());
        }
    }
    if (const std::string bad = terrain::validateOrthoColor(o.color); !bad.empty()) {
        return common::makeError("orthophoto " + bad);
    }
    return o;
}

QJsonObject onlineToJson(const OnlineTerrainConfig& o) {
    QJsonObject json{
        {QStringLiteral("provider"), QString::fromStdString(o.provider)},
        {QStringLiteral("dataset"), QString::fromStdString(o.dataset)},
        {QStringLiteral("edge"), edgeName(o.edge)},
    };
    if (o.originAltitudeM) {
        json.insert(QStringLiteral("originAltM"), *o.originAltitudeM);
    }
    return json;
}

common::Result<OnlineTerrainConfig> onlineFromJson(const QJsonObject& json) {
    OnlineTerrainConfig o;
    o.provider = json.value(QStringLiteral("provider")).toString().toStdString();
    if (o.provider.empty()) {
        return common::makeError("an online terrain needs an elevation provider");
    }
    o.dataset = json.value(QStringLiteral("dataset")).toString().toStdString();
    const auto edge =
        edgeFromName(json.value(QStringLiteral("edge")).toString(QStringLiteral("cliff")));
    if (!edge || *edge == terrain::EdgeMode::Water) {
        return common::makeError("'edge' must be \"cliff\" or \"fade\" for online terrain");
    }
    o.edge = *edge;
    if (auto r = readOptional(json, "originAltM", o.originAltitudeM); !r) {
        return std::unexpected(r.error());
    }
    return o;
}

QJsonObject processingToJson(const terrain::HeightProcessing& p) {
    return QJsonObject{
        {QStringLiteral("interpolation"),
         QString::fromUtf8(terrain::interpolationName(p.interpolation))},
        {QStringLiteral("despikeRadiusM"), p.despikeRadiusM},
        {QStringLiteral("smoothRadiusM"), p.smoothRadiusM},
    };
}

common::Result<terrain::HeightProcessing> processingFromJson(const QJsonObject& json) {
    terrain::HeightProcessing p;
    const QString name =
        json.value(QStringLiteral("interpolation")).toString(QStringLiteral("bilinear"));
    const auto interpolation = terrain::interpolationFromName(name.toStdString());
    if (!interpolation) {
        return common::makeError(
            "'processing.interpolation' must be \"bilinear\", \"bicubic\", \"bspline\" or "
            "\"nearest\"");
    }
    p.interpolation = *interpolation;
    for (const auto& [key, out] : {std::pair{"despikeRadiusM", &p.despikeRadiusM},
                                   std::pair{"smoothRadiusM", &p.smoothRadiusM}}) {
        if (auto r = readNumber(json, key, *out); !r) {
            return std::unexpected(r.error());
        }
    }
    if (const std::string bad = terrain::validateProcessing(p); !bad.empty()) {
        return common::makeError(bad);
    }
    return p;
}

QString sourceName(TerrainKind kind) {
    switch (kind) {
        case TerrainKind::File: return QStringLiteral("file");
        case TerrainKind::Online: return QStringLiteral("online");
        default: return QStringLiteral("procedural");
    }
}

}  // namespace

bool TerrainConfig::operator==(const TerrainConfig& o) const noexcept {
    return kind == o.kind && file == o.file && online == o.online && ortho == o.ortho &&
           processing == o.processing && presetKey == o.presetKey && params == o.params &&
           extent == o.extent && origin.latitudeDeg == o.origin.latitudeDeg &&
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
        {QStringLiteral("source"), sourceName(c.kind)},
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
    if (c.kind == TerrainKind::File) {
        json.insert(QStringLiteral("file"), fileToJson(c.file));
    }
    if (c.kind == TerrainKind::Online) {
        json.insert(QStringLiteral("online"), onlineToJson(c.online));
    }
    if (c.kind != TerrainKind::Procedural && c.ortho.any()) {
        json.insert(QStringLiteral("ortho"), orthoToJson(c.ortho));
    }
    if (c.kind != TerrainKind::Procedural && c.processing != terrain::HeightProcessing{}) {
        json.insert(QStringLiteral("processing"), processingToJson(c.processing));
    }
    return json;
}

common::Result<TerrainConfig> terrainFromJson(const QJsonObject& json) {
    TerrainConfig c;
    c.presetKey = json.value(QStringLiteral("preset")).toString().toStdString();
    const QString source =
        json.value(QStringLiteral("source")).toString(QStringLiteral("procedural"));
    if (source == QLatin1String("file")) {
        auto file = fileFromJson(json.value(QStringLiteral("file")).toObject());
        if (!file) {
            return std::unexpected(file.error());
        }
        c.kind = TerrainKind::File;
        c.file = std::move(*file);
    } else if (source == QLatin1String("online")) {
        auto online = onlineFromJson(json.value(QStringLiteral("online")).toObject());
        if (!online) {
            return std::unexpected(online.error());
        }
        c.kind = TerrainKind::Online;
        c.online = std::move(*online);
    } else if (source != QLatin1String("procedural")) {
        return common::makeError(
            std::format("terrain source '{}' is not available yet", source.toStdString()));
    }
    if (const QJsonValue ortho = json.value(QStringLiteral("ortho"));
        ortho.isObject() && c.kind != TerrainKind::Procedural) {
        auto o = orthoFromJson(ortho.toObject());
        if (!o) {
            return std::unexpected(o.error());
        }
        c.ortho = std::move(*o);
    }
    if (const QJsonValue processing = json.value(QStringLiteral("processing"));
        processing.isObject() && c.kind != TerrainKind::Procedural) {
        auto p = processingFromJson(processing.toObject());
        if (!p) {
            return std::unexpected(p.error());
        }
        c.processing = *p;
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

weather::WeatherSettings EnvironmentConfig::defaultFreeWeather() {
    weather::WeatherSettings s;
    s.seed = 1;
    s.preset = "clear";
    s.overrides.clouds = std::vector<weather::CloudLayer>{};
    s.overrides.precipitation = weather::Precipitation{};
    s.overrides.fog = weather::Fog{};
    s.overrides.thunderstorm = false;
    s.timelineEnabled = false;
    return s;
}

weather::WeatherSettings freeWeatherSettings(const EnvironmentConfig& c) {
    weather::WeatherSettings s = c.freeWeather;
    weather::WeatherOverrides& o = s.overrides;
    o.temperatureC = c.air.temperatureC;
    o.pressureHpa = c.air.pressureHpa;
    o.humidityPct = c.air.humidityPct;
    o.dewPointC.reset();
    o.windLevel = c.wind.level;
    o.windFromDeg = c.wind.fromDeg;
    o.visibilityM = c.visibilityM;
    s.randomWindDirection = c.wind.randomDirection;
    s.forecast = false;
    return s;
}

weather::WeatherConditions freeWeatherStart(const EnvironmentConfig& c) {
    return weather::WeatherGenerator(freeWeatherSettings(c)).plannedAt(0.0);
}

QJsonObject environmentToJson(const EnvironmentConfig& c) {
    const QJsonObject weatherJson =
        QJsonDocument::fromJson(QByteArray::fromStdString(c.freeWeather.toJson().dump())).object();
    return QJsonObject{
        {QStringLiteral("weather"), weatherJson},
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
    if (const QJsonValue w = json.value(QStringLiteral("weather")); w.isObject()) {
        const QByteArray text = QJsonDocument(w.toObject()).toJson(QJsonDocument::Compact);
        if (auto parsed = common::parseJson(text.toStdString())) {
            if (auto settings = weather::WeatherSettings::fromJson(*parsed)) {
                // The free-mode overrides are always set; missing ones keep the defaults.
                weather::WeatherSettings merged = *settings;
                const weather::WeatherOverrides& d = defaults.freeWeather.overrides;
                weather::WeatherOverrides& o = merged.overrides;
                o.clouds = o.clouds ? o.clouds : d.clouds;
                o.precipitation = o.precipitation ? o.precipitation : d.precipitation;
                o.fog = o.fog ? o.fog : d.fog;
                o.thunderstorm = o.thunderstorm ? o.thunderstorm : d.thunderstorm;
                merged.seed = std::min(merged.seed, kMaxWeatherSeed);
                c.freeWeather = merged;
            }
        }
    }
    return c;
}

storage::TerrainPresetRecord toPresetRecord(const TerrainConfig& c, const std::string& key,
                                            const std::string& name) {
    storage::TerrainPresetRecord r;
    r.key = key;
    r.name = name;
    r.sourceType = sourceName(c.kind).toStdString();
    r.seed = c.params.seed;
    r.originLatDeg = c.origin.latitudeDeg;
    r.originLonDeg = c.origin.longitudeDeg;
    r.originAltM = c.origin.heightM;
    if (c.extent) {
        r.extentWidthM = c.extent->widthM;
        r.extentLengthM = c.extent->lengthM;
    }
    QJsonObject params = paramsToJson(c.params);
    if (c.kind == TerrainKind::File) {
        params.insert(QStringLiteral("file"), fileToJson(c.file));
    }
    if (c.kind == TerrainKind::Online) {
        params.insert(QStringLiteral("online"), onlineToJson(c.online));
    }
    if (c.kind != TerrainKind::Procedural && c.ortho.any()) {
        params.insert(QStringLiteral("ortho"), orthoToJson(c.ortho));
    }
    if (c.kind != TerrainKind::Procedural && c.processing != terrain::HeightProcessing{}) {
        params.insert(QStringLiteral("processing"), processingToJson(c.processing));
    }
    r.paramsJson = QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString();
    return r;
}

common::Result<TerrainConfig> fromPresetRecord(const storage::TerrainPresetRecord& r) {
    if (r.sourceType != "procedural" && r.sourceType != "file" && r.sourceType != "online") {
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
    if (r.sourceType == "file") {
        auto file = fileFromJson(doc.object().value(QStringLiteral("file")).toObject());
        if (!file) {
            return common::makeError(std::format("preset '{}': {}", r.name, file.error().message));
        }
        c.kind = TerrainKind::File;
        c.file = std::move(*file);
    } else if (r.sourceType == "online") {
        auto online = onlineFromJson(doc.object().value(QStringLiteral("online")).toObject());
        if (!online) {
            return common::makeError(
                std::format("preset '{}': {}", r.name, online.error().message));
        }
        c.kind = TerrainKind::Online;
        c.online = std::move(*online);
    }
    if (const QJsonValue ortho = doc.object().value(QStringLiteral("ortho"));
        ortho.isObject() && c.kind != TerrainKind::Procedural) {
        auto o = orthoFromJson(ortho.toObject());
        if (!o) {
            return common::makeError(std::format("preset '{}': {}", r.name, o.error().message));
        }
        c.ortho = std::move(*o);
    }
    if (const QJsonValue processing = doc.object().value(QStringLiteral("processing"));
        processing.isObject() && c.kind != TerrainKind::Procedural) {
        auto p = processingFromJson(processing.toObject());
        if (!p) {
            return common::makeError(std::format("preset '{}': {}", r.name, p.error().message));
        }
        c.processing = *p;
    }
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

common::Result<terrain::OnlineTerrainRequest> onlineRequest(const TerrainConfig& config,
                                                            const DataServices& data) {
    const net::ProviderConfig* provider = data.provider(config.online.provider);
    if (provider == nullptr) {
        return common::makeError(
            std::format("unknown elevation provider \"{}\"", config.online.provider));
    }
    terrain::OnlineTerrainRequest r;
    r.provider = *provider;
    r.dataset = config.online.dataset;
    r.origin = geo::LatLon{config.origin.latitudeDeg, config.origin.longitudeDeg};
    r.originAltitudeM = config.online.originAltitudeM;
    r.extent = config.extent;
    r.edge = config.online.edge;
    r.processing = config.processing;
    return r;
}

namespace {

// Opens the imagery of @p config over the built terrain (FR-TER-I08: problems only disable
// it, with a warning).
void openOrtho(const TerrainConfig& config, DataServices* data, BuiltTerrain& built) {
    const OrthoConfig& o = config.ortho;
    if (!o.any()) {
        return;
    }
    if (config.kind == TerrainKind::Procedural) {
        built.orthoWarning = "imagery needs file or online elevation; imagery disabled";
        return;
    }
    if (!o.provider.empty()) {
        // Online imagery (FR-TER-O10): streamed through the data services.
        const net::ProviderConfig* provider =
            data != nullptr ? data->provider(o.provider) : nullptr;
        if (provider == nullptr || !provider->isImagery()) {
            built.orthoWarning =
                data == nullptr
                    ? "online imagery is not available here; imagery disabled"
                    : std::format("unknown imagery provider \"{}\"; imagery disabled", o.provider);
            log::warn("Imagery: {}", built.orthoWarning);
            return;
        }
        if (provider->requiresKey && !data->hasApiKey(provider->key)) {
            built.orthoWarning =
                std::format("{} needs an API key (Settings → Data providers); imagery disabled",
                            provider->name);
            log::warn("Imagery: {}", built.orthoWarning);
            return;
        }
        terrain::OrthoOptions tiles;
        auto opened = terrain::OnlineImagerySource::open(
            data->client(), terrain::OnlineImageryRequest{*provider, o.dataset, o.color},
            built.origin, {}, tiles);
        if (!opened) {
            built.orthoWarning = opened.error().message + "; imagery disabled";
            log::warn("Imagery: {}", built.orthoWarning);
            return;
        }
        const terrain::OrthoMetadata& m = (*opened)->metadata();
        if (const auto extent = built.source->extent();
            extent && !m.overlaps(-0.5 * extent->widthM, -0.5 * extent->lengthM,
                                  0.5 * extent->widthM, 0.5 * extent->lengthM)) {
            built.orthoWarning = std::format("{} has no imagery here", provider->name);
        } else if (!extent && !m.overlaps(-1.0, -1.0, 1.0, 1.0)) {
            built.orthoWarning = std::format("{} has no imagery at the origin", provider->name);
        }
        const net::DatasetConfig& d = (*opened)->dataset();
        built.orthoAttribution = d.attribution.empty() || d.attribution == provider->attribution
                                     ? provider->attribution
                                     : provider->attribution + "; " + d.attribution;
        built.ortho = std::move(*opened);
        return;
    }
    const std::vector<std::filesystem::path> files =
        data != nullptr ? data->orthoFiles(o.files) : o.files;
    auto opened = terrain::OrthophotoSource::open(files, orthoOptions(o), built.origin);
    if (!opened) {
        built.orthoWarning = opened.error().message + "; imagery disabled";
        log::warn("Orthophotos: {}", built.orthoWarning);
        return;
    }
    const terrain::OrthoMetadata& m = (*opened)->metadata();
    if (const auto extent = built.source->extent();
        extent && !m.overlaps(-0.5 * extent->widthM, -0.5 * extent->lengthM, 0.5 * extent->widthM,
                              0.5 * extent->lengthM)) {
        built.orthoWarning = "the orthophotos do not cover the terrain";
    }
    built.ortho = std::move(*opened);
    built.orthoAttribution = o.attribution;
}

common::Result<BuiltTerrain> buildHeights(const TerrainConfig& config, DataServices* data);

}  // namespace

common::Result<BuiltTerrain> buildTerrain(const TerrainConfig& config, DataServices* data) {
    auto built = buildHeights(config, data);
    if (built) {
        openOrtho(config, data, *built);
    }
    return built;
}

terrain::OrthoOptions orthoOptions(const OrthoConfig& o) {
    terrain::OrthoOptions options;
    options.crs = o.crs;
    options.bands = o.bands;
    options.color = o.color;
    return options;
}

namespace {

// FR-TER-C03: the metadata view says how the heights were processed.
void noteProcessing(const TerrainConfig& config, terrain::SourceMetadata& meta) {
    if (config.processing != terrain::HeightProcessing{}) {
        meta.notes.push_back("Heights: " + terrain::describeProcessing(config.processing));
    }
}

common::Result<BuiltTerrain> buildHeights(const TerrainConfig& config, DataServices* data) {
    BuiltTerrain built;
    if (config.kind == TerrainKind::Online) {
        if (data == nullptr) {
            return common::makeError("online elevation data is not available here");
        }
        auto request = onlineRequest(config, *data);
        if (!request) {
            return std::unexpected(request.error());
        }
        auto opened = terrain::openOnlineTerrain(data->client(), *request);
        if (!opened) {
            return common::makeError(opened.error().message);
        }
        built.source = std::move(opened->source);
        built.stream = std::move(opened->stream);
        built.metadata = std::move(opened->data);
        built.origin = built.metadata->origin;
        built.attribution = opened->online.attribution;
        built.online = std::move(opened->online);
        built.dataHash = std::move(opened->dataHash);
        noteProcessing(config, *built.metadata);
        return built;
    }
    if (config.kind == TerrainKind::Procedural) {
        auto source = terrain::ProceduralSource::create(config.params, config.extent);
        if (!source) {
            return std::unexpected(source.error());
        }
        built.source = *source;
        built.origin = config.origin;
        return built;
    }
    const FileTerrainConfig& f = config.file;
    terrain::RasterOptions options;
    std::vector<std::filesystem::path> files = f.files;
    std::optional<terrain::LidarSummary> lidar;
    if (isPointCloud(f)) {
        if (data == nullptr) {
            return common::makeError("point-cloud terrain needs the data cache");
        }
        auto dem = data->lidarDem(f.files, lidarOptions(f));
        if (!dem) {
            return std::unexpected(dem.error());
        }
        auto summary = terrain::readLidarSummary(*dem);
        if (!summary) {
            return common::makeError(summary.error().message);
        }
        lidar = std::move(*summary);
        files = {*dem};
        options.band = 1;
        options.crs.clear();  // Written into the DTM.
    } else {
        options.band = f.band;
        options.crs = f.crs;
    }

    options.verticalDatum = f.verticalDatum;
    options.origin = f.origin;
    options.originAltitudeM = f.originAltitudeM;
    options.extent = config.extent;
    options.edge = f.edge;
    options.processing = config.processing;
    auto source = terrain::RasterSource::open(files, options);
    if (!source) {
        return common::makeError(source.error().message);
    }
    built.metadata = (*source)->metadata();
    if (lidar) {
        // FR-TER-L06: what the DTM was made from.
        terrain::SourceMetadata& m = *built.metadata;
        std::string ext = f.files.front().extension().string();
        std::ranges::transform(ext, ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        m.format = ext == ".laz" ? "LAZ point cloud → DTM" : "LAS point cloud → DTM";
        m.fileCount = static_cast<int>(f.files.size());
        m.notes.push_back(std::format(
            "Point cloud: {} points, {} ground, {} noise or withheld dropped; {} cells filled "
            "by {}, {} without data",
            lidar->points, lidar->groundPoints, lidar->dropped, lidar->filledCells,
            lidar->interpolation == "idw" ? "IDW" : "TIN", lidar->noDataCells));
        if (lidar->cellHeight == "lowest") {
            m.notes.emplace_back("Cells take the lowest ground point");
        }
    }
    noteProcessing(config, *built.metadata);
    built.origin = built.metadata->origin;
    built.attribution = f.attribution;
    built.source = std::move(*source);
    return built;
}

}  // namespace

bool isPointCloud(const FileTerrainConfig& f) {
    return !f.files.empty() &&
           terrain::elevationFormatOf(f.files.front()) == terrain::ElevationFormat::PointCloud;
}

terrain::LidarOptions lidarOptions(const FileTerrainConfig& f) {
    terrain::LidarOptions o;
    o.resolutionM = f.resolutionM;
    o.interpolation = f.interpolation;
    o.cellHeight = f.cellHeight;
    o.crs = f.crs;
    o.classifyGround = f.classifyGround;
    return o;
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
