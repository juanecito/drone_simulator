#version 450
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Sky: atmosphere from the sky-view LUT, sun disc, moon disc with phase, earthshine and
// albedo texture, and the night-sky background. Drawn where the depth buffer is still at
// infinity, after the opaque geometry.

#include "common.glsl"
#include "atmosphere.glsl"

layout(binding = 3) uniform sampler2D t_moonAlbedo;

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

const float kSunLimbDarkening = 0.6;

// Visible fraction of a ray's light transmitted to space (0 if it hits the ground).
vec3 viewTransmittance(float r, vec3 dir) {
    return rayHitsGround(r, dir.y) ? vec3(0.0) : transmittanceToTop(r, dir.y);
}

float pixelAngle() { return 2.0 * u_camera.w * u_viewport.w; }

vec3 sunDisc(vec3 dir) {
    float radius = u_sunColorRadius.w;
    float a = acos(clamp(dot(dir, u_sunDirLux.xyz), -1.0, 1.0));
    float pixel = pixelAngle();
    if (a > radius + pixel) {
        return vec3(0.0);
    }
    float rho = min(a / radius, 1.0);  // 0 at the centre, 1 at the limb.
    float muLimb = sqrt(max(1.0 - rho * rho, 0.0));
    float darkening = (1.0 - kSunLimbDarkening * (1.0 - muLimb)) / (1.0 - kSunLimbDarkening / 3.0);
    float solidAngle = PI * radius * radius;
    float edge = clamp((radius - a) / pixel + 0.5, 0.0, 1.0);  // Anti-aliased limb.
    return u_sunColorRadius.rgb * (u_sunDirLux.w / solidAngle) * darkening * edge;
}

vec3 moonDisc(vec3 dir) {
    vec3 m = u_moonDirLux.xyz;
    float radius = u_moonColorRadius.w;
    float c = dot(dir, m);
    float a = acos(clamp(c, -1.0, 1.0));
    float pixel = pixelAngle();
    if (a > radius + pixel) {
        return vec3(0.0);
    }
    // Point on the lunar sphere hit by the view ray (unit sphere at distance 1 / sin r).
    float dist = 1.0 / sin(radius);
    float b = dist * c;
    float disc = max(b * b - dist * dist + 1.0, 0.0);  // Clamped: limb for the AA fringe.
    vec3 hit = dir * (b - sqrt(disc));
    vec3 n = normalize(hit - m * dist);
    vec3 toObserver = -m;
    float cosE = max(dot(n, toObserver), 0.0);
    float cosI = dot(n, u_moonToSunDisc.xyz);

    // Selenographic coordinates: lunar north ≈ ecliptic north (within 1.5°; libration is
    // ignored); +longitude points to the observer's sky west (IAU convention).
    vec3 north = normalize(u_moonNorth.xyz - dot(u_moonNorth.xyz, m) * m);
    vec3 west = cross(m, north);
    float lat = asin(clamp(dot(n, north), -1.0, 1.0));
    float lon = atan(dot(n, west), dot(n, toObserver));
    vec2 uv = vec2(0.5 + lon / (2.0 * PI), 0.5 - lat / PI);
    float albedo = texture(t_moonAlbedo, uv).r;  // Relative to the mean albedo.

    float lit = cosI > 0.0 ? cosI / max(cosI + cosE, 1e-4) : 0.0;  // Lommel–Seeliger.
    // Earthshine: sunlight reflected by the Earth; strongest at new moon.
    float earth = 1.0 - u_misc.z;
    float earthshine = 2.5e-4 * earth * earth * 0.5;
    float luminance = u_moonToSunDisc.w * albedo * (lit + earthshine);
    float edge = clamp((radius - a) / pixel + 0.5, 0.0, 1.0);
    return u_moonColorRadius.rgb * luminance * edge;
}

void main() {
    vec4 near = u_invViewProj * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(near.xyz / near.w);
    float r = cameraRadius();

    vec3 color = skyLuminance(dir, r);
    vec3 t = viewTransmittance(r, dir);
    // Airglow and unresolved starlight, brighter towards the horizon (van Rhijn).
    float airglow = u_misc.y * (1.0 + 1.5 * (1.0 - max(dir.y, 0.0)));
    color += t * airglow * vec3(0.8, 0.9, 1.1);
    color += t * (sunDisc(dir) + moonDisc(dir));
    o_color = vec4(min(color * e_exposure, vec3(60000.0)), 1.0);
}
