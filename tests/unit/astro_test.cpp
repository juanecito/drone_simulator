// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "ds/astro/events.hpp"
#include "ds/astro/nutation.hpp"
#include "ds/astro/sky_coordinates.hpp"
#include "ds/astro/stars.hpp"
#include "ds/astro/sun_moon.hpp"
#include "ds/astro/time.hpp"
#include "ds/common/math.hpp"

#include "fixture_csv.hpp"

namespace {

using namespace ds::astro;
using namespace std::chrono;
using ds::common::degToRad;
using ds::common::radToDeg;
using ds::test::CsvTable;

constexpr double kArcsec = 1.0 / 3600.0;

UtcTime utc(std::string_view iso) {
    auto t = parseUtc(iso);
    EXPECT_TRUE(t.has_value()) << iso;
    return t.value_or(UtcTime{});
}

// UTC instant corresponding to a Julian Ephemeris Day (for Meeus' examples given in TD).
UtcTime fromJde(double jde) {
    const UtcTime approx = fromJulianDay(jde);
    return fromJulianDay(jde - deltaTSeconds(decimalYear(approx)) / 86400.0);
}

// Angle between two directions given as azimuth/elevation (or RA/Dec), degrees.
double separationDeg(double lon1, double lat1, double lon2, double lat2) {
    const double a = degToRad(lat1);
    const double b = degToRad(lat2);
    const double d = degToRad(lon2 - lon1);
    const double x = std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(d);
    const double y = std::cos(b) * std::sin(d);
    const double z = std::sin(a) * std::sin(b) + std::cos(a) * std::cos(b) * std::cos(d);
    return radToDeg(std::atan2(std::hypot(x, y), z));
}

double angleDiffDeg(double a, double b) { return std::abs(ds::common::wrapDegrees180(a - b)); }

// --- Time scales (FR-AST-01) -------------------------------------------------------------------

TEST(AstroTime, JulianDay) {
    EXPECT_DOUBLE_EQ(julianDay(utc("2000-01-01T12:00:00Z")), 2451545.0);
    EXPECT_NEAR(julianDay(utc("1957-10-04T19:26:24Z")), 2436116.31, 1e-8);  // Meeus 7.a
    EXPECT_NEAR(julianDay(fromJulianDay(2460000.25)), 2460000.25, 1e-8);
}

TEST(AstroTime, DeltaT) {
    EXPECT_NEAR(deltaTSeconds(1900.0), -2.79, 1e-9);
    EXPECT_NEAR(deltaTSeconds(1950.0), 29.07, 1e-9);
    EXPECT_NEAR(deltaTSeconds(1975.0), 45.45, 1e-9);
    EXPECT_NEAR(deltaTSeconds(2000.0), 63.86, 1e-9);
    EXPECT_NEAR(deltaTSeconds(2005.0), 64.69, 0.1);
    // Continuity at the segment boundaries (within 0.5 s).
    for (const double y : {1920.0, 1941.0, 1961.0, 1986.0, 2005.0, 2050.0}) {
        EXPECT_NEAR(deltaTSeconds(y - 1e-6), deltaTSeconds(y), 0.6) << y;
    }
}

TEST(AstroTime, SiderealTimeMeeusExamples) {
    // Meeus examples 12.a and 12.b.
    EXPECT_NEAR(greenwichMeanSiderealTimeDeg(utc("1987-04-10T00:00:00Z")),
                (13.0 + 10.0 / 60.0 + 46.3668 / 3600.0) * 15.0, 1e-6);
    EXPECT_NEAR(greenwichMeanSiderealTimeDeg(utc("1987-04-10T19:21:00Z")),
                (8.0 + 34.0 / 60.0 + 57.0896 / 3600.0) * 15.0, 1e-6);
    EXPECT_NEAR(greenwichApparentSiderealTimeDeg(utc("1987-04-10T00:00:00Z")),
                (13.0 + 10.0 / 60.0 + 46.1351 / 3600.0) * 15.0, 0.05 * 15.0 / 3600.0);
}

TEST(AstroTime, NutationMeeusExample) {
    const Nutation n = nutation(2446895.5);  // Meeus 22.a, 1987-04-10 0h TD.
    EXPECT_NEAR(n.longitudeDeg, -3.788 * kArcsec, 0.5 * kArcsec);
    EXPECT_NEAR(n.obliquityDeg, 9.443 * kArcsec, 0.1 * kArcsec);
    EXPECT_NEAR(n.meanObliquityDeg, 23.0 + 26.0 / 60.0 + 27.407 * kArcsec, 0.001 * kArcsec);
    EXPECT_NEAR(n.trueObliquityDeg(), 23.0 + 26.0 / 60.0 + 36.850 * kArcsec, 0.1 * kArcsec);
}

TEST(AstroTime, ParseAndFormatIso8601) {
    EXPECT_EQ(formatUtc(utc("2026-08-12T21:40:00Z")), "2026-08-12T21:40:00Z");
    EXPECT_EQ(formatUtc(utc("2026-08-12T23:40:00+02:00")), "2026-08-12T21:40:00Z");
    EXPECT_EQ(formatUtc(utc("2026-08-12T21:40:00.250Z")), "2026-08-12T21:40:00.250Z");
    EXPECT_EQ(utc("2026-01-01T00:00:00-03:30"), utc("2026-01-01T03:30:00Z"));
    for (const char* bad : {"2026-08-12T21:40:00", "2026-02-30T00:00:00Z", "2026-8-12T21:40:00Z",
                            "2026-08-12 21:40:00Z", "2026-08-12T24:00:00Z", "", "x",
                            "2026-08-12T21:40:00Zjunk", "2026-08-12T21:40:00."}) {
        EXPECT_FALSE(parseUtc(bad).has_value()) << bad;
    }
}

TEST(AstroTime, SupportedRange) {
    EXPECT_TRUE(isSupported(utc("1900-01-01T00:00:00Z")));
    EXPECT_TRUE(isSupported(utc("2100-12-31T23:59:59Z")));
    EXPECT_FALSE(isSupported(utc("1899-12-31T23:59:59Z")));
    EXPECT_FALSE(isSupported(utc("2101-01-01T00:00:00Z")));
}

// --- Local time and DST (FR-AST-02) -------------------------------------------------------------

TEST(AstroLocalTime, MadridWinterSummerAndTransitions) {
    const auto winter = toLocalTime(utc("2026-01-15T12:00:00Z"), "Europe/Madrid");
    ASSERT_TRUE(winter.has_value()) << winter.error().message;
    EXPECT_EQ(formatLocal(winter->time), "2026-01-15 13:00:00");
    EXPECT_FALSE(winter->daylightSaving);
    EXPECT_EQ(winter->utcOffset, hours(1));

    const auto summer = toLocalTime(utc("2026-07-01T12:00:00Z"), "Europe/Madrid");
    ASSERT_TRUE(summer.has_value());
    EXPECT_EQ(formatLocal(summer->time), "2026-07-01 14:00:00");
    EXPECT_TRUE(summer->daylightSaving);
    EXPECT_EQ(summer->abbreviation, "CEST");

    // 2026-03-29: 01:00 UTC, clocks jump from 02:00 CET to 03:00 CEST.
    EXPECT_EQ(formatLocal(toLocalTime(utc("2026-03-29T00:59:59Z"), "Europe/Madrid")->time),
              "2026-03-29 01:59:59");
    EXPECT_EQ(formatLocal(toLocalTime(utc("2026-03-29T01:00:00Z"), "Europe/Madrid")->time),
              "2026-03-29 03:00:00");
}

TEST(AstroLocalTime, LocalToUtcAroundTransitions) {
    using Local = local_time<milliseconds>;
    const auto local = [](int y, unsigned m, unsigned d, int h, int mi) {
        return Local(local_days(year(y) / month(m) / day(d)).time_since_epoch() + hours(h) +
                     minutes(mi));
    };
    EXPECT_EQ(*fromLocalTime(local(2026, 7, 1, 14, 0), "Europe/Madrid"),
              utc("2026-07-01T12:00:00Z"));
    // Non-existent 02:30 (spring forward) → treated as 03:30 CEST.
    EXPECT_EQ(*fromLocalTime(local(2026, 3, 29, 2, 30), "Europe/Madrid"),
              utc("2026-03-29T01:30:00Z"));
    // Ambiguous 02:30 (fall back) → the earlier instant (still CEST).
    EXPECT_EQ(*fromLocalTime(local(2026, 10, 25, 2, 30), "Europe/Madrid"),
              utc("2026-10-25T00:30:00Z"));
    // Southern hemisphere: Sydney is on DST in January.
    EXPECT_TRUE(toLocalTime(utc("2026-01-15T00:00:00Z"), "Australia/Sydney")->daylightSaving);
}

TEST(AstroLocalTime, UnknownZoneAndSolarTime) {
    EXPECT_FALSE(toLocalTime(utc("2026-01-01T00:00:00Z"), "Mars/Olympus_Mons").has_value());
    EXPECT_EQ(formatLocal(meanSolarTime(utc("2026-01-01T12:00:00Z"), -90.0)),
              "2026-01-01 06:00:00");
    EXPECT_EQ(formatLocal(meanSolarTime(utc("2026-01-01T12:00:00Z"), 15.0)), "2026-01-01 13:00:00");
}

// --- Coordinates and refraction ----------------------------------------------------------------

TEST(AstroCoordinates, RefractionBehaviour) {
    const Atmosphere standard{};
    EXPECT_DOUBLE_EQ(refractionDeg(90.0, standard), 0.0);
    EXPECT_NEAR(refractionDeg(45.0, standard), 1.0 / 60.0, 0.05 / 60.0);  // About 1′.
    EXPECT_GT(refractionDeg(0.0, standard), 0.45);  // About half a degree at the horizon.
    EXPECT_LT(refractionDeg(0.0, standard), 0.55);
    EXPECT_GT(refractionDeg(5.0, standard), refractionDeg(10.0, standard));
    const Atmosphere thin{700.0, 10.0};  // About 3 000 m.
    EXPECT_NEAR(refractionDeg(10.0, thin), refractionDeg(10.0, standard) * 700.0 / 1010.0, 1e-12);
    EXPECT_DOUBLE_EQ(refractionDeg(-5.0, standard), refractionDeg(-1.0, standard));
}

TEST(AstroCoordinates, HorizontalOfCelestialPoleIsLatitude) {
    for (const double lat : {-60.0, -10.0, 0.0, 40.0, 75.0}) {
        const Horizontal h = equatorialToHorizontal({123.0, 90.0}, 17.0, lat);
        EXPECT_NEAR(h.elevationDeg, lat, 1e-9);
    }
}

// --- Sun (FR-AST-04) ----------------------------------------------------------------------------

TEST(AstroSun, MeeusExample25b) {
    // 1992-10-13 0h TD: apparent α = 13h13m30.749s, δ = −7°47′01.74″, R = 0.99760853 AU.
    const BodyPosition sun = sunPosition(fromJde(2448908.5), {});
    EXPECT_NEAR(sun.geocentric.rightAscensionDeg, (13.0 + 13.0 / 60.0 + 30.749 / 3600.0) * 15.0,
                1.0 * kArcsec);
    EXPECT_NEAR(sun.geocentric.declinationDeg, -(7.0 + 47.0 / 60.0 + 1.74 / 3600.0), 1.0 * kArcsec);
    EXPECT_NEAR(sun.distanceKm / kAstronomicalUnitKm, 0.99760853, 2e-7);
}

TEST(AstroSun, MatchesHorizonsWithin0p01Deg) {
    const CsvTable table("astro/horizons_sun.csv");
    ASSERT_GE(table.size(), 200U);
    double worst = 0.0;
    double worstRefracted = 0.0;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const ds::geo::Geodetic observer{table.number(i, "lat_deg"), table.number(i, "lon_deg"),
                                         table.number(i, "height_m")};
        const BodyPosition sun = sunPosition(fromJulianDay(table.number(i, "jd_ut")), observer);
        const double az = table.number(i, "azimuth_deg");
        const double sep = separationDeg(sun.airless.azimuthDeg, sun.airless.elevationDeg, az,
                                         table.number(i, "elevation_airless_deg"));
        worst = std::max(worst, sep);
        EXPECT_LT(sep, 0.01) << "row " << i;
        // Horizons gives the observer–Sun range.
        EXPECT_NEAR(sun.topocentricDistanceKm / kAstronomicalUnitKm, table.number(i, "distance_au"),
                    1e-6);
        const double refracted = table.number(i, "elevation_refracted_deg");
        if (refracted > 5.0) {
            const double err = std::abs(sun.apparent.elevationDeg - refracted);
            worstRefracted = std::max(worstRefracted, err);
            EXPECT_LT(err, 0.01) << "row " << i;
        }
    }
    RecordProperty("worst_airless_deg", std::to_string(worst));
    RecordProperty("worst_refracted_deg", std::to_string(worstRefracted));
    std::printf("Sun: worst airless %.5f deg, worst refracted %.5f deg\n", worst, worstRefracted);
}

