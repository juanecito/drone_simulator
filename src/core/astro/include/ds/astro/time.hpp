// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "ds/common/error.hpp"

/// @file
/// @brief Time base (FR-AST-01, FR-AST-02): UTC instants, Julian dates, ΔT, sidereal time,
/// ISO 8601 parsing and local civil / solar time.
/// @ingroup astro

namespace ds::astro {

/// @brief A UTC instant with millisecond resolution. UT1 is taken equal to UTC (|UT1 − UTC| <
/// 0.9 s, below the accuracy targets).
using UtcTime = std::chrono::sys_time<std::chrono::milliseconds>;

/// @brief Julian Day of J2000.0 (2000-01-01 12:00 TT).
inline constexpr double kJ2000 = 2451545.0;

/// @brief First year of the supported range (FR-AST-01).
inline constexpr int kMinSupportedYear = 1900;
/// @brief Last year of the supported range (FR-AST-01).
inline constexpr int kMaxSupportedYear = 2100;

/// @brief Julian Day (UT) of a UTC instant.
[[nodiscard]] double julianDay(UtcTime time) noexcept;

/// @brief UTC instant of a Julian Day (UT), rounded to the millisecond.
[[nodiscard]] UtcTime fromJulianDay(double julianDay) noexcept;

/// @brief Decimal year as used by the ΔT polynomials: year + (month − 0.5) / 12.
[[nodiscard]] double decimalYear(UtcTime time) noexcept;

/// @brief ΔT = TT − UT1 in seconds (Espenak & Meeus polynomials, NASA "Five Millennium Canon
/// of Solar Eclipses"; long-term parabola outside 1860–2150).
[[nodiscard]] double deltaTSeconds(double decimalYear) noexcept;

/// @brief Julian Ephemeris Day (TT) of a UTC instant: JD + ΔT.
[[nodiscard]] double julianEphemerisDay(UtcTime time) noexcept;

/// @brief Julian centuries of TT since J2000.0.
[[nodiscard]] double julianCenturiesTt(UtcTime time) noexcept;

/// @brief Greenwich mean sidereal time in degrees, 0…360 (Meeus eq. 12.4, IAU 1982).
[[nodiscard]] double greenwichMeanSiderealTimeDeg(UtcTime time) noexcept;

/// @brief Greenwich apparent sidereal time in degrees: GMST + equation of the equinoxes.
[[nodiscard]] double greenwichApparentSiderealTimeDeg(UtcTime time) noexcept;

/// @brief Local apparent sidereal time in degrees for an east-positive @p longitudeDeg.
[[nodiscard]] double localApparentSiderealTimeDeg(UtcTime time, double longitudeDeg) noexcept;

/// @brief True if @p time lies in the supported years 1900–2100.
[[nodiscard]] bool isSupported(UtcTime time) noexcept;

/// @brief Parses an ISO 8601 date-time with a UTC designator or offset, e.g.
/// `2026-08-12T21:40:00Z`, `2026-08-12T23:40:00.5+02:00`.
[[nodiscard]] common::Result<UtcTime> parseUtc(std::string_view text);

/// @brief Formats as ISO 8601 UTC, e.g. `2026-08-12T21:40:00Z` (milliseconds only if non-zero).
[[nodiscard]] std::string formatUtc(UtcTime time);

/// @brief Local civil time in an IANA time zone.
struct LocalTime {
    std::chrono::local_time<std::chrono::milliseconds> time;  ///< Wall-clock time.
    std::chrono::seconds utcOffset{0};                        ///< Offset from UTC.
    std::string abbreviation;                                 ///< E.g. "CEST".
    bool daylightSaving = false;                              ///< True during DST.
};

/// @brief Converts to local civil time with the system time-zone database (FR-AST-02).
/// @return An error for an unknown zone name.
[[nodiscard]] common::Result<LocalTime> toLocalTime(UtcTime time, std::string_view timeZone);

/// @brief Converts a local wall-clock time in @p timeZone to UTC. Ambiguous times (DST end)
/// take the earlier instant; non-existent times (DST start) are moved forward by the gap.
[[nodiscard]] common::Result<UtcTime> fromLocalTime(
    std::chrono::local_time<std::chrono::milliseconds> local, std::string_view timeZone);

/// @brief Local mean solar time at @p longitudeDeg: UTC + longitude × 4 min/°.
[[nodiscard]] std::chrono::local_time<std::chrono::milliseconds> meanSolarTime(
    UtcTime time, double longitudeDeg) noexcept;

/// @brief Formats a local time as `YYYY-MM-DD HH:MM:SS`.
[[nodiscard]] std::string formatLocal(std::chrono::local_time<std::chrono::milliseconds> time);

}  // namespace ds::astro
