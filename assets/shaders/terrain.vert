#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Terrain chunk vertices (TerrainVertex, 16 bytes). Positions are camera-relative: the chunk
// origin is subtracted from the camera position in double on the CPU (FR-GFX-06).

#include "common.glsl"

layout(location = 0) in float a_height;
layout(location = 1) in vec2 a_grid;      // Column, row.
layout(location = 2) in vec2 a_normal;    // Octahedral, snorm16 × 2 (x, z).
layout(location = 3) in vec4 a_material;  // Grass, rock, sand, snow.

layout(location = 0) uniform vec3 u_chunkOrigin;  // Grid vertex (0, 0) minus camera; y = −camera y.
layout(location = 1) uniform float u_spacing;
layout(location = 2) uniform vec4 u_extentRel;     // minX, minZ, maxX, maxZ minus camera.

layout(location = 0) out vec3 v_rel;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_material;

vec3 decodeNormal(vec2 e) {
    vec3 n = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0.0) {
        n.xz = (1.0 - abs(n.zx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

void main() {
    vec2 xz = u_chunkOrigin.xz + a_grid * u_spacing;
    xz = clamp(xz, u_extentRel.xy, u_extentRel.zw);
    v_rel = vec3(xz.x, a_height + u_chunkOrigin.y, xz.y);
    v_normal = decodeNormal(a_normal);
    v_material = a_material;
    gl_Position = u_viewProj * vec4(v_rel, 1.0);
}
