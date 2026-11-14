// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/astro/events.hpp"

#include <cmath>
#include <functional>
#include <vector>

#include "ds/astro/sun_moon.hpp"
#include "ds/common/math.hpp"

namespace ds::astro {
namespace {

using namespace std::chrono;

constexpr minutes kStep{10};
constexpr milliseconds kTolerance{500};
constexpr double kSunHorizonDeg = -50.0 / 60.0;  // 34′ refraction + 16′ semidiameter
constexpr double kRefractionDeg = 34.0 / 60.0;

using Function = std::function<double(UtcTime)>;

struct Crossing {
    UtcTime time;
    bool rising;  // f goes from negative to positive.
};

// Bisection on a sign change of @p f between @p a (value fa) and @p b.
UtcTime refine(const Function& f, UtcTime a, double fa, UtcTime b) {
    while (b - a > kTolerance) {
        const UtcTime mid = a + (b - a) / 2;
        const double fm = f(mid);
        if ((fm < 0.0) == (fa < 0.0)) {
            a = mid;
            fa = fm;
        } else {
            b = mid;
        }
    }
    return a + (b - a) / 2;
}

// All sign changes of @p f in [start, start + 24 h), plus whether f was positive at the start.
struct Scan {
    std::vector<Crossing> crossings;
    bool positiveAtStart = false;
    bool anyPositive = false;
    bool anyNegative = false;
};

Scan scan(const Function& f, UtcTime start) {
    Scan result;
    const UtcTime end = start + hours(24);
    UtcTime t0 = start;
    double f0 = f(t0);
    result.positiveAtStart = f0 >= 0.0;
    while (t0 < end) {
        const UtcTime t1 = std::min(t0 + duration_cast<milliseconds>(kStep), end);
        const double f1 = f(t1);
        (f0 >= 0.0 ? result.anyPositive : result.anyNegative) = true;
        if ((f0 < 0.0) != (f1 < 0.0)) {
            const UtcTime t = refine(f, t0, f0, t1);
            if (t >= start && t < end) {
                result.crossings.push_back({t, f1 >= 0.0});
            }
        }
        t0 = t1;
        f0 = f1;
    }
    (f0 >= 0.0 ? result.anyPositive : result.anyNegative) = true;
    return result;
}

std::optional<UtcTime> first(const Scan& s, bool rising) {
    for (const Crossing& c : s.crossings) {
        if (c.rising == rising) {
            return c.time;
        }
    }
    return std::nullopt;
}

// sin(hour angle) rises through 0 at upper transit.
double sinHourAngle(const BodyPosition& body) {
    return std::sin(common::degToRad(body.hourAngleDeg));
}

}  // namespace

SunEvents sunEvents(UtcTime start, const geo::Geodetic& observer) {
    const auto elevationAbove = [&observer](double thresholdDeg) {
        return Function([&observer, thresholdDeg](UtcTime t) {
            return sunPosition(t, observer).airless.elevationDeg - thresholdDeg;
        });
    };
    SunEvents events;
    const Scan horizon = scan(elevationAbove(kSunHorizonDeg), start);
    events.sunrise = first(horizon, true);
    events.sunset = first(horizon, false);
    events.alwaysUp = !horizon.anyNegative;
    events.alwaysDown = !horizon.anyPositive;

    const Scan civil = scan(elevationAbove(-6.0), start);
    events.civilDawn = first(civil, true);
    events.civilDusk = first(civil, false);
    const Scan nautical = scan(elevationAbove(-12.0), start);
    events.nauticalDawn = first(nautical, true);
    events.nauticalDusk = first(nautical, false);
    const Scan astronomical = scan(elevationAbove(-18.0), start);
    events.astronomicalDawn = first(astronomical, true);
    events.astronomicalDusk = first(astronomical, false);

    const Scan transit =
        scan([&observer](UtcTime t) { return sinHourAngle(sunPosition(t, observer)); }, start);
    events.solarNoon = first(transit, true);
    return events;
}

MoonEvents moonEvents(UtcTime start, const geo::Geodetic& observer) {
    MoonEvents events;
    const Scan horizon = scan(
        [&observer](UtcTime t) {
            const BodyPosition moon = moonPosition(t, observer);
            return moon.airless.elevationDeg + moon.semidiameterDeg + kRefractionDeg;
        },
        start);
    events.moonrise = first(horizon, true);
    events.moonset = first(horizon, false);
    events.alwaysUp = !horizon.anyNegative;
    events.alwaysDown = !horizon.anyPositive;
    const Scan transit =
        scan([&observer](UtcTime t) { return sinHourAngle(moonPosition(t, observer)); }, start);
    events.transit = first(transit, true);
    return events;
}

UtcTime nextLunarPhase(UtcTime after, LunarPhase phase) {
    const double target = 90.0 * static_cast<int>(phase);
    const geo::Geodetic centre{};  // Geocentric longitudes do not depend on the observer.
    const Function f = [&](UtcTime t) {
        const double elongation = moonPosition(t, centre).geocentricEcliptic.longitudeDeg -
                                  sunPosition(t, centre).geocentricEcliptic.longitudeDeg;
        return common::wrapDegrees180(elongation - target);
    };
    // The elongation grows ~12.2°/day, so with 1-day steps a genuine crossing is a change from
    // negative to positive; the wrap at ±180° goes the other way and is ignored.
    UtcTime t0 = after;
    double f0 = f(t0);
    for (int i = 0; i < 40; ++i) {
        const UtcTime t1 = t0 + days(1);
        const double f1 = f(t1);
        if (f0 < 0.0 && f1 >= 0.0) {
            return refine(f, t0, f0, t1);
        }
        t0 = t1;
        f0 = f1;
    }
    return t0;  // Unreachable: a lunation lasts at most ~29.8 days.
}

}  // namespace ds::astro
