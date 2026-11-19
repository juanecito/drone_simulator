// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/rover_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace ds::graphics {
namespace {

// Linear albedos.
constexpr glm::vec3 kBodyColor{0.30F, 0.13F, 0.015F};  // Dark orange shell.
constexpr glm::vec3 kChassisColor{0.03F, 0.03F, 0.032F};
constexpr glm::vec3 kTyreColor{0.012F, 0.012F, 0.012F};
constexpr glm::vec3 kRimColor{0.35F, 0.35F, 0.37F};
constexpr glm::vec3 kCameraColor{0.015F, 0.015F, 0.015F};
constexpr float kHeadlightLuminance = 6000.0F;  // cd/m²: lamp faces, not the beam.
constexpr float kTailLightLuminance = 800.0F;

std::shared_ptr<const Mesh> lampMesh(const glm::vec3& color, const glm::vec3& halfExtents,
                                     float luminance) {
    auto mesh = std::make_shared<Mesh>();
    appendBox(*mesh, glm::vec3(0), halfExtents, {color * 0.2F, 1.0F, luminance});
    // The emitted colour comes from the albedo hue; the shader scales it by `emissive`.
    for (MeshVertex& v : mesh->vertices) {
        v.color = glm::vec4(color, 1.0F);
    }
    return mesh;
}

}  // namespace

RoverModel::RoverModel(RoverShape shape) : shape_(std::move(shape)) {
    const glm::vec3 size(shape_.bodySize);
    const auto bottom = static_cast<float>(shape_.bodyBottomM);
    const float centreZ = bottom - 0.5F * size.z;

    auto body = std::make_shared<Mesh>();
    // Chassis tub and the shell on top of it.
    appendBox(*body, glm::vec3(0, 0, bottom - 0.2F * size.z),
              glm::vec3(0.5F * size.x, 0.42F * size.y, 0.2F * size.z), {kChassisColor});
    appendBox(*body, glm::vec3(-0.03F * size.x, 0, centreZ - 0.1F * size.z),
              glm::vec3(0.47F * size.x, 0.5F * size.y, 0.4F * size.z), {kBodyColor});
    // Axles between the wheel pairs.
    const std::size_t perSide = shape_.wheelHubs.size() / 2;
    for (std::size_t i = 0; i < perSide; ++i) {
        const glm::vec3 left(shape_.wheelHubs[i]);
        const glm::vec3 right(shape_.wheelHubs[i + perSide]);
        appendCylinder(*body, left, right, 0.08F * static_cast<float>(shape_.wheelRadiusM), 8,
                       {kChassisColor});
    }
    // FPV camera at the front of the shell, tilted up.
    {
        const float c = std::clamp(0.12F * size.y, 0.015F, 0.05F);
        const float z = centreZ - 0.5F * size.z;
        const glm::quat tilt =
            glm::angleAxis(static_cast<float>(shape_.cameraUptiltDeg * std::numbers::pi / 180.0),
                           glm::vec3(0, 1, 0));
        appendBox(*body, glm::vec3(0.4F * size.x, 0, z - 0.5F * c),
                  glm::vec3(0.5F * c, 0.5F * c, 0.5F * c), {kCameraColor}, tilt);
    }
    body_ = std::move(body);

    // Wheel along the body y axis (the axle), with a rim and a spoke so its rotation shows.
    auto wheel = std::make_shared<Mesh>();
    const auto r = static_cast<float>(shape_.wheelRadiusM);
    const auto w = static_cast<float>(shape_.wheelWidthM);
    appendCylinder(*wheel, glm::vec3(0, -0.5F * w, 0), glm::vec3(0, 0.5F * w, 0), r, 20,
                   {kTyreColor});
    for (const float side : {-1.0F, 1.0F}) {
        const float y = side * (0.5F * w + 0.001F);
        appendDisc(*wheel, glm::vec3(0, y, 0), glm::vec3(0, side, 0), 0.6F * r, 16, {kRimColor});
        appendBox(*wheel, glm::vec3(0, y + side * 0.001F, 0),
                  glm::vec3(0.55F * r, 0.001F, 0.09F * r), {kTyreColor});
    }
    wheel_ = std::move(wheel);

    const float lamp = std::clamp(0.06F * size.y, 0.006F, 0.02F);
    headlight_ = lampMesh(glm::vec3(1.0F, 0.95F, 0.85F), glm::vec3(0.003F, lamp, 0.6F * lamp),
                          kHeadlightLuminance);
    tailLight_ = lampMesh(glm::vec3(1.0F, 0.02F, 0.01F), glm::vec3(0.003F, lamp, 0.5F * lamp),
                          kTailLightLuminance);
}

glm::dvec3 RoverModel::hubAt(std::size_t index, double compressionM) const {
    glm::dvec3 hub = shape_.wheelHubs[index];
    hub.z -= compressionM - shape_.staticCompressionM;
    return hub;
}

glm::dvec3 RoverModel::headlightAt(double side) const {
    const glm::dvec3 size = shape_.bodySize;
    return {0.5 * size.x + 0.003, side * 0.3 * size.y, shape_.bodyBottomM - 0.55 * size.z};
}

void RoverModel::appendLights(const RoverVisualState& state, std::vector<SpotLight>& out) const {
    if (!state.lights) {
        return;
    }
    const double down = kHeadlightDownDeg * std::numbers::pi / 180.0;
    // Body frame: x forward, z down.
    const glm::dvec3 axis = state.orientation * glm::dvec3(std::cos(down), 0.0, std::sin(down));
    for (const double side : {-1.0, 1.0}) {
        out.push_back({.position = state.position + state.orientation * headlightAt(side),
                       .direction = axis,
                       .color = headlightColor(),
                       .intensityCd = kHeadlightCd,
                       .innerAngleRad = kHeadlightInnerDeg * std::numbers::pi / 180.0,
                       .outerAngleRad = kHeadlightOuterDeg * std::numbers::pi / 180.0,
                       .rangeM = kHeadlightRangeM});
    }
}

void RoverModel::appendDrawItems(const RoverVisualState& state, std::vector<DrawItem>& out) const {
    const glm::dquat& q = state.orientation;
    out.push_back({.mesh = body_, .position = state.position, .orientation = q});
    const std::size_t wheels = std::min(shape_.wheelHubs.size(), kRoverMaxWheels);
    for (std::size_t i = 0; i < wheels; ++i) {
        // Rolling forward turns the top of the wheel towards +x: a negative turn about +y.
        out.push_back(
            {.mesh = wheel_,
             .position = state.position + q * hubAt(i, state.compression[i]),
             .orientation = q * glm::angleAxis(-state.wheelAngle[i], glm::dvec3(0, 1, 0))});
    }
    const glm::dvec3 size = shape_.bodySize;
    const double z = shape_.bodyBottomM - 0.55 * size.z;
    const float on = state.lights ? 1.0F : 0.0F;
    for (const double side : {-1.0, 1.0}) {
        out.push_back({.mesh = headlight_,
                       .position = state.position + q * headlightAt(side),
                       .orientation = q,
                       .emissiveScale = on,
                       .castsShadow = false});
        // Tail lights: dim with the lights on, bright while braking.
        const float tail = state.braking ? 3.0F : on;
        out.push_back(
            {.mesh = tailLight_,
             .position = state.position + q * glm::dvec3(-0.47 * size.x - 0.03 * size.x - 0.003,
                                                         side * 0.35 * size.y, z),
             .orientation = q,
             .emissiveScale = tail,
             .castsShadow = false});
    }
}

}  // namespace ds::graphics
