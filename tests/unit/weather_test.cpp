// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// Weather generator (SPECIFICATION §4.2.3, FR-WX-01…03).

#include "ds/weather/weather.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "ds/common/json.hpp"

namespace {

using namespace ds::weather;
using ds::common::Json;

const std::filesystem::path kPresetsFile =
    std::filesystem::path(DS_TEST_ASSETS_DIR) / "environment" / "weather_presets.json";

WeatherSettings parse(const char* text) {
    auto json = ds::common::parseJson(text);
    EXPECT_TRUE(json.has_value());
    auto settings = WeatherSettings::fromJson(*json);
    EXPECT_TRUE(settings.has_value()) << (settings ? "" : settings.error().message);
    return settings.value_or(WeatherSettings{});
}

TEST(WeatherPresets, AssetFileMatchesBuiltInTable) {
    const auto table = WeatherPresetTable::load(kPresetsFile);
    ASSERT_TRUE(table.has_value()) << table.error().message;
    EXPECT_EQ(*table, WeatherPresetTable::builtIn());
}

TEST(WeatherPresets, AllThirteenPresetsOfTheSpecificationExist) {
    for (const char* key :
         {"clear", "few-clouds", "scattered-clouds", "broken-clouds", "overcast", "mist", "fog",
          "drizzle", "light-rain", "heavy-rain", "light-snow", "heavy-snow", "thunderstorm"}) {
        EXPECT_NE(WeatherPresetTable::builtIn().find(key), nullptr) << key;
    }
    EXPECT_EQ(WeatherPresetTable::builtIn().presets().size(), 13U);
}

TEST(WeatherPresets, RejectsDuplicatesAndAMissingClearPreset) {
    auto dup = ds::common::parseJson(R"({"schemaVersion":1,"presets":[
        {"key":"clear","name":"Clear"},{"key":"clear","name":"Again"}]})");
    EXPECT_FALSE(WeatherPresetTable::fromJson(*dup).has_value());
    auto noClear = ds::common::parseJson(R"({"schemaVersion":1,"presets":[
        {"key":"fog","name":"Fog","visibilityM":400}]})");
    EXPECT_FALSE(WeatherPresetTable::fromJson(*noClear).has_value());
}

