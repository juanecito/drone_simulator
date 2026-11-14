// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/physics/estimator.hpp"

#include <algorithm>
#include <cmath>

#include "ds/common/math.hpp"

namespace ds::physics {
namespace {

constexpr double kAccelGain = 0.25;  // Attitude correction from gravity, 1/s.
constexpr double kAccelTrustWindow =
    0.05;  // Accelerometer trust falls to 0 at ±0.05 g from the expected.
constexpr double kAccelBiasGain = 0.02;
constexpr double kGyroCalibrationTauS = 0.5;  // Bias learning while stationary.
constexpr double kMagGain = 0.5;              // Heading correction, 1/s.
constexpr double kBaroOmega = 1.5;            // Altitude filter bandwidth, rad/s.
constexpr double kGpsPositionGain = 1.0;
constexpr double kGpsVelocityGain = 2.0;
constexpr double kGpsAccelSmoothing = 0.5;  // Low-pass of the GPS-derived acceleration (per fix).
constexpr double kMaxGpsAccelAgeS = 0.5;    // Older than this: assume no acceleration.

}  // namespace

void StateEstimator::reset(double headingRad, double altitudeM) {
    state_ = {};
    state_.attitude = orientationFromEuler({0.0, 0.0, headingRad});
    state_.euler = eulerFromOrientation(state_.attitude);
    state_.altitudeM = altitudeM;
    gyroBias_ = Vec3(0.0);
    accelBiasUp_ = 0.0;
    lastBaro_ = altitudeM;
    baroValid_ = false;
    gpsAccel_ = Vec3(0.0);
    lastGpsVelocity_ = Vec3(0.0);
    gpsAge_ = 0.0;
    gpsAccelValid_ = false;
}

const EstimatedState& StateEstimator::update(const SensorReadings& s, double dt, bool stationary) {
    // Gyro calibration at rest: the bias converges to the mean reading.
    if (stationary) {
        gyroBias_ += (1.0 - std::exp(-dt / kGyroCalibrationTauS)) * (s.imu.gyro - gyroBias_);
    }

    // --- Acceleration from the GPS velocity (compensates the accelerometer in turns). ---
    gpsAge_ += dt;
    if (s.gps.fix && s.gps.fresh) {
        if (gpsAccelValid_ && gpsAge_ > 1e-6) {
            const Vec3 a = (s.gps.velocity - lastGpsVelocity_) / gpsAge_;
            gpsAccel_ += kGpsAccelSmoothing * (a - gpsAccel_);
        }
        lastGpsVelocity_ = s.gps.velocity;
        gpsAccelValid_ = true;
        gpsAge_ = 0.0;
    }
    if (!s.gps.fix || gpsAge_ > kMaxGpsAccelAgeS) {
        gpsAccel_ = Vec3(0.0);
        gpsAccelValid_ = s.gps.fix && gpsAccelValid_;
    }

    // --- Attitude: Mahony-style complementary filter. ---
    // The accelerometer measures the specific force a − g; with the vehicle's acceleration a
    // known from GPS, the expected reading points along a + g·up rather than straight up (a
    // sustained bank would otherwise be taken for level flight).
    Vec3 correction(0.0);
    const double accelNorm = glm::length(s.imu.accel);
    const Vec3 expectedWorld = gpsAccel_ + Vec3(0.0, kGravity, 0.0);
    const double expectedNorm = glm::length(expectedWorld);
    if (accelNorm > 1e-3 && expectedNorm > 1e-3) {
        // Trust the accelerometer only when it agrees in magnitude (not during manoeuvres).
        const double trust = std::clamp(
            1.0 - std::abs(accelNorm - expectedNorm) / (kAccelTrustWindow * kGravity), 0.0, 1.0);
        const Vec3 measuredUp = s.imu.accel / accelNorm;
        const Vec3 estimatedUp = worldToBody(state_.attitude, expectedWorld / expectedNorm);
        const Vec3 error = glm::cross(measuredUp, estimatedUp);
        correction += kAccelGain * trust * error;
        gyroBias_ -= kAccelBiasGain * trust * error * dt;
    }
    {
        const Euler e = eulerFromOrientation(state_.attitude);
        const double headingError = common::wrapRadiansPi(s.mag.headingRad - e.yaw);
        const Vec3 downBody = worldToBody(state_.attitude, Vec3(0.0, -1.0, 0.0));
        correction += kMagGain * headingError * downBody;
    }
    state_.rates = s.imu.gyro - gyroBias_;
    const Vec3 w = state_.rates + correction;
    const double angle = glm::length(w) * dt;
    if (angle > 0.0) {
        state_.attitude =
            glm::normalize(state_.attitude * glm::angleAxis(angle, w / glm::length(w)));
    }
    state_.euler = eulerFromOrientation(state_.attitude);

    // --- Vertical: third-order complementary filter (baro + accelerometer). ---
    const Vec3 accelWorld = state_.attitude * s.imu.accel;
    const double up = accelWorld.y - kGravity - accelBiasUp_;
    state_.altitudeM += state_.verticalSpeedMs * dt + 0.5 * up * dt * dt;
    state_.verticalSpeedMs += up * dt;
    if (s.baro.fresh) {
        lastBaro_ = s.baro.altitudeM;
        if (!baroValid_) {
            state_.altitudeM = lastBaro_;
            baroValid_ = true;
        }
    }
    if (baroValid_) {
        const double e = lastBaro_ - state_.altitudeM;
        state_.altitudeM += 3.0 * kBaroOmega * e * dt;
        state_.verticalSpeedMs += 3.0 * kBaroOmega * kBaroOmega * e * dt;
        accelBiasUp_ -= kBaroOmega * kBaroOmega * kBaroOmega * e * dt;
    }

    // --- Horizontal: GPS position/velocity with accelerometer prediction. ---
    state_.positionValid = s.gps.fix;
    Vec3 horizontalAccel(accelWorld.x, 0.0, accelWorld.z);
    state_.velocity += horizontalAccel * dt;
    state_.position += state_.velocity * dt;
    if (s.gps.fix && s.gps.fresh) {
        if (glm::length(state_.position - s.gps.position) > 50.0) {
            state_.position = s.gps.position;  // First fix: jump.
            state_.velocity = s.gps.velocity;
        }
        const double period = 0.1;
        const double kp = std::min(1.0, kGpsPositionGain * period);
        const double kv = std::min(1.0, kGpsVelocityGain * period);
        state_.position += kp * (s.gps.position - state_.position);
        state_.velocity += kv * (s.gps.velocity - state_.velocity);
    }
    state_.velocity.y = state_.verticalSpeedMs;  // Vertical speed from the baro filter.
    return state_;
}

}  // namespace ds::physics
