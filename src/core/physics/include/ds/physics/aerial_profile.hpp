// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/physics/frames.hpp"
#include "ds/physics/powertrain.hpp"

/// @file
/// @brief Aerial drone profile: every physical, motor, propeller, battery, sensor and
/// controller parameter (FR-PHY-16), its JSON form (`assets/vehicles/aerial/*.json` and the
/// `vehicle_profiles.params_json` column) and derived performance figures.
/// @ingroup physics

namespace ds::physics {

/// @brief Frame geometry and aerodynamics.
struct FrameParams {
    double massKg = 0.65;              ///< All-up weight (frame, motors, battery, camera).
    double armLengthM = 0.11;          ///< Distance from the centre to each motor axis.
    double inertiaXxKgM2 = 0.0016;     ///< Roll inertia.
    double inertiaYyKgM2 = 0.0018;     ///< Pitch inertia.
    double inertiaZzKgM2 = 0.0030;     ///< Yaw inertia.
    double dragAreaForwardM2 = 0.006;  ///< C_d·A along body x.
    double dragAreaSideM2 = 0.007;     ///< C_d·A along body y.
    double dragAreaVerticalM2 = 0.02;  ///< C_d·A along body z.
    double propHeightM = 0.02;         ///< Propeller plane above the centre of mass.
    double gearHeightM = 0.03;         ///< Landing feet below the centre of mass.
    double crashSpeedMs = 6.0;         ///< Impact speed (normal to the ground) that breaks it.
};

/// @brief Electrical parameters besides motors and battery.
struct ElectricalParams {
    double avionicsCurrentA = 0.5;  ///< Flight controller, receiver, video, camera.
    double escEfficiency = 0.95;    ///< Battery power → motor power.
};

/// @brief Sensor noise parameters (FR-PHY-10). Noise values are 1σ per sample.
struct SensorParams {
    double gyroNoiseDps = 0.1;         ///< Gyro white noise, °/s.
    double gyroBiasDps = 0.5;          ///< Turn-on gyro bias (1σ), °/s.
    double gyroBiasWalkDps = 0.005;    ///< Gyro bias random walk, °/s per √s.
    double accelNoiseMs2 = 0.05;       ///< Accelerometer white noise, m/s².
    double accelBiasMs2 = 0.05;        ///< Turn-on accelerometer bias (1σ), m/s².
    double vibrationMs2 = 2.0;         ///< Vibration amplitude at full motor speed, m/s².
    double baroNoiseM = 0.3;           ///< Barometric altitude noise, m.
    double baroRateHz = 50.0;          ///< Barometer output rate.
    double gpsRateHz = 10.0;           ///< GPS output rate.
    double gpsHorizontalNoiseM = 1.2;  ///< Horizontal position error (1σ, slowly varying).
    double gpsVerticalNoiseM = 2.5;    ///< Vertical position error (1σ, slowly varying).
    double gpsVelocityNoiseMs = 0.08;  ///< Velocity noise.
    double gpsAcquisitionS = 25.0;     ///< Time to the first fix after power-on.
    double magNoiseDeg = 1.0;          ///< Heading noise.
};

/// @brief Rate curve of one stick axis (FR-PHY-12).
/// @details `rate(x) = c·x + (m − c)·x·|x|^(1 + 4·expo)` for a stick deflection x in [−1, 1]:
/// @c c is the sensitivity around centre ("RC rate"), @c m the rate at full deflection
/// ("super rate"), @c expo how late the rate builds up towards the end of travel.
struct RateCurve {
    double centerDps = 200.0;  ///< Rate per unit deflection around the centre, °/s.
    double maxDps = 670.0;     ///< Rate at full deflection, °/s.
    double expo = 0.5;         ///< 0 … 1.
};

/// @brief PID gains of one rate axis (output: normalised torque command per rad/s).
struct PidGains {
    double p = 0.02;    ///< Proportional.
    double i = 0.06;    ///< Integral.
    double d = 0.0004;  ///< Derivative (on measurement).
};

/// @brief Flight controller parameters (FR-PHY-12/13/14).
struct ControllerParams {
    std::array<RateCurve, 3> rates{};  ///< Roll, pitch, yaw.
    std::array<PidGains, 3> pid{};     ///< Roll, pitch, yaw.
    double dTermCutoffHz = 100.0;      ///< D-term low-pass filter.
    double maxAngleDeg = 55.0;         ///< Angle mode tilt limit.
    double levelGain = 6.0;            ///< Angle mode: rate per angle error, 1/s.
    double maxClimbMs = 3.0;           ///< Altitude hold: climb rate at full stick.
    double maxDescentMs = 2.0;         ///< Altitude hold: descent rate at zero stick.
    double altitudeGain = 1.2;         ///< Altitude hold: climb rate per metre of error, 1/s.
    double climbP = 0.10;              ///< Altitude hold: throttle per m/s of climb error.
    double climbI = 0.08;              ///< Altitude hold: throttle per m of climb error.
    double idleThrottle = 0.05;        ///< Motor command when armed at zero throttle.
    bool airmode = true;               ///< Keep full attitude authority at zero throttle.
    double failsafeDescentMs = 1.5;    ///< Landing speed after RC loss.
    double failsafeDelayS = 1.0;       ///< RC loss → landing.
};

/// @brief FPV camera mounted on the drone (FR-GFX-05).
struct CameraParams {
    double uptiltDeg = 30.0;  ///< Camera tilt above the body x axis.
    double fovDeg = 120.0;    ///< Horizontal field of view.
};

/// @brief Published-style reference figures the model must reproduce within ±10 %
/// (NFR-REAL-01; derivation in docs/FLIGHT_MODEL.md).
struct ReferenceFigures {
    double hoverThrottle = 0.0;      ///< Motor command in hover (0–1).
    double thrustToWeight = 0.0;     ///< Static thrust at full command / weight.
    double hoverEnduranceMin = 0.0;  ///< Hover time on a full pack.
    double maxLevelSpeedMs = 0.0;    ///< Top speed in level flight.
    std::string source;              ///< Where the figures come from.
};

/// @brief Complete aerial drone profile (X-quadcopter).
struct AerialProfile {
    std::string key;                            ///< Stable key, e.g. "camera-250g".
    std::string name;                           ///< Display name.
    std::string description;                    ///< Short description.
    FrameParams frame;                          ///< Frame.
    MotorParams motor;                          ///< Each of the four motors.
    PropellerParams propeller;                  ///< Each of the four propellers.
    BatteryParams battery;                      ///< Battery pack.
    ElectricalParams electrical;                ///< Other electrical loads.
    SensorParams sensors;                       ///< Sensors.
    ControllerParams controller;                ///< Flight controller.
    CameraParams camera;                        ///< FPV camera.
    std::optional<ReferenceFigures> reference;  ///< Built-in profiles only.

