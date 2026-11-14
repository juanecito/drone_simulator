// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Surface lighting shared by terrain, water and objects. Requires common.glsl,
// atmosphere.glsl and shadow.glsl.

// Radius (km) of a surface point at camera-relative height relY (m).
float surfaceRadius(float relY) {
    float amsl = u_camera.z + u_camera.x + relY;
    return bottomRadius() + max(amsl, 0.0) * 0.001;
}

vec3 directLight(float r, vec3 n, vec4 dirLux, vec4 colorRadius) {
    vec3 l = dirLux.xyz;
    vec3 t = transmittanceToLight(r, l.y, colorRadius.w);
    return t * colorRadius.rgb * dirLux.w * max(dot(n, l), 0.0);
}

// Total irradiance (lux) on a Lambertian surface with normal n.
vec3 surfaceIrradiance(float r, vec3 n) {
    vec3 sky = e_skyIrradiance.rgb * (0.5 + 0.5 * n.y);
    return directLight(r, n, u_sunDirLux, u_sunColorRadius) +
           directLight(r, n, u_moonDirLux, u_moonColorRadius) + sky;
}

// As surfaceIrradiance, with the direct light of the shadow-casting body (Sun, or the Moon
// when the Sun is down; shadow.glsl) scaled by its visibility.
vec3 surfaceIrradianceShadowed(float r, vec3 n, float visibility) {
    bool sunCasts = s_texel.w < 0.5;
    vec3 sky = e_skyIrradiance.rgb * (0.5 + 0.5 * n.y);
    return directLight(r, n, u_sunDirLux, u_sunColorRadius) * (sunCasts ? visibility : 1.0) +
           directLight(r, n, u_moonDirLux, u_moonColorRadius) * (sunCasts ? 1.0 : visibility) +
           sky;
}

vec4 finishSurface(vec3 luminance, vec3 rel) {
    vec3 inScatter;
    vec3 t = aerialPerspective(rel, inScatter);
    return vec4(min((luminance * t + inScatter) * e_exposure, vec3(60000.0)), 1.0);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