// --- Moon (FR-AST-05, FR-AST-06) ----------------------------------------------------------------

TEST(AstroMoon, MeeusExample47a) {
    // 1992-04-12 0h TD: geometric λ = 133.162655° + Δψ 0.004610° = apparent λ 133.167265°,
    // β = −3.229126°, Δ = 368409.7 km, α = 134.688470°, δ = 13.768368°.
    const BodyPosition moon = moonPosition(fromJde(2448724.5), {});
    EXPECT_NEAR(moon.geocentricEcliptic.longitudeDeg, 133.167265, 1e-4);
    EXPECT_NEAR(moon.geocentricEcliptic.latitudeDeg, -3.229126, 1e-6);
    EXPECT_NEAR(moon.distanceKm, 368409.7, 0.1);
    EXPECT_NEAR(moon.geocentric.rightAscensionDeg, 134.688470, 2e-4);
    EXPECT_NEAR(moon.geocentric.declinationDeg, 13.768368, 1e-4);
}

TEST(AstroMoon, MatchesHorizonsTopocentric) {
    const CsvTable table("astro/horizons_moon.csv");
    ASSERT_GE(table.size(), 200U);
    double worst = 0.0;
    double worstRange = 0.0;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const ds::geo::Geodetic observer{table.number(i, "lat_deg"), table.number(i, "lon_deg"),
                                         table.number(i, "height_m")};
        const UtcTime t = fromJulianDay(table.number(i, "jd_ut"));
        const BodyPosition moon = moonPosition(t, observer);
        const BodyPosition sun = sunPosition(t, observer);
        const double sep =
            separationDeg(moon.airless.azimuthDeg, moon.airless.elevationDeg,
                          table.number(i, "azimuth_deg"), table.number(i, "elevation_airless_deg"));
        worst = std::max(worst, sep);
        EXPECT_LT(sep, 0.05) << "row " << i;  // NFR-REAL-10
        const double rangeError = std::abs(moon.topocentricDistanceKm -
                                           table.number(i, "distance_au") * kAstronomicalUnitKm);
        worstRange = std::max(worstRange, rangeError);

        const MoonPhase phase = moonPhase(sun, moon, observer.latitudeDeg);
        const double k = table.number(i, "illumination_percent") / 100.0;
        EXPECT_NEAR(phase.illuminatedFraction, k, 0.01) << "row " << i;
        if (k > 0.02 && k < 0.98) {  // The limb angle is undefined at new and full moon.
            EXPECT_LT(
                angleDiffDeg(phase.brightLimbPositionAngleDeg, table.number(i, "sub_solar_pa_deg")),
                2.0)
                << "row " << i;
        }
    }
    // The truncated ELP series of Meeus ch. 47 keeps distance terms down to ~1 km, so the range
    // is good to a few tens of km (≈0.02 %); that moves the parallax by < 0.5″.
    EXPECT_LT(worstRange, 100.0);
    RecordProperty("worst_topocentric_deg", std::to_string(worst));
    RecordProperty("worst_range_km", std::to_string(worstRange));
    std::printf("Moon: worst direction %.5f deg, worst range %.1f km\n", worst, worstRange);
}

