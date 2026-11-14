// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Transverse Mercator after C. F. F. Karney, "Transverse Mercator with an accuracy of a few
// nanometers", J. Geodesy 85 (2011) 475–485, eqs. (7)–(11), (35)–(36): Krüger series in the
// third flattening n, to order n⁶. Re-implemented from the published equations.

#include "ds/geo/utm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>

#include "ds/common/math.hpp"
#include "ds/geo/ellipsoid.hpp"

namespace ds::geo {
namespace {

using common::degToRad;
using common::radToDeg;

constexpr double kScale = 0.9996;              // k0
constexpr double kFalseEasting = 500000.0;     // m
constexpr double kFalseNorthingSouth = 1.0e7;  // m
constexpr int kOrder = 6;
constexpr std::string_view kBands = "CDEFGHJKLMNPQRSTUVWX";

struct Series {
    double rectifyingRadius;           // A
    std::array<double, kOrder> alpha;  // forward coefficients α1…α6
    std::array<double, kOrder> beta;   // inverse coefficients β1…β6
};

constexpr Series makeSeries(const Ellipsoid& ellipsoid) {
    const double n = ellipsoid.n();
    const double n2 = n * n;
    const double n3 = n2 * n;
    const double n4 = n3 * n;
    const double n5 = n4 * n;
    const double n6 = n5 * n;
    Series s{};
    s.rectifyingRadius = ellipsoid.a / (1.0 + n) * (1.0 + n2 / 4.0 + n4 / 64.0 + n6 / 256.0);
    s.alpha = {
        n / 2.0 - 2.0 / 3.0 * n2 + 5.0 / 16.0 * n3 + 41.0 / 180.0 * n4 - 127.0 / 288.0 * n5 +
            7891.0 / 37800.0 * n6,
        13.0 / 48.0 * n2 - 3.0 / 5.0 * n3 + 557.0 / 1440.0 * n4 + 281.0 / 630.0 * n5 -
            1983433.0 / 1935360.0 * n6,
        61.0 / 240.0 * n3 - 103.0 / 140.0 * n4 + 15061.0 / 26880.0 * n5 + 167603.0 / 181440.0 * n6,
        49561.0 / 161280.0 * n4 - 179.0 / 168.0 * n5 + 6601661.0 / 7257600.0 * n6,
        34729.0 / 80640.0 * n5 - 3418889.0 / 1995840.0 * n6,
        212378941.0 / 319334400.0 * n6,
    };
    s.beta = {
        n / 2.0 - 2.0 / 3.0 * n2 + 37.0 / 96.0 * n3 - 1.0 / 360.0 * n4 - 81.0 / 512.0 * n5 +
            96199.0 / 604800.0 * n6,
        1.0 / 48.0 * n2 + 1.0 / 15.0 * n3 - 437.0 / 1440.0 * n4 + 46.0 / 105.0 * n5 -
            1118711.0 / 3870720.0 * n6,
        17.0 / 480.0 * n3 - 37.0 / 840.0 * n4 - 209.0 / 4480.0 * n5 + 5569.0 / 90720.0 * n6,
        4397.0 / 161280.0 * n4 - 11.0 / 504.0 * n5 - 830251.0 / 7257600.0 * n6,
        4583.0 / 161280.0 * n5 - 108847.0 / 3991680.0 * n6,
        20648693.0 / 638668800.0 * n6,
    };
    return s;
}

constexpr Series kSeries = makeSeries(kWgs84);
const double kE = std::sqrt(kWgs84.e2());

// τ' (tangent of the conformal latitude) from τ = tan φ — Karney eq. (7)–(9).
double conformalTau(double tau) {
    const double e = kE;
    const double sigma = std::sinh(e * std::atanh(e * tau / std::hypot(1.0, tau)));
    return tau * std::hypot(1.0, sigma) - sigma * std::hypot(1.0, tau);
}

// Inverse of conformalTau by Newton's method — Karney eqs. (19)–(21).
double geographicTau(double tauPrime) {
    const double e2 = kWgs84.e2();
    double tau = tauPrime;
    for (int i = 0; i < 10; ++i) {
        const double tp = conformalTau(tau);
        const double dTau = (tauPrime - tp) / std::hypot(1.0, tp) * (1.0 + (1.0 - e2) * tau * tau) /
                            ((1.0 - e2) * std::hypot(1.0, tau));
        tau += dTau;
        if (std::abs(dTau) < 1e-15 * std::max(1.0, std::abs(tau))) {
            break;
        }
    }
    return tau;
}

double centralMeridianDeg(int zone) { return -183.0 + 6.0 * zone; }

bool validLatitude(double latitudeDeg) {
    return std::isfinite(latitudeDeg) && latitudeDeg >= kUtmMinLatitudeDeg &&
           latitudeDeg <= kUtmMaxLatitudeDeg;
}

// Inserts a space every three digits of the integer part: 4474567.8 → "4 474 567.8".
std::string groupThousands(double value) {
    std::string text = std::format("{:.1f}", value);
    const auto dot = text.find('.');
    std::size_t end = dot == std::string::npos ? text.size() : dot;
    const std::size_t begin = (!text.empty() && text[0] == '-') ? 1 : 0;
    while (end > begin + 3) {
        end -= 3;
        text.insert(end, 1, ' ');
    }
    return text;
}

}  // namespace

int utmZone(double latitudeDeg, double longitudeDeg) noexcept {
    const double lon = common::wrapDegrees180(longitudeDeg);
    int zone = static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1;
    zone = std::clamp(zone, 1, 60);
    // Norway: zone 32V is widened to 3°–12° E.
    if (latitudeDeg >= 56.0 && latitudeDeg < 64.0 && lon >= 3.0 && lon < 12.0) {
        return 32;
    }
    // Svalbard: zones 31X, 33X, 35X, 37X are widened; 32X, 34X, 36X are not used.
    if (latitudeDeg >= 72.0 && lon >= 0.0 && lon < 42.0) {
        if (lon < 9.0) {
            return 31;
        }
        if (lon < 21.0) {
            return 33;
        }
        if (lon < 33.0) {
            return 35;
        }
        return 37;
    }
    return zone;
}

char utmBand(double latitudeDeg) noexcept {
    const int index = static_cast<int>(std::floor((latitudeDeg + 80.0) / 8.0));
    // Band X covers 72°–84° (12°), so 84° itself still maps to X.
    return kBands[static_cast<std::size_t>(std::clamp(index, 0, 19))];
}

common::Result<UtmCoordinate> toUtm(double latitudeDeg, double longitudeDeg) {
    if (!validLatitude(latitudeDeg) || !std::isfinite(longitudeDeg)) {
        return common::makeError(
            std::format("latitude {:.6f}° is outside the UTM range ({}° to {}°)", latitudeDeg,
                        kUtmMinLatitudeDeg, kUtmMaxLatitudeDeg));
    }
    return toUtmInZone(latitudeDeg, longitudeDeg, utmZone(latitudeDeg, longitudeDeg));
}

common::Result<UtmCoordinate> toUtmInZone(double latitudeDeg, double longitudeDeg, int zone) {
    if (!validLatitude(latitudeDeg) || !std::isfinite(longitudeDeg)) {
        return common::makeError(
            std::format("latitude {:.6f}° is outside the UTM range ({}° to {}°)", latitudeDeg,
                        kUtmMinLatitudeDeg, kUtmMaxLatitudeDeg));
    }
    if (zone < 1 || zone > 60) {
        return common::makeError(std::format("invalid UTM zone {}", zone));
    }
    const double lambda = degToRad(common::wrapDegrees180(longitudeDeg - centralMeridianDeg(zone)));
    if (std::abs(lambda) > degToRad(60.0)) {
        return common::makeError(
            std::format("longitude {:.6f}° is too far from UTM zone {}", longitudeDeg, zone));
    }
    const double phi = degToRad(latitudeDeg);
    const double tauPrime = conformalTau(std::tan(phi));
    const double cosLambda = std::cos(lambda);
    const double xiPrime = std::atan2(tauPrime, cosLambda);
    const double etaPrime = std::asinh(std::sin(lambda) / std::hypot(tauPrime, cosLambda));

    double xi = xiPrime;
    double eta = etaPrime;
    for (int j = 1; j <= kOrder; ++j) {
        const double alpha = kSeries.alpha[static_cast<std::size_t>(j - 1)];
        xi += alpha * std::sin(2.0 * j * xiPrime) * std::cosh(2.0 * j * etaPrime);
        eta += alpha * std::cos(2.0 * j * xiPrime) * std::sinh(2.0 * j * etaPrime);
    }

    UtmCoordinate utm;
    utm.zone = zone;
    utm.band = utmBand(latitudeDeg);
    utm.north = latitudeDeg >= 0.0;
    utm.easting = kFalseEasting + kScale * kSeries.rectifyingRadius * eta;
    utm.northing = kScale * kSeries.rectifyingRadius * xi + (utm.north ? 0.0 : kFalseNorthingSouth);
    return utm;
}

common::Result<LatLon> fromUtm(const UtmCoordinate& utm) {
    if (utm.zone < 1 || utm.zone > 60) {
        return common::makeError(std::format("invalid UTM zone {}", utm.zone));
    }
    if (!std::isfinite(utm.easting) || !std::isfinite(utm.northing)) {
        return common::makeError("UTM coordinates must be finite numbers");
    }
    const double scale = kScale * kSeries.rectifyingRadius;
    const double xi = (utm.northing - (utm.north ? 0.0 : kFalseNorthingSouth)) / scale;
    const double eta = (utm.easting - kFalseEasting) / scale;

    double xiPrime = xi;
    double etaPrime = eta;
    for (int j = 1; j <= kOrder; ++j) {
        const double beta = kSeries.beta[static_cast<std::size_t>(j - 1)];
        xiPrime -= beta * std::sin(2.0 * j * xi) * std::cosh(2.0 * j * eta);
        etaPrime -= beta * std::cos(2.0 * j * xi) * std::sinh(2.0 * j * eta);
    }
    const double sinhEta = std::sinh(etaPrime);
    const double cosXi = std::cos(xiPrime);
    const double tauPrime = std::sin(xiPrime) / std::hypot(sinhEta, cosXi);
    const double lambda = std::atan2(sinhEta, cosXi);
    const double tau = geographicTau(tauPrime);
    return LatLon{radToDeg(std::atan(tau)),
                  common::wrapDegrees180(centralMeridianDeg(utm.zone) + radToDeg(lambda))};
}

std::string formatUtm(const UtmCoordinate& utm) {
    return std::format("{}{} {} E  {} N", utm.zone, utm.band, groupThousands(utm.easting),
                       groupThousands(utm.northing));
}

}  // namespace ds::geo
