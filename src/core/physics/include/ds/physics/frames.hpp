// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

/// @file
/// @brief Reference frames and attitude conventions of the physics module (FR-X-04).
/// @details
/// - **World frame** (shared with terrain and graphics): right-handed, Y up —
///   `x = East`, `y = Up`, `z = South` (= −North). Positions in `double`.
/// - **Body frame**: aerospace FRD — `x = forward`, `y = right`, `z = down`.
/// - **Orientation** is the unit quaternion `q` that rotates body vectors into the world frame.
/// - **Euler angles** (for display and the Angle mode): yaw ψ clockwise from north, pitch θ
///   positive nose up, roll φ positive right side down — the usual NED sequence
///   `R_nb = Rz(ψ)·Ry(θ)·Rx(φ)`.
/// - **Body rates** p, q, r (rad/s) are the FRD components of the angular velocity: positive
///   p rolls right, positive q pitches the nose up, positive r yaws the nose right.
/// @ingroup physics

namespace ds::physics {

/// @brief 3-vector in double precision.
using Vec3 = glm::dvec3;
/// @brief Quaternion in double precision.
using Quat = glm::dquat;

/// @brief Standard gravity, m/s².
inline constexpr double kGravity = 9.80665;

/// @brief Euler angles (radians).
struct Euler {
    double roll = 0.0;   ///< φ, positive right side down.
    double pitch = 0.0;  ///< θ, positive nose up.
    double yaw = 0.0;    ///< ψ, clockwise from north, (−π, π].
};

/// @brief World vector → NED (north, east, down).
[[nodiscard]] constexpr Vec3 worldToNed(const Vec3& w) noexcept { return {-w.z, w.x, -w.y}; }
/// @brief NED vector → world.
[[nodiscard]] constexpr Vec3 nedToWorld(const Vec3& n) noexcept { return {n.y, -n.z, -n.x}; }

/// @brief Body → world rotation for the Euler angles @p e.
[[nodiscard]] Quat orientationFromEuler(const Euler& e) noexcept;
/// @brief Euler angles of the body → world rotation @p q.
[[nodiscard]] Euler eulerFromOrientation(const Quat& q) noexcept;
/// @brief Tilt of the body z axis away from the vertical (0 = level, π = upside down).
[[nodiscard]] double tiltAngle(const Quat& q) noexcept;

/// @brief Rotates the body vector @p v into the world frame.
[[nodiscard]] inline Vec3 bodyToWorld(const Quat& q, const Vec3& v) noexcept { return q * v; }
/// @brief Rotates the world vector @p v into the body frame.
[[nodiscard]] inline Vec3 worldToBody(const Quat& q, const Vec3& v) noexcept {
    return glm::conjugate(q) * v;
}

}  // namespace ds::physics
