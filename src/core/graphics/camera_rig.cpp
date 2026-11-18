// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/camera_rig.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ds::graphics {
namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

}  // namespace

std::string_view cameraModeName(CameraMode mode) noexcept {
    switch (mode) {
        case CameraMode::Fpv: return "FPV";
        case CameraMode::Chase: return "Chase";
        case CameraMode::LineOfSight: return "Line of sight";
        case CameraMode::Free: return "Free";
    }
    return "?";
}

CameraMode nextCameraMode(CameraMode mode) noexcept {
    return static_cast<CameraMode>((static_cast<int>(mode) + 1) % kCameraModeCount);
}

double verticalFov(double horizontalFovRad, double aspect) noexcept {
    return 2.0 * std::atan(std::tan(0.5 * horizontalFovRad) / std::max(aspect, 1e-3));
}

Camera cameraFromAxes(const glm::dvec3& position, const glm::dvec3& forward, const glm::dvec3& up,
                      double fovYRad) {
    Camera c;
    c.position = position;
    c.fovYRad = fovYRad;
    const glm::dvec3 f = glm::normalize(forward);
    c.pitchRad = std::asin(std::clamp(f.y, -1.0, 1.0));
    // Heading clockwise from north (−z); looking straight up or down, the image top decides.
    const double horizontal = std::hypot(f.x, f.z);
    if (horizontal > 1e-6) {
        c.yawRad = std::atan2(f.x, -f.z);
    } else {
        const glm::dvec3 h = f.y < 0.0 ? up : -up;
        c.yawRad = std::atan2(h.x, -h.z);
    }
    // Roll: angle of the requested up vector from the unrolled one, about the view direction.
    Camera level = c;
    level.rollRad = 0.0;
    const glm::dvec3 r0 = level.right();
    const glm::dvec3 u0 = level.up();
    c.rollRad = std::atan2(glm::dot(up, r0), glm::dot(up, u0));
    return c;
}

void CameraRig::setChaseDistance(double distanceM) noexcept {
    chaseDistanceM_ = std::clamp(distanceM, 0.5, 50.0);
}

void CameraRig::reset(const VehiclePose& pose) {
    chaseValid_ = false;
    chasePosition_ = chaseTarget(pose);
    chaseValid_ = true;
}

glm::dvec3 CameraRig::aboveGround(glm::dvec3 p) const {
    if (ground_) {
        if (const auto h = ground_(p.x, p.z)) {
            p.y = std::max(p.y, *h + kClearanceM);
        }
    }
    return p;
}

glm::dvec3 CameraRig::chaseTarget(const VehiclePose& pose) {
    // Behind the direction of travel when moving, else behind the nose.
    glm::dvec3 dir(pose.velocity.x, 0.0, pose.velocity.z);
    if (glm::length(dir) < 2.0) {
        const glm::dvec3 nose = pose.orientation * glm::dvec3(1.0, 0.0, 0.0);
        dir = glm::dvec3(nose.x, 0.0, nose.z);
    }
    if (glm::length(dir) > 1e-6) {
        chaseDirection_ = glm::normalize(dir);
    }
    return pose.position - chaseDistanceM_ * chaseDirection_ + glm::dvec3(0.0, chaseHeight(), 0.0);
}

Camera CameraRig::topView(const VehiclePose& pose, double aspect) const {
    Camera c;
    c.position = pose.position + glm::dvec3(0.0, kTopHeightRatio * chaseDistanceM_, 0.0);
    // Heading 0 looking straight down: image up = north (−z), right = east (+x).
    c.yawRad = 0.0;
    c.pitchRad = -0.5 * std::numbers::pi;
    c.rollRad = 0.0;
    c.fovYRad = kTopFovDeg * kDeg;
    c.aspect = aspect;
    return c;
}

Camera CameraRig::behindView(const VehiclePose& pose, double aspect) const {
    const glm::dvec3 nose = pose.orientation * glm::dvec3(1.0, 0.0, 0.0);
    glm::dvec3 dir(nose.x, 0.0, nose.z);
    // Nose straight up or down: keep the chase direction.
    dir = glm::length(dir) > 1e-3 ? glm::normalize(dir) : chaseDirection_;
    const double distance = kBehindDistanceRatio * chaseDistanceM_;
    const glm::dvec3 eye = aboveGround(pose.position - distance * dir +
                                       glm::dvec3(0.0, kChaseHeightRatio * distance, 0.0));
    const glm::dvec3 look = pose.position + glm::dvec3(0.0, 0.1 * distance, 0.0) - eye;
    Camera c = cameraFromAxes(eye, look, glm::dvec3(0, 1, 0), kBehindFovDeg * kDeg);
    c.rollRad = 0.0;
    c.aspect = aspect;
    return c;
}

Camera CameraRig::update(CameraMode mode, const VehiclePose& pose, const FpvCameraParams& fpv,
                         double aspect, double dt) {
    switch (mode) {
        case CameraMode::Fpv: {
            // Body axes of the tilted camera: forward = x pitched up, up = −z pitched up.
            const glm::dquat tilt = glm::angleAxis(fpv.uptiltDeg * kDeg, glm::dvec3(0, 1, 0));
            const glm::dquat q = pose.orientation * tilt;
            const glm::dvec3 forward = q * glm::dvec3(1.0, 0.0, 0.0);
            const glm::dvec3 up = q * glm::dvec3(0.0, 0.0, -1.0);
            Camera c = cameraFromAxes(pose.position + pose.orientation * fpv.offsetBody, forward,
                                      up, verticalFov(fpv.horizontalFovDeg * kDeg, aspect));
            c.nearM = 0.02;
            c.aspect = aspect;
            return c;
        }
        case CameraMode::Chase: {
            const glm::dvec3 target = chaseTarget(pose);
            if (!chaseValid_) {
                chasePosition_ = target;
                chaseValid_ = true;
            }
            const double a = 1.0 - std::exp(-std::max(dt, 0.0) / kChaseSmoothingS);
            chasePosition_ = aboveGround(chasePosition_ + a * (target - chasePosition_));
            const glm::dvec3 look = pose.position + glm::dvec3(0.0, 0.3, 0.0) - chasePosition_;
            Camera c = cameraFromAxes(chasePosition_, look, glm::dvec3(0, 1, 0), 60.0 * kDeg);
            c.rollRad = 0.0;
            c.aspect = aspect;
            return c;
        }
        case CameraMode::LineOfSight: {
            const glm::dvec3 eye = aboveGround(pilotEye_);
            glm::dvec3 look = pose.position - eye;
            if (glm::length(look) < 1e-3) {
                look = glm::dvec3(0.0, 0.0, -1.0);
            }
            Camera c = cameraFromAxes(eye, look, glm::dvec3(0, 1, 0), kLosFovDeg * kDeg);
            c.rollRad = 0.0;
            c.aspect = aspect;
            return c;
        }
        case CameraMode::Free: break;
    }
    Camera c;
    c.aspect = aspect;
    return c;
}

}  // namespace ds::graphics
