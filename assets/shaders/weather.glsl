// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Weather (FR-WX-09, FR-GFX-09): cloud layers as noise-modulated slabs, ground fog and the
// light under the clouds. Mirrors WeatherUniforms in renderer.cpp and WeatherView in
// weather_view.hpp (cloudNoise() also in weather_view.cpp). Requires common.glsl and
// atmosphere.glsl. Heights are camera-relative world y (m).

const int kMaxCloudLayers = 3;
const float kCloudAsymmetry = 0.85;

layout(std140, binding = 4) uniform WeatherUniforms {
    vec4 w_layerHeights[kMaxCloudLayers];  // x: base; y: top; z: extinction (1/m); w: cover 0–1.
    vec4 w_layerNoise[kMaxCloudLayers];    // xy: camera position minus drift (x, z, m);
                                           // z: noise frequency (1/m); w: threshold.
    vec4 w_layerLook[kMaxCloudLayers];     // x: softness; y: type (0 cumulus, 1 stratocumulus,
                                           // 2 stratus, 3 cirrus, 4 cumulonimbus).
    vec4 w_light;           // x: sky factor; y: beam diffuse; z: clear fraction (at the
                            // ground); w: number of layers.
    vec4 w_fog;             // x: fog top; y: fog extinction (1/m, 0 = none); z: aerosol
                            // visibility (m).
    vec4 w_wind;            // xy: wind direction (x, z), unit; z: speed (m/s).
    vec4 w_surface;         // x: ground wetness 0–1; y: snow depth (m).
    vec4 w_precip;          // x: type (0 none, 1 drizzle, 2 rain, 3 snow, 4 sleet); y: rate
                            // (mm/h); z: box size (m); w: particle count.
    vec4 w_precipOffset;    // xyz: offset of the particle field (m); w: time (s, wrapped).
    vec4 w_precipVelocity;  // xyz: particle velocity relative to the camera (m/s); w: shutter (s).
};

int cloudLayerCount() { return int(w_light.w + 0.5); }

// Five octaves of value noise, each rotated by about 37° and shifted (weather_view.cpp).
float cloudNoise(vec2 q) {
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < 5; ++i) {
        sum += amplitude * valueNoise(q);
        total += amplitude;
        q = vec2(0.8 * q.x - 0.6 * q.y, 0.6 * q.x + 0.8 * q.y) * 2.03 + vec2(17.1, 4.7);
        amplitude *= 0.5;
    }
    return sum / total;
}

// Density 0–1 of layer i at camera-relative position p, at height fraction hn (0 base, 1 top).
float cloudDensity(int i, vec3 p, float hn) {
    vec2 q = p.xz + w_layerNoise[i].xy;
    int type = int(w_layerLook[i].y + 0.5);
    if (type == 3) {
        // Cirrus: streaks along the wind.
        vec2 wd = dot(w_wind.xy, w_wind.xy) > 0.5 ? w_wind.xy : vec2(1.0, 0.0);
        q = vec2(dot(q, wd) * 0.25, dot(q, vec2(-wd.y, wd.x)));
    }
    float f = cloudNoise(q * w_layerNoise[i].z);
    if (type == 0 || type == 4) {
        f -= 0.12 * hn * hn;  // Flat bases, narrower towards the top.
    } else if (type == 1) {
        float c = 2.0 * hn - 1.0;
        f -= 0.06 * c * c;  // Rounded above and below.
    }
    float t = w_layerNoise[i].w;
    return smoothstep(t, t + w_layerLook[i].x, f);
}

// Share of the diffuse light (sky) and of the direct light (on average) left at height y
// below or inside the layers (the CPU's skyFactor and clearFraction at the ground).
void lightUnderClouds(float y, out float sky, out float clear) {
    sky = 1.0;
    clear = 1.0;
    int count = cloudLayerCount();
    for (int i = 0; i < count; ++i) {
        float base = w_layerHeights[i].x;
        float top = w_layerHeights[i].y;
        if (y >= top) {
            continue;
        }
        float tau = w_layerHeights[i].z * (top - max(y, base));
        float cover = w_layerHeights[i].w;
        float td = 1.0 / (1.0 + 0.75 * (1.0 - kCloudAsymmetry) * tau);
        sky *= 1.0 - cover * (1.0 - td);
        clear *= 1.0 - cover * (1.0 - exp(-tau));
    }
}

float atmosphereRadius(float relY) {
    return bottomRadius() + max(u_camera.z + u_camera.x + relY, 0.0) * 0.001;
}

// Direct illuminance (lux) of a light at normal incidence at radius r.
vec3 lightAtNormal(float r, vec4 dirLux, vec4 colorRadius) {
    return transmittanceToLight(r, dirLux.xyz.y, colorRadius.w) * colorRadius.rgb * dirLux.w;
}

