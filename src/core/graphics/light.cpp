// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/light.hpp"

#include <algorithm>
#include <cmath>

namespace ds::graphics {
namespace {

// Closest distance used for the inverse-square law (a surface touching the lamp).
constexpr double kMinDistanceSq = 0.01;

double smoothstep(double edge0, double edge1, double x) noexcept {
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

double spotIlluminance(const SpotLight& light, const glm::dvec3& point,
                       const glm::dvec3& normal) noexcept {
    const glm::dvec3 toLight = light.position - point;
    const double distSq = std::max(glm::dot(toLight, toLight), kMinDistanceSq);
    const double dist = std::sqrt(distSq);
    const glm::dvec3 l = toLight / dist;
    const double cosAxis = glm::dot(-l, light.direction);
    const double cone =
        smoothstep(std::cos(light.outerAngleRad), std::cos(light.innerAngleRad), cosAxis);
    const double ratio = dist / light.rangeM;
    double window = std::clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    window *= window;
    return light.intensityCd * cone * window * std::max(glm::dot(normal, l), 0.0) / distSq;
}

glm::vec3 headlightColor() noexcept {
    const glm::vec3 c(1.0F, 0.93F, 0.82F);
    return c / glm::dot(c, glm::vec3(0.2126F, 0.7152F, 0.0722F));
}

}  // namespace ds::graphics
