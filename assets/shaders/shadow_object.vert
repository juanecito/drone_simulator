#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Depth-only pass of object meshes into a shadow cascade.

layout(location = 0) in vec3 a_position;

layout(location = 0) uniform mat4 u_model;      // Model → camera-relative world.
layout(location = 5) uniform mat4 u_lightClip;  // Camera-relative world → light clip.

void main() { gl_Position = u_lightClip * (u_model * vec4(a_position, 1.0)); }
