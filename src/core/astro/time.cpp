// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/astro/time.hpp"

#include <charconv>
#include <cmath>
#include <format>

#include "ds/astro/nutation.hpp"
#include "ds/common/math.hpp"

namespace ds::astro {
namespace {

using namespace std::chrono;

constexpr double kUnixEpochJd = 2440587.5;
constexpr double kMsPerDay = 86400000.0;

// Reads exactly @p digits decimal digits from @p text at @p pos.
bool readInt(std::string_view text, std::size_t& pos, std::size_t digits, int& value) {
    if (pos + digits > text.size()) {
        return false;
    }
    const char* begin = text.data() + pos;
    const auto [ptr, ec] = std::from_chars(begin, begin + digits, value);
    if (ec != std::errc{} || ptr != begin + digits) {
        return false;
    }
    pos += digits;
    return true;
}

bool expect(std::string_view text, std::size_t& pos, char c) {
    if (pos < text.size() && text[pos] == c) {
        ++pos;
        return true;
    }
    return false;
}

common::Result<const time_zone*> findZone(std::string_view name) {
    try {
        return locate_zone(name);
    } catch (const std::runtime_error&) {
        return common::makeError(std::format("unknown time zone '{}'", name));
    }
}

}  // namespace

double julianDay(UtcTime time) noexcept {
    return kUnixEpochJd + static_cast<double>(time.time_since_epoch().count()) / kMsPerDay;
}

UtcTime fromJulianDay(double jd) noexcept {
    return UtcTime(milliseconds(std::llround((jd - kUnixEpochJd) * kMsPerDay)));
}

double decimalYear(UtcTime time) noexcept {
    const year_month_day date{floor<days>(time)};
    return static_cast<double>(static_cast<int>(date.year())) +
           (static_cast<double>(static_cast<unsigned>(date.month())) - 0.5) / 12.0;
}

double deltaTSeconds(double y) noexcept {
    // Espenak & Meeus, https://eclipse.gsfc.nasa.gov/SEhelp/deltatpoly2004.html
    const auto longTerm = [](double year) {
        const double u = (year - 1820.0) / 100.0;
        return -20.0 + 32.0 * u * u;
    };
    if (y < 1860.0) {
        return longTerm(y);
    }
    if (y < 1900.0) {
        const double t = y - 1860.0;
        return 7.62 + 0.5737 * t - 0.251754 * t * t + 0.01680668 * t * t * t -
               0.0004473624 * std::pow(t, 4) + std::pow(t, 5) / 233174.0;
    }
    if (y < 1920.0) {
        const double t = y - 1900.0;
        return -2.79 + 1.494119 * t - 0.0598939 * t * t + 0.0061966 * t * t * t -
               0.000197 * std::pow(t, 4);
    }
    if (y < 1941.0) {
        const double t = y - 1920.0;
        return 21.20 + 0.84493 * t - 0.076100 * t * t + 0.0020936 * t * t * t;
    }
    if (y < 1961.0) {
        const double t = y - 1950.0;
        return 29.07 + 0.407 * t - t * t / 233.0 + t * t * t / 2547.0;
    }
    if (y < 1986.0) {
        const double t = y - 1975.0;
        return 45.45 + 1.067 * t - t * t / 260.0 - t * t * t / 718.0;
    }
    if (y < 2005.0) {
        const double t = y - 2000.0;
        return 63.86 + 0.3345 * t - 0.060374 * t * t + 0.0017275 * t * t * t +
               0.000651814 * std::pow(t, 4) + 0.00002373599 * std::pow(t, 5);
    }
    if (y < 2050.0) {
        const double t = y - 2000.0;
        return 62.92 + 0.32217 * t + 0.005589 * t * t;
    }
    if (y < 2150.0) {
        return longTerm(y) - 0.5628 * (2150.0 - y);
    }
    return longTerm(y);
}

double julianEphemerisDay(UtcTime time) noexcept {
    return julianDay(time) + deltaTSeconds(decimalYear(time)) / 86400.0;
}

double julianCenturiesTt(UtcTime time) noexcept {
    return (julianEphemerisDay(time) - kJ2000) / 36525.0;
}

double greenwichMeanSiderealTimeDeg(UtcTime time) noexcept {
    const double jd = julianDay(time);
    const double t = (jd - kJ2000) / 36525.0;
    return common::normalizeDegrees(280.46061837 + 360.98564736629 * (jd - kJ2000) +
                                    0.000387933 * t * t - t * t * t / 38710000.0);
}

double greenwichApparentSiderealTimeDeg(UtcTime time) noexcept {
    const Nutation n = nutation(julianEphemerisDay(time));
    return common::normalizeDegrees(greenwichMeanSiderealTimeDeg(time) +
                                    n.longitudeDeg *
                                        std::cos(common::degToRad(n.trueObliquityDeg())));
}

double localApparentSiderealTimeDeg(UtcTime time, double longitudeDeg) noexcept {
    return common::normalizeDegrees(greenwichApparentSiderealTimeDeg(time) + longitudeDeg);
}

bool isSupported(UtcTime time) noexcept {
    const year_month_day date{floor<days>(time)};
    const int y = static_cast<int>(date.year());
    return y >= kMinSupportedYear && y <= kMaxSupportedYear;
}

common::Result<UtcTime> parseUtc(std::string_view text) {
    const auto invalid = [&] {
        return common::makeError(std::format(
            "invalid date-time '{}': expected ISO 8601 such as 2026-08-12T21:40:00Z", text));
    };
    std::size_t pos = 0;
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int s = 0;
    if (!readInt(text, pos, 4, y) || !expect(text, pos, '-') || !readInt(text, pos, 2, mo) ||
        !expect(text, pos, '-') || !readInt(text, pos, 2, d) ||
        !(expect(text, pos, 'T') || expect(text, pos, 't')) || !readInt(text, pos, 2, h) ||
        !expect(text, pos, ':') || !readInt(text, pos, 2, mi) || !expect(text, pos, ':') ||
        !readInt(text, pos, 2, s)) {
        return invalid();
    }
    milliseconds fraction{0};
    if (expect(text, pos, '.')) {
        int scale = 100;
        std::size_t digits = 0;
        int ms = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ms += (text[pos] - '0') * scale;
            scale /= 10;
            ++pos;
            ++digits;
        }
        if (digits == 0) {
            return invalid();
        }
        fraction = milliseconds(ms);
    }
    minutes offset{0};
    if (expect(text, pos, 'Z') || expect(text, pos, 'z')) {
        // UTC.
    } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        const int sign = text[pos] == '-' ? -1 : 1;
        ++pos;
        int oh = 0;
        int om = 0;
        if (!readInt(text, pos, 2, oh) || !expect(text, pos, ':') || !readInt(text, pos, 2, om) ||
            oh > 23 || om > 59) {
            return invalid();
        }
        offset = minutes(sign * (oh * 60 + om));
    } else {
        return invalid();  // A time without zone information is ambiguous.
    }
    if (pos != text.size()) {
        return invalid();
    }
    const year_month_day date{year(y), month(static_cast<unsigned>(mo)),
                              day(static_cast<unsigned>(d))};
    if (!date.ok() || h > 23 || mi > 59 || s > 59) {
        return invalid();
    }
    return UtcTime(sys_days(date)) + hours(h) + minutes(mi) + seconds(s) + fraction - offset;
}

