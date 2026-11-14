#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Objects: Lambert diffuse plus a soft plastic highlight under sun, moon and sky light, with
// shadows, emission (LEDs) and aerial perspective. Translucent parts (propeller blur discs)
// are blended over the opaque scene.

#include "common.glsl"
#include "atmosphere.glsl"
#include "shadow.glsl"
#include "lighting.glsl"

layout(location = 7) uniform vec4 u_tint;
layout(location = 8) uniform float u_emissiveScale;

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_color;
layout(location = 3) in float v_emissive;
layout(location = 0) out vec4 o_color;

vec3 highlight(float r, vec3 n, vec3 v, vec4 dirLux, vec4 colorRadius, float visibility) {
    vec3 l = dirLux.xyz;
    vec3 h = normalize(l + v);
    const float kPower = 40.0;
    float spec = 0.04 * (kPower + 8.0) / (8.0 * PI) * pow(max(dot(n, h), 0.0), kPower);
    return transmittanceToLight(r, l.y, colorRadius.w) * colorRadius.rgb * dirLux.w * spec *
           max(dot(n, l), 0.0) * visibility;
}

void main() {
    vec3 n = normalize(v_normal);
    vec3 v = normalize(-v_rel);
    if (dot(n, v) < 0.0 && v_color.a * u_tint.a < 0.999) {
        n = -n;  // Thin translucent discs are seen from both sides.
    }
    vec3 albedo = v_color.rgb * u_tint.rgb;
    float r = surfaceRadius(v_rel.y);
    float visibility = shadowVisibility(v_rel, n);
    vec3 irradiance = surfaceIrradianceShadowed(r, n, visibility);
    vec3 lum = albedo / PI * irradiance;
    bool sunCasts = s_texel.w < 0.5;
    lum += highlight(r, n, v, u_sunDirLux, u_sunColorRadius, sunCasts ? visibility : 1.0);
    lum += highlight(r, n, v, u_moonDirLux, u_moonColorRadius, sunCasts ? 1.0 : visibility);
    lum += v_color.rgb * v_emissive * u_emissiveScale;
    vec4 c = finishSurface(lum, v_rel);
    o_color = vec4(c.rgb, v_color.a * u_tint.a);
}
