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
