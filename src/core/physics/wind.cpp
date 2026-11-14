// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/wind.hpp"

#include <algorithm>
#include <cmath>

#include "ds/common/math.hpp"

namespace ds::physics {
namespace {

constexpr double kFeetPerMetre = 3.280839895;
constexpr double kMaxProfileHeightM = 300.0;
constexpr double kWanderTimeConstantS = 60.0;
constexpr double kMinGustDurationS = 2.0;
constexpr double kMaxGustDurationS = 6.0;
constexpr double kMinRelativeAirspeed = 0.5;

// Unit vector (world) of the direction the air moves to, for wind blowing from @p fromRad.
Vec3 downwind(double fromRad) noexcept {
    // From north (0) → air moves south (+z); from east (π/2) → air moves west (−x).
    return {-std::sin(fromRad), 0.0, std::cos(fromRad)};
}

}  // namespace

WindParameters WindParameters::fromLevel(const weather::WindLevelTable& table,
                                         const weather::WindSettings& settings,
                                         std::uint64_t seed) {
    const weather::WindLevel& level = table.level(settings.level);
    WindParameters p;
    p.meanSpeed10mMs = level.meanSpeedMs;
    p.gustPeakMs = level.gustPeakMs;
    p.gustIntervalS = level.gustIntervalS;
    p.turbulenceScale = level.turbulenceScale;
    p.directionSwingDeg = level.directionSwingDeg;
    p.fromDeg = settings.fromDeg;
    if (settings.randomDirection) {
        common::SplitMix64 rng(common::mix64(seed ^ 0x57494E44ULL));  // "WIND"
        p.fromDeg = std::floor(rng.uniform() * 360.0);
    }
    return p;
}

double logProfileSpeed(double speed10m, double heightAglM, double roughnessM) noexcept {
    const double z0 = std::max(roughnessM, 1e-4);
    const double h = std::min(heightAglM, kMaxProfileHeightM);
    if (h <= z0 || speed10m <= 0.0) {
        return 0.0;
    }
    return speed10m * std::log(h / z0) / std::log(10.0 / z0);
}

WindModel::WindModel(const WindParameters& params, std::uint64_t seed)
    : params_(params), random_(common::mix64(seed ^ 0x44525944454EULL)) {
    // Gusts only add speed; lower the base so that the long-term mean stays at the table value.
    if (params_.meanSpeed10mMs > 0.0 && params_.gustIntervalS > 0.0) {
        const double meanAmplitude = 0.75 * (params_.gustPeakMs - params_.meanSpeed10mMs);
        const double meanDuration = 0.5 * (kMinGustDurationS + kMaxGustDurationS);
        const double average = 0.5 * meanAmplitude * meanDuration / params_.gustIntervalS;
        meanCompensation_ = std::max(0.5, 1.0 - average / params_.meanSpeed10mMs);
    }
    scheduleGust();
    update(0.0, 10.0, Vec3(0.0));
}

void WindModel::scheduleGust() {
    if (params_.gustIntervalS <= 0.0 || params_.gustPeakMs <= params_.meanSpeed10mMs) {
        timeToGust_ = 1e300;
        return;
    }
    // Poisson process: exponential waiting time.
    timeToGust_ = -params_.gustIntervalS * std::log(1.0 - random_.uniform());
    gustDuration_ = kMinGustDurationS + (kMaxGustDurationS - kMinGustDurationS) * random_.uniform();
    gustAmplitude_ =
        (params_.gustPeakMs - params_.meanSpeed10mMs) * (0.5 + 0.5 * random_.uniform());
    gustAngle_ = common::degToRad(10.0) * (2.0 * random_.uniform() - 1.0);
    gustElapsed_ = gustDuration_;  // Idle until timeToGust_ runs out.
}

double WindModel::currentFromDeg() const noexcept {
    return common::normalizeDegrees(params_.fromDeg + common::radToDeg(wander_));
}

void WindModel::update(double dt, double heightAglM, const Vec3& vehicleVelocity) {
    if (params_.meanSpeed10mMs <= 0.0) {
        mean_ = velocity_ = Vec3(0.0);
        return;
    }
    const double h = std::max(heightAglM, 0.0);
    const double profile = logProfileSpeed(1.0, h, params_.roughnessM);

    // Direction wander: Gauss–Markov with σ = swing/2, clamped to the swing.
    const double swing = common::degToRad(params_.directionSwingDeg);
    if (dt > 0.0 && swing > 0.0) {
        const double a = std::exp(-dt / kWanderTimeConstantS);
        wander_ = std::clamp(a * wander_ + 0.5 * swing * std::sqrt(1.0 - a * a) * random_.next(),
                             -swing, swing);
    }
    const double fromRad = common::degToRad(params_.fromDeg) + wander_;
    const Vec3 along = downwind(fromRad);
    const Vec3 across(-along.z, 0.0, along.x);  // 90° to the right of the flow.
    const double meanSpeed = params_.meanSpeed10mMs * meanCompensation_ * profile;
    mean_ = along * meanSpeed;

    // Discrete gusts ("1 − cos" shape), scaled by the height profile.
    double gust = 0.0;
    if (dt > 0.0) {
        if (gustElapsed_ < gustDuration_) {
            gustElapsed_ += dt;
            if (gustElapsed_ >= gustDuration_) {
                scheduleGust();
            }
        } else {
            timeToGust_ -= dt;
            if (timeToGust_ <= 0.0) {
                gustElapsed_ = 0.0;
            }
        }
    }
    if (gustElapsed_ < gustDuration_) {
        gust = 0.5 * gustAmplitude_ *
               (1.0 - std::cos(common::kTwoPi * gustElapsed_ / gustDuration_)) * profile;
    }
    const Vec3 gustDir = downwind(fromRad + gustAngle_);

    // Dryden turbulence (MIL-F-8785C, low altitude; formulas in feet).
    Vec3 turbulence(0.0);
    if (params_.turbulenceScale > 0.0 && dt > 0.0) {
        const double hFt = std::clamp(h, 3.0, 300.0) * kFeetPerMetre;
        const double w20 = params_.meanSpeed10mMs * logProfileSpeed(1.0, 6.096, params_.roughnessM);
        const double sigmaW = 0.1 * w20 * params_.turbulenceScale;
        const double sigmaUV = sigmaW / std::pow(0.177 + 0.000823 * hFt, 0.4);
        const double lW = hFt / kFeetPerMetre;
        const double lUV = hFt / std::pow(0.177 + 0.000823 * hFt, 1.2) / kFeetPerMetre;
        const double airspeed =
            std::max(glm::length(vehicleVelocity - mean_), kMinRelativeAirspeed);
        // Longitudinal: Gauss–Markov with correlation length L_u (exact discretisation).
        const double au = std::exp(-airspeed * dt / lUV);
        u_ = au * u_ + sigmaUV * std::sqrt(1.0 - au * au) * random_.next();
        // Lateral / vertical: H(s) = σ·√(T/S0)·(1 + √3·T·s)/(1 + T·s)², T = L/V, built from two
        // first-order stages driven by unit-PSD white noise (variance 1/dt per sample).
        auto lateral = [&](double& x1, double& x2, double sigma, double length) {
            const double t = length / airspeed;
            const double e = std::exp(-dt / t);
            const double noise = random_.next() / std::sqrt(dt);
            x1 = e * x1 + (1.0 - e) * noise;
            x2 = e * x2 + (1.0 - e) * x1;
            constexpr double kSqrt3 = 1.7320508075688772;
            return sigma * std::sqrt(t) * ((1.0 - kSqrt3) * x2 + kSqrt3 * x1);
        };
        const double v = lateral(v1_, v2_, sigmaUV, lUV);
        const double w = lateral(w1_, w2_, sigmaW, lW);
        // Turbulence fades out near the ground with the mean profile.
        const double fade = std::min(1.0, profile);
        turbulence = (along * u_ + across * v + Vec3(0.0, w, 0.0)) * fade;
    }
    velocity_ = mean_ + gustDir * gust + turbulence;
}

}  // namespace ds::physics
