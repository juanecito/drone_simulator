#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Full-screen triangle; v_ndc is the normalised device position.

layout(location = 0) out vec2 v_ndc;

void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);  // Depth 0 = infinitely far (reversed-Z).
}
