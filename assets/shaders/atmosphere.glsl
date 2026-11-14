// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Physically based atmosphere (FR-GFX-08/09). Implemented from the published methods:
//  - E. Bruneton, "A Qualitative and Quantitative Evaluation of 8 Clear Sky Models" (2016) and
//    "Precomputed Atmospheric Scattering" (2008, 2017 rev.): transmittance LUT parametrisation.
//  - S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
//    (EGSR 2020): multiple-scattering LUT and sky-view LUT.
// Distances in km. Requires common.glsl.

layout(std140, binding = 1) uniform AtmosphereUniforms {
    vec4 a_rayleigh;   // rgb: scattering (1/km) at sea level; w: scale height (km)
    vec4 a_mie;        // rgb: scattering (1/km); w: scale height (km)
    vec4 a_mieExt;     // rgb: extinction (1/km); w: asymmetry g
    vec4 a_ozone;      // rgb: absorption (1/km) at peak; w: peak altitude (km)
    vec4 a_radii;      // x: planet radius; y: atmosphere top radius; z: ozone half width
    vec4 a_ground;     // rgb: ground albedo
};

layout(binding = 0) uniform sampler2D t_transmittance;
layout(binding = 1) uniform sampler2D t_multiScatter;
layout(binding = 2) uniform sampler2D t_skyView;

float bottomRadius() { return a_radii.x; }
float topRadius() { return a_radii.y; }

struct Medium {
    vec3 rayleigh;    // Scattering coefficients.
    vec3 mie;
    vec3 extinction;
};

Medium sampleMedium(float altitudeKm) {
    float h = max(altitudeKm, 0.0);
    float dR = exp(-h / a_rayleigh.w);
    float dM = exp(-h / a_mie.w);
    float dO = max(0.0, 1.0 - abs(h - a_ozone.w) / a_radii.z);
    Medium m;
    m.rayleigh = a_rayleigh.rgb * dR;
    m.mie = a_mie.rgb * dM;
    m.extinction = m.rayleigh + a_mieExt.rgb * dM + a_ozone.rgb * dO;
    return m;
}

float rayleighPhase(float c) { return 3.0 / (16.0 * PI) * (1.0 + c * c); }

float miePhase(float c) {  // Cornette–Shanks.
    float g = a_mieExt.w;
    float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + c * c) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}

// Distance along a ray from radius r with cosine mu to the sphere of radius R (outside hit),
// or −1 if none.
float distanceToSphere(float r, float mu, float R) {
    float disc = r * r * (mu * mu - 1.0) + R * R;
    if (disc < 0.0) {
        return -1.0;
    }
    float s = sqrt(disc);
    float t1 = -r * mu - s;
    float t2 = -r * mu + s;
    return t1 > 0.0 ? t1 : (t2 > 0.0 ? t2 : -1.0);
}

float distanceToTop(float r, float mu) {
    float disc = r * r * (mu * mu - 1.0) + topRadius() * topRadius();
    return max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
}

bool rayHitsGround(float r, float mu) {
    return mu < 0.0 && r * r * (mu * mu - 1.0) + bottomRadius() * bottomRadius() >= 0.0;
}

float distanceToGround(float r, float mu) {
    float disc = r * r * (mu * mu - 1.0) + bottomRadius() * bottomRadius();
    return max(-r * mu - sqrt(max(disc, 0.0)), 0.0);
}

// --- Transmittance LUT (Bruneton) ------------------------------------------------------------

vec2 transmittanceUv(float r, float mu) {
    float H = sqrt(topRadius() * topRadius() - bottomRadius() * bottomRadius());
    float rho = sqrt(max(r * r - bottomRadius() * bottomRadius(), 0.0));
    float d = distanceToTop(r, mu);
    float dMin = topRadius() - r;
    float dMax = rho + H;
    return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}

