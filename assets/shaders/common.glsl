// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Shared declarations. Mirrors FrameUniforms and ExposureState in render_types.hpp.
// Positions are camera-relative (FR-GFX-06); luminance is in cd/m², illuminance in lux.

const float PI = 3.14159265358979;

layout(std140, binding = 0) uniform FrameUniforms {
    mat4 u_viewProj;           // Camera-relative world → clip (reversed-Z, infinite far).
    mat4 u_invViewProj;        // Inverse of u_viewProj.
    mat4 u_equatorialToWorld;  // Apparent equatorial of date → world (stars).
    vec4 u_sunDirLux;          // xyz: direction to the Sun; w: illuminance above the atmosphere.
    vec4 u_sunColorRadius;     // rgb: colour (luminance 1); w: angular radius (rad).
    vec4 u_moonDirLux;         // As for the Sun.
    vec4 u_moonColorRadius;
    vec4 u_moonToSunDisc;      // xyz: Moon → Sun direction; w: moon-disc luminance scale.
    vec4 u_moonNorth;          // xyz: Moon's north pole direction (≈ ecliptic north pole).
    vec4 u_viewport;           // width, height, 1/width, 1/height (pixels).
    vec4 u_camera;             // x: camera world y (m); y: camera altitude AMSL (km);
                               // z: origin altitude AMSL (m); w: tan(fovY / 2).
    vec4 u_misc;               // x: time (s); y: night-sky luminance; z: moon illuminated
                               // fraction; w: star point size (px).
    vec4 u_water;              // x: water level (world y, m); y: 1 if water is enabled.
};

layout(std430, binding = 0) buffer ExposureState {
    float e_exposure;       // Pre-exposure applied to every colour written to the HDR target.
    float e_ev100;          // Current EV100.
    float e_avgLuminance;   // Measured average scene luminance (cd/m²).
    float e_pad0;
    vec4 e_skyIrradiance;   // rgb: irradiance on an upward surface from the sky (lux).
    vec4 e_groundIrradiance;  // rgb: irradiance on a downward surface from the ground (lux).
    uint e_histogram[256];
};

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}

// Interleaved gradient noise (Jimenez 2014): a per-pixel offset in [0, 1) whose pattern the
// eye reads as fine grain rather than as noise or bands.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}
