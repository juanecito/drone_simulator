// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "ds/graphics/mesh.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

/// @file
/// @brief Procedural ground drone (RC-style skid-steer rover) model: body, wheels that turn
/// and follow the suspension, headlights and tail lights (FR-GFX-20, FR-GFX-08d). Generic
/// shapes only: not modelled on any commercial product.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Most wheels of a rover model.
inline constexpr std::size_t kRoverMaxWheels = 6;

/// @brief Dimensions of a rover, in its body frame (x forward, y right, z down, origin at the
/// centre of mass), metres.
struct RoverShape {
    /// Wheel hub positions with the suspension at its static compression (left side first,
    /// front to rear, then the right side).
    std::vector<glm::dvec3> wheelHubs;
    double wheelRadiusM = 0.065;            ///< Tyre radius.
    double wheelWidthM = 0.05;              ///< Tyre width.
    glm::dvec3 bodySize{0.40, 0.24, 0.13};  ///< Body length, width, height.
    double bodyBottomM = 0.06;              ///< Body underside below the CoG (z).
    double staticCompressionM = 0.0;        ///< Suspension compression at rest.
    double cameraUptiltDeg = 5.0;           ///< FPV camera tilt (visual only).
};

/// @brief What changes from frame to frame.
struct RoverVisualState {
    glm::dvec3 position{0.0};                           ///< Body origin, world.
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};         ///< Body (FRD) → world.
    std::array<double, kRoverMaxWheels> wheelAngle{};   ///< Wheel rotation, rad (+ = forward).
    std::array<double, kRoverMaxWheels> compression{};  ///< Suspension compression, m.
    bool lights = true;                                 ///< Headlights and tail lights on.
    bool braking = false;                               ///< Brake lights.
};

/// @brief GPU-independent rover model: builds the meshes once, emits draw items per frame.
class RoverModel {
public:
    /// @brief Builds the meshes for @p shape.
    explicit RoverModel(RoverShape shape);

    /// @brief Appends the items for @p state to @p out.
    void appendDrawItems(const RoverVisualState& state, std::vector<DrawItem>& out) const;

    /// @brief The shape.
    [[nodiscard]] const RoverShape& shape() const noexcept { return shape_; }
    /// @brief Hub of wheel @p index for a suspension compression of @p compressionM (body
    /// frame): more compression lifts the wheel towards the body.
    [[nodiscard]] glm::dvec3 hubAt(std::size_t index, double compressionM) const;

private:
    RoverShape shape_;
    std::shared_ptr<const Mesh> body_;
    std::shared_ptr<const Mesh> wheel_;
    std::shared_ptr<const Mesh> headlight_;
    std::shared_ptr<const Mesh> tailLight_;
};

}  // namespace ds::graphics
