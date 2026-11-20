// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/frames.hpp"
#include "ds/physics/powertrain.hpp"
#include "ds/physics/profile_params.hpp"
#include "ds/physics/surface.hpp"

/// @file
/// @brief Ground drone (RC-style rover) profile: chassis, wheels, suspension, tyres,
/// drivetrain, battery, sensors, drive controller and limits (FR-GND-12), its JSON form
/// (`assets/vehicles/ground/*.json` and the `vehicle_profiles.params_json` column) and derived
/// performance figures (FR-GND-13).
/// @ingroup physics

namespace ds::physics {

/// @brief Drive configuration (FR-GND-01). Ackermann crawlers and tracked rovers come later.
enum class DriveLayout {
    SkidSteer4,  ///< Four wheels, one motor each, skid steering.
    SkidSteer6,  ///< Six wheels, one motor each, skid steering.
};

/// @brief "skid-steer-4wd" or "skid-steer-6wd".
[[nodiscard]] std::string_view driveLayoutName(DriveLayout layout) noexcept;

/// @brief Chassis (sprung mass) geometry and aerodynamics.
struct ChassisParams {
    double massKg = 3.0;               ///< All-up mass (chassis, wheels, motors, battery).
    double cogHeightM = 0.11;          ///< Centre of mass above level ground, at rest.
    double lengthM = 0.40;             ///< Body length (collision box).
    double widthM = 0.24;              ///< Body width (collision box).
    double heightM = 0.13;             ///< Body height (collision box).
    double clearanceM = 0.05;          ///< Body underside above level ground, at rest.
    double inertiaXxKgM2 = 0.025;      ///< Roll inertia.
    double inertiaYyKgM2 = 0.046;      ///< Pitch inertia.
    double inertiaZzKgM2 = 0.06;       ///< Yaw inertia.
    double dragAreaForwardM2 = 0.04;   ///< C_d·A along body x.
    double dragAreaSideM2 = 0.06;      ///< C_d·A along body y.
    double dragAreaVerticalM2 = 0.11;  ///< C_d·A along body z.
    double crashSpeedMs = 4.0;         ///< Impact speed against an obstacle that breaks it.
};

/// @brief Wheel geometry.
struct WheelParams {
    double wheelbaseM = 0.26;     ///< Front axle to rear axle.
    double trackWidthM = 0.30;    ///< Left to right wheel centres.
    double radiusM = 0.065;       ///< Tyre radius.
    double widthM = 0.05;         ///< Tyre width.
    double inertiaKgM2 = 0.0002;  ///< Wheel and tyre about the axle.
};

/// @brief Per-wheel spring–damper (FR-GND-03).
struct SuspensionParams {
    double stiffnessNPerM = 600.0;  ///< Spring rate at the wheel.
    double dampingNsPerM = 25.0;    ///< Damper rate at the wheel.
    double travelM = 0.03;          ///< Travel from full droop to the bump stop.
};

/// @brief Tyre model (simplified Magic Formula on the combined slip, FR-GND-04).
/// @details `F = μ·N·sin(C·atan(B·s))`, with μ the surface's peak friction times
/// @ref gripScale and s the combined slip.
struct TyreParams {
    double gripScale = 1.0;               ///< Compound grip relative to the reference tyre.
    double stiffnessB = 12.0;             ///< Magic Formula stiffness factor B.
    double shapeC = 1.35;                 ///< Magic Formula shape factor C.
    double rollingResistanceScale = 1.0;  ///< Scales the surface's rolling resistance.
};

/// @brief Brushed DC gear motor driving each wheel (FR-GND-05).
struct DrivetrainParams {
    double motorKv = 850.0;             ///< Motor speed constant, rpm/V.
    double motorResistanceOhm = 2.4;    ///< Winding resistance.
    double motorNoLoadCurrentA = 0.25;  ///< No-load current (friction of motor and gears).
    double motorInertiaKgM2 = 1.5e-6;   ///< Armature inertia.
    double gearRatio = 30.0;            ///< Motor turns per wheel turn.
    double gearEfficiency = 0.7;        ///< Gearbox efficiency.
    double maxCurrentA = 6.0;           ///< ESC current limit per motor.
    double brakeTorqueNm = 0.8;         ///< Holding brake torque at each wheel.
};

/// @brief Drive controller (FR-GND-11).
struct DriveControllerParams {
    double steeringGain = 0.8;    ///< Side difference at full steering (arcade mix).
    double throttleExpo = 0.2;    ///< 0 … 1: softer response around neutral.
    double cruiseSpeedMs = 1.8;   ///< Speed control: speed at full stick.
    double speedP = 0.6;          ///< Speed control: motor command per m/s of error.
    double speedI = 1.2;          ///< Speed control: integral gain, 1/s per m/s.
    double failsafeDelayS = 0.5;  ///< RC loss → stop.
};

/// @brief Operating limits.
struct GroundLimits {
    double wadingDepthM = 0.05;  ///< Deepest water it can drive through (FR-GND-09).
    double stuckDetectS = 5.0;   ///< No progress under power for this long ⇒ stuck (FR-GND-08).
};

/// @brief Wheel encoders (odometry, FR-GND-10).
struct EncoderParams {
    int countsPerRev = 360;  ///< Encoder counts per wheel revolution.
};

/// @brief Complete ground drone profile.
struct GroundProfile {
    std::string key;                                         ///< Stable key, e.g. "rover-4wd-3kg".
    std::string name;                                        ///< Display name.
    std::string description;                                 ///< Short description.
    DriveLayout layout = DriveLayout::SkidSteer4;            ///< Drive configuration.
    ChassisParams chassis;                                   ///< Chassis.
    WheelParams wheels;                                      ///< Wheels.
    SuspensionParams suspension;                             ///< Suspension.
    TyreParams tyre;                                         ///< Tyres.
    DrivetrainParams drivetrain;                             ///< Motors and gearboxes.
    BatteryParams battery;                                   ///< Battery pack.
    ElectricalParams electrical;                             ///< Other electrical loads.
    SensorParams sensors;                                    ///< IMU, barometer, GPS, compass.
    EncoderParams encoders;                                  ///< Wheel encoders.
    DriveControllerParams controller;                        ///< Drive controller.
    GroundLimits limits;                                     ///< Wading depth, stuck detection.
    CameraParams camera{.uptiltDeg = 5.0, .fovDeg = 110.0};  ///< FPV camera.
    /// Model file drawn instead of the procedural body (FR-GFX-12): `.gltf`, `.glb` or `.obj`,
    /// a name in the model folders or an absolute path; fitted to the body. Empty = procedural.
    std::string modelFile;

