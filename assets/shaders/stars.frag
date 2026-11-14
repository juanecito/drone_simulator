#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "common.glsl"

layout(location = 0) in vec3 v_luminance;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 d = (gl_PointCoord - 0.5) * u_misc.w;
    float w = exp(-dot(d, d) / (2.0 * 0.36));
    o_color = vec4(min(v_luminance * w * e_exposure, vec3(60000.0)), 1.0);
}
