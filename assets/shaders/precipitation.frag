#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Rain streaks and snowflakes (precipitation.vert), blended over the scene.

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in float v_round;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 c = v_uv * 2.0 - 1.0;
    float shape = v_round > 0.5 ? smoothstep(1.0, 0.4, length(c))
                                : (1.0 - c.x * c.x) * smoothstep(1.0, 0.7, abs(c.y));
    o_color = vec4(v_color.rgb, v_color.a * shape);
}
