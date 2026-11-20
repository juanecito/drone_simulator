// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ds/physics/drive_controller.hpp"
#include "ds/physics/ground_profile.hpp"
#include "ds/physics/sensors.hpp"
#include "ds/physics/vehicle.hpp"

/// @file
/// @brief The ground drone (RC-style skid-steer rover, §4.2.2): sprung chassis on per-wheel
/// spring–dampers, Magic Formula tyres on the terrain material, one DC gear motor per wheel
/// drawing from the shared battery model, sensors with wheel encoders, the drive controller,
/// and rollover, stuck and water detection.
/// @details The chassis is a 6-DoF rigid body; the wheel spin rates are integrated together
/// with it in one RK4 step, sub-stepped so the stiff tyre slip dynamics stay stable at any
/// tick rate (FR-LOOP-01). Equations and figures: docs/GROUND_MODEL.md.
/// @ingroup physics

namespace ds::physics {

/// @brief Simulated rover.
class GroundVehicle final : public Vehicle {
public:
    /// @brief Vehicle built from @p profile; sensor noise from @p seed.
    GroundVehicle(const GroundProfile& profile, std::uint64_t seed);

    [[nodiscard]] VehicleType type() const noexcept override { return VehicleType::Ground; }
    [[nodiscard]] const std::string& profileKey() const noexcept override { return profile_.key; }
    void reset(const Vec3& position, double headingRad, const EnvironmentSample& env) override;
    void updateSensors(const EnvironmentSample& env, double dt) override;
    void updateController(const PilotCommand& command, double dt) override;
    void integrate(const EnvironmentSample& env, double dt) override;
    void resolveCollisions(const EnvironmentSample& env, double dt) override;
    void resolveObstacles(const ObstacleProbe& probe) override;
    [[nodiscard]] std::vector<VehicleEvent> takeEvents() override;
    [[nodiscard]] const RigidBodyState& bodyState() const noexcept override { return body_; }
    [[nodiscard]] VehicleTelemetry telemetry() const override;
    [[nodiscard]] std::span<const CollisionPoint> collisionPoints() const noexcept override {
        return points_;
    }
    [[nodiscard]] double restHeightM() const noexcept override;
    [[nodiscard]] bool armed() const noexcept override { return controller_.armed(); }
    [[nodiscard]] const Battery& battery() const noexcept override { return battery_; }
    [[nodiscard]] const MassProperties& mass() const noexcept override { return mass_; }
    [[nodiscard]] const CameraParams& camera() const noexcept override { return profile_.camera; }
    [[nodiscard]] const SensorReadings& sensors() const noexcept override {
        return sensors_.readings();
    }
    void setBodyState(const RigidBodyState& state) noexcept override { body_ = state; }
    void attachPayload(double massKg, const Vec3& offsetBody, double dragAreaM2) override;
    void releasePayload() override;
    void setFaults(const VehicleFaults& faults) override;
    void loseBatteryCapacity(double fraction) override { battery_.loseCapacity(fraction); }

    /// @brief Profile.
    [[nodiscard]] const GroundProfile& profile() const noexcept { return profile_; }
    /// @brief Drive controller.
    [[nodiscard]] const DriveController& controller() const noexcept { return controller_; }
    /// @brief Wheel spin rates, rad/s (first wheelCount() entries).
    [[nodiscard]] const std::array<double, kMaxWheels>& wheelSpeeds() const noexcept {
        return omega_;
    }
    /// @brief Integration sub-steps per tick of @p dt seconds.
    [[nodiscard]] int substepsFor(double dt) const noexcept;
    /// @brief Bypasses the drive controller with fixed side commands (tests); empty to use the
    /// controller again. Applied only while armed.
    void overrideDrive(std::optional<DriveOutput> output) noexcept { driveOverride_ = output; }

private:
    // Terrain plane under one contact point, sampled once per tick.
    struct ContactPlane {
        Vec3 point{0.0};             // A point of the plane (x, height, z).
        Vec3 normal{0.0, 1.0, 0.0};  // Upward unit normal.
        SurfaceMaterial material = SurfaceMaterial::Grass;
    };
    // Per-wheel results of one force evaluation.
    struct WheelLoads {
        std::array<double, kMaxWheels> load{};
        std::array<double, kMaxWheels> compression{};
        std::array<double, kMaxWheels> slip{};
        std::array<double, kMaxWheels> longitudinal{};
        std::array<bool, kMaxWheels> contact{};
        std::array<Vec3, kMaxWheels> contactPoint{};
        std::array<bool, kMaxWheels> slipping{};    // Held tyre beyond its grip or brake.
        std::array<Vec3, kMaxWheels> slidAnchor{};  // Where a slipping tyre's anchor moves.
    };
    using WheelArray = std::array<double, kMaxWheels>;