void transmittanceParams(vec2 uv, out float r, out float mu) {
    float H = sqrt(topRadius() * topRadius() - bottomRadius() * bottomRadius());
    float rho = H * uv.y;
    r = sqrt(rho * rho + bottomRadius() * bottomRadius());
    float dMin = topRadius() - r;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

// Transmittance from radius r along mu to the top of the atmosphere (ignores the ground).
vec3 transmittanceToTop(float r, float mu) {
    return textureLod(t_transmittance, transmittanceUv(r, mu), 0.0).rgb;
}

// Transmittance towards a light, including the planet's shadow (soft over the disc size).
vec3 transmittanceToLight(float r, float mu, float angularRadius) {
    float sinH = bottomRadius() / r;
    float cosH = -sqrt(max(1.0 - sinH * sinH, 0.0));
    float a = max(angularRadius, 1e-4);
    float visible = smoothstep(-a, a, mu - cosH);
    return transmittanceToTop(r, mu) * visible;
}

// Transmittance over a segment of length d from (r, mu) (Bruneton's GetTransmittance).
vec3 transmittanceSegment(float r, float mu, float d, bool hitsGround) {
    float rd = clamp(sqrt(d * d + 2.0 * r * mu * d + r * r), bottomRadius(), topRadius());
    float mud = clamp((r * mu + d) / rd, -1.0, 1.0);
    if (hitsGround) {
        return min(transmittanceToTop(rd, -mud) / max(transmittanceToTop(r, -mu), vec3(1e-6)),
                   vec3(1.0));
    }
    return min(transmittanceToTop(r, mu) / max(transmittanceToTop(rd, mud), vec3(1e-6)),
               vec3(1.0));
}

// --- Multiple scattering LUT (Hillaire) ------------------------------------------------------

vec3 multiScattering(float r, float muLight) {
    vec2 uv = vec2(muLight * 0.5 + 0.5,
                   (r - bottomRadius()) / (topRadius() - bottomRadius()));
    return textureLod(t_multiScatter, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).rgb;
}

// --- Sky-view LUT (Hillaire): azimuth from north × zenith angle, non-linear around the horizon

float horizonZenith(float r) {  // Zenith angle of the geometric horizon (> π/2).
    float beta = acos(clamp(sqrt(max(r * r - bottomRadius() * bottomRadius(), 0.0)) / r, -1.0, 1.0));
    return PI - beta;
}

vec3 skyViewDirection(vec2 uv, float r) {
    float zh = horizonZenith(r);
    float zenith;
    if (uv.y < 0.5) {
        float c = 1.0 - 2.0 * uv.y;
        zenith = zh * (1.0 - c * c);
    } else {
        float c = 2.0 * uv.y - 1.0;
        zenith = zh + (PI - zh) * c * c;
    }
    float azimuth = uv.x * 2.0 * PI;
    float s = sin(zenith);
    return vec3(s * sin(azimuth), cos(zenith), -s * cos(azimuth));
}

vec2 skyViewUv(vec3 dir, float r) {
    float zh = horizonZenith(r);
    float zenith = acos(clamp(dir.y, -1.0, 1.0));
    float v;
    if (zenith < zh) {
        float c = sqrt(max(1.0 - zenith / zh, 0.0));
        v = 0.5 * (1.0 - c);
    } else {
        float c = sqrt(max((zenith - zh) / (PI - zh), 0.0));
        v = 0.5 * (1.0 + c);
    }
    float azimuth = atan(dir.x, -dir.z);
    float u = azimuth / (2.0 * PI);
    return vec2(fract(u), v);
}

vec3 skyLuminance(vec3 dir, float r) { return textureLod(t_skyView, skyViewUv(dir, r), 0.0).rgb; }

// Camera radius (km) from the frame uniforms.
float cameraRadius() { return bottomRadius() + max(u_camera.y, 0.002); }

// Optical depth (dimensionless, per channel) of the straight segment from radius r with
// cosine mu over d km, by midpoint integration. Stable at grazing angles, unlike ratios of
// transmittance-LUT values (Bruneton 2017, §"GetTransmittance").
vec3 opticalDepth(float r, float mu, float d, int steps) {
    float dt = d / float(steps);
    vec3 depth = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + 0.5) * dt;
        float ri = sqrt(max(r * r + t * t + 2.0 * r * mu * t, 0.0));
        depth += sampleMedium(ri - bottomRadius()).extinction;
    }
    return depth * dt;
}

// Aerial perspective for a surface at camera-relative position rel (m): returns the
// transmittance between camera and surface and writes the in-scattered luminance (cd/m²)
// to inScatter. The in-scattering is the sky-view luminance of the full ray scaled by the
// fraction of its extinction that lies in front of the surface.
vec3 aerialPerspective(vec3 rel, out vec3 inScatter) {
    float r = cameraRadius();
    float dist = length(rel);
    vec3 v = rel / max(dist, 1e-3);
    float dKm = dist * 0.001;
    float mu = v.y;
    vec3 segment = opticalDepth(r, mu, dKm, 8);
    vec3 full;
    if (rayHitsGround(r, mu)) {
        full = opticalDepth(r, mu, max(distanceToGround(r, mu), dKm), 12);
    } else {
        full = -log(max(transmittanceToTop(r, mu), vec3(1e-30)));
    }
    full = max(full, segment);
    vec3 tSeg = exp(-segment);
    vec3 tFull = exp(-full);
    inScatter = skyLuminance(v, r) * (1.0 - tSeg) / max(1.0 - tFull, vec3(1e-5));
    return tSeg;
}
