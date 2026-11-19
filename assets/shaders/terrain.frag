#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Terrain: material splatting with procedural detail, sun/moon/sky lighting, local spot
// lights (headlights) and aerial perspective, with cascaded shadows of the Sun or the Moon
// (FR-GFX-10).

#include "common.glsl"
#include "atmosphere.glsl"
#include "shadow.glsl"
#include "lighting.glsl"

layout(location = 3) uniform vec3 u_cameraPos;  // Camera world position (float; detail noise only).

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_material;
layout(location = 0) out vec4 o_color;

const vec3 kGrass = vec3(0.060, 0.090, 0.035);
const vec3 kRock = vec3(0.170, 0.150, 0.130);
const vec3 kSand = vec3(0.420, 0.370, 0.270);
const vec3 kSnow = vec3(0.820, 0.850, 0.900);

void main() {
    vec3 n = normalize(v_normal);
    vec4 w = v_material / max(dot(v_material, vec4(1.0)), 1e-4);
    vec2 p = v_rel.xz + u_cameraPos.xz;
    float dist = length(v_rel);
    // Detail fades out with distance to avoid shimmering.
    float detail = valueNoise(p * 0.11) * 0.6 + valueNoise(p * 0.023) * 0.4;
    detail = mix(detail, 0.5, smoothstep(300.0, 3000.0, dist));
    vec3 albedo = w.x * kGrass + w.y * kRock + w.z * kSand + w.w * kSnow;
    albedo *= 0.8 + 0.4 * detail;
    float visibility = shadowVisibility(v_rel, n);
    vec3 irradiance = surfaceIrradianceShadowed(surfaceRadius(v_rel.y), n, visibility) +
                      spotIrradiance(v_rel, n);
    vec3 luminance = albedo / PI * irradiance;
    o_color = finishSurface(luminance, v_rel);
}
