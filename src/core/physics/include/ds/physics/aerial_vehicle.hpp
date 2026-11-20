// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "ds/common/random.hpp"
#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/flight_controller.hpp"
#include "ds/physics/sensors.hpp"
#include "ds/physics/vehicle.hpp"

/// @file
/// @brief The aerial drone (X-quadcopter): rigid body + motors + propellers + battery +
/// aerodynamics + contacts + sensors + estimator + flight controller.
/// @ingroup physics

namespace ds::physics {

/// @brief Simulated multirotor.
class AerialVehicle final : public Vehicle {
public:
    /// @brief Vehicle built from @p profile; sensor noise and other randomness from @p seed.
    AerialVehicle(const AerialProfile& profile, std::uint64_t seed);

    [[nodiscard]] VehicleType type() const noexcept override { return VehicleType::Aerial; }
    [[nodiscard]] const std::string& profileKey() const noexcept override { return profile_.key; }
    void reset(const Vec3& position, double headingRad, const EnvironmentSample& env) override;
    void updateSensors(const EnvironmentSample& env, double dt) override;
    void updateController(const PilotCommand& command, double dt) override;
    void integrate(const EnvironmentSample& env, double dt) override;
    void resolveCollisions(const EnvironmentSample& env, double dt) override;
    /// @brief Contacts with obstacles (trees, masts, buildings, gate frames): every collision
    /// point inside one is pushed out along the contact normal and its velocity into the
    /// obstacle removed. A hard hit is a Collision event, and above half the frame's crash
    /// speed (or with spinning propellers) a crash "collision with <name>". Call after
    /// resolveCollisions().
    void resolveObstacles(const ObstacleProbe& probe) override;
    [[nodiscard]] std::vector<VehicleEvent> takeEvents() override;
    [[nodiscard]] const RigidBodyState& bodyState() const noexcept override { return body_; }
    [[nodiscard]] VehicleTelemetry telemetry() const override;
    [[nodiscard]] std::span<const CollisionPoint> collisionPoints() const noexcept override {
        return points_;
    }
    [[nodiscard]] double restHeightM() const noexcept override;
    [[nodiscard]] bool armed() const noexcept override { return controller_.armed(); }
    [[nodiscard]] const CameraParams& camera() const noexcept override { return profile_.camera; }

    /// @brief Profile.
    [[nodiscard]] const AerialProfile& profile() const noexcept { return profile_; }
    /// @brief Flight controller (tests).
    [[nodiscard]] const FlightController& controller() const noexcept { return controller_; }
    /// @brief Battery.
    [[nodiscard]] const Battery& battery() const noexcept override { return battery_; }
    /// @brief Last sensor readings.
    [[nodiscard]] const SensorReadings& sensors() const noexcept override {
        return sensors_.readings();
    }
    /// @brief Mass properties (including payload).
    [[nodiscard]] const MassProperties& mass() const noexcept override { return mass_; }
    /// @brief Rotor speeds, rad/s.
    [[nodiscard]] const std::array<double, AerialProfile::kMotorCount>& rotorSpeeds()
        const noexcept {
        return omega_;
    }
    /// @brief Overrides the state (tests; e.g. drop from a height).
    void setBodyState(const RigidBodyState& state) noexcept override { body_ = state; }
    /// @brief Bypasses the flight controller with fixed ESC commands (tests, system
    /// identification); empty to use the controller again. Applied only while armed, unless
    /// @p evenIfDisarmed.
    void overrideMotors(std::optional<std::array<double, AerialProfile::kMotorCount>> commands,
                        bool evenIfDisarmed = false) noexcept {
        motorOverride_ = commands;
        overrideDisarmed_ = evenIfDisarmed;
    }
    /// @brief Attaches a payload of @p massKg at @p offsetBody with drag area @p dragAreaM2
    /// (FR-PHY-08).
    void attachPayload(double massKg, const Vec3& offsetBody, double dragAreaM2) override;
    /// @brief Releases the payload.
    void releasePayload() override;
    /// @brief Rain water carried on the airframe, kg (FR-WX-06).
    [[nodiscard]] double waterMassKg() const noexcept { return waterMassKg_; }
    /// @brief Force of the falling precipitation on the airframe in @p env for a vehicle in
    /// state @p s (world frame), N: the momentum flux of the drops relative to the vehicle,
    /// `ρ_w·v_rel²·A` with the liquid water content `ρ_w = R / v_fall`.
    [[nodiscard]] Vec3 precipitationForce(const RigidBodyState& s,
                                          const EnvironmentSample& env) const noexcept;
    /// @brief Disables gravity, drag and contacts (free-fall and RK4 tests).
    void setIdealConditions(bool gravityOnly) noexcept { gravityOnly_ = gravityOnly; }
    /// @brief Last non-gravitational acceleration, body frame.
    [[nodiscard]] const Vec3& specificForce() const noexcept { return specificForce_; }

private:
    struct RotorLoads {
        std::array<double, AerialProfile::kMotorCount> thrust{};
        std::array<double, AerialProfile::kMotorCount> torque{};
    };
    Wrench computeWrench(const RigidBodyState& s, const EnvironmentSample& env,
                         RotorLoads* loads) const;
    void updateMassProperties();
    void pushEvent(VehicleEvent::Kind kind, double value = 0.0, std::string text = {});
    void crash(double speed, std::string reason);

    AerialProfile profile_;
    std::uint64_t seed_;
    SensorSuite sensors_;
    StateEstimator estimator_;
    FlightController controller_;
    Battery battery_;
    common::GaussianRandom random_{0};
    RigidBodyState body_;
    MassProperties mass_;
    std::vector<CollisionPoint> points_;
    std::vector<bool> inContact_;
    std::vector<double> preStepNormalSpeed_;
    std::array<double, AerialProfile::kMotorCount> omega_{};
    std::array<double, AerialProfile::kMotorCount> omegaDot_{};
    std::array<double, AerialProfile::kMotorCount> commands_{};
    std::array<double, AerialProfile::kMotorCount> propAngle_{};
    RotorLoads loads_;
    std::optional<std::array<double, AerialProfile::kMotorCount>> motorOverride_;
    bool overrideDisarmed_ = false;
    std::vector<VehicleEvent> events_;
    Vec3 specificForce_{0.0, 0.0, -kGravity};
    Vec3 lastWind_{0.0};
    double maxOmega_ = 1.0;
    double vrsFluctuation_ = 0.0;
    double throttle_ = 0.0;
    bool crashed_ = false;
    bool inWater_ = false;
    bool onGround_ = true;
    bool wasArmed_ = false;
    bool wasFailsafe_ = false;
    ArmBlock lastBlock_ = ArmBlock::None;
    BatteryWarning lastWarning_ = BatteryWarning::None;
    bool gravityOnly_ = false;
    // Payload.
    double payloadMassKg_ = 0.0;
    Vec3 payloadOffset_{0.0};
    double payloadDragArea_ = 0.0;
    // Rain (FR-WX-06): water on the airframe, and the value the mass properties use.
    double waterMassKg_ = 0.0;
    double waterInMassKg_ = 0.0;
};

}  // namespace ds::physics
