// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "ds/graphics/camera.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

/// @file
/// @brief Vehicle cameras: FPV (on board), chase and line of sight (pilot on the ground), plus
/// the free-fly camera (FR-GFX-05).
/// @ingroup graphics

namespace ds::graphics {

/// @brief Camera modes, in the order the `C` key cycles through them.
enum class CameraMode { Fpv, Chase, LineOfSight, Free };
/// @brief Number of camera modes.
inline constexpr int kCameraModeCount = 4;

/// @brief Display name ("FPV", "Chase", "Line of sight", "Free").
[[nodiscard]] std::string_view cameraModeName(CameraMode mode) noexcept;
/// @brief The next mode in the cycle.
[[nodiscard]] CameraMode nextCameraMode(CameraMode mode) noexcept;

/// @brief Vehicle pose seen by the cameras.
struct VehiclePose {
    glm::dvec3 position{0.0};                    ///< World position.
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};  ///< Body (x forward, y right, z down) → world.
    glm::dvec3 velocity{0.0};                    ///< World velocity.
};

/// @brief On-board camera.
struct FpvCameraParams {
    double uptiltDeg = 30.0;                  ///< Tilt above the body x axis.
    double horizontalFovDeg = 120.0;          ///< Horizontal field of view.
    glm::dvec3 offsetBody{0.06, 0.0, -0.02};  ///< Lens position in the body frame.
};

/// @brief Vertical field of view for a horizontal one and an aspect ratio (width / height).
[[nodiscard]] double verticalFov(double horizontalFovRad, double aspect) noexcept;

/// @brief A camera at @p position looking along @p forward with @p up as the top of the image
/// (yaw, pitch and roll are derived; robust when looking straight up or down).
[[nodiscard]] Camera cameraFromAxes(const glm::dvec3& position, const glm::dvec3& forward,
                                    const glm::dvec3& up, double fovYRad);

/// @brief Moves the vehicle cameras. The chase camera is smoothed, so it keeps state.
class CameraRig {
public:
    /// @brief Ground height under (x, z) (cameras stay above it).
    using GroundFunction = std::function<std::optional<double>(double x, double z)>;

    /// @brief Default chase distance behind the vehicle.
    static constexpr double kChaseDistanceM = 3.5;
    /// @brief Chase camera height above the vehicle, as a fraction of the distance.
    static constexpr double kChaseHeightRatio = 0.35;
    /// @brief Smoothing time constant of the chase camera.
    static constexpr double kChaseSmoothingS = 0.25;
    /// @brief Line-of-sight camera vertical field of view (a human eye, roughly).
    static constexpr double kLosFovDeg = 50.0;
    /// @brief Minimum height of a camera above the ground.
    static constexpr double kClearanceM = 0.3;
    /// @brief Top view: height above the vehicle, as a multiple of the chase distance.
    static constexpr double kTopHeightRatio = 2.0;
    /// @brief Top view vertical field of view.
    static constexpr double kTopFovDeg = 60.0;
    /// @brief Behind view: distance behind the vehicle, as a fraction of the chase distance.
    static constexpr double kBehindDistanceRatio = 0.4;
    /// @brief Behind view vertical field of view.
    static constexpr double kBehindFovDeg = 45.0;

    /// @brief Sets the ground function.
    void setGround(GroundFunction ground) { ground_ = std::move(ground); }
    /// @brief Pilot eye position for the line-of-sight view.
    void setPilotEye(const glm::dvec3& eye) noexcept { pilotEye_ = eye; }
    /// @brief Pilot eye position.
    [[nodiscard]] const glm::dvec3& pilotEye() const noexcept { return pilotEye_; }
    /// @brief Chase distance behind the vehicle (scale it with the vehicle size); the height
    /// above it follows (kChaseHeightRatio). Clamped to 0.5–50 m.
    void setChaseDistance(double distanceM) noexcept;
    /// @brief Chase distance.
    [[nodiscard]] double chaseDistance() const noexcept { return chaseDistanceM_; }
    /// @brief Chase height above the vehicle.
    [[nodiscard]] double chaseHeight() const noexcept {
        return kChaseHeightRatio * chaseDistanceM_;
    }
    /// @brief Snaps the chase camera behind @p pose (after a reset or a mode change).
    void reset(const VehiclePose& pose);

    /// @brief Camera for @p mode (not Free) at frame time @p dt, for a viewport of @p aspect.
    [[nodiscard]] Camera update(CameraMode mode, const VehiclePose& pose,
                                const FpvCameraParams& fpv, double aspect, double dt);

    /// @brief Top view for the chase insets: straight down from above the vehicle, north at
    /// the top of the image and east on the right (a map of the vehicle and the terrain
    /// around it). The height follows the chase distance (kTopHeightRatio).
    [[nodiscard]] Camera topView(const VehiclePose& pose, double aspect) const;
    /// @brief Behind view for the chase insets: rigidly behind the vehicle's nose (heading
    /// only, horizon level, no smoothing), slightly above it and looking at it; kept above
    /// the ground.
    [[nodiscard]] Camera behindView(const VehiclePose& pose, double aspect) const;

private:
    [[nodiscard]] glm::dvec3 chaseTarget(const VehiclePose& pose);
    [[nodiscard]] glm::dvec3 aboveGround(glm::dvec3 p) const;

    GroundFunction ground_;
    glm::dvec3 pilotEye_{0.0, 1.7, 0.0};
    glm::dvec3 chasePosition_{0.0};
    glm::dvec3 chaseDirection_{0.0, 0.0, -1.0};
    double chaseDistanceM_ = kChaseDistanceM;
    bool chaseValid_ = false;
};

}  // namespace ds::graphics
