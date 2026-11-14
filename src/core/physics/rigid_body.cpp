// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/rigid_body.hpp"

#include <cmath>

#include <glm/matrix.hpp>

namespace ds::physics {
namespace {

RigidBodyState advanced(const RigidBodyState& s, const RigidBodyDerivative& d, double h) {
    RigidBodyState out;
    out.position = s.position + d.velocity * h;
    out.orientation = glm::normalize(s.orientation + d.orientationRate * h);
    out.velocity = s.velocity + d.acceleration * h;
    out.angularVelocity = s.angularVelocity + d.angularAcceleration * h;
    return out;
}

bool finite(const Vec3& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

RigidBodyDerivative derivative(const RigidBodyState& s, const MassProperties& mass,
                               const Wrench& wrench, double gravity) noexcept {
    RigidBodyDerivative d;
    d.velocity = s.velocity;
    d.acceleration = wrench.forceWorld / mass.massKg + Vec3(0.0, -gravity, 0.0);
    const Quat omega(0.0, s.angularVelocity.x, s.angularVelocity.y, s.angularVelocity.z);
    d.orientationRate = 0.5 * (s.orientation * omega);
    const Vec3 iw = mass.inertia * s.angularVelocity;
    d.angularAcceleration =
        glm::inverse(mass.inertia) * (wrench.torqueBody - glm::cross(s.angularVelocity, iw));
    return d;
}

Wrench integrateRk4(RigidBodyState& state, const MassProperties& mass, const WrenchFunction& wrench,
                    double dt, double gravity) {
    const Wrench w1 = wrench(state);
    const RigidBodyDerivative k1 = derivative(state, mass, w1, gravity);
    const RigidBodyState s2 = advanced(state, k1, 0.5 * dt);
    const RigidBodyDerivative k2 = derivative(s2, mass, wrench(s2), gravity);
    const RigidBodyState s3 = advanced(state, k2, 0.5 * dt);
    const RigidBodyDerivative k3 = derivative(s3, mass, wrench(s3), gravity);
    const RigidBodyState s4 = advanced(state, k3, dt);
    const RigidBodyDerivative k4 = derivative(s4, mass, wrench(s4), gravity);

    const double h6 = dt / 6.0;
    state.position += h6 * (k1.velocity + 2.0 * k2.velocity + 2.0 * k3.velocity + k4.velocity);
    state.velocity +=
        h6 * (k1.acceleration + 2.0 * k2.acceleration + 2.0 * k3.acceleration + k4.acceleration);
    state.angularVelocity += h6 * (k1.angularAcceleration + 2.0 * k2.angularAcceleration +
                                   2.0 * k3.angularAcceleration + k4.angularAcceleration);
    const Quat dq = k1.orientationRate + 2.0 * k2.orientationRate + 2.0 * k3.orientationRate +
                    k4.orientationRate;
    state.orientation = glm::normalize(state.orientation + dq * h6);
    return w1;
}

bool isFinite(const RigidBodyState& s) noexcept {
    const Quat& q = s.orientation;
    const bool quatOk = std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) &&
                        std::isfinite(q.z) && glm::length(q) > 0.5;
    return quatOk && finite(s.position) && finite(s.velocity) && finite(s.angularVelocity);
}

}  // namespace ds::physics
