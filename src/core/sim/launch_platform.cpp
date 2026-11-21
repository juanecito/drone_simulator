// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/launch_platform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "ds/common/math.hpp"

namespace ds::sim {
namespace {

// Ground samples per side of the pad: at least this many, at most this far apart.
constexpr int kMinSamples = 9;
constexpr double kSampleSpacingM = 0.5;
// Above the highest sample: a bump between samples stays under the top.
constexpr double kClearanceM = 0.03;
// Points this far under the top of a platform that is the vehicle's ground are left to the
// ground contact (its penalty spring lets the gear sink a few millimetres).
constexpr double kTopBandM = 0.2;

}  // namespace

bool LaunchPlatform::covers(double x, double z) const noexcept {
    return std::abs(x - centre.x) <= halfSizeM && std::abs(z - centre.z) <= halfSizeM;
}

double footprintRadiusM(std::span<const physics::CollisionPoint> points) noexcept {
    double r = 0.0;
    for (const physics::CollisionPoint& p : points) {
        r = std::max(r, std::hypot(p.body.x, p.body.y));  // Body x forward, y right, z down.
    }
    return r;
}

double launchPlatformHalfSizeM(double footprintRadiusM, double gpsNoiseM) noexcept {
    return std::max(kLaunchPlatformMinHalfSizeM,
                    footprintRadiusM + kLaunchPlatformMarginM +
                        kLaunchPlatformGpsSigmas * std::max(gpsNoiseM, 0.0));
}

std::optional<LaunchPlatform> launchPlatformFor(const physics::GroundQuery& ground, double x,
                                                double z, double halfSizeM) {
    if (!ground) {
        return std::nullopt;
    }
    const double half = std::max(halfSizeM, kLaunchPlatformMinHalfSizeM);
    const double minNormalY = std::cos(common::degToRad(kLaunchPlatformSlopeDeg));
    double lowest = std::numeric_limits<double>::infinity();
    double highest = -std::numeric_limits<double>::infinity();
    bool steep = false;
    const int samples =
        std::max(kMinSamples, static_cast<int>(std::ceil(2.0 * half / kSampleSpacingM)) + 1);
    for (int i = 0; i < samples; ++i) {
        for (int j = 0; j < samples; ++j) {
            const double sx = x - half + 2.0 * half * i / (samples - 1);
            const double sz = z - half + 2.0 * half * j / (samples - 1);
            const physics::GroundSample g = ground(sx, sz);
            lowest = std::min(lowest, g.heightM);
            highest = std::max(highest, g.heightM);
            steep = steep || g.normal.y < minNormalY;
        }
    }
    // Uneven ground (steps, bumps between samples) counts by the height range across the
    // pad's diagonal; even planes are caught by their normals.
    const double meanSlope = (highest - lowest) / (2.0 * std::numbers::sqrt2 * half);
    if (!steep && meanSlope <= std::tan(common::degToRad(kLaunchPlatformSlopeDeg))) {
        return std::nullopt;
    }
    LaunchPlatform p;
    p.centre = physics::Vec3(x, highest + kClearanceM, z);
    p.halfSizeM = half;
    p.bottomM = lowest - kLaunchPlatformFootingM;
    return p;
}

std::optional<physics::ObstacleContact> launchPlatformContact(const LaunchPlatform& platform,
                                                              const physics::Vec3& p,
                                                              bool topIsGround) {
    const double dx = p.x - platform.centre.x;
    const double dz = p.z - platform.centre.z;
    const double h = platform.halfSizeM;
    const double top = platform.centre.y;
    if (std::abs(dx) > h || std::abs(dz) > h || p.y > top || p.y < platform.bottomM) {
        return std::nullopt;
    }
    const double toTop = top - p.y;
    if (topIsGround && toTop < kTopBandM) {
        return std::nullopt;
    }
    physics::ObstacleContact c;
    c.name = "the launch platform";
    c.restitution = 0.2;  // Concrete.
    c.friction = 0.5;
    // The nearest face; the top only when it is not the ground.
    c.depthM = std::numeric_limits<double>::infinity();
    if (!topIsGround) {
        c.depthM = toTop;
        c.normal = physics::Vec3(0.0, 1.0, 0.0);
    }
    const double toX = h - std::abs(dx);
    const double toZ = h - std::abs(dz);
    if (toX < c.depthM) {
        c.depthM = toX;
        c.normal = physics::Vec3(dx >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
    }
    if (toZ < c.depthM) {
        c.depthM = toZ;
        c.normal = physics::Vec3(0.0, 0.0, dz >= 0.0 ? 1.0 : -1.0);
    }
    return c;
}

}  // namespace ds::sim