    /// @brief Number of wheels of the layout.
    [[nodiscard]] int wheelCount() const noexcept;
    /// @brief Hub positions at rest in the body frame (x forward, y right, z down), relative
    /// to the centre of mass; left side first, front to rear.
    [[nodiscard]] std::vector<Vec3> wheelHubsAtRest() const;
    /// @brief True for wheels on the right side (index into wheelHubsAtRest()).
    [[nodiscard]] bool isRightWheel(int index) const noexcept;
    /// @brief Static suspension compression under the vehicle's own weight, m.
    [[nodiscard]] double staticCompressionM() const noexcept;
};

/// @brief Parses a profile; error messages name the JSON path of every bad value.
[[nodiscard]] common::Result<GroundProfile> groundProfileFromJson(const common::Json& json);
/// @brief Reads a profile file.
[[nodiscard]] common::Result<GroundProfile> loadGroundProfile(const std::filesystem::path& file);
/// @brief Serialises a profile (round-trips through groundProfileFromJson()).
[[nodiscard]] common::Json groundProfileToJson(const GroundProfile& profile);

/// @brief An editable numeric ground profile parameter (Vehicle tab).
using GroundParamDescriptor = BasicParamDescriptor<GroundProfile>;

/// @brief All editable numeric parameters, in display order. The JSON reader uses the same
/// ranges, so the editor can never produce a profile the loader rejects.
[[nodiscard]] std::span<const GroundParamDescriptor> groundParameters();

/// @brief Climbing limit on one surface material.
struct SlopeLimit {
    SurfaceMaterial material = SurfaceMaterial::Grass;  ///< Surface.
    double maxSlopeDeg = 0.0;                           ///< Steepest slope it can climb.
    bool tractionLimited = true;  ///< Wheels slip (true) or the motors stall (false) first.
};

/// @brief Performance figures derived from a profile with the model's own equations, on level
/// ground in still ISA sea-level air (Vehicle tab, FR-GND-13).
struct GroundStats {
    double topSpeedMs = 0.0;         ///< On paved ground, full battery.
    double accelerationTimeS = 0.0;  ///< 0 → 90 % of the top speed on paved ground.
    double stallTorqueNm = 0.0;      ///< Torque at each wheel at stall (current limit).
    std::vector<SlopeLimit> slopes;  ///< Climbing limit per material.
    double rolloverSideDeg = 0.0;    ///< Static rollover angle, sideways.
    double rolloverPitchDeg = 0.0;   ///< Static tip-over angle, front/back.
    double cruiseSpeedMs = 0.0;      ///< Speed of the range estimate (70 % of the top speed).
    double cruiseCurrentA = 0.0;     ///< Battery current at that speed on grass.
    double rangeKm = 0.0;            ///< Estimated range at that speed on grass.
    double enduranceMin = 0.0;       ///< Driving time at that speed on grass.
};

/// @brief Computes GroundStats for @p profile.
[[nodiscard]] GroundStats computeGroundStats(const GroundProfile& profile);

}  // namespace ds::physics
