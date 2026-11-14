// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ds/astro/sky_coordinates.hpp"
#include "ds/astro/time.hpp"
#include "ds/common/error.hpp"
#include "ds/geo/coordinates.hpp"

/// @file
/// @brief Bright-star catalogue (Yale BSC5) and apparent star positions (FR-AST-08).
/// @ingroup astro

namespace ds::astro {

/// @brief One catalogue star at J2000 (FK5).
struct CatalogStar {
    int hr = 0;                        ///< Harvard Revised number.
    std::string name;                  ///< Bayer/Flamsteed name, may be empty.
    double rightAscensionDeg = 0.0;    ///< α at J2000.
    double declinationDeg = 0.0;       ///< δ at J2000.
    double pmRaArcsecPerYear = 0.0;    ///< Proper motion μα·cos δ.
    double pmDecArcsecPerYear = 0.0;   ///< Proper motion μδ.
    double visualMagnitude = 0.0;      ///< V.
    std::optional<double> colorIndex;  ///< B − V, if known.
};

/// @brief Loads the catalogue CSV (assets/sky/bsc5.csv), keeping stars up to @p maxMagnitude.
/// @return The stars, or an error for a missing or malformed file.
[[nodiscard]] common::Result<std::vector<CatalogStar>> loadStarCatalog(
    const std::filesystem::path& file, double maxMagnitude = 6.5);

/// @brief Everything that depends only on time and observer, computed once per update and
/// shared by all stars (FR-AST-10).
class SkyFrame {
public:
    /// @brief Frame for @p time seen from @p observer.
    SkyFrame(UtcTime time, const geo::Geodetic& observer);

    /// @brief Apparent geocentric position of date: proper motion, precession (IAU 1976, Meeus
    /// eq. 21.4), nutation and annual aberration (Meeus eqs. 23.1, 23.3). Stellar parallax is
    /// ignored (< 1″ for every catalogue star).
    [[nodiscard]] Equatorial apparent(const CatalogStar& star) const;

    /// @brief Azimuth/elevation of @p star including refraction.
    [[nodiscard]] Horizontal horizontal(const CatalogStar& star,
                                        const Atmosphere& atmosphere = {}) const;

    /// @brief Local apparent sidereal time, degrees.
    [[nodiscard]] double localSiderealDeg() const noexcept { return localSiderealDeg_; }

private:
    double yearsSinceJ2000_;
    double zetaRad_;
    double zRad_;
    double thetaRad_;
    double nutationLongitudeRad_;
    double nutationObliquityRad_;
    double obliquityRad_;
    double sunLongitudeRad_;
    double eccentricity_;
    double perihelionRad_;
    double localSiderealDeg_;
    double latitudeDeg_;
};

}  // namespace ds::astro
