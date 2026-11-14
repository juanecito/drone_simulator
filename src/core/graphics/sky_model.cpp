// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/sky_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "ds/astro/nutation.hpp"
#include "ds/common/math.hpp"

namespace ds::graphics {
namespace {

using common::degToRad;

// Solar illuminance at 1 AU above the atmosphere (photopic), lux.
constexpr double kSolarIlluminanceLux = 128'000.0;
// Effective temperature of the Sun, K.
constexpr double kSunTemperatureK = 5772.0;

double gaussianLobe(double x, double mu, double sigmaLow, double sigmaHigh) noexcept {
    const double t = (x - mu) / (x < mu ? sigmaLow : sigmaHigh);
    return std::exp(-0.5 * t * t);
}

glm::dvec3 cieXyz(double lambdaNm) noexcept {
    const double x = 1.056 * gaussianLobe(lambdaNm, 599.8, 37.9, 31.0) +
                     0.362 * gaussianLobe(lambdaNm, 442.0, 16.0, 26.7) -
                     0.065 * gaussianLobe(lambdaNm, 501.1, 20.4, 26.2);
    const double y = 0.821 * gaussianLobe(lambdaNm, 568.8, 46.9, 40.5) +
                     0.286 * gaussianLobe(lambdaNm, 530.9, 16.3, 31.1);
    const double z = 1.217 * gaussianLobe(lambdaNm, 437.0, 11.8, 36.0) +
                     0.681 * gaussianLobe(lambdaNm, 459.0, 26.0, 13.8);
    return {x, y, z};
}

glm::vec3 toVec3(const glm::dvec3& v) { return glm::vec3(v); }

}  // namespace

double magnitudeToIlluminanceLux(double magnitude) noexcept {
    return std::pow(10.0, -0.4 * (magnitude + 13.98));
}

double moonMagnitude(double phaseAngleDeg, double distanceKm) noexcept {
    const double a = std::abs(phaseAngleDeg);
    return -12.73 + 0.026 * a + 4.0e-9 * a * a * a * a + 5.0 * std::log10(distanceKm / 384'400.0);
}

double colorIndexToTemperature(double bv) noexcept {
    bv = std::clamp(bv, -0.4, 2.0);
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62));
}

glm::vec3 blackbodyColor(double kelvin) noexcept {
    constexpr double c2 = 1.4387769e-2;  // Second radiation constant, m·K.
    glm::dvec3 xyz(0.0);
    for (double nm = 380.0; nm <= 780.0; nm += 5.0) {
        const double m = nm * 1e-9;
        const double radiance = 1.0 / (m * m * m * m * m * (std::exp(c2 / (m * kelvin)) - 1.0));
        xyz += cieXyz(nm) * radiance;
    }
    xyz /= xyz.y;
    glm::dvec3 rgb(3.2406 * xyz.x - 1.5372 * xyz.y - 0.4986 * xyz.z,
                   -0.9689 * xyz.x + 1.8758 * xyz.y + 0.0415 * xyz.z,
                   0.0557 * xyz.x - 0.2040 * xyz.y + 1.0570 * xyz.z);
    rgb = glm::max(rgb, glm::dvec3(0.0));
    const double luminance = 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
    return toVec3(rgb / luminance);
}

glm::dvec3 horizontalToWorld(double azimuthDeg, double elevationDeg) noexcept {
    const double az = degToRad(azimuthDeg);
    const double el = degToRad(elevationDeg);
    const double east = std::cos(el) * std::sin(az);
    const double north = std::cos(el) * std::cos(az);
    return {east, std::sin(el), -north};
}

glm::dmat3 equatorialToWorld(double lastDeg, double latitudeDeg) noexcept {
    const double t = degToRad(lastDeg);
    const double p = degToRad(latitudeDeg);
    const double st = std::sin(t);
    const double ct = std::cos(t);
    const double sp = std::sin(p);
    const double cp = std::cos(p);
    // E = −sinθ·ex + cosθ·ey;  N = −sinφ·cosθ·ex − sinφ·sinθ·ey + cosφ·ez;
    // U = cosφ·cosθ·ex + cosφ·sinθ·ey + sinφ·ez;  world = (E, U, −N).
    glm::dmat3 m(0.0);  // Column-major: m[column][row].
    m[0] = {-st, cp * ct, sp * ct};
    m[1] = {ct, cp * st, sp * st};
    m[2] = {0.0, sp, -cp};
    return m;
}

