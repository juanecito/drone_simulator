#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Object meshes (MeshVertex, 44 bytes). The model matrix is camera-relative (FR-GFX-06).

#include "common.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_emissive;

layout(location = 0) uniform mat4 u_model;   // Model → camera-relative world.
layout(location = 4) uniform mat3 u_normal;  // Inverse transpose of the model rotation/scale.

layout(location = 0) out vec3 v_rel;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_color;
layout(location = 3) out float v_emissive;

void main() {
    vec4 rel = u_model * vec4(a_position, 1.0);
    v_rel = rel.xyz;
    v_normal = u_normal * a_normal;
    v_color = a_color;
    v_emissive = a_emissive;
    gl_Position = u_viewProj * rel;
}
