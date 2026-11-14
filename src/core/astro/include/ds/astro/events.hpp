// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>

#include "ds/astro/time.hpp"
#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Rise, set, transit and twilight times, and lunar phases (FR-AST-07).
/// @details Events are searched numerically in a 24-hour window, so polar day and night simply
/// produce no rise/set (with the corresponding flag set). Definitions follow the USNO: sunrise
/// and sunset when the Sun's centre is 50′ below the horizon (34′ refraction + 16′ radius);
/// moonrise and moonset for the Moon's upper limb with 34′ refraction (topocentric); twilights
/// at −6°, −12° and −18° for the Sun's centre.
/// @ingroup astro

namespace ds::astro {

/// @brief Sun events within one 24-hour window.
struct SunEvents {
    std::optional<UtcTime> astronomicalDawn;  ///< Sun rises through −18°.
    std::optional<UtcTime> nauticalDawn;      ///< Sun rises through −12°.
    std::optional<UtcTime> civilDawn;         ///< Sun rises through −6°.
    std::optional<UtcTime> sunrise;           ///< Upper limb appears.
    std::optional<UtcTime> solarNoon;         ///< Upper transit.
    std::optional<UtcTime> sunset;            ///< Upper limb disappears.
    std::optional<UtcTime> civilDusk;         ///< Sun sets through −6°.
    std::optional<UtcTime> nauticalDusk;      ///< Sun sets through −12°.
    std::optional<UtcTime> astronomicalDusk;  ///< Sun sets through −18°.
    bool alwaysUp = false;    ///< Polar day: the Sun stays above the rise/set altitude.
    bool alwaysDown = false;  ///< Polar night: the Sun stays below the rise/set altitude.
};

/// @brief Moon events within one 24-hour window.
struct MoonEvents {
    std::optional<UtcTime> moonrise;  ///< Upper limb appears.
    std::optional<UtcTime> transit;   ///< Upper transit.
    std::optional<UtcTime> moonset;   ///< Upper limb disappears.
    bool alwaysUp = false;            ///< Above the horizon for the whole window.
    bool alwaysDown = false;          ///< Below the horizon for the whole window.
};

/// @brief Sun events in [@p windowStart, @p windowStart + 24 h). Pass the local midnight of
/// the day of interest, converted to UTC.
[[nodiscard]] SunEvents sunEvents(UtcTime windowStart, const geo::Geodetic& observer);

/// @brief Moon events in [@p windowStart, @p windowStart + 24 h). A day can have no moonrise or
/// no moonset even outside the polar regions.
[[nodiscard]] MoonEvents moonEvents(UtcTime windowStart, const geo::Geodetic& observer);

/// @brief Principal lunar phases.
enum class LunarPhase {
    NewMoon,       ///< Elongation 0°.
    FirstQuarter,  ///< Elongation 90°.
    FullMoon,      ///< Elongation 180°.
    LastQuarter,   ///< Elongation 270°.
};

/// @brief First instant after @p after at which the Moon reaches @p phase (geocentric
/// apparent ecliptic longitudes, as in almanacs), to within a second.
[[nodiscard]] UtcTime nextLunarPhase(UtcTime after, LunarPhase phase);

}  // namespace ds::astro
