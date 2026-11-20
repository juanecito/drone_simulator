// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "ds/common/random.hpp"
#include "ds/physics/frames.hpp"
#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief Wind field: mean wind with a logarithmic height profile, Dryden turbulence
/// (MIL-F-8785C, low altitude), discrete "1 − cosine" gusts and slow direction wander, all
/// set by a wind level 0–10 and seeded (FR-PHY-06, FR-WIND-01…06).
/// @ingroup physics

namespace ds::physics {

/// @brief Fully resolved wind parameters.
struct WindParameters {
    double meanSpeed10mMs = 0.0;     ///< Mean wind at 10 m above ground.
    double gustPeakMs = 0.0;         ///< Peak speed in gusts at 10 m.
    double gustIntervalS = 0.0;      ///< Mean time between gusts (0 = none).
    double turbulenceScale = 0.0;    ///< Dryden intensity multiplier (0 = none).
    double directionSwingDeg = 0.0;  ///< ± direction wander.
    double fromDeg = 270.0;          ///< Mean direction the wind blows from, degrees true.
    double roughnessM = 0.03;        ///< Surface roughness length z₀ (short grass).

    /// @brief Parameters of a wind level and direction; with WindSettings::randomDirection the
    /// direction is drawn from @p seed.
    [[nodiscard]] static WindParameters fromLevel(const weather::WindLevelTable& table,
                                                  const weather::WindSettings& settings,
                                                  std::uint64_t seed);
    /// @brief Parameters of a (possibly interpolated) level blowing from @p fromDeg.
    [[nodiscard]] static WindParameters fromLevel(const weather::WindLevel& level, double fromDeg);
};

/// @brief Mean wind speed at @p heightAglM for a speed @p speed10m at 10 m and roughness
/// @p roughnessM: `U(h) = U10 · ln(h/z₀) / ln(10/z₀)` (0 at or below z₀; constant above 300 m).
[[nodiscard]] double logProfileSpeed(double speed10m, double heightAglM,
                                     double roughnessM) noexcept;

/// @brief Time-varying wind at the vehicle.
/// @details Advanced once per simulation tick along the vehicle's path (the Dryden filters
/// need the speed of the air relative to the vehicle). Level 0 gives exactly zero.
class WindModel {
public:
    /// @brief Calm air.
    WindModel() = default;
    /// @brief Wind with @p params; all randomness comes from @p seed (FR-WIND-04).
    WindModel(const WindParameters& params, std::uint64_t seed);

    /// @brief Advances by @p dt at height @p heightAglM with the vehicle moving at
    /// @p vehicleVelocity (world frame).
    void update(double dt, double heightAglM, const Vec3& vehicleVelocity);

    /// @brief Changes the parameters (the weather changing) without restarting the turbulence,
    /// gust and wander states.
    void setParameters(const WindParameters& params);
    /// @brief Wind velocity (world frame, the direction the air moves to), m/s.
    [[nodiscard]] const Vec3& velocity() const noexcept { return velocity_; }
    /// @brief Mean component only (profile + wander, no gusts or turbulence).
    [[nodiscard]] const Vec3& meanVelocity() const noexcept { return mean_; }
    /// @brief Current direction the mean wind blows from, degrees true.
    [[nodiscard]] double currentFromDeg() const noexcept;
    /// @brief Parameters.
    [[nodiscard]] const WindParameters& params() const noexcept { return params_; }
    /// @brief True while a gust is blowing.
    [[nodiscard]] bool inGust() const noexcept { return gustElapsed_ < gustDuration_; }

private:
    void scheduleGust();
    void updateCompensation();

    WindParameters params_;
    common::GaussianRandom random_{0};
    double meanCompensation_ = 1.0;  // Keeps the long-term mean at the table value.
    // Dryden filter states (u: first order; v, w: two cascaded first-order stages).
    double u_ = 0.0;
    double v1_ = 0.0;
    double v2_ = 0.0;
    double w1_ = 0.0;
    double w2_ = 0.0;
    // Direction wander (Gauss–Markov, radians).
    double wander_ = 0.0;
    // Gusts.
    double timeToGust_ = 0.0;
    double gustElapsed_ = 0.0;
    double gustDuration_ = 0.0;
    double gustAmplitude_ = 0.0;
    double gustAngle_ = 0.0;
    Vec3 mean_{0.0};
    Vec3 velocity_{0.0};
};

}  // namespace ds::physics
