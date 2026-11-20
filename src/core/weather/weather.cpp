// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/weather/weather.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include "ds/common/random.hpp"

namespace ds::weather {
namespace {

using common::Json;
using common::JsonReader;

constexpr std::array<std::pair<CloudType, std::string_view>, 5> kCloudNames{{
    {CloudType::Cumulus, "cumulus"},
    {CloudType::Stratocumulus, "stratocumulus"},
    {CloudType::Stratus, "stratus"},
    {CloudType::Cirrus, "cirrus"},
    {CloudType::Cumulonimbus, "cumulonimbus"},
}};
constexpr std::array<std::pair<PrecipitationType, std::string_view>, 5> kPrecipitationNames{{
    {PrecipitationType::None, "none"},
    {PrecipitationType::Drizzle, "drizzle"},
    {PrecipitationType::Rain, "rain"},
    {PrecipitationType::Snow, "snow"},
    {PrecipitationType::Sleet, "sleet"},
}};
constexpr std::array<std::pair<Intensity, std::string_view>, 3> kIntensityNames{{
    {Intensity::Light, "light"},
    {Intensity::Moderate, "moderate"},
    {Intensity::Heavy, "heavy"},
}};

// Thickness of a layer whose top is not given, by genus.
double defaultThicknessM(CloudType type) noexcept {
    switch (type) {
        case CloudType::Cumulus: return 800.0;
        case CloudType::Stratocumulus: return 500.0;
        case CloudType::Stratus: return 400.0;
        case CloudType::Cirrus: return 1000.0;
        case CloudType::Cumulonimbus: return 8000.0;
    }
    return 500.0;
}

// Magnus coefficients (Alduchov & Eskridge 1996), over water.
constexpr double kMagnusA = 17.625;
constexpr double kMagnusB = 243.04;

// Wind profile used to move the clouds (as physics::logProfileSpeed over short grass).
constexpr double kRoughnessM = 0.03;
constexpr double kMaxProfileHeightM = 300.0;

constexpr double kCloudFluctuationOktas = 0.75;
constexpr double kCloudPeriodS = 300.0;
constexpr double kPrecipitationFluctuation = 0.4;
constexpr double kPrecipitationPeriodS = 120.0;
constexpr double kMaxSeekStepS = 10.0;
constexpr double kMinVisibilityM = 10.0;

double lerp(double a, double b, double s) noexcept { return a + (b - a) * s; }

double smoothstep(double s) noexcept {
    s = std::clamp(s, 0.0, 1.0);
    return s * s * (3.0 - 2.0 * s);
}

void sortLayers(std::vector<CloudLayer>& layers) {
    std::ranges::stable_sort(layers, {}, &CloudLayer::baseM);
    if (layers.size() > kMaxCloudLayers) {
        layers.resize(kMaxCloudLayers);
    }
}

template <typename Enum, std::size_t N>
std::string_view nameOf(const std::array<std::pair<Enum, std::string_view>, N>& table,
                        Enum value) noexcept {
    for (const auto& [e, name] : table) {
        if (e == value) {
            return name;
        }
    }
    return table[0].second;
}

template <typename Enum, std::size_t N>
std::optional<Enum> parse(const std::array<std::pair<Enum, std::string_view>, N>& table,
                          std::string_view name) noexcept {
    for (const auto& [e, n] : table) {
        if (n == name) {
            return e;
        }
    }
    return std::nullopt;
}

template <typename Enum, std::size_t N>
std::vector<std::string_view> namesOf(
    const std::array<std::pair<Enum, std::string_view>, N>& table) {
    std::vector<std::string_view> names;
    for (const auto& [e, n] : table) {
        names.push_back(n);
    }
    return names;
}

std::optional<std::uint64_t> readSeed(const Json& json) {
    if (!json.is_object() || !json.contains("seed")) {
        return std::uint64_t{0};
    }
    const Json& s = json["seed"];
    if (s.is_number_unsigned()) {
        return s.get<std::uint64_t>();
    }
    if (s.is_number_integer() && s.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(s.get<std::int64_t>());
    }
    return std::nullopt;
}

}  // namespace

std::string_view cloudTypeName(CloudType type) noexcept { return nameOf(kCloudNames, type); }
std::optional<CloudType> parseCloudType(std::string_view name) noexcept {
    return parse(kCloudNames, name);
}
std::string_view precipitationTypeName(PrecipitationType type) noexcept {
    return nameOf(kPrecipitationNames, type);
}
std::optional<PrecipitationType> parsePrecipitationType(std::string_view name) noexcept {
    return parse(kPrecipitationNames, name);
}
std::string_view intensityName(Intensity intensity) noexcept {
    return nameOf(kIntensityNames, intensity);
}
std::optional<Intensity> parseIntensity(std::string_view name) noexcept {
    return parse(kIntensityNames, name);
}

double intensityRateMmH(PrecipitationType type, Intensity intensity) noexcept {
    const auto i = static_cast<std::size_t>(intensity);
    switch (type) {
        case PrecipitationType::None: return 0.0;
        case PrecipitationType::Drizzle: return std::array{0.1, 0.3, 0.6}.at(i);
        case PrecipitationType::Rain: return std::array{1.0, 4.0, 12.0}.at(i);
        case PrecipitationType::Snow:
        case PrecipitationType::Sleet: return std::array{0.5, 1.5, 4.0}.at(i);
    }
    return 0.0;
}

Intensity intensityOf(PrecipitationType type, double rateMmH) noexcept {
    const double light = intensityRateMmH(type, Intensity::Light);
    const double moderate = intensityRateMmH(type, Intensity::Moderate);
    const double heavy = intensityRateMmH(type, Intensity::Heavy);
    if (type == PrecipitationType::None || rateMmH < std::sqrt(light * moderate)) {
        return Intensity::Light;
    }
    return rateMmH < std::sqrt(moderate * heavy) ? Intensity::Moderate : Intensity::Heavy;
}

double WeatherConditions::totalCover() const noexcept {
    double clear = 1.0;
    for (const CloudLayer& l : clouds) {
        clear *= 1.0 - std::clamp(l.coverOktas / 8.0, 0.0, 1.0);
    }
    return 1.0 - clear;
}

double WeatherConditions::visibilityAt(double heightAglM) const noexcept {
    for (const CloudLayer& l : clouds) {
        if (l.coverOktas >= 4.0 && heightAglM >= l.baseM && heightAglM <= l.topM) {
            return kInCloudVisibilityM;
        }
    }
    if (fog.topM > 0.0 && heightAglM > fog.topM) {
        return std::max(visibilityM, kAboveFogVisibilityM);
    }
    return visibilityM;
}

std::optional<double> WeatherConditions::ceilingM() const noexcept {
    std::optional<double> ceiling;
    for (const CloudLayer& l : clouds) {
        if (l.coverOktas >= 5.0 && (!ceiling || l.baseM < *ceiling)) {
            ceiling = l.baseM;
        }
    }
    return ceiling;
}

WeatherConditions defaultConditions() { return WeatherConditions{}; }

double humidityFromDewPoint(double temperatureC, double dewPointC) noexcept {
    const double es = std::exp(kMagnusA * temperatureC / (kMagnusB + temperatureC));
    const double e = std::exp(kMagnusA * dewPointC / (kMagnusB + dewPointC));
    return std::clamp(100.0 * e / es, 0.0, 100.0);
}

double dewPointFromHumidity(double temperatureC, double humidityPct) noexcept {
    const double rh = std::clamp(humidityPct, 0.1, 100.0) / 100.0;
    const double g = std::log(rh) + kMagnusA * temperatureC / (kMagnusB + temperatureC);
    return kMagnusB * g / (kMagnusA - g);
}

// --- WeatherOverrides ------------------------------------------------------------------------

void WeatherOverrides::applyTo(WeatherConditions& c) const {
    if (temperatureC) {
        c.air.temperatureC = *temperatureC;
    }
    if (pressureHpa) {
        c.air.pressureHpa = *pressureHpa;
    }
    if (humidityPct) {
        c.air.humidityPct = std::clamp(*humidityPct, 0.0, 100.0);
    } else if (dewPointC) {
        c.air.humidityPct = humidityFromDewPoint(c.air.temperatureC, *dewPointC);
    }
    if (windLevel) {
        c.windLevel = std::clamp(*windLevel, 0, WindLevelTable::kLevelCount - 1);
    }
    if (windFromDeg) {
        c.windFromDeg = *windFromDeg;
    }
    if (clouds) {
        c.clouds = *clouds;
        sortLayers(c.clouds);
    }
    if (precipitation) {
        c.precipitation = *precipitation;
        if (c.precipitation.type == PrecipitationType::None) {
            c.precipitation.rateMmH = 0.0;
        }
    }
    if (visibilityM) {
        c.visibilityM = *visibilityM;
    }
    if (fog) {
        c.fog = *fog;
    }
    if (thunderstorm) {
        c.thunderstorm = *thunderstorm;
    }
}

void WeatherOverrides::read(JsonReader& r) {
    const auto optional = [&r](std::string_view key, double min, double max) {
        return r.has(key) ? std::optional<double>(r.number(key, min, max)) : std::nullopt;
    };
    temperatureC = optional("temperatureC", -60.0, 60.0);
    humidityPct = optional("humidityPct", 0.0, 100.0);
    dewPointC = optional("dewPointC", -80.0, 60.0);
    pressureHpa = optional("pressureHpa", 850.0, 1100.0);
    visibilityM = optional("visibilityM", 10.0, 100'000.0);
    if (auto wind = r.optionalObject("wind")) {
        if (wind->has("level")) {
            windLevel = wind->integer("level", 0, WindLevelTable::kLevelCount - 1);
        }
        if (wind->has("fromDeg")) {
            windFromDeg = wind->number("fromDeg", 0.0, 360.0);
        }
    }
    if (r.has("clouds")) {
        const std::size_t n = r.array("clouds");
        if (n > kMaxCloudLayers) {
            r.fail("clouds", std::format("at most {} layers", kMaxCloudLayers));
        }
        std::vector<CloudLayer> layers;
        for (std::size_t i = 0; i < std::min(n, kMaxCloudLayers); ++i) {
            JsonReader e = r.element("clouds", i);
            CloudLayer l;
            l.type =
                parseCloudType(e.choice("type", namesOf(kCloudNames))).value_or(CloudType::Cumulus);
            l.coverOktas = e.number("coverOktas", 0.0, 8.0);
            l.baseM = e.number("baseM", 0.0, 20'000.0);
            l.topM = e.number("topM", 0.0, 20'000.0, l.baseM + defaultThicknessM(l.type));
            if (l.topM <= l.baseM) {
                e.fail("topM", "must be above baseM");
                l.topM = l.baseM + defaultThicknessM(l.type);
            }
            layers.push_back(l);
        }
        sortLayers(layers);
        clouds = std::move(layers);
    }
    if (auto p = r.optionalObject("precipitation")) {
        Precipitation out;
        out.type = parsePrecipitationType(p->choice("type", namesOf(kPrecipitationNames)))
                       .value_or(PrecipitationType::None);
        if (out.type != PrecipitationType::None) {
            if (p->has("rateMmH")) {
                out.rateMmH = p->number("rateMmH", 0.0, 200.0);
            } else {
                const std::string i = p->has("intensity")
                                          ? p->choice("intensity", namesOf(kIntensityNames))
                                          : std::string("moderate");
                out.rateMmH =
                    intensityRateMmH(out.type, parseIntensity(i).value_or(Intensity::Moderate));
            }
        }
        precipitation = out;
    }
    if (auto f = r.optionalObject("fog")) {
        Fog out;
        out.topM = f->number("topM", 0.0, 2000.0, 0.0);
        out.haze = f->boolean("haze", false);
        fog = out;
    }
    if (r.has("thunderstorm")) {
        thunderstorm = r.boolean("thunderstorm", false);
    }
}

void WeatherOverrides::write(Json& o) const {
    if (temperatureC) {
        o["temperatureC"] = *temperatureC;
    }
    if (humidityPct) {
        o["humidityPct"] = *humidityPct;
    }
    if (dewPointC) {
        o["dewPointC"] = *dewPointC;
    }
    if (pressureHpa) {
        o["pressureHpa"] = *pressureHpa;
    }
    if (windLevel || windFromDeg) {
        Json w = Json::object();
        if (windLevel) {
            w["level"] = *windLevel;
        }
        if (windFromDeg) {
            w["fromDeg"] = *windFromDeg;
        }
        o["wind"] = std::move(w);
    }
    if (clouds) {
        Json layers = Json::array();
        for (const CloudLayer& l : *clouds) {
            layers.push_back({{"type", cloudTypeName(l.type)},
                              {"coverOktas", l.coverOktas},
                              {"baseM", l.baseM},
                              {"topM", l.topM}});
        }
        o["clouds"] = std::move(layers);
    }
    if (precipitation) {
        Json p = {{"type", precipitationTypeName(precipitation->type)}};
        if (precipitation->type != PrecipitationType::None) {
            p["rateMmH"] = precipitation->rateMmH;
        }
        o["precipitation"] = std::move(p);
    }
    if (visibilityM) {
        o["visibilityM"] = *visibilityM;
    }
    if (fog) {
        Json f = {{"topM", fog->topM}};
        if (fog->haze) {
            f["haze"] = true;
        }
        o["fog"] = std::move(f);
    }
    if (thunderstorm) {
        o["thunderstorm"] = *thunderstorm;
    }
}

bool WeatherOverrides::empty() const noexcept { return *this == WeatherOverrides{}; }

// --- Presets ---------------------------------------------------------------------------------

const WeatherPresetTable& WeatherPresetTable::builtIn() {
    // Same content as assets/environment/weather_presets.json (a test compares them).
    static const WeatherPresetTable kTable = [] {
        using enum CloudType;
        using P = PrecipitationType;
        const auto preset = [](std::string key, std::string name, std::vector<CloudLayer> clouds,
                               Precipitation precipitation, double visibilityM,
                               double fogTopM = 0.0) {
            WeatherPreset p{std::move(key), std::move(name), {}};
            p.values.clouds = std::move(clouds);
            sortLayers(*p.values.clouds);
            p.values.precipitation = precipitation;
            p.values.visibilityM = visibilityM;
            p.values.fog = Fog{fogTopM, false};
            p.values.thunderstorm = false;
            return p;
        };
        const auto rate = [](P type, Intensity i) {
            return Precipitation{type, intensityRateMmH(type, i)};
        };
        const Precipitation none{};
        WeatherPresetTable t;
        auto& v = t.presets_;
        v.push_back(preset("clear", "Clear", {}, none, 50'000.0));
        v.push_back(preset("few-clouds", "Few clouds", {{Cumulus, 2, 1200, 1800}}, none, 40'000.0));
        v.push_back(preset("scattered-clouds", "Scattered clouds", {{Cumulus, 4, 1000, 2000}}, none,
                           30'000.0));
        v.push_back(preset("broken-clouds", "Broken clouds", {{Stratocumulus, 6, 900, 1500}}, none,
                           25'000.0));
        v.push_back(preset("overcast", "Overcast", {{Stratus, 8, 600, 1200}}, none, 20'000.0));
        v.back().values.humidityPct = 80.0;
        v.push_back(preset("mist", "Mist", {}, none, 3'000.0, 200.0));
        v.back().values.humidityPct = 93.0;
        v.push_back(preset("fog", "Fog", {}, none, 500.0, 120.0));
        v.back().values.humidityPct = 99.0;
        v.push_back(preset("drizzle", "Drizzle", {{Stratus, 8, 300, 900}},
                           rate(P::Drizzle, Intensity::Moderate), 8'000.0));
        v.back().values.humidityPct = 95.0;
        v.push_back(preset("light-rain", "Light rain", {{Stratus, 8, 700, 2500}},
                           rate(P::Rain, Intensity::Light), 10'000.0));
        v.back().values.humidityPct = 88.0;
        v.push_back(preset("heavy-rain", "Heavy rain", {{Stratus, 8, 500, 3000}},
                           rate(P::Rain, Intensity::Heavy), 3'000.0));
        v.back().values.humidityPct = 95.0;
        v.push_back(preset("light-snow", "Light snow", {{Stratus, 8, 800, 2500}},
                           rate(P::Snow, Intensity::Light), 4'000.0));
        v.back().values.temperatureC = -2.0;
        v.back().values.humidityPct = 90.0;
        v.push_back(preset("heavy-snow", "Heavy snow", {{Stratus, 8, 500, 3000}},
                           rate(P::Snow, Intensity::Heavy), 1'000.0));
        v.back().values.temperatureC = -4.0;
        v.back().values.humidityPct = 95.0;
        v.push_back(preset("thunderstorm", "Thunderstorm",
                           {{Cumulus, 3, 1200, 2200}, {Cumulonimbus, 6, 900, 11000}},
                           {P::Rain, 20.0}, 5'000.0));
        v.back().values.humidityPct = 90.0;
        v.back().values.thunderstorm = true;
        return t;
    }();
    return kTable;
}

common::Result<WeatherPresetTable> WeatherPresetTable::fromJson(const Json& json) {
    JsonReader root(json);
    (void)root.integer("schemaVersion", 1, 1);
    WeatherPresetTable table;
    const std::size_t n = root.array("presets");
    for (std::size_t i = 0; i < n; ++i) {
        JsonReader e = root.element("presets", i);
        WeatherPreset p;
        p.key = e.string("key");
        p.name = e.string("name");
        p.values.read(e);
        if (table.find(p.key) != nullptr) {
            e.fail("key", std::format("\"{}\" appears more than once", p.key));
        }
        table.presets_.push_back(std::move(p));
    }
    if (root.ok() && table.find("clear") == nullptr) {
        root.fail("presets", "the \"clear\" preset is missing");
    }
    if (!root.ok()) {
        return common::makeError("weather presets: " + root.errorText());
    }
    return table;
}

common::Result<WeatherPresetTable> WeatherPresetTable::load(const std::filesystem::path& file) {
    auto json = common::readJsonFile(file, std::size_t{1} << 20U);
    if (!json) {
        return std::unexpected(json.error());
    }
    return fromJson(*json);
}

const WeatherPreset* WeatherPresetTable::find(std::string_view key) const noexcept {
    const auto it = std::ranges::find(presets_, key, &WeatherPreset::key);
    return it == presets_.end() ? nullptr : &*it;
}

// --- Settings --------------------------------------------------------------------------------

common::Result<WeatherSettings> WeatherSettings::fromJson(const Json& json,
                                                          const WeatherPresetTable& presets) {
    JsonReader r(json);
    WeatherSettings s;
    if (const auto seed = readSeed(json)) {
        s.seed = *seed;
    } else {
        r.fail("seed", "must be a non-negative integer");
    }
    s.preset = r.string("preset", "clear");
    if (presets.find(s.preset) == nullptr) {
        r.fail("preset", std::format("unknown preset \"{}\"", s.preset));
    }
    s.overrides.read(r);
    s.randomWindDirection = r.boolean("randomWindDirection", false);
    s.timelineEnabled = r.boolean("timelineEnabled", true);
    s.forecast = r.boolean("forecast", false);
    const std::size_t n = r.optionalArray("timeline");
    for (std::size_t i = 0; i < n; ++i) {
        JsonReader e = r.element("timeline", i);
        WeatherChange c;
        c.atS = e.number("atS", 0.0, 1e7);
        c.transitionS = e.number("transitionS", 0.0, 1e7, 0.0);
        c.preset = e.string("preset", "");
        if (!c.preset.empty() && presets.find(c.preset) == nullptr) {
            e.fail("preset", std::format("unknown preset \"{}\"", c.preset));
        }
        c.overrides.read(e);
        s.timeline.push_back(std::move(c));
    }
    std::ranges::stable_sort(s.timeline, {}, &WeatherChange::atS);
    if (!r.ok()) {
        return common::makeError("weather: " + r.errorText());
    }
    return s;
}

Json WeatherSettings::toJson() const {
    Json o = Json::object();
    o["seed"] = seed;
    o["preset"] = preset;
    overrides.write(o);
    if (randomWindDirection) {
        o["randomWindDirection"] = true;
    }
    if (!timeline.empty()) {
        Json entries = Json::array();
        for (const WeatherChange& c : timeline) {
            Json e = {{"atS", c.atS}};
            if (c.transitionS > 0.0) {
                e["transitionS"] = c.transitionS;
            }
            if (!c.preset.empty()) {
                e["preset"] = c.preset;
            }
            c.overrides.write(e);
            entries.push_back(std::move(e));
        }
        o["timeline"] = std::move(entries);
    }
    if (!timelineEnabled) {
        o["timelineEnabled"] = false;
    }
    if (forecast) {
        o["forecast"] = true;
    }
    return o;
}

WeatherSettings WeatherSettings::fromBasic(const AirConditions& air, const WindSettings& wind,
                                           double visibilityM, std::uint64_t seed) {
    WeatherSettings s;
    s.seed = seed;
    s.preset = "clear";
    s.overrides.temperatureC = air.temperatureC;
    s.overrides.pressureHpa = air.pressureHpa;
    s.overrides.humidityPct = air.humidityPct;
    s.overrides.windLevel = wind.level;
    s.overrides.windFromDeg = wind.fromDeg;
    s.overrides.visibilityM = visibilityM;
    s.randomWindDirection = wind.randomDirection;
    return s;
}

// --- Blending --------------------------------------------------------------------------------

WeatherConditions blend(const WeatherConditions& a, const WeatherConditions& b, double s) {
    s = std::clamp(s, 0.0, 1.0);
    if (s <= 0.0) {
        return a;
    }
    if (s >= 1.0) {
        return b;
    }
    const bool second = s >= 0.5;
    WeatherConditions c;
    c.air.temperatureC = lerp(a.air.temperatureC, b.air.temperatureC, s);
    c.air.pressureHpa = lerp(a.air.pressureHpa, b.air.pressureHpa, s);
    c.air.humidityPct = lerp(a.air.humidityPct, b.air.humidityPct, s);
    c.windLevel = lerp(a.windLevel, b.windLevel, s);
    const double turn = std::remainder(b.windFromDeg - a.windFromDeg, 360.0);
    c.windFromDeg = a.windFromDeg + turn * s;
    c.windFromDeg -= 360.0 * std::floor(c.windFromDeg / 360.0);

    const std::size_t n = std::max(a.clouds.size(), b.clouds.size());
    for (std::size_t i = 0; i < n; ++i) {
        const CloudLayer* la = i < a.clouds.size() ? &a.clouds[i] : nullptr;
        const CloudLayer* lb = i < b.clouds.size() ? &b.clouds[i] : nullptr;
        CloudLayer l;
        if (la != nullptr && lb != nullptr) {
            l.type = second ? lb->type : la->type;
            l.coverOktas = lerp(la->coverOktas, lb->coverOktas, s);
            l.baseM = lerp(la->baseM, lb->baseM, s);
            l.topM = lerp(la->topM, lb->topM, s);
        } else if (la != nullptr) {
            l = *la;
            l.coverOktas *= 1.0 - s;
        } else {
            l = *lb;
            l.coverOktas *= s;
        }
        c.clouds.push_back(l);
    }
    sortLayers(c.clouds);

    const Precipitation& pa = a.precipitation;
    const Precipitation& pb = b.precipitation;
    if (pa.type == pb.type || pb.type == PrecipitationType::None) {
        c.precipitation = {pa.type, lerp(pa.rateMmH, pb.rateMmH, s)};
    } else if (pa.type == PrecipitationType::None) {
        c.precipitation = {pb.type, lerp(0.0, pb.rateMmH, s)};
    } else {
        c.precipitation = {second ? pb.type : pa.type, lerp(pa.rateMmH, pb.rateMmH, s)};
    }

    c.visibilityM = std::exp(lerp(std::log(std::max(a.visibilityM, kMinVisibilityM)),
                                  std::log(std::max(b.visibilityM, kMinVisibilityM)), s));
    c.fog.topM = lerp(a.fog.topM, b.fog.topM, s);
    c.fog.haze = second ? b.fog.haze : a.fog.haze;
    c.thunderstorm = second ? b.thunderstorm : a.thunderstorm;
    return c;
}

// --- Description -----------------------------------------------------------------------------

std::string_view coverName(double oktas) noexcept {
    const double o = std::round(oktas);
    return o <= 2.0 ? "few" : o <= 4.0 ? "scattered" : o <= 7.0 ? "broken" : "overcast";
}

std::string describe(const WeatherConditions& c, const WindLevelTable& windTable) {
    std::vector<std::string> parts;
    const int level = static_cast<int>(std::lround(c.windLevel));
    if (level <= 0) {
        parts.emplace_back("Calm");
    } else {
        const WindLevel w = windTable.interpolated(c.windLevel);
        parts.push_back(std::format("Wind {:03.0f}° level {} ({:.0f} km/h, gusts {:.0f})",
                                    std::fmod(std::round(c.windFromDeg) + 360.0, 360.0), level,
                                    w.meanSpeedMs * 3.6, w.gustPeakMs * 3.6));
    }
    std::vector<std::string> sky;
    for (const CloudLayer& l : c.clouds) {
        if (l.coverOktas >= 0.5) {
            sky.push_back(std::format("{}{} at {:.0f} m", coverName(l.coverOktas),
                                      coverName(l.coverOktas) == "overcast" ? "" : " clouds",
                                      l.baseM));
            if (l.type == CloudType::Cumulonimbus) {
                sky.back() += " (cumulonimbus)";
            }
        }
    }
    if (sky.empty()) {
        parts.emplace_back("clear sky");
    } else {
        std::string joined = sky.front();
        for (std::size_t i = 1; i < sky.size(); ++i) {
            joined += ", " + sky[i];
        }
        parts.push_back(std::move(joined));
    }
    if (c.thunderstorm) {
        parts.emplace_back("thunderstorm");
    }
    if (c.precipitation.type != PrecipitationType::None && c.precipitation.rateMmH > 0.0) {
        parts.push_back(std::format(
            "{} {}", intensityName(intensityOf(c.precipitation.type, c.precipitation.rateMmH)),
            precipitationTypeName(c.precipitation.type)));
    }
    if (c.fog.topM > 0.0) {
        parts.push_back(std::format("{} up to {:.0f} m", c.fog.haze ? "haze" : "fog", c.fog.topM));
    } else if (c.fog.haze) {
        parts.emplace_back("haze");
    }
    if (c.visibilityM < 1000.0) {
        parts.push_back(
            std::format("visibility {:.0f} m", std::round(c.visibilityM / 10.0) * 10.0));
    } else if (c.visibilityM < 10'000.0) {
        parts.push_back(std::format("visibility {:.1f} km", c.visibilityM / 1000.0));
    } else {
        parts.push_back(std::format("visibility {:.0f} km", c.visibilityM / 1000.0));
    }
    parts.push_back(std::format("{:.0f} °C", c.air.temperatureC));
    parts.push_back(std::format("QNH {:.0f}", c.air.pressureHpa));
    std::string out = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) {
        out += " · " + parts[i];
    }
    return out;
}

// --- Ground ----------------------------------------------------------------------------------

namespace {

constexpr double kSoakingRainMm = 1.0;
constexpr double kDryingTimeS = 1800.0;  // At 50 % humidity.
constexpr double kMaxDryingTimeS = 21'600.0;
constexpr double kSnowPerWater = 10.0;  // Fresh snow depth per water equivalent.
constexpr double kSnowMaxC = 1.0;
constexpr double kMeltMPerDegreeHour = 0.005;
constexpr double kInitialSnowHours = 3.0;

bool liquid(PrecipitationType type) noexcept {
    return type == PrecipitationType::Drizzle || type == PrecipitationType::Rain ||
           type == PrecipitationType::Sleet;
}

bool settles(const WeatherConditions& c) noexcept {
    return (c.precipitation.type == PrecipitationType::Snow ||
            c.precipitation.type == PrecipitationType::Sleet) &&
           c.air.temperatureC <= kSnowMaxC && c.precipitation.rateMmH > 0.0;
}

}  // namespace

GroundCover initialGroundCover(const WeatherConditions& c) noexcept {
    GroundCover g;
    const double rate = c.precipitation.rateMmH;
    if (liquid(c.precipitation.type) && rate > 0.0) {
        g.wetness = 1.0;
    } else if ((c.fog.topM > 0.0 && c.visibilityM < 1000.0) || c.air.humidityPct >= 97.0) {
        g.wetness = 0.3;
    }
    if (settles(c)) {
        g.snowDepthM = std::clamp(rate * kSnowPerWater * kInitialSnowHours / 1000.0, 0.01, 0.30);
        g.wetness = c.air.temperatureC > 0.0 ? 0.6 : 0.0;
    }
    return g;
}

void updateGroundCover(GroundCover& g, const WeatherConditions& c, double dt) noexcept {
    if (dt <= 0.0) {
        return;
    }
    const double hours = dt / 3600.0;
    const double rate = c.precipitation.rateMmH;
    const bool raining = liquid(c.precipitation.type) && rate > 0.0;
    if (raining) {
        g.wetness = std::min(1.0, g.wetness + rate * hours / kSoakingRainMm);
    } else {
        const double dryness = std::max(0.05, 1.0 - c.air.humidityPct / 100.0);
        const double tau = std::min(kMaxDryingTimeS, kDryingTimeS * 0.5 / dryness);
        g.wetness *= std::exp(-dt / tau);
    }
    if (settles(c)) {
        g.snowDepthM += rate * kSnowPerWater * hours / 1000.0;
    } else if (g.snowDepthM > 0.0 && c.air.temperatureC > 0.0) {
        g.snowDepthM =
            std::max(0.0, g.snowDepthM - kMeltMPerDegreeHour * c.air.temperatureC * hours);
        g.wetness = std::max(g.wetness, 0.6);  // Melt water.
    }
}

double fallSpeedMs(const Precipitation& p) noexcept {
    switch (p.type) {
        case PrecipitationType::None: return 0.0;
        case PrecipitationType::Drizzle: return 2.0;
        case PrecipitationType::Rain:
            // Larger drops in heavier rain (median diameter grows with the rate).
            return std::clamp(5.0 + 1.9 * std::log10(std::max(p.rateMmH, 0.1)), 3.5, 9.0);
        case PrecipitationType::Sleet: return 4.0;
        case PrecipitationType::Snow: return 1.0;
    }
    return 0.0;
}

// --- Generator -------------------------------------------------------------------------------

WeatherGenerator::WeatherGenerator() : WeatherGenerator(WeatherSettings{}) {}

WeatherGenerator::WeatherGenerator(WeatherSettings settings, const WeatherPresetTable& presets,
                                   const WindLevelTable& windTable)
    : settings_(std::move(settings)), windTable_(windTable), base_(defaultConditions()) {
    const auto applyPreset = [&presets](std::string_view key, WeatherConditions& c) {
        if (const WeatherPreset* p = presets.find(key)) {
            p->values.applyTo(c);
        } else if (const WeatherPreset* clear = presets.find("clear")) {
            clear->values.applyTo(c);
        }
    };
    applyPreset(settings_.preset, base_);
    settings_.overrides.applyTo(base_);
    if (settings_.randomWindDirection) {
        // Same draw as physics::WindParameters::fromLevel for this seed.
        common::SplitMix64 rng(common::mix64(settings_.seed ^ 0x57494E44ULL));  // "WIND"
        base_.windFromDeg = std::floor(rng.uniform() * 360.0);
    }
    std::ranges::stable_sort(settings_.timeline, {}, &WeatherChange::atS);
    WeatherConditions previous = base_;
    for (const WeatherChange& change : settings_.timeline) {
        WeatherConditions target = previous;
        if (!change.preset.empty()) {
            applyPreset(change.preset, target);
        }
        change.overrides.applyTo(target);
        targets_.push_back(target);
        previous = std::move(target);
    }
    restart();
}

WeatherConditions WeatherGenerator::plannedAt(double timeS) const {
    WeatherConditions c = base_;
    if (!settings_.timelineEnabled) {
        return c;
    }
    for (std::size_t i = 0; i < settings_.timeline.size(); ++i) {
        const WeatherChange& change = settings_.timeline[i];
        if (timeS < change.atS) {
            break;
        }
        const double s =
            change.transitionS > 0.0 ? smoothstep((timeS - change.atS) / change.transitionS) : 1.0;
        c = blend(c, targets_[i], s);
    }
    return c;
}

double WeatherGenerator::noise(std::uint64_t channel, double timeS, double periodS) const noexcept {
    // Smooth value noise in [-1, 1]: random values at knots every period, smoothstep between.
    const auto octave = [this, channel, timeS](double period, std::uint64_t salt) {
        const double x = std::max(timeS, 0.0) / period;
        const double k = std::floor(x);
        const auto knot = [&](double index) {
            const std::uint64_t h =
                common::mix64(settings_.seed ^ (channel * 0x9E3779B97F4A7C15ULL) ^ (salt << 56U) ^
                              static_cast<std::uint64_t>(index));
            return 2.0 * (static_cast<double>(h >> 11U) * 0x1.0p-53) - 1.0;
        };
        return lerp(knot(k), knot(k + 1.0), smoothstep(x - k));
    };
    return 0.7 * octave(periodS, 1) + 0.3 * octave(periodS / 3.0, 2);
}

WeatherConditions WeatherGenerator::conditionsAt(double timeS) const {
    WeatherConditions c = plannedAt(timeS);
    for (std::size_t i = 0; i < c.clouds.size(); ++i) {
        CloudLayer& l = c.clouds[i];
        const double room = std::clamp(std::min(l.coverOktas, 8.0 - l.coverOktas), 0.0, 1.0);
        l.coverOktas = std::clamp(
            l.coverOktas + kCloudFluctuationOktas * room * noise(i + 1, timeS, kCloudPeriodS), 0.0,
            8.0);
    }
    if (c.precipitation.type != PrecipitationType::None && c.precipitation.rateMmH > 0.0) {
        const double factor =
            1.0 + kPrecipitationFluctuation * noise(16, timeS, kPrecipitationPeriodS);
        c.precipitation.rateMmH *= factor;
        // Heavier showers see less far (visibility ∝ rate^−0.6, Koschmieder-type fits).
        c.visibilityM = std::max(kMinVisibilityM, c.visibilityM * std::pow(factor, -0.6));
    }
    return c;
}

double WeatherGenerator::windSpeedAt(double level, double heightAglM) const noexcept {
    const double speed10 = windTable_.interpolated(level).meanSpeedMs;
    const double h = std::min(heightAglM, kMaxProfileHeightM);
    if (h <= kRoughnessM || speed10 <= 0.0) {
        return 0.0;
    }
    return speed10 * std::log(h / kRoughnessM) / std::log(10.0 / kRoughnessM);
}

void WeatherGenerator::restart() {
    state_ = WeatherState{};
    state_.conditions = conditionsAt(0.0);
    state_.ground = initialGroundCover(plannedAt(0.0));  // The hours before: no fluctuation.
}

void WeatherGenerator::moveClouds(double dt) {
    const WeatherConditions& c = state_.conditions;
    const double from = c.windFromDeg * std::numbers::pi / 180.0;
    // The air moves away from the "from" direction.
    const double east = -std::sin(from);
    const double north = -std::cos(from);
    for (std::size_t i = 0; i < std::min(c.clouds.size(), kMaxCloudLayers); ++i) {
        const CloudLayer& l = c.clouds[i];
        const double speed = windSpeedAt(c.windLevel, 0.5 * (l.baseM + l.topM));
        state_.cloudDrift[i].eastM += east * speed * dt;
        state_.cloudDrift[i].northM += north * speed * dt;
    }
}

const WeatherState& WeatherGenerator::advance(double dt) {
    if (dt > 0.0) {
        moveClouds(dt);
        state_.timeS += dt;
        state_.conditions = conditionsAt(state_.timeS);
        updateGroundCover(state_.ground, state_.conditions, dt);
    }
    return state_;
}

const WeatherState& WeatherGenerator::seek(double timeS) {
    restart();
    while (state_.timeS < timeS) {
        advance(std::min(kMaxSeekStepS, timeS - state_.timeS));
    }
    return state_;
}

}  // namespace ds::weather
