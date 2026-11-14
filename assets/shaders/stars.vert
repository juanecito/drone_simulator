#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Stars as point sprites. Each star's illuminance (above the atmosphere) is spread over a
// Gaussian spot so that the integrated luminance over a reference pixel grid equals E · T.

#include "common.glsl"
#include "atmosphere.glsl"

struct Star {
    vec4 directionLux;  // xyz: apparent equatorial direction; w: illuminance (lux).
    vec4 colorMag;      // rgb: colour (luminance 1); w: visual magnitude.
};

layout(std430, binding = 1) readonly buffer Stars {
    Star s_stars[];
};

layout(location = 0) out vec3 v_luminance;

const float kSpotArea = 2.26;  // ∫ exp(−d² / (2 σ²)) for σ = 0.6 px, in px².

void main() {
    Star star = s_stars[gl_VertexID];
    vec3 dir = mat3(u_equatorialToWorld) * star.directionLux.xyz;
    float r = cameraRadius();
    vec3 t = rayHitsGround(r, dir.y) ? vec3(0.0) : transmittanceToTop(r, dir.y);
    // Hidden behind the moon disc.
    if (dot(dir, u_moonDirLux.xyz) > cos(u_moonColorRadius.w)) {
        t = vec3(0.0);
    }
    // Brightness is normalised to the pixel size of a 1080-line, 60° view, so that the eye-like
    // visibility of stars does not depend on the window size (a star covers about one
    // pixel at any resolution).
    const float kReferencePixelAngle = 2.0 * 0.57735 / 1080.0;
    float pixelSolidAngle = kReferencePixelAngle * kReferencePixelAngle;
    v_luminance = star.colorMag.rgb * star.directionLux.w * t / (pixelSolidAngle * kSpotArea);
    gl_Position = u_viewProj * vec4(dir, 0.0);
    gl_Position.z = 0.0;  // At infinity.
    gl_PointSize = u_misc.w;
    if (all(equal(t, vec3(0.0)))) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // Outside the clip volume.
    }
}
