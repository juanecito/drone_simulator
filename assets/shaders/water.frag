#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Water: Fresnel reflection of the sky, sun/moon glint and a dark body colour.

#include "common.glsl"
#include "atmosphere.glsl"
#include "shadow.glsl"
#include "lighting.glsl"

layout(location = 3) uniform vec3 u_cameraPos;  // Camera world position (detail noise only).

layout(location = 0) in vec3 v_rel;
layout(location = 0) out vec4 o_color;

vec3 glint(float r, vec3 n, vec3 v, vec4 dirLux, vec4 colorRadius) {
    vec3 l = dirLux.xyz;
    vec3 h = normalize(l + v);
    const float kPower = 600.0;
    float spec = (kPower + 8.0) / (8.0 * PI) * pow(max(dot(n, h), 0.0), kPower);
    return transmittanceToLight(r, l.y, colorRadius.w) * colorRadius.rgb * dirLux.w * spec *
           max(dot(n, l), 0.0);
}

void main() {
    vec2 p = v_rel.xz + u_cameraPos.xz;
    float t = u_misc.x;
    float dist = length(v_rel);
    vec2 slope = vec2(valueNoise(p * 0.15 + t * 0.30) - 0.5, valueNoise(p.yx * 0.17 - t * 0.25) - 0.5);
    slope *= 0.12 * (1.0 - smoothstep(200.0, 4000.0, dist));
    vec3 n = normalize(vec3(slope.x, 1.0, slope.y));
    vec3 v = normalize(-v_rel);
    float r = surfaceRadius(v_rel.y);
    float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 refl = reflect(-v, n);
    refl.y = max(refl.y, 0.01);
    vec3 sky = skyLuminance(normalize(refl), cameraRadius());
    vec3 body = vec3(0.010, 0.025, 0.030) / PI * surfaceIrradiance(r, vec3(0.0, 1.0, 0.0));
    vec3 luminance = mix(body, sky, fresnel) + fresnel *
        (glint(r, n, v, u_sunDirLux, u_sunColorRadius) + glint(r, n, v, u_moonDirLux, u_moonColorRadius));
    o_color = finishSurface(luminance, v_rel);
}
