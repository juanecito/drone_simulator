// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>

namespace ds::graphics {

ShadowSetup computeShadowCascades(const Camera& camera, const glm::dvec3& towardsLight,
                                  int resolution,
                                  const std::array<double, kShadowCascades>& splitsM,
                                  double depthExtraM) {
    ShadowSetup setup;
    const double length = glm::length(towardsLight);
    if (length < 1e-9 || resolution <= 0) {
        return setup;
    }
    const glm::dvec3 l = towardsLight / length;
    setup.lightDirection = glm::vec3(l);
    setup.enabled = true;

    // Light basis (world): z towards the light, x and y across.
    const glm::dvec3 helper = std::abs(l.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 lx = glm::normalize(glm::cross(helper, l));
    const glm::dvec3 ly = glm::cross(l, lx);

    const glm::dvec3 f = camera.forward();
    const glm::dvec3 r = camera.right();
    const glm::dvec3 u = camera.up();
    const double tanY = std::tan(0.5 * camera.fovYRad);
    const double tanX = tanY * camera.aspect;
    const glm::dvec3& cam = camera.position;

    double nearM = camera.nearM;
    for (int i = 0; i < kShadowCascades; ++i) {
        const double farM = splitsM[static_cast<std::size_t>(i)];
        // Bounding sphere of the slice [nearM, farM]: centre on the view axis where the
        // distances to the near and far corners are equal (clamped to the far plane).
        const double k2 = tanX * tanX + tanY * tanY;
        const double centreDist = std::min(farM, 0.5 * (nearM + farM) * (1.0 + k2));
        const glm::dvec3 centreRel = f * centreDist;
        double radius = 0.0;
        for (const double d : {nearM, farM}) {
            for (const double sx : {-1.0, 1.0}) {
                for (const double sy : {-1.0, 1.0}) {
                    const glm::dvec3 corner = f * d + r * (sx * tanX * d) + u * (sy * tanY * d);
                    radius = std::max(radius, glm::length(corner - centreRel));
                }
            }
        }
        // Quantise the radius so the texel size does not change with small camera turns.
        radius = std::ceil(radius * 16.0) / 16.0;
        const double texel = 2.0 * radius / resolution;

        // Snap the centre to whole texels in world-fixed light coordinates.
        const glm::dvec3 centreWorld = cam + centreRel;
        double cx = glm::dot(centreWorld, lx);
        double cy = glm::dot(centreWorld, ly);
        const double cz = glm::dot(centreWorld, l);
        cx = std::floor(cx / texel) * texel;
        cy = std::floor(cy / texel) * texel;

        // Depth: 0 at the light side of the sphere, 1 at the far side plus the extra margin.
        const double zNear = cz + radius;               // Towards the light.
        const double zFar = cz - radius - depthExtraM;  // Away from the light.
        const double range = zNear - zFar;

        // Camera-relative p: light coords = basis·(p + cam) − centre.
        const double ox = glm::dot(cam, lx) - cx;
        const double oy = glm::dot(cam, ly) - cy;
        const double oz = glm::dot(cam, l);
        glm::dmat4 m(0.0);
        // Column-major: m[column][row].
        m[0][0] = lx.x / radius;
        m[1][0] = lx.y / radius;
        m[2][0] = lx.z / radius;
        m[3][0] = ox / radius;
        m[0][1] = ly.x / radius;
        m[1][1] = ly.y / radius;
        m[2][1] = ly.z / radius;
        m[3][1] = oy / radius;
        m[0][2] = -l.x / range;
        m[1][2] = -l.y / range;
        m[2][2] = -l.z / range;
        m[3][2] = (zNear - oz) / range;
        m[3][3] = 1.0;

        ShadowCascade& c = setup.cascades[static_cast<std::size_t>(i)];
        c.matrix = glm::mat4(m);
        c.farM = static_cast<float>(farM);
        c.radiusM = static_cast<float>(radius);
        c.texelM = static_cast<float>(texel);
        c.depthRangeM = static_cast<float>(range);
        nearM = farM;
    }
    return setup;
}

}  // namespace ds::graphics
