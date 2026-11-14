// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "ds/physics/sensors.hpp"

/// @file
/// @brief State estimation from the sensors (FR-PHY-11): complementary filter for the attitude
/// (gyro + accelerometer + compass) and baro/accelerometer and GPS fusion for altitude and
/// position. The flight controller only sees this estimate, never the ground truth.
/// @ingroup physics

namespace ds::physics {

/// @brief Estimated state.
struct EstimatedState {
    Quat attitude{1.0, 0.0, 0.0, 0.0};  ///< Body → world rotation.
    Euler euler;                        ///< Attitude as Euler angles.
    Vec3 rates{0.0};                    ///< Bias-corrected body rates, rad/s.
    double altitudeM = 0.0;             ///< Barometric altitude (QNH reference).
    double verticalSpeedMs = 0.0;       ///< Climb rate.
    Vec3 position{0.0};                 ///< World position (GPS fusion).
    Vec3 velocity{0.0};                 ///< World velocity (GPS fusion).
    bool positionValid = false;         ///< GPS has a fix.
};

/// @brief Complementary-filter estimator.
class StateEstimator {
public:
    /// @brief Starts level, heading @p headingRad, at barometric altitude @p altitudeM.
    void reset(double headingRad, double altitudeM);
    /// @brief Fuses one tick of sensor data.
    /// @param sensors Readings of this tick.
    /// @param dt Tick length, seconds.
    /// @param stationary The vehicle sits still (disarmed on the ground): the gyro bias is
    ///        calibrated from the readings, like a flight controller does before arming.
    const EstimatedState& update(const SensorReadings& sensors, double dt, bool stationary = false);
    /// @brief Current estimate.
    [[nodiscard]] const EstimatedState& state() const noexcept { return state_; }

private:
    EstimatedState state_;
    Vec3 gyroBias_{0.0};
    double accelBiasUp_ = 0.0;
    double lastBaro_ = 0.0;
    bool baroValid_ = false;
    Vec3 gpsAccel_{0.0};
    Vec3 lastGpsVelocity_{0.0};
    double gpsAge_ = 0.0;
    bool gpsAccelValid_ = false;
};

}  // namespace ds::physics