SkyState computeSkyState(astro::UtcTime time, const geo::Geodetic& observer) {
    SkyState s;
    s.time = time;
    s.observer = observer;
    const astro::BodyPosition sun = astro::sunPosition(time, observer);
    const astro::BodyPosition moon = astro::moonPosition(time, observer);
    s.moonPhase = astro::moonPhase(sun, moon, observer.latitudeDeg);

    const glm::dvec3 sunDir = horizontalToWorld(sun.apparent.azimuthDeg, sun.apparent.elevationDeg);
    const glm::dvec3 moonDir =
        horizontalToWorld(moon.apparent.azimuthDeg, moon.apparent.elevationDeg);

    const double au = sun.distanceKm / astro::kAstronomicalUnitKm;
    s.sun.direction = toVec3(sunDir);
    s.sun.illuminanceLux = static_cast<float>(kSolarIlluminanceLux / (au * au));
    s.sun.color = blackbodyColor(kSunTemperatureK);
    s.sun.angularRadiusRad = static_cast<float>(degToRad(sun.semidiameterDeg));
    s.sun.azimuthDeg = sun.apparent.azimuthDeg;
    s.sun.elevationDeg = sun.apparent.elevationDeg;

    s.moon.direction = toVec3(moonDir);
    s.moon.illuminanceLux = static_cast<float>(magnitudeToIlluminanceLux(
        moonMagnitude(s.moonPhase.phaseAngleDeg, moon.topocentricDistanceKm)));
    s.moon.color = s.sun.color;  // Reflected sunlight (the slight reddening is ignored).
    s.moon.angularRadiusRad = static_cast<float>(degToRad(moon.semidiameterDeg));
    s.moon.azimuthDeg = moon.apparent.azimuthDeg;
    s.moon.elevationDeg = moon.apparent.elevationDeg;

    const glm::dvec3 moonToSun =
        glm::normalize(sunDir * sun.topocentricDistanceKm - moonDir * moon.topocentricDistanceKm);
    s.moonToSun = toVec3(moonToSun);

    const double lat = degToRad(observer.latitudeDeg);
    s.celestialNorth = toVec3(glm::dvec3(0.0, std::sin(lat), -std::cos(lat)));

    // Full-moon illuminance at the current distance spread over the disc, ×2 because the
    // Lommel–Seeliger law averages to ½ over a fully lit disc seen face-on.
    const double radius = degToRad(moon.semidiameterDeg);
    const double solidAngle = common::kPi * radius * radius;
    const double fullMoonLux =
        magnitudeToIlluminanceLux(moonMagnitude(0.0, moon.topocentricDistanceKm));
    s.moonDiscLuminance = static_cast<float>(2.0 * fullMoonLux / solidAngle);

    const double last = astro::localApparentSiderealTimeDeg(time, observer.longitudeDeg);
    const glm::dmat3 toWorld = equatorialToWorld(last, observer.latitudeDeg);
    s.equatorialToWorld = glm::mat3(toWorld);
    const double obliquity =
        degToRad(astro::nutation(astro::julianEphemerisDay(time)).trueObliquityDeg());
    s.moonNorth = toVec3(
        glm::normalize(toWorld * glm::dvec3(0.0, -std::sin(obliquity), std::cos(obliquity))));
    return s;
}

void SkyModel::setCatalog(std::vector<astro::CatalogStar> catalog) {
    catalog_ = std::move(catalog);
    starsValid_ = false;
}

bool SkyModel::update(astro::UtcTime time, const geo::Geodetic& observer) {
    using namespace std::chrono;
    bool changed = false;
    const bool moved = !valid_ ||
                       std::abs(observer.latitudeDeg - state_.observer.latitudeDeg) > 1e-3 ||
                       std::abs(observer.longitudeDeg - state_.observer.longitudeDeg) > 1e-3 ||
                       std::abs(observer.heightM - state_.observer.heightM) > 100.0;
    if (moved || abs(time - state_.time) >= milliseconds(100)) {
        state_ = computeSkyState(time, observer);
        valid_ = true;
        changed = true;
    }
    if (!catalog_.empty() && (!starsValid_ || abs(time - starsTime_) > hours(24))) {
        const astro::SkyFrame frame(time, observer);
        stars_.clear();
        stars_.reserve(catalog_.size());
        for (const astro::CatalogStar& star : catalog_) {
            const astro::Equatorial eq = frame.apparent(star);
            const double ra = degToRad(eq.rightAscensionDeg);
            const double dec = degToRad(eq.declinationDeg);
            StarInstance instance;
            instance.direction = glm::vec3(static_cast<float>(std::cos(dec) * std::cos(ra)),
                                           static_cast<float>(std::cos(dec) * std::sin(ra)),
                                           static_cast<float>(std::sin(dec)));
            instance.illuminanceLux =
                static_cast<float>(magnitudeToIlluminanceLux(star.visualMagnitude));
            instance.color = blackbodyColor(colorIndexToTemperature(star.colorIndex.value_or(0.6)));
            instance.magnitude = static_cast<float>(star.visualMagnitude);
            stars_.push_back(instance);
        }
        starsTime_ = time;
        starsValid_ = true;
        ++starsVersion_;
        changed = true;
    }
    return changed;
}

}  // namespace ds::graphics
