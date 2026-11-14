// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "ds/common/random.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/rigid_body.hpp"

/// @file
/// @brief Simulated sensors (FR-PHY-10): IMU with noise, bias, bias drift and motor
/// vibration; barometer with noise and ground-effect error; GPS with slowly varying error,
/// satellites, HDOP and acquisition time; compass.
/// @ingroup physics

namespace ds::physics {

/// @brief Inertial measurement (body frame).
struct ImuSample {
    Vec3 gyro{0.0};   ///< Angular rate, rad/s.
    Vec3 accel{0.0};  ///< Specific force (acceleration minus gravity), m/s².
};

/// @brief Barometric altitude.
struct BaroSample {
    double altitudeM = 0.0;  ///< Pressure altitude above the QNH reference (≈ AMSL).
    bool fresh = false;      ///< A new sample arrived this tick.
};

/// @brief GPS fix.
struct GpsSample {
    bool fix = false;    ///< 3D fix available.
    int satellites = 0;  ///< Satellites used.
    double hdop = 99.9;  ///< Horizontal dilution of precision.
    Vec3 position{0.0};  ///< World position, m.
    Vec3 velocity{0.0};  ///< World velocity, m/s.
    bool fresh = false;  ///< A new sample arrived this tick.
};

/// @brief Compass heading.
struct MagSample {
    double headingRad = 0.0;  ///< Heading, clockwise from true north.
};

/// @brief All sensor outputs of one tick.
struct SensorReadings {
    ImuSample imu;    ///< IMU.
    BaroSample baro;  ///< Barometer.
    GpsSample gps;    ///< GPS.
    MagSample mag;    ///< Compass.
};

/// @brief Ground truth the sensors measure.
struct SensorTruth {
    RigidBodyState body;                 ///< True state.
    Vec3 specificForceBody{0.0};         ///< Non-gravitational acceleration, body frame.
    double staticPressurePa = 101325.0;  ///< Air pressure at the vehicle.
    double qnhHpa = 1013.25;             ///< Altimeter setting.
    double heightAboveGroundM = 10.0;    ///< For the barometer's ground-effect error.
    double motorSpeedFraction = 0.0;     ///< Mean motor speed / maximum (vibration level).
    double motorFrequencyHz = 0.0;       ///< Mean motor rotation frequency (vibration).
};

/// @brief The vehicle's sensors. Deterministic for a given seed (FR-X-05).
class SensorSuite {
public:
    /// @brief Sensors with @p params; noise from @p seed.
    SensorSuite(const SensorParams& params, std::uint64_t seed);

    /// @brief Power-on: new turn-on biases, GPS starts searching.
    void reset();
    /// @brief Samples the sensors after @p dt seconds.
    const SensorReadings& update(const SensorTruth& truth, double dt);
    /// @brief Last readings.
    [[nodiscard]] const SensorReadings& readings() const noexcept { return readings_; }
    /// @brief Current gyro bias (tests).
    [[nodiscard]] const Vec3& gyroBias() const noexcept { return gyroBias_; }

private:
    SensorParams params_;
    std::uint64_t seed_;
    common::GaussianRandom random_{0};
    SensorReadings readings_;
    Vec3 gyroBias_{0.0};
    Vec3 accelBias_{0.0};
    Vec3 gpsError_{0.0};
    double vibrationPhase_ = 0.0;
    double sinceOn_ = 0.0;
    double baroTimer_ = 0.0;
    double gpsTimer_ = 0.0;
    double satelliteDrift_ = 0.0;
};

}  // namespace ds::physics
