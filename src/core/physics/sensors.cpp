// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/sensors.hpp"

#include <algorithm>
#include <cmath>

#include "ds/common/math.hpp"
#include "ds/physics/atmosphere.hpp"

namespace ds::physics {
namespace {

constexpr double kGpsErrorTimeConstantS = 60.0;
constexpr double kBaroGroundEffectM = 0.4;  // Prop wash raises the static pressure near ground.

Vec3 gaussian(common::GaussianRandom& r, double sigma) {
    const double x = r.next(sigma);
    const double y = r.next(sigma);
    const double z = r.next(sigma);
    return {x, y, z};
}

}  // namespace

SensorSuite::SensorSuite(const SensorParams& params, std::uint64_t seed)
    : params_(params), seed_(seed) {
    reset();
}

void SensorSuite::reset() {
    random_ = common::GaussianRandom(common::mix64(seed_ ^ 0x53454E53ULL));  // "SENS"
    gyroBias_ = gaussian(random_, common::degToRad(params_.gyroBiasDps));
    accelBias_ = gaussian(random_, params_.accelBiasMs2);
    gpsError_ =
        Vec3(random_.next(params_.gpsHorizontalNoiseM), random_.next(params_.gpsVerticalNoiseM),
             random_.next(params_.gpsHorizontalNoiseM));
    vibrationPhase_ = 0.0;
    sinceOn_ = 0.0;
    baroTimer_ = 0.0;
    gpsTimer_ = 0.0;
    satelliteDrift_ = random_.uniform() * 4.0;
    readings_ = {};
}

const SensorReadings& SensorSuite::update(const SensorTruth& t, double dt) {
    sinceOn_ += dt;

    // IMU.
    const double walk = common::degToRad(params_.gyroBiasWalkDps) * std::sqrt(dt);
    gyroBias_ += gaussian(random_, walk);
    // Motor vibration, averaged over the tick like an IMU integrating between samples: a plain
    // sample of a sinusoid near a multiple of the tick rate would alias into a false offset.
    const double phase0 = vibrationPhase_;
    const double deltaPhase = common::kTwoPi * t.motorFrequencyHz * dt;
    vibrationPhase_ = std::fmod(vibrationPhase_ + deltaPhase, common::kTwoPi);
    const auto meanSin = [&](double offset) {
        if (deltaPhase < 1e-6) {
            return std::sin(phase0 + offset);
        }
        return (std::cos(phase0 + offset) - std::cos(phase0 + deltaPhase + offset)) / deltaPhase;
    };
    const double vib = params_.vibrationMs2 * t.motorSpeedFraction * t.motorSpeedFraction;
    const Vec3 vibration(0.3 * vib * meanSin(1.1), 0.3 * vib * meanSin(2.3), vib * meanSin(0.0));
    readings_.imu.gyro = t.body.angularVelocity + gyroBias_ +
                         gaussian(random_, common::degToRad(params_.gyroNoiseDps)) +
                         0.01 * vibration;
    readings_.imu.accel =
        t.specificForceBody + accelBias_ + gaussian(random_, params_.accelNoiseMs2) + vibration;

    // Barometer (sample and hold).
    baroTimer_ += dt;
    readings_.baro.fresh = false;
    if (baroTimer_ >= 1.0 / params_.baroRateHz) {
        baroTimer_ = std::fmod(baroTimer_, 1.0 / params_.baroRateHz);
        const double groundEffect =
            t.motorSpeedFraction > 0.05
                ? -kBaroGroundEffectM * std::exp(-std::max(t.heightAboveGroundM, 0.0) / 0.5)
                : 0.0;
        readings_.baro.altitudeM = pressureAltitudeM(t.staticPressurePa, t.qnhHpa) +
                                   random_.next(params_.baroNoiseM) + groundEffect +
                                   faults_.baroOffsetM;
        readings_.baro.fresh = true;
    }

    // Compass.
    const Vec3 forward = worldToNed(t.body.orientation * Vec3(1.0, 0.0, 0.0));
    readings_.mag.headingRad = common::wrapRadiansPi(
        std::atan2(forward.y, forward.x) + random_.next(common::degToRad(params_.magNoiseDeg)) +
        faults_.compassOffsetRad);

    // GPS: slowly varying position error (Gauss–Markov), white velocity noise.
    const double a = std::exp(-dt / kGpsErrorTimeConstantS);
    const double b = std::sqrt(1.0 - a * a);
    gpsError_.x = a * gpsError_.x + b * random_.next(params_.gpsHorizontalNoiseM);
    gpsError_.y = a * gpsError_.y + b * random_.next(params_.gpsVerticalNoiseM);
    gpsError_.z = a * gpsError_.z + b * random_.next(params_.gpsHorizontalNoiseM);
    satelliteDrift_ = std::clamp(satelliteDrift_ + random_.next(0.02 * std::sqrt(dt)), 0.0, 6.0);
    gpsTimer_ += dt;
    readings_.gps.fresh = false;
    if (gpsTimer_ >= 1.0 / params_.gpsRateHz) {
        gpsTimer_ = std::fmod(gpsTimer_, 1.0 / params_.gpsRateHz);
        const bool fix = sinceOn_ >= params_.gpsAcquisitionS && !faults_.gpsLost;
        readings_.gps.fix = fix;
        // Satellites appear during acquisition; then 10–16 in view (none while lost).
        const double progress =
            faults_.gpsLost
                ? 0.0
                : std::clamp(sinceOn_ / std::max(params_.gpsAcquisitionS, 1e-3), 0.0, 1.0);
        readings_.gps.satellites =
            static_cast<int>(std::lround(progress * (10.0 + satelliteDrift_)));
        readings_.gps.hdop =
            readings_.gps.satellites > 3 ? 0.5 + 6.0 / readings_.gps.satellites : 99.9;
        if (fix) {
            readings_.gps.position = t.body.position + gpsError_ + faults_.gpsOffset;
            readings_.gps.velocity =
                t.body.velocity + gaussian(random_, params_.gpsVelocityNoiseMs);
        }
        readings_.gps.fresh = true;
    }
    return readings_;
}

}  // namespace ds::physics
