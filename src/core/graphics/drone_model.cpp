// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/drone_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ds::graphics {
namespace {

// Linear albedos.
constexpr glm::vec3 kBodyColor{0.035F, 0.037F, 0.040F};
constexpr glm::vec3 kArmColor{0.020F, 0.020F, 0.022F};  // Carbon fibre.
constexpr glm::vec3 kMotorColor{0.30F, 0.30F, 0.32F};
constexpr glm::vec3 kCameraColor{0.015F, 0.015F, 0.015F};
constexpr glm::vec3 kFrontPropColor{0.70F, 0.16F, 0.02F};  // Orange: shows the front.
constexpr glm::vec3 kRearPropColor{0.03F, 0.03F, 0.03F};
constexpr float kLedLuminance = 4000.0F;  // cd/m²: bright in daylight, dazzling at night.

glm::vec3 toVec3(const glm::dvec3& v) { return glm::vec3(v); }

std::shared_ptr<const Mesh> bladeMesh(double diameter, int blades, const glm::vec3& color) {
    auto mesh = std::make_shared<Mesh>();
    const auto radius = static_cast<float>(0.5 * diameter);
    const float chord = std::max(0.08F * radius, 0.004F);
    const float thickness = std::max(0.02F * radius, 0.0015F);
    const MeshMaterial m{color};
    const int n = std::clamp(blades, 2, 6);
    for (int i = 0; i < n; ++i) {
        const float angle =
            2.0F * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n);
        // Blade along +x before the rotation about the shaft (body z).
        const glm::quat r = glm::angleAxis(angle, glm::vec3(0, 0, 1)) *
                            glm::angleAxis(0.25F, glm::vec3(1, 0, 0));  // Blade pitch.
        appendBox(*mesh, r * glm::vec3(0.5F * radius, 0, 0),
                  glm::vec3(0.5F * radius, 0.5F * chord, 0.5F * thickness), m, r);
    }
    appendCylinder(*mesh, glm::vec3(0, 0, 0.004F), glm::vec3(0, 0, -0.006F), 0.12F * radius, 10,
                   {kMotorColor});
    return mesh;
}

std::shared_ptr<const Mesh> ledMesh(const glm::vec3& color, float size) {
    auto mesh = std::make_shared<Mesh>();
    appendSphere(*mesh, glm::vec3(0), size, 8, {color * 0.2F, 1.0F, kLedLuminance});
    // The emitted colour comes from the albedo hue; the shader scales it by `emissive`.
    for (MeshVertex& v : mesh->vertices) {
        v.color = glm::vec4(color, 1.0F);
    }
    return mesh;
}

}  // namespace

DroneShape droneShapeFor(const std::array<glm::dvec3, 4>& motors, double propDiameterM, int blades,
                         double restHeightM, double cameraUptiltDeg) {
    DroneShape s;
    s.motors = motors;
    s.propDiameterM = propDiameterM;
    s.blades = blades;
    double arm = 0.0;
    for (const glm::dvec3& m : motors) {
        arm = std::max(arm, std::hypot(m.x, m.y));
    }
    arm = std::max(arm, 0.03);
    s.bodySize = glm::dvec3(0.75 * arm, 0.38 * arm, std::max(0.22 * arm, 0.02));
    s.armWidthM = std::max(0.09 * arm, 0.006);
    s.motorRadiusM = std::clamp(0.09 * propDiameterM, 0.006, 0.05);
    s.gearHeightM = std::max(restHeightM - 0.5 * s.bodySize.z, 0.01);
    s.cameraUptiltDeg = cameraUptiltDeg;
    return s;
}

