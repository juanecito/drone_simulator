// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <vector>

#include "ds/astro/stars.hpp"
#include "ds/astro/sun_moon.hpp"
#include "ds/astro/time.hpp"
#include "ds/geo/coordinates.hpp"

#include <glm/glm.hpp>

/// @file
/// @brief Sun, moon and stars as light sources and sky objects for rendering (FR-GFX-08,
/// FR-GFX-08a/b/c, FR-AST-09).
/// @ingroup graphics

namespace ds::graphics {

/// @brief A celestial light source as seen by the observer.
struct SkyLight {
    glm::vec3 direction{0.0F, 1.0F, 0.0F};  ///< Unit vector towards the body (world frame).
    float illuminanceLux = 0.0F;    ///< Illuminance at normal incidence above the atmosphere.
    glm::vec3 color{1.0F};          ///< Linear sRGB colour with luminance 1.
    float angularRadiusRad = 0.0F;  ///< Apparent angular radius.
    double azimuthDeg = 0.0;        ///< Apparent (refracted) azimuth from north.
    double elevationDeg = 0.0;      ///< Apparent (refracted) elevation.
};

/// @brief Everything the renderer needs from astronomy for one instant and place.
struct SkyState {
    astro::UtcTime time{};       ///< Instant.
    geo::Geodetic observer;      ///< Observer.
    SkyLight sun;                ///< The Sun.
    SkyLight moon;               ///< The Moon.
    astro::MoonPhase moonPhase;  ///< Phase data (UI read-out).
    /// Unit vector from the Moon towards the Sun (world frame): lights the moon disc, which
    /// gives the correct phase and bright-limb orientation (FR-GFX-08a).
    glm::vec3 moonToSun{0.0F, 1.0F, 0.0F};
    glm::vec3 celestialNorth{0.0F, 0.0F, -1.0F};  ///< Towards the north celestial pole.
    /// Towards the north ecliptic pole, which the Moon's spin axis follows to within 1.5°:
    /// orients the moon texture (the terminator comes from @c moonToSun).
    glm::vec3 moonNorth{0.0F, 1.0F, 0.0F};
    /// Luminance (cd/m²) of a fully lit moon-disc point of average albedo seen face-on; the
    /// shader multiplies it by the Lommel–Seeliger law and the albedo texture.
    float moonDiscLuminance = 0.0F;
    /// Rotation from apparent equatorial coordinates of date (x → α = 0, z → north pole) to
    /// the world frame, via local apparent sidereal time and latitude.
    glm::mat3 equatorialToWorld{1.0F};
};

/// @brief One star for the GPU (std430 layout: two vec4).
struct StarInstance {
    glm::vec3 direction{0.0F};    ///< Apparent equatorial unit vector of date.
    float illuminanceLux = 0.0F;  ///< Illuminance above the atmosphere.
    glm::vec3 color{1.0F};        ///< Linear sRGB, luminance 1.
    float magnitude = 0.0F;       ///< Visual magnitude (for filtering).
};
static_assert(sizeof(StarInstance) == 32);

/// @brief Computes SkyState from time and observer; keeps the star list up to date.
/// @details update() recomputes the Sun and Moon when the time moved by ≥ 100 ms or the
/// observer by ≥ 100 m (FR-AST-10), and the stars' apparent places when the date moved by
/// more than one day.
class SkyModel {
public:
    /// @brief Uses @p catalog for the stars.
    void setCatalog(std::vector<astro::CatalogStar> catalog);
    /// @brief Updates for @p time and @p observer. Returns true if anything was recomputed.
    bool update(astro::UtcTime time, const geo::Geodetic& observer);
    /// @brief Current state (valid after the first update()).
    [[nodiscard]] const SkyState& state() const noexcept { return state_; }
    /// @brief Stars in apparent equatorial coordinates of date.
    [[nodiscard]] const std::vector<StarInstance>& stars() const noexcept { return stars_; }
    /// @brief Increments whenever stars() changes.
    [[nodiscard]] std::uint64_t starsVersion() const noexcept { return starsVersion_; }

private:
    std::vector<astro::CatalogStar> catalog_;
    std::vector<StarInstance> stars_;
    std::uint64_t starsVersion_ = 0;
    astro::UtcTime starsTime_{};
    bool starsValid_ = false;
    bool valid_ = false;
    SkyState state_;
};

/// @brief Computes the sky state without caching (tests, one-off use).
[[nodiscard]] SkyState computeSkyState(astro::UtcTime time, const geo::Geodetic& observer);

/// @brief Illuminance (lux) of a point source of visual magnitude @p magnitude
/// (m = 0 ⇒ 2.54·10⁻⁶ lux).
[[nodiscard]] double magnitudeToIlluminanceLux(double magnitude) noexcept;

/// @brief Apparent magnitude of the Moon at phase angle @p phaseAngleDeg and distance
/// @p distanceKm (Allen's phase law; opposition surge not modelled).
[[nodiscard]] double moonMagnitude(double phaseAngleDeg, double distanceKm) noexcept;

/// @brief Effective temperature (K) from colour index B − V (Ballesteros 2012).
[[nodiscard]] double colorIndexToTemperature(double bMinusV) noexcept;

/// @brief Linear sRGB colour of a black body at @p kelvin, scaled to luminance 1 (CIE 1931
/// matching functions after Wyman, Sloan & Shirley 2013).
[[nodiscard]] glm::vec3 blackbodyColor(double kelvin) noexcept;

/// @brief World unit vector for azimuth (from north, towards east) and elevation, degrees.
[[nodiscard]] glm::dvec3 horizontalToWorld(double azimuthDeg, double elevationDeg) noexcept;

/// @brief Rotation from apparent equatorial of date to world for local apparent sidereal
/// time @p lastDeg and latitude @p latitudeDeg.
[[nodiscard]] glm::dmat3 equatorialToWorld(double lastDeg, double latitudeDeg) noexcept;

}  // namespace ds::graphics