// Luminance of a white isotropic scatterer (fog, rain) in the open at height relY, lit by
// the sky and by the Sun and the Moon through the clouds on average.
vec3 ambientLuminance(float relY) {
    float r = atmosphereRadius(relY);
    vec3 horizontal = lightAtNormal(r, u_sunDirLux, u_sunColorRadius) * max(u_sunDirLux.y, 0.0) +
                      lightAtNormal(r, u_moonDirLux, u_moonColorRadius) * max(u_moonDirLux.y, 0.0);
    float sky;
    float clear;
    lightUnderClouds(relY, sky, clear);
    return (horizontal * (clear + max(sky - clear, 0.0)) + e_skyIrradiance.rgb * sky) / PI;
}

// Share of the sky above height relY hidden by the clouds (1 − clear fraction).
float overcastAbove(float relY) {
    float sky;
    float clear;
    lightUnderClouds(relY, sky, clear);
    return 1.0 - clear;
}

// Transmittance of the clouds from camera-relative point p towards the light l (cloud
// shadows): one sample per layer, at mid-height or at p if p is higher.
float cloudShadow(vec3 p, vec3 l) {
    if (l.y <= 0.0) {
        return 1.0;
    }
    float t = 1.0;
    int count = cloudLayerCount();
    for (int i = 0; i < count; ++i) {
        float base = w_layerHeights[i].x;
        float top = w_layerHeights[i].y;
        if (p.y >= top) {
            continue;
        }
        float ys = max(0.5 * (base + top), p.y);
        vec3 s = p + l * ((ys - p.y) / l.y);
        float d = cloudDensity(i, s, (ys - base) / (top - base));
        float path = min((top - max(base, p.y)) / max(l.y, 0.02), 20000.0);
        t *= exp(-w_layerHeights[i].z * d * path);
    }
    return t;
}

float henyeyGreenstein(float c, float g) {
    float d = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (4.0 * PI * d * sqrt(d));
}

// Light scattered towards the viewer (direction dir) by cloud layer i at camera-relative p,
// height fraction hn and density d (per unit extinction: the in-scattering source).
vec3 cloudSource(int i, vec3 p, float hn, float d, vec3 dir) {
    float r = atmosphereRadius(p.y);
    float top = w_layerHeights[i].y;
    float sigma = w_layerHeights[i].z;
    int type = int(w_layerLook[i].y + 0.5);
    float sky;
    float clear;
    lightUnderClouds(top, sky, clear);  // Light left by the layers above.
    // Multiple scattering: the light on the top falls to the diffuse transmittance at the base.
    float tau = sigma * (top - w_layerHeights[i].x);
    float td = 1.0 / (1.0 + 0.75 * (1.0 - kCloudAsymmetry) * tau);
    float inside = mix(td, 1.0, pow(hn, 0.7));
    // Convective clouds catch low sunlight on their sides.
    float side = (type == 0 || type == 4) ? 0.35 : 0.0;
    vec3 total = e_skyIrradiance.rgb * sky * inside / PI;
    for (int k = 0; k < 2; ++k) {
        vec4 dirLux = k == 0 ? u_sunDirLux : u_moonDirLux;
        vec4 colorRadius = k == 0 ? u_sunColorRadius : u_moonColorRadius;
        vec3 l = dirLux.xyz;
        vec3 normal = lightAtNormal(r, dirLux, colorRadius) * sky;
        float onTop = mix(max(l.y, 0.0), 0.6 * smoothstep(-0.03, 0.1, l.y), side);
        total += normal * onTop * inside / PI;
        if (l.y > 0.0) {
            // Single scattering of the direct beam: bright, thin edges (silver lining).
            float tauLight = sigma * d * (top - p.y) / max(l.y, 0.05);
            total += normal * henyeyGreenstein(dot(dir, l), 0.6) * exp(-tauLight);
        }
    }
    return total * 0.95;
}

// Part [t0, t1] of the ray from the camera along dir (up to maxDist) inside layer i. With
// horizon, rays that would enter the layer beyond maxDist see it at maxDist (an overcast sky
// reaches the horizon).
// atHorizon is set for such a segment (the clouds there merge into a band).
bool layerSegment(int i, vec3 dir, float maxDist, bool horizon, out float t0, out float t1,
                  out bool atHorizon) {
    float base = w_layerHeights[i].x;
    float top = w_layerHeights[i].y;
    atHorizon = false;
    if (abs(dir.y) < 1e-6) {
        t0 = 0.0;
        t1 = (base <= 0.0 && top >= 0.0) ? maxDist : -1.0;
    } else {
        float ta = base / dir.y;
        float tb = top / dir.y;
        t0 = max(min(ta, tb), 0.0);
        t1 = min(max(ta, tb), maxDist);
    }
    if (horizon && base > 0.0 && (dir.y <= 0.0 || t0 >= maxDist)) {
        t0 = maxDist - 3000.0;
        t1 = maxDist;
        atHorizon = true;
    }
    return t1 > t0;
}

