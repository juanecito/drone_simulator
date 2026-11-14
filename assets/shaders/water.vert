#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Water plane: a quad at the water level around the camera, clipped to the given rectangle.

#include "common.glsl"

layout(location = 0) uniform vec4 u_rectRel;  // minX, minZ, maxX, maxZ minus camera.

layout(location = 0) out vec3 v_rel;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 xz = mix(u_rectRel.xy, u_rectRel.zw, corner);
    v_rel = vec3(xz.x, u_water.x - u_camera.x, xz.y);
    gl_Position = u_viewProj * vec4(v_rel, 1.0);
}