TEST(AstroMoon, WaxingAndZenithAngle) {
    // Evening crescent: waxing, bright limb towards the Sun (low in the west-ish).
    const ds::geo::Geodetic madrid{40.4168, -3.7038, 667.0};
    const UtcTime t = utc("2026-08-16T20:00:00Z");  // ~4 days after the new moon of Aug 12.
    const BodyPosition sun = sunPosition(t, madrid);
    const BodyPosition moon = moonPosition(t, madrid);
    const MoonPhase phase = moonPhase(sun, moon, madrid.latitudeDeg);
    EXPECT_TRUE(phase.waxing);
    EXPECT_LT(phase.illuminatedFraction, 0.35);
    EXPECT_GT(phase.illuminatedFraction, 0.05);
    // The Sun is below the horizon to the west: the bright limb points down-right in the sky,
    // i.e. between 90° and 270° from the zenith direction.
    EXPECT_GT(phase.brightLimbZenithAngleDeg, 90.0);
    EXPECT_LT(phase.brightLimbZenithAngleDeg, 270.0);
}

// --- Events (FR-AST-07, NFR-REAL-10) ------------------------------------------------------------

std::optional<UtcTime> pick(const SunEvents& e, const std::string& event) {
    if (event == "rise")
        return e.sunrise;
    if (event == "set")
        return e.sunset;
    if (event == "transit")
        return e.solarNoon;
    if (event == "civil_dawn")
        return e.civilDawn;
    if (event == "civil_dusk")
        return e.civilDusk;
    return std::nullopt;
}

