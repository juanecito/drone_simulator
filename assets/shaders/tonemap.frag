#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Tone mapping of the pre-exposed HDR image: raindrops on the lens (FR-WX-09), daylight white
// balance, scotopic (night) desaturation, ACES filmic curve (Narkowicz 2015 fit), sRGB
// encoding and dithering.

#include "common.glsl"

layout(binding = 4) uniform sampler2D t_hdr;
// Lower-left corner of the view in the target (non-zero for picture-in-picture insets).
layout(location = 0) uniform ivec2 u_offset;
// Linear sRGB chromatic adaptation that makes midday daylight neutral (whiteBalanceMatrix()).
layout(location = 1) uniform mat3 u_whiteBalance;
// x: wetness of the lens 0–1 (raindrops); y: time (s).
layout(location = 2) uniform vec2 u_lens;

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

vec3 aces(vec3 x) {
    return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 toSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

// Raindrops on the lens: out-of-focus drops that each show a small, upside-down image of the
// scene behind them, darker at the rim. Drops come and go (and run down a little) on a grid
// of cells; the wetter the lens, the more cells hold one.
vec3 lensDrops(vec2 px, vec3 c) {
    vec2 size = vec2(textureSize(t_hdr, 0));
    float cell = size.y / 8.0;
    vec2 g = px / cell;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 id = floor(g) + vec2(i, j);
            float h0 = hash(id + 0.37);
            float period = 5.0 + 7.0 * h0;
            float age = u_lens.y / period + h0;
            float generation = floor(age);
            float life = fract(age);
            if (hash(id + generation * 1.731) > 0.75 * u_lens.x) {
                continue;
            }
            vec2 jitter = vec2(hash(id + generation + 3.1), hash(id + generation + 7.7)) - 0.5;
            vec2 center = (id + 0.5 + 0.7 * jitter) * cell;
            center.y -= 0.3 * cell * life * life;  // Runs down late in its life.
            float radius = cell * (0.08 + 0.22 * hash(id + generation + 11.3));
            vec2 d = px - center;
            float r = length(d) / radius;
            if (r >= 1.0) {
                continue;
            }
            float fade = smoothstep(0.0, 0.08, life) * (1.0 - smoothstep(0.75, 1.0, life));
            float edge = smoothstep(1.0, 0.8, r);
            vec2 source = clamp(center - d * 3.0, vec2(0.0), size - 1.0);
            vec3 through = texelFetch(t_hdr, ivec2(source), 0).rgb *
                           mix(0.55, 1.0, sqrt(1.0 - r * r));
            c = mix(c, through, fade * edge);
        }
    }
    return c;
}

void main() {
    vec2 px = gl_FragCoord.xy - vec2(u_offset);
    vec3 c = texelFetch(t_hdr, ivec2(px), 0).rgb;
    if (u_lens.x > 0.0) {
        c = lensDrops(px, c);
    }
    c = max(u_whiteBalance * c, vec3(0.0));
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