    /// @brief Number of motors.
    static constexpr int kMotorCount = 4;
    /// @brief Motor positions in the body frame (x forward, y right; z = −propHeight).
    [[nodiscard]] std::array<Vec3, kMotorCount> motorPositions() const noexcept;
    /// @brief Spin direction seen from above: +1 clockwise, −1 counter-clockwise.
    [[nodiscard]] static constexpr std::array<double, kMotorCount> spinDirections() noexcept {
        // Front-right, rear-right, rear-left, front-left; diagonals turn the same way.
        return {-1.0, 1.0, -1.0, 1.0};
    }
};

/// @brief Parses a profile; error messages name the JSON path of every bad value.
[[nodiscard]] common::Result<AerialProfile> aerialProfileFromJson(const common::Json& json);
/// @brief Reads a profile file.
[[nodiscard]] common::Result<AerialProfile> loadAerialProfile(const std::filesystem::path& file);
/// @brief Serialises a profile (round-trips through aerialProfileFromJson()).
[[nodiscard]] common::Json aerialProfileToJson(const AerialProfile& profile);

/// @brief Kind of an editable parameter.
enum class ParamKind { Number, Integer };

/// @brief An editable numeric profile parameter (Vehicle tab).
struct ParamDescriptor {
    std::string_view pointer;                         ///< JSON pointer, e.g. "/motor/kv".
    std::string_view group;                           ///< Group shown in the Vehicle tab.
    std::string_view label;                           ///< Label.
    std::string_view unit;                            ///< Unit symbol ("" if none).
    double min;                                       ///< Smallest valid value.
    double max;                                       ///< Largest valid value.
    int decimals;                                     ///< Decimals shown.
    ParamKind kind;                                   ///< Number or integer.
    std::function<double(const AerialProfile&)> get;  ///< Reads the value.
    std::function<void(AerialProfile&, double)> set;  ///< Writes the value.
};

/// @brief All editable numeric parameters, in display order. The JSON reader uses the same
/// ranges, so the editor can never produce a profile the loader rejects.
[[nodiscard]] std::span<const ParamDescriptor> aerialParameters();

/// @brief Performance figures derived from a profile with the model's own equations, in ISA
/// sea-level air without wind (Vehicle tab; FR-WIND-07; NFR-REAL-01/08).
struct AerialStats {
    double hoverThrottle = 0.0;      ///< Motor command in hover.
    double hoverCurrentA = 0.0;      ///< Battery current in hover.
    double hoverPowerW = 0.0;        ///< Battery power in hover.
    double thrustToWeight = 0.0;     ///< Static thrust at full command / weight.
    double maxThrustN = 0.0;         ///< Total static thrust at full command.
    double hoverEnduranceMin = 0.0;  ///< Hover time from full to empty.
    /// Level top speed in Angle / Altitude-hold mode (tilt limit and full command).
    double maxLevelSpeedMs = 0.0;
    /// Strongest steady wind it can hold position in (tilt limit, 85 % command).
    double maxWindMs = 0.0;
    double maxWindTiltDeg = 0.0;  ///< Tilt needed at that wind.
};

/// @brief Steady state holding position in a horizontal wind (NFR-REAL-08).
struct WindHoldState {
    bool feasible = false;  ///< Within the tilt limit and 85 % of the command.
    double tiltDeg = 0.0;   ///< Tilt into the wind.
    double throttle = 0.0;  ///< Motor command.
    double powerW = 0.0;    ///< Battery power.
    double currentA = 0.0;  ///< Battery current.
};

/// @brief Computes AerialStats for @p profile.
[[nodiscard]] AerialStats computeAerialStats(const AerialProfile& profile);

/// @brief Tilt and power needed to hover in place in a steady horizontal wind @p windMs
/// (nose into the wind), at @p soc state of charge, with air density @p rho.
[[nodiscard]] WindHoldState windHoldState(const AerialProfile& profile, double windMs,
                                          double rho = 1.225, double soc = 0.9);

}  // namespace ds::physics
