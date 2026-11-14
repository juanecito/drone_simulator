// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/geo/magnetic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "ds/common/math.hpp"

#include "wmm_data.hpp"

// Implemented from the formulas of the WMM2025 Technical Report (Chulliat et al., NOAA NCEI,
// 2025, doi:10.25923/prbc-s316), section 1.2.

namespace ds::geo {
namespace {

using detail::kWmmDegree;

constexpr double kReferenceRadiusM = 6371200.0;  // Geomagnetic reference radius a.
constexpr double kValidYears = 5.0;
constexpr double kMaxLatitudeDeg = 89.9999;
constexpr double kCautionNt = 6000.0;
constexpr double kUnreliableNt = 2000.0;

constexpr std::size_t kSize = kWmmDegree + 1;
using Table = std::array<std::array<double, kSize>, kSize>;

/// Schmidt semi-normalisation factors S(n, m).
consteval Table schmidtFactors() {
    Table s{};
    s[0][0] = 1.0;
    for (std::size_t n = 1; n < kSize; ++n) {
        const auto dn = static_cast<double>(n);
        s[n][0] = s[n - 1][0] * (2.0 * dn - 1.0) / dn;
        for (std::size_t m = 1; m <= n; ++m) {
            const auto dm = static_cast<double>(m);
            const double k = (m == 1) ? 2.0 : 1.0;
            // consteval: no std::sqrt; Newton iteration is exact to rounding here.
            const double x = (dn - dm + 1.0) * k / (dn + dm);
            double r = 1.0;
            for (int i = 0; i < 60; ++i) {
                r = 0.5 * (r + x / r);
            }
            s[n][m] = s[n][m - 1] * r;
        }
    }
    return s;
}

constexpr Table kSchmidt = schmidtFactors();

/// Schmidt semi-normalised associated Legendre functions P(n, m)(cos θ) and dP/dθ, θ being
/// the geocentric colatitude.
void legendre(double cosTheta, double sinTheta, Table& p, Table& dp) {
    p = {};
    dp = {};
    p[0][0] = 1.0;
    for (std::size_t n = 1; n < kSize; ++n) {
        const auto dn = static_cast<double>(n);
        for (std::size_t m = 0; m <= n; ++m) {
            const auto dm = static_cast<double>(m);
            if (m == n) {
                p[n][m] = sinTheta * p[n - 1][m - 1];
                dp[n][m] = sinTheta * dp[n - 1][m - 1] + cosTheta * p[n - 1][m - 1];
            } else if (n == 1) {
                p[n][m] = cosTheta * p[0][0];
                dp[n][m] = cosTheta * dp[0][0] - sinTheta * p[0][0];
            } else {
                const double k =
                    ((dn - 1.0) * (dn - 1.0) - dm * dm) / ((2.0 * dn - 1.0) * (2.0 * dn - 3.0));
                p[n][m] = cosTheta * p[n - 1][m] - k * p[n - 2][m];
                dp[n][m] = cosTheta * dp[n - 1][m] - sinTheta * p[n - 1][m] - k * dp[n - 2][m];
            }
        }
    }
    for (std::size_t n = 0; n < kSize; ++n) {
        for (std::size_t m = 0; m <= n; ++m) {
            p[n][m] *= kSchmidt[n][m];
            dp[n][m] *= kSchmidt[n][m];
        }
    }
}

}  // namespace

std::string_view magneticModelName() noexcept { return detail::kWmmName; }
double magneticModelStartYear() noexcept { return detail::kWmmEpoch; }
double magneticModelEndYear() noexcept { return detail::kWmmEpoch + kValidYears; }

MagneticField magneticField(const Geodetic& position, double decimalYear) noexcept {
    MagneticField field;
    field.dateValid =
        decimalYear >= magneticModelStartYear() && decimalYear < magneticModelEndYear();
    const double years = std::clamp(decimalYear, magneticModelStartYear(), magneticModelEndYear()) -
                         detail::kWmmEpoch;

    // Geodetic → geocentric spherical (r, φ', λ).
    Geodetic p = position;
    p.latitudeDeg = std::clamp(p.latitudeDeg, -kMaxLatitudeDeg, kMaxLatitudeDeg);
    const Ecef e = toEcef(p);
    const double rho = std::hypot(e.x, e.y);
    const double r = std::hypot(rho, e.z);
    const double latitudeC = std::atan2(e.z, rho);
    const double longitude = common::degToRad(p.longitudeDeg);
    const double cosLatC = std::cos(latitudeC);

    Table pnm;
    Table dpnm;
    legendre(std::sin(latitudeC), cosLatC, pnm, dpnm);

    std::array<double, kSize> cosM{};
    std::array<double, kSize> sinM{};
    for (std::size_t m = 0; m < kSize; ++m) {
        cosM[m] = std::cos(static_cast<double>(m) * longitude);
        sinM[m] = std::sin(static_cast<double>(m) * longitude);
    }
    const double ratio = kReferenceRadiusM / r;
    std::array<double, kSize> ratioPow{};  // (a/r)^(n+2)
    ratioPow[0] = ratio * ratio;
    for (std::size_t n = 1; n < kSize; ++n) {
        ratioPow[n] = ratioPow[n - 1] * ratio;
    }

    // Geocentric components (eqs. 10–12).
    double xc = 0.0;
    double yc = 0.0;
    double zc = 0.0;
    for (const detail::WmmTerm& t : detail::kWmmTerms) {
        const auto n = static_cast<std::size_t>(t.n);
        const auto m = static_cast<std::size_t>(t.m);
        const double g = t.g + years * t.gDot;
        const double h = t.h + years * t.hDot;
        const double a = ratioPow[n];
        const double gh = g * cosM[m] + h * sinM[m];
        xc += a * gh * dpnm[n][m];  // −dP/dφ' = dP/dθ.
        yc += a * t.m * (g * sinM[m] - h * cosM[m]) * pnm[n][m];
        zc -= (t.n + 1.0) * a * gh * pnm[n][m];
    }
    yc /= cosLatC;

    // Rotate to the ellipsoidal frame (eq. 17).
    const double psi = latitudeC - common::degToRad(p.latitudeDeg);
    field.northNt = xc * std::cos(psi) - zc * std::sin(psi);
    field.eastNt = yc;
    field.downNt = xc * std::sin(psi) + zc * std::cos(psi);
    field.horizontalNt = std::hypot(field.northNt, field.eastNt);
    field.totalNt = std::hypot(field.horizontalNt, field.downNt);
    field.declinationDeg = common::radToDeg(std::atan2(field.eastNt, field.northNt));
    field.inclinationDeg = common::radToDeg(std::atan2(field.downNt, field.horizontalNt));
    return field;
}

CompassQuality compassQuality(const MagneticField& field) noexcept {
    if (field.horizontalNt < kUnreliableNt) {
        return CompassQuality::Unreliable;
    }
    return field.horizontalNt < kCautionNt ? CompassQuality::Caution : CompassQuality::Good;
}

double decimalYear(std::chrono::sys_time<std::chrono::milliseconds> time) noexcept {
    using namespace std::chrono;
    const year y = year_month_day{floor<days>(time)}.year();
    const sys_days start{y / January / 1};
    const sys_days next{(y + years{1}) / January / 1};
    const duration<double> elapsed = time - start;
    const duration<double> length = next - start;
    return static_cast<int>(y) + elapsed / length;
}

}  // namespace ds::geo
