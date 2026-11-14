// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "ds/common/error.hpp"
#include "ds/geo/coordinates.hpp"

/// @file
/// @brief WGS84 ⇄ UTM (FR-GEO-02a), with Karney's 6th-order Krüger series (nanometre accuracy
/// inside a zone), the Norway/Svalbard zone exceptions and MGRS latitude bands.
/// @ingroup geo

namespace ds::geo {

/// @brief Southernmost latitude covered by UTM (polar regions use UPS, out of scope).
inline constexpr double kUtmMinLatitudeDeg = -80.0;
/// @brief Northernmost latitude covered by UTM.
inline constexpr double kUtmMaxLatitudeDeg = 84.0;

/// @brief A UTM position.
struct UtmCoordinate {
    int zone = 0;           ///< Zone number, 1–60.
    char band = 'N';        ///< MGRS latitude band letter C–X (I and O are skipped).
    bool north = true;      ///< Hemisphere (false ⇒ 10 000 km false northing was applied).
    double easting = 0.0;   ///< Metres, including the 500 km false easting.
    double northing = 0.0;  ///< Metres, including the false northing in the south.
};

/// @brief Standard zone for a position, including the Norway (32V) and Svalbard (31X–37X)
/// exceptions. @p latitudeDeg must be within the UTM range.
[[nodiscard]] int utmZone(double latitudeDeg, double longitudeDeg) noexcept;

/// @brief Latitude band letter (C–X) for a latitude within the UTM range.
[[nodiscard]] char utmBand(double latitudeDeg) noexcept;

/// @brief Converts a WGS84 position to UTM in its standard zone (FR-GEO-02a).
/// @return An error outside −80°…84° latitude or for non-finite input.
[[nodiscard]] common::Result<UtmCoordinate> toUtm(double latitudeDeg, double longitudeDeg);

/// @brief Converts a WGS84 position to UTM in a given @p zone (1–60), e.g. to keep using a
/// neighbouring zone. Accuracy degrades slowly beyond about 3.5° from the central meridian.
[[nodiscard]] common::Result<UtmCoordinate> toUtmInZone(double latitudeDeg, double longitudeDeg,
                                                        int zone);

/// @brief Converts UTM back to WGS84. Only @c zone, @c north, @c easting and @c northing are
/// used (the band is informative).
[[nodiscard]] common::Result<LatLon> fromUtm(const UtmCoordinate& utm);

/// @brief Display text, e.g. `30T 440 123.4 E  4 474 567.8 N` (FR-GFX-14a): zone and band,
/// then easting and northing with thousands separated by spaces and one decimal (decimetres).
[[nodiscard]] std::string formatUtm(const UtmCoordinate& utm);

}  // namespace ds::geo
