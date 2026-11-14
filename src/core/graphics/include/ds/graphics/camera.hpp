// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <functional>
#include <optional>

#include <glm/glm.hpp>

/// @file
/// @brief Camera with reversed-Z infinite projection, frustum culling and the free-fly
/// controller (FR-GFX-05, FR-GFX-06).
/// @ingroup graphics

namespace ds::graphics {

/// @brief A perspective camera in the world frame (x = east, y = up, z = south).
/// @details The position is kept in `double`; matrices are camera-relative (the view matrix
/// has no translation) so that geometry far from the origin keeps full float precision
/// (FR-GFX-06). Depth is reversed (near plane → 1, infinity → 0), which needs
/// `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` and `glDepthFunc(GL_GREATER)`.
struct Camera {
    glm::dvec3 position{0.0, 0.0, 0.0};  ///< World position, metres.
    double yawRad = 0.0;                 ///< Heading, clockwise from north (0 = north, π/2 = east).
    double pitchRad = 0.0;               ///< Elevation of the view direction, positive = up.
    double rollRad = 0.0;  ///< Bank about the view direction, positive = right side down.
    double fovYRad = 1.0471975511965976;  ///< Vertical field of view (default 60°).
    double nearM = 0.1;                   ///< Near plane distance.
    double aspect = 16.0 / 9.0;           ///< Width / height.

    /// @brief Unit view direction in the world frame.
    [[nodiscard]] glm::dvec3 forward() const noexcept;
    /// @brief Unit vector to the right of the view direction (horizontal).
    [[nodiscard]] glm::dvec3 right() const noexcept;
    /// @brief Unit up vector of the image.
    [[nodiscard]] glm::dvec3 up() const noexcept;
    /// @brief Rotation-only view matrix (camera-relative world → view space).
    [[nodiscard]] glm::mat4 view() const noexcept;
    /// @brief Reversed-Z infinite perspective projection (zero-to-one depth range).
    [[nodiscard]] glm::mat4 projection() const noexcept;
    /// @brief projection() · view().
    [[nodiscard]] glm::mat4 viewProjection() const noexcept { return projection() * view(); }
};

/// @brief An axis-aligned box relative to the camera, metres.
struct RelativeBox {
    glm::vec3 min{0.0F};  ///< Minimum corner.
    glm::vec3 max{0.0F};  ///< Maximum corner.
};

/// @brief View frustum (left, right, bottom, top, near planes; the far plane is at infinity).
class Frustum {
public:
    /// @brief Extracts the planes of @p viewProjection (camera-relative).
    explicit Frustum(const glm::mat4& viewProjection) noexcept;
    /// @brief False only if @p box is certainly outside (conservative test).
    [[nodiscard]] bool intersects(const RelativeBox& box) const noexcept;

private:
    std::array<glm::vec4, 5> planes_{};
};

/// @brief Inputs of the free-fly camera for one frame.
struct FreeFlyInput {
    double forward = 0.0;        ///< −1 … 1 (W/S).
    double right = 0.0;          ///< −1 … 1 (D/A).
    double up = 0.0;             ///< −1 … 1 (E/Q).
    double yawDeltaRad = 0.0;    ///< Mouse look, accumulated this frame.
    double pitchDeltaRad = 0.0;  ///< Mouse look, accumulated this frame.
    bool boost = false;          ///< Shift: 10× speed.
};

/// @brief Debug free-fly camera (FR-GFX-05): moves at a speed that grows with the height
/// above the ground and never goes below the terrain.
class FreeFlyController {
public:
    /// @brief Ground height at world (x, z), or nothing if unknown.
    using GroundFunction = std::function<std::optional<double>(double x, double z)>;

    /// @brief Base speed, m/s (scaled by the height above ground and by boost).
    void setBaseSpeed(double metresPerSecond) noexcept { baseSpeed_ = metresPerSecond; }
    /// @brief Current base speed.
    [[nodiscard]] double baseSpeed() const noexcept { return baseSpeed_; }
    /// @brief Ground lookup used to keep the camera above the terrain.
    void setGround(GroundFunction ground) { ground_ = std::move(ground); }

    /// @brief Applies @p input over @p dtSeconds to @p camera.
    void update(Camera& camera, const FreeFlyInput& input, double dtSeconds) const;

    /// @brief Minimum clearance above the ground, metres.
    static constexpr double kClearanceM = 1.5;

private:
    double baseSpeed_ = 30.0;
    GroundFunction ground_;
};

}  // namespace ds::graphics