    Wrench evaluate(const RigidBodyState& s, const WheelArray& omega, WheelArray& omegaDot,
                    WheelLoads* loads) const;
    [[nodiscard]] double motorTorque(int wheel, double omega) const noexcept;
    [[nodiscard]] double motorCurrent(int wheel, double omega) const noexcept;
    void sampleTerrain(const EnvironmentSample& env);
    void step(double h);
    void updateMassProperties();
    void pushEvent(VehicleEvent::Kind kind, double value = 0.0, std::string text = {});
    void crash(double speed, std::string reason);
    [[nodiscard]] DriveConditions conditions() const noexcept;
    [[nodiscard]] Vec3 pointWorld(const Vec3& body) const noexcept;

    GroundProfile profile_;
    int wheels_ = 4;
    SensorSuite sensors_;
    StateEstimator estimator_;
    DriveController controller_;
    Battery battery_;
    VehicleFaults faults_;
    RigidBodyState body_;
    MassProperties mass_;
    std::vector<Vec3> mounts_;            // Suspension mounts (body, from the reference point).
    std::vector<CollisionPoint> points_;  // Body box corners and underside.
    std::vector<int> sampledPoints_;      // Points whose terrain is sampled (the underside).
    std::array<ContactPlane, kMaxWheels> wheelPlanes_{};
    std::vector<ContactPlane> pointPlanes_;
    std::vector<double> preStepNormalSpeed_;
    std::vector<bool> pointContact_;
    WheelArray omega_{};
    WheelArray wheelAngle_{};
    std::array<std::int64_t, kMaxWheels> encoderCounts_{};
    WheelLoads loads_;
    DriveOutput output_;
    std::optional<DriveOutput> driveOverride_;
    // Standing still on the brake: each tyre is held at its contact point by a stiff
    // tangential spring (static friction) until the grip or the brake gives way.
    bool holding_ = false;
    std::array<Vec3, kMaxWheels> anchors_{};
    double supplyVolts_ = 0.0;
    Vec3 wind_{0.0};
    double airDensity_ = 1.225;
    std::optional<double> waterLevel_;
    std::vector<VehicleEvent> events_;
    Vec3 specificForce_{0.0, 0.0, -kGravity};
    double maxWheelOmega_ = 1.0;
    double odometerM_ = 0.0;
    double odometrySpeed_ = 0.0;
    double rolloverTimer_ = 0.0;
    double stuckTimer_ = 0.0;
    Vec3 stuckAnchor_{0.0};
    bool stuckReported_ = false;
    double waterDepth_ = 0.0;
    double slopeDeg_ = 0.0;
    SurfaceMaterial surface_ = SurfaceMaterial::Grass;
    weather::GroundCover cover_;  // Wet ground and snow (FR-WX-07).
    bool crashed_ = false;
    bool rolledOver_ = false;
    bool inWater_ = false;
    bool onGround_ = true;
    bool wasArmed_ = false;
    bool wasFailsafe_ = false;
    ArmBlock lastBlock_ = ArmBlock::None;
    BatteryWarning lastWarning_ = BatteryWarning::None;
    // Payload.
    double payloadMassKg_ = 0.0;
    Vec3 payloadOffset_{0.0};
    double payloadDragArea_ = 0.0;
};

}  // namespace ds::physics
