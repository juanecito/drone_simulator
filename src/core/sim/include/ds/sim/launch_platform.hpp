// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <span>

#include "ds/physics/vehicle.hpp"

/// @file
/// @brief The launch platform: a level, solid pad under an aerial drone's start position on
/// sloped ground, used for take-off and as its landing spot.
/// @ingroup sim

namespace ds::sim {

/// @brief Ground steeper than this under the drone's footprint gets a launch platform.
/// @details Disarmed drones resting on a slope tip over from about 15° (1.5 kg survey drone)
/// to 25° (5" racer), and sit tilted on gentler slopes.
inline constexpr double kLaunchPlatformSlopeDeg = 5.0;
/// @brief Smallest half side of the platform's top (a 2 × 2 m pad).
inline constexpr double kLaunchPlatformMinHalfSizeM = 1.0;
/// @brief Room around the drone's footprint on each side, metres.
inline constexpr double kLaunchPlatformMarginM = 0.6;
/// @brief GPS errors (1σ) the pad covers on each side, so that a return to home, which lands
/// where the GPS puts home, still lands on it (touchdowns measured up to 2.8σ away).
inline constexpr double kLaunchPlatformGpsSigmas = 2.5;
/// @brief How far the platform reaches below the lowest ground under it (a plinth set into
/// the slope), metres.
inline constexpr double kLaunchPlatformFootingM = 0.5;

/// @brief A level square pad, aligned with the world axes.
struct LaunchPlatform {
    physics::Vec3 centre{0.0};                       ///< Centre of the top (x, world height, z).
    double halfSizeM = kLaunchPlatformMinHalfSizeM;  ///< Half side of the square top.
    double bottomM = 0.0;                            ///< World height of the bottom.

    /// @brief True if world (@p x, @p z) is over the top.
    [[nodiscard]] bool covers(double x, double z) const noexcept;
    /// @brief Equality.
    bool operator==(const LaunchPlatform&) const = default;
};

/// @brief Horizontal reach of a vehicle's collision points from its centre, metres.
[[nodiscard]] double footprintRadiusM(std::span<const physics::CollisionPoint> points) noexcept;

/// @brief Half side of the pad for a drone with footprint radius @p footprintRadiusM and GPS
/// error @p gpsNoiseM (1σ): the footprint, the margin and kLaunchPlatformGpsSigmas, at least
/// kLaunchPlatformMinHalfSizeM.
[[nodiscard]] double launchPlatformHalfSizeM(double footprintRadiusM, double gpsNoiseM) noexcept;

/// @brief The platform with half side @p halfSizeM (launchPlatformHalfSizeM()) at (@p x,
/// @p z) on @p ground, or nullopt where the ground under it is flat enough
/// (kLaunchPlatformSlopeDeg).
/// @details The top is level with the highest ground under the pad (sampled on a grid), so
/// nothing pokes through it; the plinth reaches kLaunchPlatformFootingM below the lowest.
[[nodiscard]] std::optional<LaunchPlatform> launchPlatformFor(const physics::GroundQuery& ground,
                                                              double x, double z, double halfSizeM);

/// @brief Contact of the world point @p p with the platform's solid body, or nullopt.
/// @details With @p topIsGround (the vehicle stands over the top, which is then its ground
/// plane and handled by the ground contact) points just under the top are left alone, and
/// only the sides push out; otherwise the top pushes up too, like a roof.
[[nodiscard]] std::optional<physics::ObstacleContact> launchPlatformContact(
    const LaunchPlatform& platform, const physics::Vec3& p, bool topIsGround);

}  // namespace ds::sim