std::string formatUtc(UtcTime time) {
    const auto ms = time.time_since_epoch() % 1000;
    if (ms.count() == 0) {
        return std::format("{:%FT%T}Z", floor<seconds>(time));
    }
    return std::format("{:%FT%T}Z", time);
}

common::Result<LocalTime> toLocalTime(UtcTime time, std::string_view timeZone) {
    const auto zone = findZone(timeZone);
    if (!zone) {
        return std::unexpected(zone.error());
    }
    const sys_info info = (*zone)->get_info(time);
    return LocalTime{(*zone)->to_local(time), info.offset, info.abbrev, info.save != minutes{0}};
}

common::Result<UtcTime> fromLocalTime(local_time<milliseconds> local, std::string_view timeZone) {
    const auto zone = findZone(timeZone);
    if (!zone) {
        return std::unexpected(zone.error());
    }
    const local_info info = (*zone)->get_info(local);
    switch (info.result) {
        case local_info::unique:
        case local_info::ambiguous: return UtcTime(local.time_since_epoch() - info.first.offset);
        case local_info::nonexistent:
            // In the gap: shift by the size of the gap (wall clock jumps forward).
            return UtcTime(local.time_since_epoch() - info.first.offset);
        default: return common::makeError("cannot convert local time");
    }
}

local_time<milliseconds> meanSolarTime(UtcTime time, double longitudeDeg) noexcept {
    const auto offset = milliseconds(std::llround(longitudeDeg * 240000.0));  // 4 min per degree
    return local_time<milliseconds>(time.time_since_epoch() + offset);
}

std::string formatLocal(local_time<milliseconds> time) {
    return std::format("{:%F %T}", floor<seconds>(time));
}

}  // namespace ds::astro
