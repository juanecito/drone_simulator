// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Surface lighting shared by terrain, water and objects. Requires common.glsl,
// atmosphere.glsl, shadow.glsl and weather.glsl.

// Local spot lights (headlights, FR-GFX-08d). Mirrors LightUniforms in renderer.cpp and
// spotIlluminance() in light.cpp; positions are camera-relative.
const int kMaxSpotLights = 8;
layout(std140, binding = 3) uniform LightUniforms {
    vec4 l_count;                       // x: number of lights.
    vec4 l_posRange[kMaxSpotLights];    // xyz: position; w: range (m).
    vec4 l_dirCos[kMaxSpotLights];      // xyz: beam axis; w: cosine of the outer half-angle.
    vec4 l_colorCd[kMaxSpotLights];     // rgb: colour (luminance 1); w: intensity (cd).
    vec4 l_cosInner[kMaxSpotLights];    // x: cosine of the inner half-angle.
};

// Illuminance (lux) of the spot lights on a surface at rel with normal n.
vec3 spotIrradiance(vec3 rel, vec3 n) {
    vec3 e = vec3(0.0);
    int count = min(int(l_count.x), kMaxSpotLights);
    for (int i = 0; i < count; ++i) {
        vec3 d = l_posRange[i].xyz - rel;
        float distSq = max(dot(d, d), 0.01);
        float dist = sqrt(distSq);
        vec3 l = d / dist;
        float cone = smoothstep(l_dirCos[i].w, l_cosInner[i].x, dot(-l, l_dirCos[i].xyz));
        float ratio = dist / l_posRange[i].w;
        float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
        e += l_colorCd[i].rgb * (l_colorCd[i].w * cone * window * window *
                                 max(dot(n, l), 0.0) / distSq);
    }
    return e;
}

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

// Diffuse irradiance (lux) at camera-relative p with normal n: the clear sky dimmed by the
// clouds above, plus the direct light the clouds scatter downwards (overcast light).
vec3 diffuseIrradiance(vec3 p, float r, vec3 n) {
    float sky;
    float clear;
    lightUnderClouds(p.y, sky, clear);
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 horizontal = directLight(r, up, u_sunDirLux, u_sunColorRadius) +
                      directLight(r, up, u_moonDirLux, u_moonColorRadius);
    return (e_skyIrradiance.rgb * sky + horizontal * max(sky - clear, 0.0)) * (0.5 + 0.5 * n.y);
}

// Total irradiance (lux) on a Lambertian surface at camera-relative p with normal n; the
// direct light passes the cloud shadows.
vec3 surfaceIrradiance(vec3 p, float r, vec3 n) {
    return directLight(r, n, u_sunDirLux, u_sunColorRadius) * cloudShadow(p, u_sunDirLux.xyz) +
           directLight(r, n, u_moonDirLux, u_moonColorRadius) * cloudShadow(p, u_moonDirLux.xyz) +
           diffuseIrradiance(p, r, n);
}

// As surfaceIrradiance, with the direct light of the shadow-casting body (Sun, or the Moon
// when the Sun is down; shadow.glsl) scaled by its visibility.
vec3 surfaceIrradianceShadowed(vec3 p, float r, vec3 n, float visibility) {
    bool sunCasts = s_texel.w < 0.5;
    return directLight(r, n, u_sunDirLux, u_sunColorRadius) * cloudShadow(p, u_sunDirLux.xyz) *
               (sunCasts ? visibility : 1.0) +
           directLight(r, n, u_moonDirLux, u_moonColorRadius) * cloudShadow(p, u_moonDirLux.xyz) *
               (sunCasts ? 1.0 : visibility) +
           diffuseIrradiance(p, r, n);
}

#ifndef DS_NO_FINISH_SURFACE
// Pixel jitter for the cloud samples (breaks up banding).
float pixelJitter() { return interleavedGradientNoise(gl_FragCoord.xy); }

// Aerial perspective, clouds and fog between the camera and a surface at camera-relative
// position rel; returns the pre-exposed colour.
vec4 finishSurface(vec3 luminance, vec3 rel) {
    vec3 inScatter;
    vec3 t = aerialPerspective(rel, inScatter);
    // Under the clouds the haze is lit by the overcast, not by the clear sky.
    vec3 ambient = ambientLuminance(0.0);
    inScatter = mix(inScatter, (1.0 - t) * ambient, overcastAbove(0.0));
    float dist = length(rel);
    vec3 dir = rel / max(dist, 1e-3);
    vec3 c = applyWeather(luminance * t + inScatter, dir, dist, false, 3, pixelJitter(), ambient);
    return vec4(min(c * e_exposure, vec3(60000.0)), 1.0);
}
#endif
