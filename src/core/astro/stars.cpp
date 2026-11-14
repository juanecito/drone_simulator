// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/astro/stars.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <string_view>

#include "ds/astro/nutation.hpp"
#include "ds/astro/sun_moon.hpp"
#include "ds/common/math.hpp"

namespace ds::astro {
namespace {

using common::degToRad;
using common::normalizeDegrees;
using common::radToDeg;

constexpr double kArcsecToRad = common::kPi / (180.0 * 3600.0);

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const auto comma = line.find(',', start);
        fields.push_back(line.substr(start, comma - start));
        if (comma == std::string_view::npos) {
            return fields;
        }
        start = comma + 1;
    }
}

bool parse(std::string_view text, double& value) {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

}  // namespace

common::Result<std::vector<CatalogStar>> loadStarCatalog(const std::filesystem::path& file,
                                                         double maxMagnitude) {
    std::ifstream in(file);
    if (!in) {
        return common::makeError(std::format("cannot open star catalogue {}", file.string()));
    }
    std::vector<CatalogStar> stars;
    std::string line;
    bool header = true;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (header) {  // Column names.
            header = false;
            continue;
        }
        const auto fields = split(line);
        CatalogStar star;
        double hr = 0.0;
        if (fields.size() != 8 || !parse(fields[0], hr) ||
            !parse(fields[2], star.rightAscensionDeg) || !parse(fields[3], star.declinationDeg) ||
            !parse(fields[4], star.pmRaArcsecPerYear) ||
            !parse(fields[5], star.pmDecArcsecPerYear) || !parse(fields[6], star.visualMagnitude)) {
            return common::makeError(
                std::format("{}:{}: malformed star catalogue line", file.string(), lineNumber));
        }
        if (!fields[7].empty()) {
            double bv = 0.0;
            if (!parse(fields[7], bv)) {
                return common::makeError(
                    std::format("{}:{}: malformed B−V colour", file.string(), lineNumber));
            }
            star.colorIndex = bv;
        }
        if (star.visualMagnitude > maxMagnitude) {
            continue;
        }
        star.hr = static_cast<int>(hr);
        star.name = std::string(fields[1]);
        stars.push_back(std::move(star));
    }
    if (stars.empty()) {
        return common::makeError(std::format("star catalogue {} has no stars", file.string()));
    }
    return stars;
}

SkyFrame::SkyFrame(UtcTime time, const geo::Geodetic& observer)
    : latitudeDeg_(observer.latitudeDeg) {
    const double jde = julianEphemerisDay(time);
    const double t = (jde - kJ2000) / 36525.0;
    yearsSinceJ2000_ = t * 100.0;
    // Precession angles from J2000 (Meeus eq. 21.3 with T = 0).
    zetaRad_ = (2306.2181 * t + 0.30188 * t * t + 0.017998 * t * t * t) * kArcsecToRad;
    zRad_ = (2306.2181 * t + 1.09468 * t * t + 0.018203 * t * t * t) * kArcsecToRad;
    thetaRad_ = (2004.3109 * t - 0.42665 * t * t - 0.041833 * t * t * t) * kArcsecToRad;

    const Nutation n = nutation(jde);
    nutationLongitudeRad_ = degToRad(n.longitudeDeg);
    nutationObliquityRad_ = degToRad(n.obliquityDeg);
    obliquityRad_ = degToRad(n.trueObliquityDeg());

    // True geometric longitude of the Sun for the aberration terms (remove the aberration and
    // nutation already contained in the apparent longitude).
    const BodyPosition sun = sunPosition(time, geo::Geodetic{});
    const double radiusAu = sun.distanceKm / kAstronomicalUnitKm;
    sunLongitudeRad_ = degToRad(sun.geocentricEcliptic.longitudeDeg - n.longitudeDeg +
                                20.4898 / 3600.0 / radiusAu);
    eccentricity_ = 0.016708634 - 0.000042037 * t - 0.0000001267 * t * t;
    perihelionRad_ = degToRad(102.93735 + 1.71946 * t + 0.00046 * t * t);
    localSiderealDeg_ = localApparentSiderealTimeDeg(time, observer.longitudeDeg);
}

Equatorial SkyFrame::apparent(const CatalogStar& star) const {
    // Proper motion to the date.
    const double dec0 =
        degToRad(star.declinationDeg) + star.pmDecArcsecPerYear * yearsSinceJ2000_ * kArcsecToRad;
    const double ra0 = degToRad(star.rightAscensionDeg) +
                       star.pmRaArcsecPerYear * yearsSinceJ2000_ * kArcsecToRad /
                           std::cos(degToRad(star.declinationDeg));

    // Precession (Meeus eq. 21.4).
    const double a = std::cos(dec0) * std::sin(ra0 + zetaRad_);
    const double b = std::cos(thetaRad_) * std::cos(dec0) * std::cos(ra0 + zetaRad_) -
                     std::sin(thetaRad_) * std::sin(dec0);
    const double c = std::sin(thetaRad_) * std::cos(dec0) * std::cos(ra0 + zetaRad_) +
                     std::cos(thetaRad_) * std::sin(dec0);
    const double ra = std::atan2(a, b) + zRad_;
    const double dec = std::atan2(c, std::hypot(a, b));

    const double sinRa = std::sin(ra);
    const double cosRa = std::cos(ra);
    const double tanDec = std::tan(dec);
    const double sinEps = std::sin(obliquityRad_);
    const double cosEps = std::cos(obliquityRad_);

    // Nutation (Meeus eq. 23.1).
    const double dRaNut = (cosEps + sinEps * sinRa * tanDec) * nutationLongitudeRad_ -
                          cosRa * tanDec * nutationObliquityRad_;
    const double dDecNut = sinEps * cosRa * nutationLongitudeRad_ + sinRa * nutationObliquityRad_;

    // Annual aberration (Meeus eq. 23.3).
    const double kappa = 20.49552 * kArcsecToRad;
    const double ek = eccentricity_ * kappa;
    const double sinSun = std::sin(sunLongitudeRad_);
    const double cosSun = std::cos(sunLongitudeRad_);
    const double sinPi = std::sin(perihelionRad_);
    const double cosPi = std::cos(perihelionRad_);
    const double cosDec = std::cos(dec);
    const double sinDec = std::sin(dec);
    const double dRaAb = (-kappa * (cosRa * cosSun * cosEps + sinRa * sinSun) +
                          ek * (cosRa * cosPi * cosEps + sinRa * sinPi)) /
                         cosDec;
    const double common1 = std::tan(obliquityRad_) * cosDec - sinRa * sinDec;
    const double dDecAb = -kappa * (cosSun * cosEps * common1 + cosRa * sinDec * sinSun) +
                          ek * (cosPi * cosEps * common1 + cosRa * sinDec * sinPi);

    return {normalizeDegrees(radToDeg(ra + dRaNut + dRaAb)), radToDeg(dec + dDecNut + dDecAb)};
}

Horizontal SkyFrame::horizontal(const CatalogStar& star, const Atmosphere& atmosphere) const {
    Horizontal h = equatorialToHorizontal(apparent(star), localSiderealDeg_, latitudeDeg_);
    h.elevationDeg += refractionDeg(h.elevationDeg, atmosphere);
    return h;
}

}  // namespace ds::astro
