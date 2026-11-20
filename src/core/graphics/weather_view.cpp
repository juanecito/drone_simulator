// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/graphics/weather_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "ds/common/math.hpp"

namespace ds::graphics {
namespace {

// Koschmieder: visibility V ⇔ extinction 3.912 / V.
constexpr double kKoschmieder = 3.912;
// Visibility of the air above a haze or precipitation layer, m.
constexpr double kClearAirVisibilityM = 20'000.0;
// Top of a haze layer without clouds above it, m AGL.
constexpr double kHazeTopM = 1500.0;
// Asymmetry of cloud droplets' scattering.
constexpr double kCloudAsymmetry = 0.85;

struct CloudTypeLook {
    double extinctionPerM;  // Inside the cloud (3.912 / 0.06 ≈ 65 m visibility in cumulus).
    double featureM;        // Size of the cloud features.
    double softness;        // Edge width, noise units.
};

CloudTypeLook lookOf(weather::CloudType type) noexcept {
    switch (type) {
        case weather::CloudType::Cumulus: return {0.06, 1500.0, 0.06};
        case weather::CloudType::Stratocumulus: return {0.05, 900.0, 0.10};
        case weather::CloudType::Stratus: return {0.04, 4000.0, 0.25};
        case weather::CloudType::Cirrus: return {0.002, 3000.0, 0.20};
        case weather::CloudType::Cumulonimbus: return {0.08, 5000.0, 0.05};
    }
    return {0.06, 1500.0, 0.06};
}

// The GLSL hash and value noise of lighting.glsl, in float so the distribution matches.
float fract(float x) noexcept { return x - std::floor(x); }

float hash12(float px, float py) noexcept {
    float x = fract(px * 0.1031F);
    float y = fract(py * 0.1031F);
    float z = fract(px * 0.1031F);
    const float d = x * (y + 33.33F) + y * (z + 33.33F) + z * (x + 33.33F);
    x += d;
    y += d;
    z += d;
    return fract((x + y) * z);
}

float valueNoise(float px, float py) noexcept {
    const float ix = std::floor(px);
    const float iy = std::floor(py);
    const float fx = px - ix;
    const float fy = py - iy;
    const float ux = fx * fx * (3.0F - 2.0F * fx);
    const float uy = fy * fy * (3.0F - 2.0F * fy);
    const float a = hash12(ix, iy);
    const float b = hash12(ix + 1.0F, iy);
    const float c = hash12(ix, iy + 1.0F);
    const float d = hash12(ix + 1.0F, iy + 1.0F);
    const float ab = a + (b - a) * ux;
    const float cd = c + (d - c) * ux;
    return ab + (cd - ab) * uy;
}

// Sorted samples of cloudNoise() over a large area (its distribution).
const std::vector<double>& noiseSamples() {
    static const std::vector<double> samples = [] {
        constexpr int kCount = 1 << 12;
        std::vector<double> v;
        v.reserve(kCount);
        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        const auto next = [&state] {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
        };
        for (int i = 0; i < kCount; ++i) {
            const double x = (next() - 0.5) * 400.0;
            const double y = (next() - 0.5) * 400.0;
            v.push_back(cloudNoise({x, y}));
        }
        std::ranges::sort(v);
        return v;
    }();
    return samples;
}

double meanDensity(double threshold, double softness) {
    double sum = 0.0;
    for (const double f : noiseSamples()) {
        const double t = std::clamp((f - threshold) / softness, 0.0, 1.0);
        sum += t * t * (3.0 - 2.0 * t);
    }
    return sum / static_cast<double>(noiseSamples().size());
}

}  // namespace

double cloudNoise(glm::dvec2 q) noexcept {
    // Five octaves; each is rotated by about 37° (and shifted) to hide the lattice.
    auto x = static_cast<float>(q.x);
    auto y = static_cast<float>(q.y);
    float sum = 0.0F;
    float amplitude = 0.5F;
    float total = 0.0F;
    for (int i = 0; i < 5; ++i) {
        sum += amplitude * valueNoise(x, y);
        total += amplitude;
        const float rx = 0.8F * x - 0.6F * y;
        const float ry = 0.6F * x + 0.8F * y;
        x = rx * 2.03F + 17.1F;
        y = ry * 2.03F + 4.7F;
        amplitude *= 0.5F;
    }
    return static_cast<double>(sum / total);
}

namespace {

// Thresholds per cloud type at cover k / kCoverSteps, computed once: the exact bisection
// costs milliseconds, and the view is rebuilt every frame.
constexpr int kCoverSteps = 64;
constexpr std::array kCloudTypes{weather::CloudType::Cumulus, weather::CloudType::Stratocumulus,
                                 weather::CloudType::Stratus, weather::CloudType::Cirrus,
                                 weather::CloudType::Cumulonimbus};

double tabulatedThreshold(weather::CloudType type, double cover) {
    using Row = std::array<double, kCoverSteps + 1>;
    static std::array<Row, kCloudTypes.size()> table{};
    static std::array<std::once_flag, kCloudTypes.size()> built;
    if (cover <= 0.0 || cover >= 1.0) {
        return cloudCoverThreshold(cover, lookOf(type).softness);
    }
    const auto index = static_cast<std::size_t>(type);
    // Each type's row is built when a layer of that type first appears.
    std::call_once(built.at(index), [index] {
        for (int k = 0; k <= kCoverSteps; ++k) {
            table.at(index)[static_cast<std::size_t>(k)] = cloudCoverThreshold(
                static_cast<double>(k) / kCoverSteps, lookOf(kCloudTypes.at(index)).softness);
        }
    });
    const Row& row = table.at(index);
    const double x = cover * kCoverSteps;
    const auto k = std::min(static_cast<std::size_t>(x), static_cast<std::size_t>(kCoverSteps - 1));
    const double f = x - static_cast<double>(k);
    // The ends are out of the noise's range (±1): interpolate inside it only.
    const double a = k == 0 ? row[1] + (row[1] - row[2]) : row[k];
    const double b = k + 1 == kCoverSteps ? row[k] + (row[k] - row[k - 1]) : row[k + 1];
    return a + (b - a) * f;
}

}  // namespace

double cloudCoverThreshold(double cover, double softness) {
    // The noise is a weighted mean of values in [0, 1]: a threshold of 1 hides every cloud,
    // one below −softness (and the cumulus taper, 0.12) fills the sky.
    if (cover <= 0.0) {
        return 1.0;
    }
    if (cover >= 1.0) {
        return -1.0;
    }
    const std::vector<double>& s = noiseSamples();
    const double lo0 = s.front() - softness - 0.01;
    const double hi0 = s.back() + 0.01;
    // The mean density falls as the threshold rises.
    double lo = lo0;
    double hi = hi0;
    for (int i = 0; i < 24; ++i) {  // ≈ 1e-7 of the noise range.
        const double mid = 0.5 * (lo + hi);
        if (meanDensity(mid, softness) > cover) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

double cloudDiffuseTransmittance(double opticalDepth) noexcept {
    return 1.0 / (1.0 + 0.75 * (1.0 - kCloudAsymmetry) * std::max(opticalDepth, 0.0));
}

double lightningLux(double timeS) noexcept {
    // One window of 30 s may hold a flash (70 %), at a random time inside it; the flash
    // before it may still be glowing, so the previous window counts too.
    constexpr double kWindowS = 30.0;
    const auto unit = [](std::uint64_t key, std::uint64_t salt) {
        std::uint64_t z = key * 0x9E3779B97F4A7C15ULL + salt * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) / static_cast<double>(1ULL << 53);
    };
    const double window = std::floor(timeS / kWindowS);
    double lux = 0.0;
    for (double w = window - 1.0; w <= window; w += 1.0) {
        if (w < 0.0) {
            continue;
        }
        const auto key = static_cast<std::uint64_t>(w);
        if (unit(key, 1) > 0.7) {
            continue;
        }
        const double start = w * kWindowS + unit(key, 2) * (kWindowS - 1.0);
        // From 0.3 lux (far away) to 10 lux (close), log-uniform.
        const double peak = 0.3 * std::exp(std::log(10.0 / 0.3) * unit(key, 3));
        const int strokes = 1 + static_cast<int>(unit(key, 4) * 3.0);
        double at = start;
        for (int k = 0; k < strokes; ++k) {
            const double dt = timeS - at;
            if (dt >= 0.0) {
                lux += peak * (k == 0 ? 1.0 : 0.6) * std::exp(-dt / 0.03);
            }
            at += 0.05 + 0.1 * unit(key, 10 + static_cast<std::uint64_t>(k));
        }
    }
    return lux;
}

int precipitationParticles(weather::PrecipitationType type, double rateMmH) noexcept {
    const double r = std::max(rateMmH, 0.0);
    if (r <= 0.0) {
        return 0;
    }
    switch (type) {
        case weather::PrecipitationType::None: return 0;
        case weather::PrecipitationType::Drizzle:
            return static_cast<int>(std::clamp(2500.0 + 6000.0 * r, 2500.0, 6000.0));
        case weather::PrecipitationType::Rain:
            return static_cast<int>(std::clamp(400.0 + 1200.0 * std::sqrt(r), 600.0, 8000.0));
        case weather::PrecipitationType::Sleet:
            return static_cast<int>(std::clamp(400.0 + 1500.0 * std::sqrt(r), 600.0, 8000.0));
        case weather::PrecipitationType::Snow:
            return static_cast<int>(std::clamp(3000.0 * std::sqrt(r), 800.0, 10000.0));
    }
    return 0;
}

WeatherView makeWeatherView(const weather::WeatherConditions& c,
                            std::span<const weather::Drift> drift,
                            const weather::GroundCover& ground, double groundY, double windSpeedMs,
                            double timeS) {
    WeatherView v;
    v.timeS = timeS;
    for (std::size_t i = 0; i < c.clouds.size() && v.layerCount < v.layers.size(); ++i) {
        const weather::CloudLayer& layer = c.clouds[i];
        const double cover = std::clamp(layer.coverOktas / 8.0, 0.0, 1.0);
        if (cover <= 0.0 || layer.topM <= layer.baseM) {
            continue;
        }
        const CloudTypeLook look = lookOf(layer.type);
        CloudLayerView& l = v.layers[v.layerCount++];
        l.type = layer.type;
        l.baseY = groundY + layer.baseM;
        l.topY = groundY + layer.topM;
        l.cover = cover;
        l.extinctionPerM = look.extinctionPerM;
        l.frequency = 1.0 / look.featureM;
        l.softness = look.softness;
        l.threshold = tabulatedThreshold(layer.type, cover);
        l.diffuseTransmittance =
            cloudDiffuseTransmittance(look.extinctionPerM * (layer.topM - layer.baseM));
        if (i < drift.size()) {
            l.driftM = {drift[i].eastM, -drift[i].northM};
        }
    }
    std::sort(v.layers.begin(), v.layers.begin() + static_cast<std::ptrdiff_t>(v.layerCount),
              [](const CloudLayerView& a, const CloudLayerView& b) { return a.baseY < b.baseY; });

    // Light under the clouds: each layer passes its clear share and, of the covered share,
    // the diffuse transmittance (scattered) or exp(−τ) (direct).
    double sky = 1.0;
    double direct = 1.0;
    for (std::size_t i = v.layerCount; i-- > 0;) {
        CloudLayerView& l = v.layers[i];
        l.lightFactor = sky;
        const double tau = l.extinctionPerM * (l.topY - l.baseY);
        sky *= 1.0 - l.cover * (1.0 - l.diffuseTransmittance);
        direct *= 1.0 - l.cover * (1.0 - std::exp(-tau));
    }
    v.skyFactor = sky;
    v.clearFraction = direct;
    v.beamDiffuse = std::max(sky - direct, 0.0);

    // Low visibility: a ground fog layer, or (haze, rain, snow) an obscured layer up to the
    // cloud base. Both scatter grey light from the overcast; the air above keeps a clear-air
    // visibility so the clouds and the sky above them stay lit by the full sunlight.
    const double visibility = std::max(c.visibilityM, 10.0);
    double topM = c.fog.topM;
    if (topM <= 0.0 && visibility < kClearAirVisibilityM) {
        topM = kHazeTopM;
        for (std::size_t i = 0; i < v.layerCount; ++i) {
            topM = std::min(topM, v.layers[i].baseY - groundY);
        }
    }
    if (topM > 0.0) {
        const double aerosol = std::max(
            visibility, c.fog.topM > 0.0 ? weather::kAboveFogVisibilityM : kClearAirVisibilityM);
        v.fogTopY = groundY + topM;
        v.fogExtinctionPerM = std::max(kKoschmieder / visibility - kKoschmieder / aerosol, 0.0);
        v.aerosolVisibilityM = aerosol;
    } else {
        v.aerosolVisibilityM = visibility;
    }

    v.precipitation = c.precipitation.type;
    v.rateMmH = c.precipitation.rateMmH;
    v.fallSpeedMs = weather::fallSpeedMs(c.precipitation);
    v.particles = precipitationParticles(c.precipitation.type, c.precipitation.rateMmH);
    const double towards = common::degToRad(c.windFromDeg + 180.0);
    v.windMs = glm::dvec3(std::sin(towards), 0.0, -std::cos(towards)) * std::max(windSpeedMs, 0.0);
    v.lightningLux = c.thunderstorm ? lightningLux(timeS) : 0.0;
    v.wetness = std::clamp(ground.wetness, 0.0, 1.0);
    v.snowDepthM = std::max(ground.snowDepthM, 0.0);
    return v;
}

}  // namespace ds::graphics
