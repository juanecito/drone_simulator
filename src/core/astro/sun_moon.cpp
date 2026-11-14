// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Re-implemented from J. Meeus, "Astronomical Algorithms", 2nd ed. (1998), chapters 25, 32,
// 40, 47 and 48, with the VSOP87D and table 47.A/B coefficients from their generated files.

#include "ds/astro/sun_moon.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>

#include "ds/astro/nutation.hpp"
#include "ds/common/math.hpp"

#include "moon_tables.hpp"
#include "vsop87_earth.hpp"

namespace ds::astro {
namespace {

using common::degToRad;
using common::normalizeDegrees;
using common::radToDeg;

constexpr double kSunRadiusKm = 695700.0;  // IAU 2015 nominal
constexpr double kMoonRadiusKm = 1737.4;
constexpr double kArcsec = 1.0 / 3600.0;

double evaluate(const std::array<std::span<const detail::VsopTerm>, 6>& series, double tau) {
    double result = 0.0;
    double power = 1.0;
    for (const auto& terms : series) {
        double sum = 0.0;
        for (const auto& term : terms) {
            sum += term.a * std::cos(term.b + term.c * tau);
        }
        result += sum * power;
        power *= tau;
    }
    return result;
}

double polynomial(double t, std::initializer_list<double> coefficients) {
    double result = 0.0;
    double power = 1.0;
    for (const double c : coefficients) {
        result += c * power;
        power *= t;
    }
    return result;
}

// Fills the observer-dependent fields from the apparent geocentric position.
BodyPosition finish(BodyPosition body, UtcTime time, const geo::Geodetic& observer,
                    const Atmosphere& atmosphere, double radiusKm) {
    const double lst = localApparentSiderealTimeDeg(time, observer.longitudeDeg);
    const Topocentric topo = toTopocentric(body.geocentric, body.distanceKm, observer, lst);
    body.topocentric = topo.equatorial;
    body.topocentricDistanceKm = topo.distanceKm;
    body.hourAngleDeg = common::wrapDegrees180(lst - topo.equatorial.rightAscensionDeg);
    body.airless = equatorialToHorizontal(topo.equatorial, lst, observer.latitudeDeg);
    body.apparent = body.airless;
    body.apparent.elevationDeg += refractionDeg(body.airless.elevationDeg, atmosphere);
    body.semidiameterDeg = radToDeg(std::asin(radiusKm / topo.distanceKm));
    return body;
}

}  // namespace

BodyPosition sunPosition(UtcTime time, const geo::Geodetic& observer,
                         const Atmosphere& atmosphere) {
    const double jde = julianEphemerisDay(time);
    const double tau = (jde - kJ2000) / 365250.0;  // Julian millennia
    const double t = tau * 10.0;                   // Julian centuries

    const double earthL = evaluate(detail::kEarthL, tau);
    const double earthB = evaluate(detail::kEarthB, tau);
    const double radiusAu = evaluate(detail::kEarthR, tau);

    // Geocentric geometric position (dynamical frame of date).
    double lambda = normalizeDegrees(radToDeg(earthL) + 180.0);
    double beta = -radToDeg(earthB);
    // Conversion to the FK5 system (Meeus eq. 32.3).
    const double lambdaPrime = degToRad(lambda - 1.397 * t - 0.00031 * t * t);
    lambda += -0.09033 * kArcsec;
    beta += 0.03916 * kArcsec * (std::cos(lambdaPrime) - std::sin(lambdaPrime));

    const Nutation n = nutation(jde);
    // Nutation and annual aberration (Meeus eq. 25.10).
    const double apparentLambda = lambda + n.longitudeDeg - 20.4898 * kArcsec / radiusAu;

    BodyPosition body;
    body.geocentricEcliptic = {normalizeDegrees(apparentLambda), beta};
    body.geocentric = eclipticToEquatorial(body.geocentricEcliptic, n.trueObliquityDeg());
    body.distanceKm = radiusAu * kAstronomicalUnitKm;
    return finish(body, time, observer, atmosphere, kSunRadiusKm);
}

BodyPosition moonPosition(UtcTime time, const geo::Geodetic& observer,
                          const Atmosphere& atmosphere) {
    const double jde = julianEphemerisDay(time);
    const double t = (jde - kJ2000) / 36525.0;

    // Fundamental arguments, degrees (Meeus eqs. 47.1–47.5).
    const double lp = polynomial(
        t, {218.3164477, 481267.88123421, -0.0015786, 1.0 / 538841.0, -1.0 / 65194000.0});
    const double d = polynomial(
        t, {297.8501921, 445267.1114034, -0.0018819, 1.0 / 545868.0, -1.0 / 113065000.0});
    const double m = polynomial(t, {357.5291092, 35999.0502909, -0.0001536, 1.0 / 24490000.0});
    const double mp =
        polynomial(t, {134.9633964, 477198.8675055, 0.0087414, 1.0 / 69699.0, -1.0 / 14712000.0});
    const double f = polynomial(
        t, {93.2720950, 483202.0175233, -0.0036539, -1.0 / 3526000.0, 1.0 / 863310000.0});
    const double a1 = 119.75 + 131.849 * t;
    const double a2 = 53.09 + 479264.290 * t;
    const double a3 = 313.45 + 481266.484 * t;
    const double e = 1.0 - 0.002516 * t - 0.0000074 * t * t;  // Earth orbit eccentricity factor

    const double rd = degToRad(d);
    const double rm = degToRad(m);
    const double rmp = degToRad(mp);
    const double rf = degToRad(f);
    const auto eccentricity = [e](int multipleOfM) {
        const int k = std::abs(multipleOfM);
        return k == 0 ? 1.0 : (k == 1 ? e : e * e);
    };

    double sumL = 0.0;
    double sumR = 0.0;
    for (const auto& term : detail::kMoonLr) {
        const double arg = term.d * rd + term.m * rm + term.mp * rmp + term.f * rf;
        const double factor = eccentricity(term.m);
        sumL += term.sumL * factor * std::sin(arg);
        sumR += term.sumR * factor * std::cos(arg);
    }
    double sumB = 0.0;
    for (const auto& term : detail::kMoonB) {
        const double arg = term.d * rd + term.m * rm + term.mp * rmp + term.f * rf;
        sumB += term.sumB * eccentricity(term.m) * std::sin(arg);
    }
    // Additive terms: Venus (A1), Jupiter (A2) and the Earth's flattening (L').
    sumL += 3958.0 * std::sin(degToRad(a1)) + 1962.0 * std::sin(degToRad(lp - f)) +
            318.0 * std::sin(degToRad(a2));
    sumB += -2235.0 * std::sin(degToRad(lp)) + 382.0 * std::sin(degToRad(a3)) +
            175.0 * std::sin(degToRad(a1 - f)) + 175.0 * std::sin(degToRad(a1 + f)) +
            127.0 * std::sin(degToRad(lp - mp)) - 115.0 * std::sin(degToRad(lp + mp));

    const Nutation n = nutation(jde);
    BodyPosition body;
    body.geocentricEcliptic = {normalizeDegrees(lp + sumL / 1.0e6 + n.longitudeDeg), sumB / 1.0e6};
    body.geocentric = eclipticToEquatorial(body.geocentricEcliptic, n.trueObliquityDeg());
    body.distanceKm = 385000.56 + sumR / 1000.0;
    return finish(body, time, observer, atmosphere, kMoonRadiusKm);
}

MoonPhase moonPhase(const BodyPosition& sun, const BodyPosition& moon,
                    double latitudeDeg) noexcept {
    // Seen from the observer: topocentric Moon, (practically) geocentric Sun.
    const double a0 = degToRad(sun.topocentric.rightAscensionDeg);
    const double d0 = degToRad(sun.topocentric.declinationDeg);
    const double a = degToRad(moon.topocentric.rightAscensionDeg);
    const double d = degToRad(moon.topocentric.declinationDeg);

    // Geocentric elongation ψ and phase angle i (Meeus eqs. 48.2, 48.3).
    const double cosPsi =
        std::sin(d0) * std::sin(d) + std::cos(d0) * std::cos(d) * std::cos(a0 - a);
    const double psi = std::acos(std::clamp(cosPsi, -1.0, 1.0));
    const double r = sun.topocentricDistanceKm;
    const double delta = moon.topocentricDistanceKm;
    const double i = std::atan2(r * std::sin(psi), delta - r * std::cos(psi));

    MoonPhase phase;
    phase.phaseAngleDeg = radToDeg(i);
    phase.illuminatedFraction = (1.0 + std::cos(i)) / 2.0;
    phase.elongationDeg = normalizeDegrees(moon.geocentricEcliptic.longitudeDeg -
                                           sun.geocentricEcliptic.longitudeDeg);
    phase.waxing = phase.elongationDeg < 180.0;
    // Position angle of the bright limb (Meeus eq. 48.5).
    const double chi =
        std::atan2(std::cos(d0) * std::sin(a0 - a),
                   std::sin(d0) * std::cos(d) - std::cos(d0) * std::sin(d) * std::cos(a0 - a));
    phase.brightLimbPositionAngleDeg = normalizeDegrees(radToDeg(chi));
    const double q =
        parallacticAngleDeg(moon.hourAngleDeg, moon.topocentric.declinationDeg, latitudeDeg);
    phase.brightLimbZenithAngleDeg = normalizeDegrees(phase.brightLimbPositionAngleDeg - q);
    return phase;
}

}  // namespace ds::astro
