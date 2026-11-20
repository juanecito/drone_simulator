// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "ds/weather/weather.hpp"

#include <glm/glm.hpp>

/// @file
/// @brief The weather as the renderer draws it (FR-WX-09, FR-GFX-09): cloud layers as
/// noise-modulated slabs, ground fog, precipitation particles around the camera and the light
/// under the clouds. Built on the CPU from `weather::WeatherConditions`; mirrored by
/// `assets/shaders/weather.glsl`.
/// @ingroup graphics

namespace ds::graphics {

/// @brief One cloud layer, ready for the shaders.
/// @details The layer fills the heights [@ref baseY, @ref topY] (world y). Its horizontal
/// density is `smoothstep(threshold, threshold + softness, fbm(q))`, where `q` is the world
/// position minus the drift times @ref frequency (cirrus is stretched along the wind), so
/// that on average it covers @ref cover of the sky (cloudCoverThreshold()).
struct CloudLayerView {
    weather::CloudType type = weather::CloudType::Cumulus;  ///< Cloud type.
    double baseY = 1000.0;                                  ///< World height of the base, m.
    double topY = 1500.0;                                   ///< World height of the top, m.
    double cover = 0.0;                 ///< Fraction of the sky covered (oktas / 8).
    double extinctionPerM = 0.06;       ///< Extinction inside the cloud, 1/m.
    double frequency = 1.0 / 1200.0;    ///< Noise frequency, 1/m (about 1 / feature size).
    double softness = 0.05;             ///< Width of the density edge, noise units.
    double threshold = 1.0;             ///< Noise value where the cloud starts.
    double diffuseTransmittance = 1.0;  ///< Share of the light on the top that leaves the base.
    double lightFactor = 1.0;           ///< Light left on the top after the layers above.
    glm::dvec2 driftM{0.0};             ///< How far the layer has moved (east, south = +x, +z).
};

/// @brief The weather to draw this frame (FrameInput::weather). The default is a clear sky.
struct WeatherView {
    std::array<CloudLayerView, weather::kMaxCloudLayers> layers{};  ///< Lowest first.
    std::size_t layerCount = 0;                                     ///< Layers in use.
    /// Visibility of the aerosol in the whole atmosphere (the sky and aerial perspective).
    double aerosolVisibilityM = 60'000.0;
    /// World height of the top of the low obscured layer: a ground fog, or haze, rain or snow
    /// below the cloud base (1500 m without clouds) when the visibility is under 20 km.
    double fogTopY = 0.0;
    /// Extinction in that layer on top of the aerosol's (0 = none), 1/m: together they give
    /// the weather's visibility.
    double fogExtinctionPerM = 0.0;
    weather::PrecipitationType precipitation = weather::PrecipitationType::None;  ///< Type.
    double rateMmH = 0.0;      ///< Precipitation rate, mm/h of water.
    double fallSpeedMs = 0.0;  ///< Fall speed of the drops or flakes.
    int particles = 0;         ///< Drops or flakes drawn around the camera.
    glm::dvec3 windMs{0.0};    ///< Wind (blowing towards), world frame.
    double wetness = 0.0;      ///< Ground wetness 0–1.
    double snowDepthM = 0.0;   ///< Snow lying on the ground.
    double timeS = 0.0;        ///< Weather clock (animates the precipitation).
    /// Share of the clear-sky diffuse light that reaches the ground (clouds dim it).
    double skyFactor = 1.0;
    /// Share of the horizontal direct light that reaches the ground as diffuse light, after
    /// scattering in the clouds.
    double beamDiffuse = 0.0;
    /// Share of the sky free of clouds, Π(1 − cover): the average share of the direct light.
    double clearFraction = 1.0;
};

/// @brief Side of the cube around the camera where precipitation particles are drawn, m.
inline constexpr double kPrecipitationBoxM = 24.0;

/// @brief Builds the view of @p conditions.
/// @param conditions The weather now.
/// @param drift      How far each layer has drifted (WeatherState::cloudDrift).
/// @param ground     Wet ground and snow cover.
/// @param groundY    World height the cloud and fog heights (AGL) are measured from.
/// @param windSpeedMs Mean wind speed near the ground (drifts the rain and snow).
/// @param timeS      Weather clock.
[[nodiscard]] WeatherView makeWeatherView(const weather::WeatherConditions& conditions,
                                          std::span<const weather::Drift> drift,
                                          const weather::GroundCover& ground, double groundY,
                                          double windSpeedMs, double timeS);

/// @brief The cloud-shape noise at @p q (fractal value noise, 5 octaves, about 0–1): the CPU
/// mirror of `cloudNoise()` in `weather.glsl`.
[[nodiscard]] double cloudNoise(glm::dvec2 q) noexcept;

/// @brief Threshold for which `smoothstep(t, t + softness, cloudNoise(q))` averages to
/// @p cover over the plane (from the distribution of cloudNoise()). 0 → above every noise
/// value, 1 → below every value.
[[nodiscard]] double cloudCoverThreshold(double cover, double softness);

/// @brief Diffuse transmittance of a cloud of vertical optical depth @p opticalDepth
/// (two-stream approximation with asymmetry 0.85).
[[nodiscard]] double cloudDiffuseTransmittance(double opticalDepth) noexcept;

/// @brief Number of drops or flakes drawn around the camera for @p type at @p rateMmH.
[[nodiscard]] int precipitationParticles(weather::PrecipitationType type, double rateMmH) noexcept;

}  // namespace ds::graphics
