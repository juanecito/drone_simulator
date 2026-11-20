// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "ds/graphics/mesh.hpp"
#include "ds/graphics/mesh_loader.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

/// @file
/// @brief Procedural quadcopter model with spinning propellers, motion-blur discs and
/// navigation lights (FR-GFX-12, FR-GFX-08d). Generic shapes only: not modelled on any
/// commercial product.
/// @ingroup graphics

namespace ds::graphics {

/// @brief Dimensions of a quadcopter, in its body frame (x forward, y right, z down), metres.
struct DroneShape {
    /// Motor positions: front-right, rear-right, rear-left, front-left.
    std::array<glm::dvec3, 4> motors{};
    std::array<double, 4> spin{-1.0, 1.0, -1.0, 1.0};  ///< +1 = clockwise seen from above.
    double propDiameterM = 0.127;                      ///< Propeller diameter.
    int blades = 3;                                    ///< Blades per propeller.
    glm::dvec3 bodySize{0.10, 0.05, 0.035};            ///< Central body length, width, height.
    double armWidthM = 0.012;                          ///< Width of the arms.
    double motorRadiusM = 0.012;                       ///< Motor bell radius.
    double gearHeightM = 0.03;      ///< Height of the landing feet below the arms.
    double cameraUptiltDeg = 30.0;  ///< FPV camera tilt (visual only).
};

/// @brief Shape derived from the motor positions and the propeller: body, arm and motor sizes
/// scale with the frame (used when a profile gives no explicit dimensions).
[[nodiscard]] DroneShape droneShapeFor(const std::array<glm::dvec3, 4>& motors,
                                       double propDiameterM, int blades, double restHeightM,
                                       double cameraUptiltDeg);

/// @brief What changes from frame to frame.
struct DroneVisualState {
    glm::dvec3 position{0.0};                    ///< Body origin, world.
    glm::dquat orientation{1.0, 0.0, 0.0, 0.0};  ///< Body (FRD) → world.
    std::array<double, 4> propAngleRad{};        ///< Propeller angles.
    std::array<double, 4> rotorSpeedFraction{};  ///< Speed / maximum speed (blur above ~15 %).
    bool lights = true;                          ///< Navigation lights on.
    double timeS = 0.0;                          ///< Animation time (strobe).
};

/// @brief GPU-independent drone model: builds the meshes once, emits draw items per frame.
class DroneModel {
public:
    /// @brief Rotor speed fraction above which a propeller is drawn as a blur disc.
    static constexpr double kBlurSpeedFraction = 0.15;

    /// @brief Builds the meshes for @p shape.
    explicit DroneModel(const DroneShape& shape);

    /// @brief Appends the items for @p state to @p out.
    void appendDrawItems(const DroneVisualState& state, std::vector<DrawItem>& out) const;

    /// @brief The shape.
    [[nodiscard]] const DroneShape& shape() const noexcept { return shape_; }

    /// @brief Bounds of the procedural frame (body, arms, motors, feet), body frame: where a
    /// model file is fitted.
    [[nodiscard]] MeshBounds frameBounds() const;
    /// @brief Draws @p mesh (body frame) instead of the procedural frame; the propellers and
    /// the navigation lights stay. Null restores the procedural frame.
    void setFrameMesh(std::shared_ptr<const Mesh> mesh);

private:
    DroneShape shape_;
    std::shared_ptr<const Mesh> frame_;
    std::shared_ptr<const Mesh> customFrame_;
    std::array<std::shared_ptr<const Mesh>, 2> blades_;  // Front (orange), rear (dark).
    std::shared_ptr<const Mesh> blur_;
    std::shared_ptr<const Mesh> ledRed_;
    std::shared_ptr<const Mesh> ledGreen_;
    std::shared_ptr<const Mesh> ledWhite_;
};

}  // namespace ds::graphics
