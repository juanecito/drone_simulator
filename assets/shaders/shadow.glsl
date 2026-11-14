// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Cascaded shadow maps of the Sun or the Moon (FR-GFX-10). Mirrors ShadowUniforms in
// renderer.cpp. Requires common.glsl.

layout(std140, binding = 2) uniform ShadowUniforms {
    mat4 s_matrix[3];  // Camera-relative position → light clip (xy −1…1, depth 0…1).
    vec4 s_far;        // xyz: view distance where each cascade ends; w: 1 if enabled.
    vec4 s_texel;      // xyz: texel size of each cascade (m); w: 0 = Sun casts, 1 = Moon.
    vec4 s_forward;    // xyz: camera view direction; w: unused.
    vec4 s_depthRange; // xyz: depth range of each cascade (m).
};

layout(binding = 5) uniform sampler2DArrayShadow s_map;

// Visibility of the shadow-casting light (1 = lit) at camera-relative position rel with
// surface normal n.
float shadowVisibility(vec3 rel, vec3 n) {
    if (s_far.w < 0.5) {
        return 1.0;
    }
    float viewDist = dot(rel, s_forward.xyz);
    int c = viewDist < s_far.x ? 0 : (viewDist < s_far.y ? 1 : 2);
    if (viewDist >= s_far.z) {
        return 1.0;
    }
    vec3 l = s_texel.w < 0.5 ? u_sunDirLux.xyz : u_moonDirLux.xyz;
    float texel = s_texel[c];
    // Normal offset against acne, larger at grazing light.
    float cosL = clamp(dot(n, l), 0.0, 1.0);
    vec3 p = rel + n * texel * (1.5 + 2.0 * (1.0 - cosL));
    vec4 s = s_matrix[c] * vec4(p, 1.0);
    vec2 uv = s.xy * 0.5 + 0.5;
    float bias = texel * 0.5 / s_depthRange[c];
    float ref = s.z - bias;
    // 3×3 PCF on top of the hardware 2×2 comparison filter.
    vec2 size = vec2(textureSize(s_map, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(s_map, vec4(uv + vec2(x, y) / size, float(c), ref));
        }
    }
    float visibility = sum / 9.0;
    // Fade out over the last 10 % of the furthest cascade.
    float fade = smoothstep(0.9 * s_far.z, s_far.z, viewDist);
    return mix(visibility, 1.0, fade);
}