std::optional<UtcTime> pick(const MoonEvents& e, const std::string& event) {
    if (event == "rise")
        return e.moonrise;
    if (event == "set")
        return e.moonset;
    if (event == "transit")
        return e.transit;
    return std::nullopt;
}

TEST(AstroEvents, MatchUsno) {
    const CsvTable table("astro/usno_rise_set.csv");
    ASSERT_GE(table.size(), 400U);
    std::map<std::string, SunEvents> sunCache;
    std::map<std::string, MoonEvents> moonCache;
    int compared = 0;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const ds::geo::Geodetic observer{table.number(i, "lat_deg"), table.number(i, "lon_deg"),
                                         0.0};
        const std::string& date = table.text(i, "date");
        const UtcTime start = utc(date + "T00:00:00Z");
        const std::string key = table.text(i, "site") + date;
        const std::string& body = table.text(i, "body");
        const std::string& event = table.text(i, "event");
        const std::string& expected = table.text(i, "time_utc");

        std::optional<UtcTime> ours;
        if (body == "sun") {
            if (!sunCache.contains(key)) {
                sunCache[key] = sunEvents(start, observer);
            }
            ours = pick(sunCache[key], event);
        } else {
            if (!moonCache.contains(key)) {
                moonCache[key] = moonEvents(start, observer);
            }
            ours = pick(moonCache[key], event);
        }
        const std::string where = key + " " + body + " " + event;
        if (expected.empty()) {
            // USNO omits the transit of a body that stays below the horizon; we still compute
            // it, so only a missing rise/set/twilight must match.
            if (event != "transit") {
                EXPECT_FALSE(ours.has_value()) << where << " ours: " << formatUtc(*ours);
            }
            continue;
        }
        ASSERT_TRUE(ours.has_value()) << where << " expected " << expected;
        const UtcTime reference = utc(date + "T" + expected + ":00Z");
        // At high latitudes the Moon can rise or set twice in one day; USNO's list then has two
        // entries and the fixture keeps the last one, while we report the first.
        if (body == "moon" && reference - *ours > minutes(30)) {
            const MoonEvents later = moonEvents(*ours + minutes(1), observer);
            const auto again = pick(later, event);
            if (again && *again < start + days(1)) {
                ours = again;
            }
        }
        const double errorMin =
            std::abs(duration<double, std::ratio<60>>(*ours - reference).count());
        // USNO times are rounded to the minute; NFR-REAL-10: Sun 1 min, Moon 2 min.
        const double tolerance = body == "sun" ? 1.0 : 2.0;
        EXPECT_LE(errorMin, tolerance)
            << where << " ours " << formatUtc(*ours) << " USNO " << expected;
        ++compared;
    }
    EXPECT_GT(compared, 300);
}

