#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Depth-only pass of terrain chunks into a shadow cascade (same layout as terrain.vert).

layout(location = 0) in float a_height;
layout(location = 1) in vec2 a_grid;

layout(location = 0) uniform vec3 u_chunkOrigin;  // Grid vertex (0, 0) minus camera.
layout(location = 1) uniform float u_spacing;
layout(location = 2) uniform vec4 u_extentRel;    // minX, minZ, maxX, maxZ minus camera.
layout(location = 5) uniform mat4 u_lightClip;    // Camera-relative world → light clip.

void main() {
    vec2 xz = u_chunkOrigin.xz + a_grid * u_spacing;
    xz = clamp(xz, u_extentRel.xy, u_extentRel.zw);
    gl_Position = u_lightClip * vec4(xz.x, a_height + u_chunkOrigin.y, xz.y, 1.0);
}
