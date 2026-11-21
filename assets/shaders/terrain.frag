#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Terrain: material splatting with procedural detail, or orthophoto albedo where imagery
// exists (FR-GFX-07, FR-TER-I04/I07); sun/moon/sky lighting, local spot lights (headlights)
// and aerial perspective, with cascaded shadows of the Sun or the Moon (FR-GFX-10).

#include "common.glsl"
#include "atmosphere.glsl"
#include "shadow.glsl"
#include "weather.glsl"
#include "lighting.glsl"

layout(location = 3) uniform vec3 u_cameraPos;  // Camera world position (float; detail noise only).
// Orthophoto tiles of this chunk (4 × 4, row by row): layer (−1 = none), scale, offset x, z
// within the layer, which may hold a coarser stand-in tile.
layout(location = 5) uniform int u_orthoOn;
layout(location = 6) uniform vec4 u_orthoSlots[16];
// sRGB tiles with premultiplied alpha.
layout(binding = 6) uniform sampler2DArray t_ortho;

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_material;
layout(location = 3) in vec2 v_chunkUv;
layout(location = 0) out vec4 o_color;

// Orthophoto albedo (linear, premultiplied) and coverage at this fragment.
vec4 orthophoto() {
    vec2 g = clamp(v_chunkUv, 0.0, 1.0) * 4.0;
    // Gradients of the continuous position, so mip selection has no seams at tile borders.
    vec2 gx = dFdx(v_chunkUv * 4.0);
    vec2 gy = dFdy(v_chunkUv * 4.0);
    if (u_orthoOn == 0) {
        return vec4(0.0);
    }
    ivec2 cell = min(ivec2(g), ivec2(3));
    vec4 slot = u_orthoSlots[cell.y * 4 + cell.x];
    if (slot.x < 0.0) {
        return vec4(0.0);
    }
    vec2 uv = slot.zw + slot.y * (g - vec2(cell));
    return textureGrad(t_ortho, vec3(uv, slot.x), gx * slot.y, gy * slot.y);
}

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
    // Imagery replaces the materials where it exists, blending at its soft edges (FR-TER-I04).
    vec4 photo = orthophoto();
    albedo = albedo * (1.0 - photo.a) + photo.rgb;
    // Weather (FR-WX-09): lying snow on gentle slopes; wet ground is darker and shiny.
    float snow = smoothstep(0.003, 0.04, w_surface.y) * smoothstep(0.55, 0.8, n.y);
    albedo = mix(albedo, kSnow * (0.95 + 0.1 * detail), snow);
    float wet = w_surface.x * (1.0 - snow);
    albedo *= 1.0 - 0.45 * wet;
    float r = surfaceRadius(v_rel.y);
    float visibility = shadowVisibility(v_rel, n);
    vec3 irradiance = surfaceIrradianceShadowed(v_rel, r, n, visibility) + spotIrradiance(v_rel, n);
    vec3 luminance = albedo / PI * irradiance;
    if (wet > 0.0) {
        // A water film reflects the sky (Fresnel, rough).
        vec3 v = normalize(-v_rel);
        float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
        vec3 refl = reflect(-v, n);
        refl.y = max(refl.y, 0.01);
        vec3 sky = mix(skyLuminance(normalize(refl), cameraRadius()), ambientLuminance(v_rel.y),
                       overcastAbove(v_rel.y));
        luminance += wet * 0.5 * fresnel * sky;
    }
    o_color = finishSurface(luminance, v_rel);
}
