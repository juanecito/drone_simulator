// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/terrain/procedural_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string_view>

#include "ds/common/random.hpp"

namespace ds::terrain {
namespace {

constexpr std::size_t kMaxFlatZones = 64;

common::Result<void> checkRange(std::string_view name, double value, double low, double high) {
    if (!std::isfinite(value) || value < low || value > high) {
        return common::makeError(
            std::format("{} must be between {} and {} (got {})", name, low, high, value));
    }
    return {};
}

double smoothstep(double edge0, double edge1, double x) noexcept {
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double lerp(double a, double b, double t) noexcept { return a + (b - a) * t; }

}  // namespace

common::Result<void> validate(const ProceduralParams& p) {
    if (p.octaves < 1 || p.octaves > 16) {
        return common::makeError(
            std::format("octaves must be between 1 and 16 (got {})", p.octaves));
    }
    for (const auto& check :
         {checkRange("feature size", p.featureSizeM, 50.0, 50'000.0),
          checkRange("lacunarity", p.lacunarity, 1.5, 4.0),
          checkRange("persistence", p.persistence, 0.05, 0.95),
          checkRange("amplitude", p.amplitudeM, 0.0, 8000.0),
          checkRange("base height", p.baseHeightM, -2000.0, 8000.0),
          checkRange("mountains", p.mountains, 0.0, 1.0), checkRange("warp", p.warp, 0.0, 1.0),
          checkRange("erosion", p.erosion, 0.0, 1.0)}) {
        if (!check) {
            return check;
        }
    }
    if (p.waterLevelM) {
        if (auto r = checkRange("water level", *p.waterLevelM, -2000.0, 8000.0); !r) {
            return r;
        }
    }
    if (p.snowLineM) {
        if (auto r = checkRange("snow line", *p.snowLineM, -2000.0, 20'000.0); !r) {
            return r;
        }
    }
    if (p.flatZones.size() > kMaxFlatZones) {
        return common::makeError(std::format("at most {} flat zones are allowed", kMaxFlatZones));
    }
    for (const FlatZone& zone : p.flatZones) {
        for (const auto& check : {checkRange("flat zone x", zone.x, -1.0e7, 1.0e7),
                                  checkRange("flat zone z", zone.z, -1.0e7, 1.0e7),
                                  checkRange("flat zone radius", zone.radiusM, 1.0, 20'000.0),
                                  checkRange("flat zone blend", zone.blendM, 0.0, 20'000.0)}) {
            if (!check) {
                return check;
            }
        }
        if (zone.heightM) {
            if (auto r = checkRange("flat zone height", *zone.heightM, -2000.0, 10'000.0); !r) {
                return r;
            }
        }
    }
    return {};
}

common::Result<void> validate(const Extent& extent) {
    if (auto r = checkRange("terrain width", extent.widthM, kMinExtentM, kMaxExtentM); !r) {
        return r;
    }
    return checkRange("terrain length", extent.lengthM, kMinExtentM, kMaxExtentM);
}

common::Result<std::shared_ptr<const ProceduralSource>> ProceduralSource::create(
    const ProceduralParams& params, const std::optional<Extent>& extent) {
    if (auto r = validate(params); !r) {
        return std::unexpected(r.error());
    }
    if (extent) {
        if (auto r = validate(*extent); !r) {
            return std::unexpected(r.error());
        }
    }
    return std::make_shared<const ProceduralSource>(params, extent);
}

ProceduralSource::ProceduralSource(const ProceduralParams& params,
                                   const std::optional<Extent>& extent)
    : params_(params),
      extent_(extent),
      hills_(common::mix64(params.seed ^ 0x68696C6CULL)),
      ridges_(common::mix64(params.seed ^ 0x72696467ULL)),
      mask_(common::mix64(params.seed ^ 0x6D61736BULL)),
      warpX_(common::mix64(params.seed ^ 0x77617278ULL)),
      warpZ_(common::mix64(params.seed ^ 0x7761727AULL)) {
    // Each octave samples a different, far-away region so octaves are uncorrelated.
    common::SplitMix64 rng(params.seed);
    for (auto& offset : offsets_) {
        offset = {rng.uniform() * 2.0e4 - 1.0e4, rng.uniform() * 2.0e4 - 1.0e4};
    }
    zoneHeights_.reserve(params.flatZones.size());
    for (const FlatZone& zone : params.flatZones) {
        zoneHeights_.push_back(zone.heightM ? *zone.heightM
                                            : natural(zone.x, zone.z, kProceduralDetailLimitM));
    }
    const double relief = params.amplitudeM * (0.5 + 2.0 * params.mountains);
    snowLine_ = params.snowLineM ? *params.snowLineM : params.baseHeightM + 0.65 * relief + 400.0;
}

double ProceduralSource::natural(double x, double z, double minWavelength) const {
    const ProceduralParams& p = params_;
    double qx = x / p.featureSizeM;
    double qz = z / p.featureSizeM;

    if (p.warp > 0.0) {  // Domain warping (FR-TER-P03, S): displace by low-frequency noise.
        const double wx = 0.6 * warpX_.value(qx * 0.7 + 17.3, qz * 0.7 - 4.1) +
                          0.3 * warpX_.value(qx * 1.6 - 9.2, qz * 1.6 + 2.7);
        const double wz = 0.6 * warpZ_.value(qx * 0.7 - 3.3, qz * 0.7 + 8.8) +
                          0.3 * warpZ_.value(qx * 1.6 + 5.4, qz * 1.6 - 1.9);
        qx += p.warp * 0.8 * wx;
        qz += p.warp * 0.8 * wz;
    }

    // fBm hills; the accumulated slope damps finer octaves (erosion approximation, after
    // I. Quilez's "derivative fBm"): valleys and slopes become smoother than ridges.
    double hills = 0.0;
    double norm = 0.0;
    double amplitude = 1.0;
    double frequency = 1.0;
    double gx = 0.0;
    double gz = 0.0;
    const double erosionGain = 2.0 * p.erosion;
    for (int k = 0; k < p.octaves; ++k) {
        if (p.featureSizeM / frequency < minWavelength) {
            break;
        }
        const auto& off = offsets_[static_cast<std::size_t>(k)];
        const NoiseSample s = hills_.sample(qx * frequency + off[0], qz * frequency + off[1]);
        gx += amplitude * frequency * s.dx;
        gz += amplitude * frequency * s.dy;
        hills += amplitude * s.value / (1.0 + erosionGain * (gx * gx + gz * gz));
        norm += amplitude;
        amplitude *= p.persistence;
        frequency *= p.lacunarity;
    }
    hills = norm > 0.0 ? hills / norm : 0.0;

    double mountains = 0.0;
    if (p.mountains > 0.0) {
        // Ridged multifractal (Musgrave), clustered by a low-frequency mask.
        const double maskValue =
            smoothstep(-0.15, 0.55, mask_.value(qx * 0.35 + 31.7, qz * 0.35 - 12.9));
        if (maskValue > 0.0) {
            double sum = 0.0;
            double ridgeNorm = 0.0;
            double weight = 1.0;
            amplitude = 1.0;
            frequency = 1.0;
            for (int k = 0; k < p.octaves; ++k) {
                if (p.featureSizeM / frequency < minWavelength) {
                    break;
                }
                const auto& off = offsets_[static_cast<std::size_t>(k)];
                double n =
                    1.0 - std::abs(ridges_.value(qx * frequency - off[1], qz * frequency + off[0]));
                n *= n;
                n *= weight;
                weight = std::clamp(n * 2.0, 0.0, 1.0);
                sum += n * amplitude;
                ridgeNorm += amplitude;
                amplitude *= p.persistence;
                frequency *= p.lacunarity;
            }
            mountains = ridgeNorm > 0.0 ? maskValue * sum / ridgeNorm : 0.0;
        }
    }
    return p.baseHeightM + p.amplitudeM * (0.5 * hills + 2.0 * p.mountains * mountains);
}

double ProceduralSource::shaped(double x, double z, double minWavelength) const {
    if (extent_) {
        x = std::clamp(x, -0.5 * extent_->widthM, 0.5 * extent_->widthM);
        z = std::clamp(z, -0.5 * extent_->lengthM, 0.5 * extent_->lengthM);
    }
    double h = natural(x, z, minWavelength);

    for (std::size_t i = 0; i < params_.flatZones.size(); ++i) {
        const FlatZone& zone = params_.flatZones[i];
        const double d = std::hypot(x - zone.x, z - zone.z);
        const double w = zone.blendM > 0.0 ? smoothstep(zone.radiusM + zone.blendM, zone.radiusM, d)
                                           : (d <= zone.radiusM ? 1.0 : 0.0);
        if (w > 0.0) {
            h = lerp(h, zoneHeights_[i], w);
        }
    }

    if (extent_ && params_.edge != EdgeMode::Cliff) {
        const double margin = std::min(1000.0, 0.1 * std::min(extent_->widthM, extent_->lengthM));
        const double inside =
            std::min(0.5 * extent_->widthM - std::abs(x), 0.5 * extent_->lengthM - std::abs(z));
        const double t = smoothstep(0.0, margin, inside);
        if (t < 1.0) {
            const double target = params_.edge == EdgeMode::Fade
                                      ? params_.baseHeightM
                                      : params_.waterLevelM.value_or(params_.baseHeightM) -
                                            std::max(10.0, 0.05 * params_.amplitudeM);
            h = lerp(target, h, t);
        }
    }
    return h;
}

double ProceduralSource::height(double x, double z) const {
    return shaped(x, z, kProceduralDetailLimitM);
}

void ProceduralSource::sampleGrid(double x0, double z0, double step, int cols, int rows,
                                  std::span<double> out) const {
    const double minWavelength = std::max(kProceduralDetailLimitM, 2.0 * step);
    for (int row = 0; row < rows; ++row) {
        const double z = z0 + row * step;
        for (int col = 0; col < cols; ++col) {
            out[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) +
                static_cast<std::size_t>(col)] = shaped(x0 + col * step, z, minWavelength);
        }
    }
}

std::string ProceduralSource::describe() const {
    return std::format("procedural (seed {}, {} octaves, feature {} m, amplitude {} m)",
                       params_.seed, params_.octaves, params_.featureSizeM, params_.amplitudeM);
}

}  // namespace ds::terrain