TEST(AstroEvents, PolarDayAndNight) {
    const ds::geo::Geodetic tromso{69.6492, 18.9553, 10.0};
    const SunEvents midsummer = sunEvents(utc("2026-06-21T00:00:00Z"), tromso);
    EXPECT_TRUE(midsummer.alwaysUp);
    EXPECT_FALSE(midsummer.sunrise.has_value());
    EXPECT_FALSE(midsummer.sunset.has_value());
    EXPECT_TRUE(midsummer.solarNoon.has_value());

    const SunEvents midwinter = sunEvents(utc("2026-12-21T00:00:00Z"), tromso);
    EXPECT_TRUE(midwinter.alwaysDown);
    EXPECT_FALSE(midwinter.sunrise.has_value());
    EXPECT_TRUE(midwinter.civilDawn.has_value());  // Twilight still happens at noon.
}

TEST(AstroEvents, TwilightOrder) {
    const ds::geo::Geodetic madrid{40.4168, -3.7038, 0.0};
    const SunEvents e = sunEvents(utc("2026-03-20T00:00:00Z"), madrid);
    ASSERT_TRUE(e.astronomicalDawn && e.nauticalDawn && e.civilDawn && e.sunrise && e.solarNoon &&
                e.sunset && e.civilDusk && e.nauticalDusk && e.astronomicalDusk);
    EXPECT_LT(*e.astronomicalDawn, *e.nauticalDawn);
    EXPECT_LT(*e.nauticalDawn, *e.civilDawn);
    EXPECT_LT(*e.civilDawn, *e.sunrise);
    EXPECT_LT(*e.sunrise, *e.solarNoon);
    EXPECT_LT(*e.solarNoon, *e.sunset);
    EXPECT_LT(*e.sunset, *e.civilDusk);
    EXPECT_LT(*e.civilDusk, *e.nauticalDusk);
    EXPECT_LT(*e.nauticalDusk, *e.astronomicalDusk);
}

