// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <functional>

#include "ds/physics/frames.hpp"

#include <glm/mat3x3.hpp>

/// @file
/// @brief 6-DoF rigid body integrated with classic Runge–Kutta 4 (FR-PHY-01).
/// @ingroup physics

namespace ds::physics {

/// @brief Kinematic state of a rigid body.
struct RigidBodyState {
    Vec3 position{0.0};                    ///< Centre of mass, world frame, m (double precision).
    Quat orientation{1.0, 0.0, 0.0, 0.0};  ///< Body (FRD) → world rotation.
    Vec3 velocity{0.0};                    ///< Velocity of the centre of mass, world frame, m/s.
    Vec3 angularVelocity{0.0};             ///< Body rates (p, q, r), body frame, rad/s.
};

/// @brief Mass properties about the centre of mass.
struct MassProperties {
    double massKg = 1.0;           ///< Total mass.
    glm::dmat3 inertia{1.0};       ///< Inertia tensor in the body frame, kg·m².
    Vec3 centreOfMassOffset{0.0};  ///< CoG offset from the reference point (body), m.
};

/// @brief Forces and torques acting on the body, excluding gravity.
struct Wrench {
    Vec3 forceWorld{0.0};  ///< Sum of forces, world frame, N.
    Vec3 torqueBody{0.0};  ///< Sum of torques about the centre of mass, body frame, N·m.
};

/// @brief Time derivative of a RigidBodyState.
struct RigidBodyDerivative {
    Vec3 velocity{0.0};                        ///< d(position)/dt.
    Quat orientationRate{0.0, 0.0, 0.0, 0.0};  ///< d(orientation)/dt.
    Vec3 acceleration{0.0};                    ///< d(velocity)/dt (including gravity).
    Vec3 angularAcceleration{0.0};             ///< d(angular velocity)/dt, body frame.
};

/// @brief Computes the wrench for a state (called four times per RK4 step).
using WrenchFunction = std::function<Wrench(const RigidBodyState&)>;

/// @brief Rigid-body equations of motion: Newton for translation, Euler for rotation
/// (`I·ω̇ = τ − ω × I·ω`), quaternion kinematics `q̇ = ½·q ⊗ (0, ω)`.
[[nodiscard]] RigidBodyDerivative derivative(const RigidBodyState& state,
                                             const MassProperties& mass, const Wrench& wrench,
                                             double gravity = kGravity) noexcept;

/// @brief Advances @p state by @p dt with RK4; the orientation is renormalised.
/// @return The wrench evaluated at the start of the step (for sensors and telemetry).
Wrench integrateRk4(RigidBodyState& state, const MassProperties& mass, const WrenchFunction& wrench,
                    double dt, double gravity = kGravity);

/// @brief True if every component of @p state is finite and the quaternion is not degenerate
/// (FR-PHY-17).
[[nodiscard]] bool isFinite(const RigidBodyState& state) noexcept;

}  // namespace ds::physics