// Integrates the cloud layers from the camera along dir up to maxDist (m). Returns the
// transmittance; writes the in-scattered luminance and the mean distance of the light.
// steps: samples per layer; jitter in [0, 1) breaks up banding.
float cloudMarch(vec3 dir, float maxDist, bool horizon, int steps, float jitter,
                 out vec3 luminance, out float meanDist) {
    luminance = vec3(0.0);
    meanDist = 0.0;
    int count = cloudLayerCount();
    if (count == 0) {
        return 1.0;
    }
    // Layers in the order the ray meets them.
    float s0[kMaxCloudLayers];
    float s1[kMaxCloudLayers];
    int order[kMaxCloudLayers];
    bool band[kMaxCloudLayers];
    int n = 0;
    for (int i = 0; i < count; ++i) {
        float t0;
        float t1;
        bool atHorizon;
        if (layerSegment(i, dir, maxDist, horizon, t0, t1, atHorizon)) {
            int j = n++;
            while (j > 0 && s0[j - 1] > t0) {
                s0[j] = s0[j - 1];
                s1[j] = s1[j - 1];
                order[j] = order[j - 1];
                band[j] = band[j - 1];
                --j;
            }
            s0[j] = t0;
            s1[j] = t1;
            order[j] = i;
            band[j] = atHorizon;
        }
    }
    float transmittance = 1.0;
    float weight = 0.0;
    for (int k = 0; k < n && transmittance > 0.01; ++k) {
        int i = order[k];
        float base = w_layerHeights[i].x;
        float thickness = w_layerHeights[i].y - base;
        float dt = (s1[k] - s0[k]) / float(steps);
        for (int s = 0; s < steps; ++s) {
            float start = s0[k] + float(s) * dt;
            float t = start + jitter * dt;
            vec3 p = dir * t;
            float hn = clamp((p.y - base) / thickness, 0.0, 1.0);
            float d = band[k] ? smoothstep(0.0, 0.5, w_layerHeights[i].w)
                              : cloudDensity(i, p, hn);
            if (d <= 0.0) {
                continue;
            }
            float sigma = w_layerHeights[i].z * d;
            float a = 1.0 - exp(-sigma * dt);
            float w = transmittance * a;
            // The light of an optically thick step comes from its first mean free path.
            vec3 ps = dir * (start + min(jitter * dt, 1.0 / sigma));
            float hs = clamp((ps.y - base) / thickness, 0.0, 1.0);
            luminance += w * cloudSource(i, ps, hs, d, dir);
            meanDist += w * t;
            weight += w;
            transmittance *= 1.0 - a;
        }
    }
    meanDist = weight > 0.0 ? meanDist / weight : 0.0;
    return transmittance;
}

// Transmittance of the ground fog from the camera along dir over dist (m).
float fogTransmittance(vec3 dir, float dist) {
    if (w_fog.y <= 0.0) {
        return 1.0;
    }
    float top = w_fog.x;
    float len = 0.0;
    if (abs(dir.y) < 1e-5) {
        len = top > 0.0 ? dist : 0.0;
    } else {
        float tTop = top / dir.y;
        if (top > 0.0) {
            len = dir.y > 0.0 ? min(dist, tTop) : dist;  // The camera is in the fog.
        } else {
            len = dir.y < 0.0 ? max(dist - tTop, 0.0) : 0.0;
        }
    }
    return exp(-w_fog.y * len);
}

// Clouds and fog between the camera and a point at distance dist along dir, over the
// luminance behind them. hazeColor: luminance the aerosols tend to with distance.
vec3 applyWeather(vec3 behind, vec3 dir, float dist, bool horizon, int steps, float jitter,
                  vec3 hazeColor) {
    vec3 cloud;
    float meanDist;
    float tc = cloudMarch(dir, dist, horizon, steps, jitter, cloud, meanDist);
    vec3 c = behind;
    if (tc < 1.0) {
        // Distant clouds fade into the haze (Koschmieder).
        float haze = exp(-3.912 * meanDist / max(w_fog.z, 50.0));
        c = behind * tc + cloud * haze + (1.0 - tc) * (1.0 - haze) * hazeColor;
    }
    float tf = fogTransmittance(dir, dist);
    if (tf < 1.0) {
        c = c * tf + ambientLuminance(0.0) * 0.9 * (1.0 - tf);
    }
    return c;
}