TEST(AstroEvents, LunarPhasesMatchUsno) {
    const CsvTable table("astro/usno_phases.csv");
    ASSERT_GE(table.size(), 40U);
    const std::map<std::string, LunarPhase> names{{"New Moon", LunarPhase::NewMoon},
                                                  {"First Quarter", LunarPhase::FirstQuarter},
                                                  {"Full Moon", LunarPhase::FullMoon},
                                                  {"Last Quarter", LunarPhase::LastQuarter}};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const UtcTime reference = utc(table.text(i, "time_utc"));
        const LunarPhase phase = names.at(table.text(i, "phase"));
        const UtcTime ours = nextLunarPhase(reference - days(3), phase);
        const double errorMin =
            std::abs(duration<double, std::ratio<60>>(ours - reference).count());
        EXPECT_LE(errorMin, 2.0) << table.text(i, "phase") << " " << table.text(i, "time_utc")
                                 << " ours " << formatUtc(ours);
    }
}

// --- Stars (FR-AST-08) --------------------------------------------------------------------------

std::filesystem::path catalogue() {
    return std::filesystem::path(DS_TEST_ASSETS_DIR) / "sky" / "bsc5.csv";
}

TEST(AstroStars, CatalogueLoads) {
    const auto all = loadStarCatalog(catalogue(), 99.0);
    ASSERT_TRUE(all.has_value()) << all.error().message;
    EXPECT_GT(all->size(), 9000U);
    const auto visible = loadStarCatalog(catalogue(), 6.5);
    ASSERT_TRUE(visible.has_value());
    EXPECT_GT(visible->size(), 8000U);
    EXPECT_LT(visible->size(), all->size());
    const auto sirius = std::ranges::find(*visible, 2491, &CatalogStar::hr);
    ASSERT_NE(sirius, visible->end());
    EXPECT_NEAR(sirius->visualMagnitude, -1.46, 1e-9);
    EXPECT_NEAR(sirius->pmDecArcsecPerYear, -1.205, 1e-9);
}

TEST(AstroStars, MalformedCatalogueIsAnError) {
    const auto path = std::filesystem::temp_directory_path() / "ds_bad_catalogue.csv";
    std::ofstream(path) << "hr,name,ra,dec,pmra,pmdec,v,bv\n1,X,abc,0,0,0,1,0\n";
    EXPECT_FALSE(loadStarCatalog(path).has_value());
    std::filesystem::remove(path);
    EXPECT_FALSE(loadStarCatalog("/nonexistent/stars.csv").has_value());
}

TEST(AstroStars, ApparentPlacesMatchAstropy) {
    const auto stars = loadStarCatalog(catalogue(), 99.0);
    ASSERT_TRUE(stars.has_value());
    const CsvTable table("astro/stars_apparent.csv");
    ASSERT_GE(table.size(), 30U);
    for (std::size_t i = 0; i < table.size(); ++i) {
        const int hr = static_cast<int>(table.number(i, "hr"));
        const auto star = std::ranges::find(*stars, hr, &CatalogStar::hr);
        ASSERT_NE(star, stars->end());
        const SkyFrame frame(utc(table.text(i, "time_utc")), {});
        const Equatorial p = frame.apparent(*star);
        const double sep = separationDeg(p.rightAscensionDeg, p.declinationDeg,
                                         table.number(i, "ra_deg"), table.number(i, "dec_deg"));
        EXPECT_LT(sep, 2.0 * kArcsec) << "HR " << hr << " " << table.text(i, "time_utc");
    }
}

TEST(AstroStars, PolarisAltitudeIsLatitude) {
    const auto stars = loadStarCatalog(catalogue(), 3.0);
    ASSERT_TRUE(stars.has_value());
    const auto polaris = std::ranges::find(*stars, 424, &CatalogStar::hr);
    ASSERT_NE(polaris, stars->end());
    for (const double lat : {10.0, 40.4168, 69.6}) {
        for (const char* when : {"2026-01-01T00:00:00Z", "2026-07-01T06:00:00Z"}) {
            const SkyFrame frame(utc(when), {lat, -3.7, 0.0});
            const Horizontal h = frame.horizontal(*polaris, {1010.0, 10.0});
            EXPECT_NEAR(h.elevationDeg, lat, 0.8)
                << lat << " " << when;  // Polaris ≈ 0.65° off the pole
        }
    }
}

}  // namespace