DroneModel::DroneModel(const DroneShape& shape) : shape_(shape) {
    auto frame = std::make_shared<Mesh>();
    const glm::vec3 body = toVec3(shape.bodySize);
    const float armHalfWidth = static_cast<float>(0.5 * shape.armWidthM);
    const auto motorR = static_cast<float>(shape.motorRadiusM);
    const float motorH = 1.1F * motorR;
    // Central body and top plate.
    appendBox(*frame, glm::vec3(0), 0.5F * body, {kBodyColor});
    appendBox(*frame, glm::vec3(0, 0, -0.5F * body.z - 0.002F),
              glm::vec3(0.45F * body.x, 0.45F * body.y, 0.002F), {kArmColor});
    // FPV camera at the front, tilted up.
    {
        const float c = 0.3F * body.y;
        const glm::quat tilt =
            glm::angleAxis(static_cast<float>(shape.cameraUptiltDeg * std::numbers::pi / 180.0),
                           glm::vec3(0, 1, 0));
        appendBox(*frame, glm::vec3(0.5F * body.x + 0.4F * c, 0, -0.1F * body.z),
                  glm::vec3(0.5F * c, 0.5F * c, 0.5F * c), {kCameraColor}, tilt);
        appendCylinder(*frame, glm::vec3(0.5F * body.x + 0.8F * c, 0, -0.1F * body.z),
                       glm::vec3(0.5F * body.x + 1.2F * c, 0, -0.1F * body.z - 0.3F * c), 0.3F * c,
                       10, {glm::vec3(0.01F)});
    }
    for (const glm::dvec3& md : shape.motors) {
        const glm::vec3 m = toVec3(md);
        const glm::vec2 dir = glm::normalize(glm::vec2(m.x, m.y));
        const float length = glm::length(glm::vec2(m.x, m.y));
        const glm::quat r = glm::angleAxis(std::atan2(dir.y, dir.x), glm::vec3(0, 0, 1));
        // Arm from the centre to the motor, at mid body height.
        appendBox(*frame, glm::vec3(0.5F * m.x, 0.5F * m.y, 0.0F),
                  glm::vec3(0.5F * length, armHalfWidth, 0.3F * armHalfWidth), {kArmColor}, r);
        // Motor bell on top of the arm end.
        appendCylinder(*frame, glm::vec3(m.x, m.y, -0.3F * armHalfWidth),
                       glm::vec3(m.x, m.y, -0.3F * armHalfWidth - motorH), motorR, 16,
                       {kMotorColor});
        // Landing foot under the motor.
        const auto gear = static_cast<float>(shape.gearHeightM);
        appendCylinder(*frame, glm::vec3(m.x, m.y, 0.3F * armHalfWidth), glm::vec3(m.x, m.y, gear),
                       0.35F * armHalfWidth, 8, {kArmColor});
    }
    frame_ = std::move(frame);

    blades_[0] = bladeMesh(shape.propDiameterM, shape.blades, kFrontPropColor);
    blades_[1] = bladeMesh(shape.propDiameterM, shape.blades, kRearPropColor);
    auto blur = std::make_shared<Mesh>();
    blur->transparent = true;
    appendDisc(*blur, glm::vec3(0), glm::vec3(0, 0, -1),
               static_cast<float>(0.5 * shape.propDiameterM), 32, {glm::vec3(0.05F), 0.22F, 0.0F});
    blur_ = std::move(blur);
    const float led = std::max(0.25F * armHalfWidth, 0.0025F);
    ledRed_ = ledMesh(glm::vec3(1.0F, 0.02F, 0.01F), led);
    ledGreen_ = ledMesh(glm::vec3(0.02F, 1.0F, 0.05F), led);
    ledWhite_ = ledMesh(glm::vec3(1.0F), led);
}

void DroneModel::appendDrawItems(const DroneVisualState& state, std::vector<DrawItem>& out) const {
    const glm::dquat& q = state.orientation;
    out.push_back({.mesh = frame_, .position = state.position, .orientation = q});
    const double armHalfWidth = 0.5 * shape_.armWidthM;
    const double propZ = -0.3 * armHalfWidth - 1.1 * shape_.motorRadiusM - 0.004;
    for (std::size_t i = 0; i < 4; ++i) {
        const glm::dvec3 hub(shape_.motors[i].x, shape_.motors[i].y, propZ);
        const glm::dvec3 position = state.position + q * hub;
        const bool front = shape_.motors[i].x > 0.0;
        if (state.rotorSpeedFraction[i] > kBlurSpeedFraction) {
            // Motion blur: a translucent disc, denser at higher speed.
            const auto density =
                static_cast<float>(std::clamp(0.6 + 0.6 * state.rotorSpeedFraction[i], 0.0, 1.0));
            const glm::vec4 tint =
                front ? glm::vec4(6.0F, 2.0F, 0.6F, density) : glm::vec4(1.0F, 1.0F, 1.0F, density);
            out.push_back({.mesh = blur_,
                           .position = position,
                           .orientation = q,
                           .tint = tint,
                           .castsShadow = false});
        } else {
            // Clockwise seen from above (+1) is a positive rotation about body +z (down).
            const double angle = shape_.spin[i] * state.propAngleRad[i];
            out.push_back({.mesh = blades_[front ? 0 : 1],
                           .position = position,
                           .orientation = q * glm::angleAxis(angle, glm::dvec3(0, 0, 1))});
        }
    }
    // Navigation lights: red on the left, green on the right (front arms), white strobe at
    // the rear (1 Hz double flash).
    const double phase = std::fmod(state.timeS, 1.0);
    const bool strobe = phase < 0.05 || (phase > 0.15 && phase < 0.2);
    const auto ledAt = [&](std::size_t motor) {
        const glm::dvec3 m = shape_.motors[motor];
        return state.position + q * glm::dvec3(m.x, m.y, 0.3 * armHalfWidth + 0.003);
    };
    const float on = state.lights ? 1.0F : 0.0F;
    out.push_back({.mesh = ledRed_, .position = ledAt(3), .orientation = q, .emissiveScale = on});
    out.push_back({.mesh = ledGreen_, .position = ledAt(0), .orientation = q, .emissiveScale = on});
    const glm::dvec3 rear =
        state.position + q * glm::dvec3(-0.5 * shape_.bodySize.x, 0.0, -0.5 * shape_.bodySize.z);
    out.push_back({.mesh = ledWhite_,
                   .position = rear,
                   .orientation = q,
                   .emissiveScale = state.lights && strobe ? 1.0F : 0.0F});
}

}  // namespace ds::graphics
