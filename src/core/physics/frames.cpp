// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/frames.hpp"

#include <algorithm>
#include <cmath>

namespace ds::physics {
namespace {

// World-from-NED rotation as a quaternion: maps N → −z, E → +x, D → −y.
Quat worldFromNed() noexcept {
    // Columns of the rotation matrix are the images of the NED axes.
    const glm::dmat3 m(Vec3(0.0, 0.0, -1.0), Vec3(1.0, 0.0, 0.0), Vec3(0.0, -1.0, 0.0));
    return glm::quat_cast(m);
}

}  // namespace

Quat orientationFromEuler(const Euler& e) noexcept {
    // R_nb = Rz(ψ)·Ry(θ)·Rx(φ) about the NED axes (x = N, y = E, z = D).
    const Quat qz = glm::angleAxis(e.yaw, Vec3(0.0, 0.0, 1.0));
    const Quat qy = glm::angleAxis(e.pitch, Vec3(0.0, 1.0, 0.0));
    const Quat qx = glm::angleAxis(e.roll, Vec3(1.0, 0.0, 0.0));
    return glm::normalize(worldFromNed() * qz * qy * qx);
}

Euler eulerFromOrientation(const Quat& q) noexcept {
    const Vec3 forward = worldToNed(q * Vec3(1.0, 0.0, 0.0));
    const Vec3 right = worldToNed(q * Vec3(0.0, 1.0, 0.0));
    const Vec3 down = worldToNed(q * Vec3(0.0, 0.0, 1.0));
    Euler e;
    e.pitch = std::asin(std::clamp(-forward.z, -1.0, 1.0));
    e.yaw = std::atan2(forward.y, forward.x);
    e.roll = std::atan2(right.z, down.z);
    return e;
}

double tiltAngle(const Quat& q) noexcept {
    const Vec3 down = q * Vec3(0.0, 0.0, 1.0);  // Body z (down) in the world frame.
    return std::acos(std::clamp(-down.y, -1.0, 1.0));
}

}  // namespace ds::physics
