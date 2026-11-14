// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/camera.hpp"

#include <algorithm>
#include <cmath>

namespace ds::graphics {
namespace {

constexpr double kMaxPitch = 1.5533430342749532;  // 89°

}  // namespace

glm::dvec3 Camera::forward() const noexcept {
    const double c = std::cos(pitchRad);
    // Heading 0 = north = −z; π/2 = east = +x.
    return {c * std::sin(yawRad), std::sin(pitchRad), -c * std::cos(yawRad)};
}

namespace {

// Axes before the roll: right is horizontal, up completes the right-handed basis.
glm::dvec3 levelRight(double yawRad) noexcept { return {std::cos(yawRad), 0.0, std::sin(yawRad)}; }

}  // namespace

glm::dvec3 Camera::right() const noexcept {
    const glm::dvec3 r0 = levelRight(yawRad);
    if (rollRad == 0.0) {
        return r0;
    }
    const glm::dvec3 u0 = glm::cross(r0, forward());
    return std::cos(rollRad) * r0 - std::sin(rollRad) * u0;
}

glm::dvec3 Camera::up() const noexcept {
    const glm::dvec3 r0 = levelRight(yawRad);
    const glm::dvec3 u0 = glm::cross(r0, forward());
    if (rollRad == 0.0) {
        return u0;
    }
    return std::sin(rollRad) * r0 + std::cos(rollRad) * u0;
}

glm::mat4 Camera::view() const noexcept {
    const glm::vec3 f(forward());
    const glm::vec3 r(right());
    const glm::vec3 u(up());
    // Rows of the rotation are the camera axes; the camera looks along −z in view space.
    glm::mat4 m(1.0F);
    m[0][0] = r.x;
    m[1][0] = r.y;
    m[2][0] = r.z;
    m[0][1] = u.x;
    m[1][1] = u.y;
    m[2][1] = u.z;
    m[0][2] = -f.x;
    m[1][2] = -f.y;
    m[2][2] = -f.z;
    return m;
}

glm::mat4 Camera::projection() const noexcept {
    const float f = static_cast<float>(1.0 / std::tan(0.5 * fovYRad));
    glm::mat4 p(0.0F);
    p[0][0] = f / static_cast<float>(aspect);
    p[1][1] = f;
    // clip.z = near, clip.w = −z_view ⇒ depth = near / −z_view: 1 at the near plane, → 0 at ∞.
    p[3][2] = static_cast<float>(nearM);
    p[2][3] = -1.0F;
    return p;
}

Frustum::Frustum(const glm::mat4& m) noexcept {
    const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);
    planes_ = {row3 + row0, row3 - row0, row3 + row1, row3 - row1, row3 - row2};  // near: z ≤ w
    for (auto& plane : planes_) {
        plane /= glm::length(glm::vec3(plane));
    }
}

bool Frustum::intersects(const RelativeBox& box) const noexcept {
    for (const auto& plane : planes_) {
        // The box corner farthest along the plane normal.
        const glm::vec3 p(plane.x >= 0.0F ? box.max.x : box.min.x,
                          plane.y >= 0.0F ? box.max.y : box.min.y,
                          plane.z >= 0.0F ? box.max.z : box.min.z);
        if (glm::dot(glm::vec3(plane), p) + plane.w < 0.0F) {
            return false;
        }
    }
    return true;
}

void FreeFlyController::update(Camera& camera, const FreeFlyInput& input, double dt) const {
    camera.yawRad = std::remainder(camera.yawRad + input.yawDeltaRad, 2.0 * 3.141592653589793);
    camera.pitchRad = std::clamp(camera.pitchRad + input.pitchDeltaRad, -kMaxPitch, kMaxPitch);

    std::optional<double> ground;
    if (ground_) {
        ground = ground_(camera.position.x, camera.position.z);
    }
    const double heightAbove = ground ? std::max(camera.position.y - *ground, 0.0) : 50.0;
    // Faster when high: crossing terrain seen from 2 km should not take minutes.
    double speed = baseSpeed_ * std::max(1.0, heightAbove / 40.0);
    if (input.boost) {
        speed *= 10.0;
    }
    glm::dvec3 move = camera.forward() * input.forward + camera.right() * input.right +
                      glm::dvec3(0.0, 1.0, 0.0) * input.up;
    if (glm::length(move) > 1.0) {
        move = glm::normalize(move);
    }
    camera.position += move * (speed * dt);

    if (ground_) {
        if (const auto g = ground_(camera.position.x, camera.position.z)) {
            camera.position.y = std::max(camera.position.y, *g + kClearanceM);
        }
    }
}

}  // namespace ds::graphics
