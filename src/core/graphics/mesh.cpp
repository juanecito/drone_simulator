// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ds::graphics {
namespace {

constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;

std::uint32_t base(const Mesh& mesh) { return static_cast<std::uint32_t>(mesh.vertices.size()); }

void push(Mesh& mesh, const glm::vec3& p, const glm::vec3& n, const MeshMaterial& m) {
    mesh.vertices.push_back({p, n, glm::vec4(m.albedo, m.opacity), m.emissive});
}

void quad(Mesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
}

// Two unit vectors perpendicular to n (and to each other).
void basis(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 helper = std::abs(n.y) < 0.9F ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(helper, n));
    v = glm::cross(n, u);
}

}  // namespace

void appendBox(Mesh& mesh, const glm::vec3& centre, const glm::vec3& h,
               const MeshMaterial& material, const glm::quat& rotation) {
    // Each face: outward normal, and two in-plane axes with u × v = normal (CCW from outside).
    struct Face {
        glm::vec3 n, u, v;
    };
    constexpr Face kFaces[] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    for (const Face& f : kFaces) {
        const std::uint32_t b = base(mesh);
        const glm::vec3 c = f.n * h;
        const glm::vec3 u = f.u * h;
        const glm::vec3 v = f.v * h;
        const glm::vec3 n = rotation * f.n;
        for (const glm::vec2 s :
             {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
            push(mesh, centre + rotation * (c + s.x * u + s.y * v), n, material);
        }
        quad(mesh, b, b + 1, b + 2, b + 3);
    }
}

void appendCylinder(Mesh& mesh, const glm::vec3& a, const glm::vec3& b, float radius, int segments,
                    const MeshMaterial& material) {
    const int n = std::max(segments, 3);
    const glm::vec3 axis = glm::normalize(b - a);
    glm::vec3 u;
    glm::vec3 v;
    basis(axis, u, v);
    // Side.
    const std::uint32_t side = base(mesh);
    for (int i = 0; i <= n; ++i) {
        const float t = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
        const glm::vec3 radial = std::cos(t) * u + std::sin(t) * v;
        push(mesh, a + radius * radial, radial, material);
        push(mesh, b + radius * radial, radial, material);
    }
    for (int i = 0; i < n; ++i) {
        const auto k = side + static_cast<std::uint32_t>(2 * i);
        quad(mesh, k, k + 2, k + 3, k + 1);
    }
    // Caps.
    for (const bool top : {false, true}) {
        const glm::vec3 centre = top ? b : a;
        const glm::vec3 normal = top ? axis : -axis;
        const std::uint32_t c = base(mesh);
        push(mesh, centre, normal, material);
        for (int i = 0; i <= n; ++i) {
            const float t = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
            push(mesh, centre + radius * (std::cos(t) * u + std::sin(t) * v), normal, material);
        }
        for (int i = 0; i < n; ++i) {
            const auto k = c + 1 + static_cast<std::uint32_t>(i);
            if (top) {
                mesh.indices.insert(mesh.indices.end(), {c, k, k + 1});
            } else {
                mesh.indices.insert(mesh.indices.end(), {c, k + 1, k});
            }
        }
    }
}

void appendDisc(Mesh& mesh, const glm::vec3& centre, const glm::vec3& normal, float radius,
                int segments, const MeshMaterial& material) {
    const int n = std::max(segments, 3);
    glm::vec3 u;
    glm::vec3 v;
    const glm::vec3 axis = glm::normalize(normal);
    basis(axis, u, v);
    for (const float side : {1.0F, -1.0F}) {
        const std::uint32_t c = base(mesh);
        push(mesh, centre, side * axis, material);
        for (int i = 0; i <= n; ++i) {
            const float t = kTwoPi * static_cast<float>(i) / static_cast<float>(n);
            push(mesh, centre + radius * (std::cos(t) * u + std::sin(t) * v), side * axis,
                 material);
        }
        for (int i = 0; i < n; ++i) {
            const auto k = c + 1 + static_cast<std::uint32_t>(i);
            if (side > 0.0F) {
                mesh.indices.insert(mesh.indices.end(), {c, k, k + 1});
            } else {
                mesh.indices.insert(mesh.indices.end(), {c, k + 1, k});
            }
        }
    }
}

void appendSphere(Mesh& mesh, const glm::vec3& centre, float radius, int segments,
                  const MeshMaterial& material) {
    const int slices = std::max(segments, 4);
    const int stacks = std::max(segments / 2, 2);
    const std::uint32_t b = base(mesh);
    for (int i = 0; i <= stacks; ++i) {
        const float phi =
            std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(stacks);
        for (int j = 0; j <= slices; ++j) {
            const float theta = kTwoPi * static_cast<float>(j) / static_cast<float>(slices);
            const glm::vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta));
            push(mesh, centre + radius * n, n, material);
        }
    }
    const auto row = static_cast<std::uint32_t>(slices + 1);
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const std::uint32_t k =
                b + static_cast<std::uint32_t>(i) * row + static_cast<std::uint32_t>(j);
            quad(mesh, k, k + 1, k + row + 1, k + row);
        }
    }
}

}  // namespace ds::graphics
