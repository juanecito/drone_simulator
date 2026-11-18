#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Tone mapping of the pre-exposed HDR image: scotopic (night) desaturation, ACES filmic curve
// (Narkowicz 2015 fit), sRGB encoding and dithering.

#include "common.glsl"

layout(binding = 4) uniform sampler2D t_hdr;
// Lower-left corner of the view in the target (non-zero for picture-in-picture insets).
layout(location = 0) uniform ivec2 u_offset;

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) {
    return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 toSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

void main() {
    vec3 c = texelFetch(t_hdr, ivec2(gl_FragCoord.xy) - u_offset, 0).rgb;
    float y = luminance(c);
    float absolute = y / max(e_exposure, 1e-12);
    // Mesopic blend: rods take over below ~3 cd/m², fully scotopic below ~0.003 cd/m².
    float photopic = clamp((log(max(absolute, 1e-9)) / log(10.0) + 2.5) / 3.0, 0.0, 1.0);
    vec3 scotopic = y * vec3(0.85, 0.95, 1.15);
    c = mix(scotopic, c, photopic);
    vec3 srgb = toSrgb(aces(c));
    srgb += (hash(gl_FragCoord.xy) - 0.5) / 255.0;
    o_color = vec4(srgb, 1.0);
}
