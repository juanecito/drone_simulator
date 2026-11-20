#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Rain streaks and snowflakes in a cube around the camera (FR-WX-09). Six vertices per
// particle; positions come from a hash of the particle number, moved by the weather's offset
// (fall and wind, minus the camera position) and wrapped into the cube, so the particles stay
// still in the world while the camera moves through them. Rain is drawn as streaks along its
// velocity relative to the camera over the shutter time; snow as soft round flakes.

#define DS_NO_FINISH_SURFACE  // Vertex shader: no gl_FragCoord.
#include "common.glsl"
#include "atmosphere.glsl"
#include "shadow.glsl"
#include "weather.glsl"
#include "lighting.glsl"

layout(location = 0) out vec4 v_color;  // rgb: pre-exposed luminance; a: opacity.
layout(location = 1) out vec2 v_uv;     // x across, y along the particle (0–1).
layout(location = 2) flat out float v_round;  // 1 for snowflakes.

vec3 hash31(float n) {
    vec3 p = fract(vec3(n) * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.xxy + p.yzz) * p.zyx);
}

const vec2 kCorners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1),
                                vec2(0, 1));

void main() {
    int id = gl_VertexID / 6;
    vec2 corner = kCorners[gl_VertexID % 6];
    float box = w_precip.z;
    vec3 seed = hash31(float(id) + 0.5);
    vec3 rel = mod(seed * box + w_precipOffset.xyz, box) - 0.5 * box;
    int type = int(w_precip.x + 0.5);
    bool snow = type == 3 || (type == 4 && (id & 1) == 0);
    float time = w_precipOffset.w;
    if (snow) {
        // Flakes flutter as they fall.
        rel.xz += 0.25 * sin(time * vec2(1.3, 1.7) + seed.yz * 6.2831);
    }
    float dist = max(length(rel), 1e-3);
    vec3 view = rel / dist;
    float pixel = 2.0 * u_camera.w * u_viewport.w * dist;  // Size of a pixel there (m).

    vec3 axis;
    vec3 across;
    float length_;
    float width;
    float opacity;
    if (snow) {
        float size = 0.004 + 0.006 * seed.x;
        width = max(size, 1.5 * pixel);
        length_ = width;
        vec3 helper = abs(view.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        across = normalize(cross(view, helper));
        axis = cross(across, view);
        // A flake smaller than a pixel still shows, fainter.
        opacity = 0.9 * clamp(size / width, 0.35, 1.0);
    } else {
        vec3 vel = w_precipVelocity.xyz;
        float speed = length(vel);
        axis = speed > 1e-3 ? vel / speed : vec3(0.0, -1.0, 0.0);
        float drop = type == 1 ? 0.0005 : 0.0015;  // Drop diameter (m).
        length_ = max(speed * w_precipVelocity.w, 0.02);
        width = max(drop, 1.0 * pixel);
        vec3 c = cross(axis, view);
        across = dot(c, c) > 1e-8 ? normalize(c) : normalize(cross(view, vec3(1.0, 0.0, 0.0)));
        // Streaks thinner than a pixel still show, fainter.
        opacity = (type == 1 ? 0.12 : 0.2) * clamp(sqrt(drop / width), 0.25, 1.0);
    }
    // Fade at the edges of the cube and right in front of the lens.
    opacity *= smoothstep(0.5 * box, 0.35 * box, dist) * smoothstep(0.15, 0.6, dist);

    vec3 pos = rel + axis * (length_ * (corner.y - 0.5)) + across * (width * (corner.x - 0.5));
    gl_Position = u_viewProj * vec4(pos, 1.0);

    // Drops refract their surroundings; flakes are white and catch the direct light too.
    vec3 lum = ambientLuminance(rel.y) + spotIrradiance(rel, -view) / PI;
    if (snow) {
        float r = atmosphereRadius(rel.y);
        lum = 0.9 * (lum + (directLight(r, -view, u_sunDirLux, u_sunColorRadius) *
                                cloudShadow(rel, u_sunDirLux.xyz) +
                            directLight(r, -view, u_moonDirLux, u_moonColorRadius) *
                                cloudShadow(rel, u_moonDirLux.xyz)) / PI);
    }
    v_color = vec4(min(lum * e_exposure, vec3(60000.0)), opacity);
    v_uv = corner;
    v_round = snow ? 1.0 : 0.0;
}