TEST(WeatherConditions, PresetsGiveTheirSky) {
    const WeatherConditions clear =
        WeatherGenerator(parse(R"({"seed":1,"preset":"clear"})")).plannedAt(0.0);
    EXPECT_TRUE(clear.clouds.empty());
    EXPECT_EQ(clear.precipitation.type, PrecipitationType::None);
    EXPECT_DOUBLE_EQ(clear.visibilityM, 50'000.0);
    EXPECT_DOUBLE_EQ(clear.air.temperatureC, 15.0);

    const WeatherConditions overcast =
        WeatherGenerator(parse(R"({"seed":1,"preset":"overcast"})")).plannedAt(0.0);
    ASSERT_EQ(overcast.clouds.size(), 1U);
    EXPECT_DOUBLE_EQ(overcast.totalCover(), 1.0);
    EXPECT_EQ(overcast.ceilingM(), 600.0);

    const WeatherConditions storm =
        WeatherGenerator(parse(R"({"seed":1,"preset":"thunderstorm"})")).plannedAt(0.0);
    ASSERT_EQ(storm.clouds.size(), 2U);
    EXPECT_EQ(storm.clouds[0].type, CloudType::Cumulonimbus);  // Lowest base first.
    EXPECT_TRUE(storm.thunderstorm);

    const WeatherConditions snow =
        WeatherGenerator(parse(R"({"seed":1,"preset":"heavy-snow"})")).plannedAt(0.0);
    EXPECT_EQ(snow.precipitation.type, PrecipitationType::Snow);
    EXPECT_LT(snow.air.temperatureC, 0.0);
    EXPECT_EQ(intensityOf(snow.precipitation.type, snow.precipitation.rateMmH), Intensity::Heavy);
}

TEST(WeatherConditions, OverridesChangeThePreset) {
    const WeatherConditions c = WeatherGenerator(parse(R"({
        "seed": 9, "preset": "broken-clouds", "temperatureC": 12, "humidityPct": 80,
        "pressureHpa": 1009, "wind": {"level": 4, "fromDeg": 270},
        "clouds": [{"type": "stratocumulus", "coverOktas": 6, "baseM": 900, "topM": 1500}],
        "precipitation": {"type": "rain", "intensity": "light"}, "visibilityM": 9000,
        "fog": {"topM": 50}})"))
                                    .plannedAt(0.0);
    EXPECT_DOUBLE_EQ(c.air.temperatureC, 12.0);
    EXPECT_DOUBLE_EQ(c.air.humidityPct, 80.0);
    EXPECT_DOUBLE_EQ(c.air.pressureHpa, 1009.0);
    EXPECT_DOUBLE_EQ(c.windLevel, 4.0);
    EXPECT_DOUBLE_EQ(c.windFromDeg, 270.0);
    EXPECT_DOUBLE_EQ(c.precipitation.rateMmH, 1.0);
    EXPECT_DOUBLE_EQ(c.visibilityM, 9000.0);
    EXPECT_DOUBLE_EQ(c.fog.topM, 50.0);
}

TEST(WeatherConditions, DewPointGivesTheHumidity) {
    EXPECT_NEAR(humidityFromDewPoint(20.0, 20.0), 100.0, 1e-9);
    EXPECT_NEAR(humidityFromDewPoint(20.0, 10.0), 52.5, 0.5);  // Psychrometric tables.
    EXPECT_NEAR(dewPointFromHumidity(20.0, humidityFromDewPoint(20.0, 4.0)), 4.0, 1e-9);
    const WeatherConditions c =
        WeatherGenerator(parse(R"({"seed":1,"preset":"clear","temperatureC":20,"dewPointC":10})"))
            .plannedAt(0.0);
    EXPECT_NEAR(c.air.humidityPct, 52.5, 0.5);
}

TEST(WeatherConditions, VisibilityInCloudAndAboveFog) {
    WeatherConditions c;
    c.visibilityM = 400.0;
    c.fog.topM = 100.0;
    c.clouds = {{CloudType::Stratus, 8.0, 500.0, 900.0}, {CloudType::Cirrus, 2.0, 6000.0, 7000.0}};
    EXPECT_DOUBLE_EQ(c.visibilityAt(10.0), 400.0);
    EXPECT_DOUBLE_EQ(c.visibilityAt(200.0), kAboveFogVisibilityM);
    EXPECT_DOUBLE_EQ(c.visibilityAt(700.0), kInCloudVisibilityM);
    EXPECT_DOUBLE_EQ(c.visibilityAt(6500.0), kAboveFogVisibilityM);  // Thin cirrus: no.
}

TEST(WeatherTimeline, RainArrivesSmoothly) {
    // The example of SPECIFICATION §4.2.3: rain and stronger wind after 4 minutes.
    const WeatherGenerator g(parse(R"({
        "seed": 90210, "preset": "broken-clouds", "temperatureC": 12,
        "wind": {"level": 4, "fromDeg": 270},
        "timeline": [{"atS": 240, "transitionS": 120, "preset": "light-rain",
                      "wind": {"level": 6}}]})"));
    const WeatherConditions before = g.plannedAt(200.0);
    const WeatherConditions middle = g.plannedAt(300.0);
    const WeatherConditions after = g.plannedAt(400.0);
    EXPECT_EQ(before.precipitation.type, PrecipitationType::None);
    EXPECT_DOUBLE_EQ(before.windLevel, 4.0);
    EXPECT_EQ(middle.precipitation.type, PrecipitationType::Rain);
    EXPECT_NEAR(middle.windLevel, 5.0, 1e-9);  // Half-way (smoothstep(0.5) = 0.5).
    EXPECT_NEAR(middle.precipitation.rateMmH, 0.5, 1e-9);
    EXPECT_EQ(after.precipitation.type, PrecipitationType::Rain);
    EXPECT_DOUBLE_EQ(after.windLevel, 6.0);
    EXPECT_DOUBLE_EQ(after.windFromDeg, 270.0);      // Direction kept.
    EXPECT_DOUBLE_EQ(after.air.temperatureC, 12.0);  // Temperature kept.
    ASSERT_EQ(after.clouds.size(), 1U);
    EXPECT_EQ(after.clouds[0].type, CloudType::Stratus);
    EXPECT_DOUBLE_EQ(after.clouds[0].coverOktas, 8.0);

    // Continuous: no jumps between consecutive seconds.
    double previous = g.plannedAt(0.0).visibilityM;
    for (double t = 1.0; t <= 600.0; t += 1.0) {
        const double v = g.plannedAt(t).visibilityM;
        EXPECT_LT(std::abs(std::log(v / previous)), 0.05) << t;
        previous = v;
    }

    WeatherSettings off = g.settings();
    off.timelineEnabled = false;
    EXPECT_EQ(WeatherGenerator(off).plannedAt(400.0).precipitation.type, PrecipitationType::None);
}

TEST(WeatherTimeline, WindDirectionTurnsAlongTheShorterArc) {
    const WeatherGenerator g(parse(R"({"seed":1,"preset":"clear","wind":{"level":3,"fromDeg":350},
        "timeline":[{"atS":0,"transitionS":100,"wind":{"fromDeg":30}}]})"));
    EXPECT_NEAR(g.plannedAt(50.0).windFromDeg, 10.0, 1e-9);
}

TEST(WeatherGenerator, SameSeedSameWeatherAndCoverFluctuates) {
    const WeatherSettings s = parse(R"({"seed":42,"preset":"scattered-clouds"})");
    const WeatherGenerator a(s);
    const WeatherGenerator b(s);
    WeatherSettings other = s;
    other.seed = 43;
    const WeatherGenerator c(other);
    double minCover = 8.0;
    double maxCover = 0.0;
    bool differs = false;
    for (double t = 0.0; t < 3600.0; t += 10.0) {
        const double cover = a.conditionsAt(t).clouds.at(0).coverOktas;
        EXPECT_DOUBLE_EQ(cover, b.conditionsAt(t).clouds.at(0).coverOktas);
        differs = differs || cover != c.conditionsAt(t).clouds.at(0).coverOktas;
        minCover = std::min(minCover, cover);
        maxCover = std::max(maxCover, cover);
    }
    EXPECT_TRUE(differs);
    EXPECT_GE(minCover, 4.0 - 0.75);
    EXPECT_LE(maxCover, 4.0 + 0.75);
    EXPECT_GT(maxCover - minCover, 0.4);

    // Clear and overcast skies stay as they are.
    const WeatherGenerator ovc(parse(R"({"seed":42,"preset":"overcast"})"));
    for (double t = 0.0; t < 600.0; t += 30.0) {
        EXPECT_DOUBLE_EQ(ovc.conditionsAt(t).clouds.at(0).coverOktas, 8.0);
    }
}

TEST(WeatherGenerator, PrecipitationFluctuatesAroundItsRate) {
    const WeatherGenerator g(parse(R"({"seed":7,"preset":"heavy-rain"})"));
    double sum = 0.0;
    int n = 0;
    for (double t = 0.0; t < 7200.0; t += 5.0) {
        const double rate = g.conditionsAt(t).precipitation.rateMmH;
        EXPECT_GE(rate, 12.0 * 0.6 - 1e-9);
        EXPECT_LE(rate, 12.0 * 1.4 + 1e-9);
        sum += rate;
        ++n;
    }
    EXPECT_NEAR(sum / n, 12.0, 1.5);
}

TEST(WeatherGenerator, CloudsDriftWithTheWind) {
    WeatherGenerator g(
        parse(R"({"seed":1,"preset":"broken-clouds","wind":{"level":4,"fromDeg":270}})"));
    for (int i = 0; i < 600; ++i) {
        g.advance(0.1);
    }
    const WeatherState& s = g.state();
    EXPECT_NEAR(s.timeS, 60.0, 1e-9);
    // From the west → moves east, at the speed at mid-layer height (constant above 300 m).
    const double speed = g.windSpeedAt(4.0, 1200.0);
    EXPECT_GT(speed, 6.7);
    EXPECT_NEAR(s.cloudDrift[0].eastM, speed * 60.0, 1e-6);
    EXPECT_NEAR(s.cloudDrift[0].northM, 0.0, 1e-6);
    const WeatherState jumped = g.seek(60.0);
    EXPECT_NEAR(jumped.cloudDrift[0].eastM, speed * 60.0, 1e-6);
    g.restart();
    EXPECT_DOUBLE_EQ(g.state().cloudDrift[0].eastM, 0.0);
}

TEST(WeatherGenerator, RandomDirectionComesFromTheSeed) {
    const WeatherSettings a = WeatherSettings::fromBasic(
        {}, {.level = 3, .fromDeg = 0.0, .randomDirection = true}, 20'000.0, 5);
    WeatherSettings b = a;
    b.seed = 6;
    const double da = WeatherGenerator(a).plannedAt(0.0).windFromDeg;
    EXPECT_DOUBLE_EQ(da, WeatherGenerator(a).plannedAt(0.0).windFromDeg);
    EXPECT_NE(da, WeatherGenerator(b).plannedAt(0.0).windFromDeg);
    EXPECT_DOUBLE_EQ(WeatherGenerator(a).plannedAt(0.0).visibilityM, 20'000.0);
}

TEST(WeatherSettings, JsonRoundTrip) {
    const WeatherSettings s = parse(R"({
        "seed": 18446744073709551615, "preset": "overcast", "dewPointC": 3,
        "wind": {"level": 2}, "precipitation": {"type": "drizzle", "rateMmH": 0.2},
        "timeline": [{"atS": 600, "preset": "fog"}, {"atS": 60, "transitionS": 30,
                      "clouds": []}],
        "forecast": true})");
    EXPECT_EQ(s.seed, 18446744073709551615ULL);
    ASSERT_EQ(s.timeline.size(), 2U);
    EXPECT_DOUBLE_EQ(s.timeline[0].atS, 60.0);  // Sorted by time.
    const auto back = WeatherSettings::fromJson(s.toJson());
    ASSERT_TRUE(back.has_value()) << back.error().message;
    EXPECT_EQ(*back, s);
}

TEST(WeatherSettings, RejectsUnknownPresets) {
    auto json = ds::common::parseJson(R"({"seed":1,"preset":"hail"})");
    EXPECT_FALSE(WeatherSettings::fromJson(*json).has_value());
    json =
        ds::common::parseJson(R"({"seed":1,"preset":"clear","timeline":[{"atS":1,"preset":"x"}]})");
    EXPECT_FALSE(WeatherSettings::fromJson(*json).has_value());
}

TEST(WeatherConditions, BriefingSummary) {
    // The example of FR-WX-10.
    const WeatherConditions c = WeatherGenerator(parse(R"({
        "seed": 1, "preset": "light-rain", "temperatureC": 12, "pressureHpa": 1009,
        "wind": {"level": 4, "fromDeg": 270}, "visibilityM": 6000,
        "clouds": [{"type": "stratocumulus", "coverOktas": 6, "baseM": 900}]})"))
                                    .plannedAt(0.0);
    EXPECT_EQ(describe(c),
              "Wind 270° level 4 (24 km/h, gusts 34) · broken clouds at 900 m · "
              "light rain · visibility 6.0 km · 12 °C · QNH 1009");
    const WeatherConditions fog =
        WeatherGenerator(parse(R"({"seed":1,"preset":"fog"})")).plannedAt(0.0);
    EXPECT_EQ(describe(fog),
              "Calm · clear sky · fog up to 120 m · visibility 500 m · 15 °C · QNH 1013");
}

TEST(WindLevels, InterpolatedLevels) {
    const WindLevelTable& t = WindLevelTable::builtIn();
    EXPECT_EQ(t.interpolated(4.0), t.level(4));
    const WindLevel half = t.interpolated(4.5);
    EXPECT_DOUBLE_EQ(half.meanSpeedMs, 0.5 * (6.7 + 9.4));
    EXPECT_DOUBLE_EQ(t.interpolated(0.5).gustIntervalS, t.level(1).gustIntervalS);
    EXPECT_EQ(t.interpolated(-3.0), t.level(0));
    EXPECT_EQ(t.interpolated(42.0), t.level(10));
}

}  // namespace
