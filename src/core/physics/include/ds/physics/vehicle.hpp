// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ds/physics/aerial_profile.hpp"
#include "ds/physics/atmosphere.hpp"
#include "ds/physics/estimator.hpp"
#include "ds/physics/pilot_command.hpp"
#include "ds/physics/powertrain.hpp"
#include "ds/physics/rigid_body.hpp"
#include "ds/physics/sensors.hpp"
#include "ds/physics/surface.hpp"

/// @file
/// @brief Common interface of aerial and ground vehicles (SPECIFICATION §4.2): rigid body,
/// battery, sensors, controller, collision shape and telemetry, so that the loop, missions,
/// cameras, HUD and storage treat both types alike.
/// @ingroup physics

namespace ds::physics {

/// @brief Vehicle type.
enum class VehicleType { Aerial, Ground };

/// @brief "aerial" or "ground" (as stored in the database).
[[nodiscard]] std::string_view vehicleTypeName(VehicleType type) noexcept;

/// @brief Terrain under a point: height and surface normal (world frame).
struct GroundSample {
    double heightM = 0.0;        ///< World height of the surface.
    Vec3 normal{0.0, 1.0, 0.0};  ///< Upward unit normal.
    double friction = 0.8;       ///< Friction coefficient of the surface material.
    SurfaceMaterial material = SurfaceMaterial::Grass;  ///< Material (tyre grip, FR-GND-04).
};

/// @brief Terrain lookup at world (x, z). Must be thread-safe.
using GroundQuery = std::function<GroundSample(double x, double z)>;

/// @brief Environment at the vehicle for one tick (computed once per tick by the world).
struct EnvironmentSample {
    AirState air;                       ///< Air at the vehicle.
    double qnhHpa = 1013.25;            ///< Altimeter setting.
    double ambientC = 15.0;             ///< Air temperature, °C (battery cooling).
    Vec3 wind{0.0};                     ///< Wind velocity, world frame.
    Vec3 groundPoint{0.0};              ///< Terrain point under the vehicle (x, height, z).
    GroundSample ground;                ///< Terrain under the vehicle (local plane for contacts).
    std::optional<double> waterLevelM;  ///< World height of the water surface, if any.
    /// Terrain lookup for contacts away from the centre (the wheels of ground drones); null
    /// ⇒ the local plane @ref ground is used everywhere. Not owned.
    const GroundQuery* groundQuery = nullptr;
};

/// @brief A collision point inside an obstacle (see AerialVehicle::resolveObstacles()).
struct ObstacleContact {
    Vec3 normal{0.0, 1.0, 0.0};  ///< Unit world direction out of the obstacle.
    double depthM = 0.0;         ///< Penetration along @ref normal.
    std::string name;            ///< Obstacle name, for the crash reason.
    double restitution = 0.2;    ///< Fraction of the normal speed kept when bouncing off.
    double friction = 0.0;       ///< Coulomb friction coefficient along the surface.
    /// @brief Mass of the obstacle, kg. It does not move; a light obstacle takes part of the
    /// impact (the bounce is scaled by `M / (m + M)`).
    double massKg = std::numeric_limits<double>::infinity();
};

/// @brief The contact of a world point with the obstacles around it, or nullopt.
using ObstacleProbe = std::function<std::optional<ObstacleContact>(const Vec3& point)>;

/// @brief Something that happened to the vehicle during a tick.
struct VehicleEvent {
    /// @brief Event kinds.
    enum class Kind {
        Collision,        ///< Contact with an impact (value: impact speed, m/s).
        Crash,            ///< Destroyed (value: impact speed; text: reason).
        BatteryLow,       ///< Low-voltage warning.
        BatteryCritical,  ///< Critical-voltage warning.
        BatteryDepleted,  ///< Empty: motors stop.
        WaterEntered,     ///< Fell into water.
        Armed,            ///< Motors armed.
        Disarmed,         ///< Motors disarmed.
        ArmRefused,       ///< Pre-arm check failed (text: reason).
        FailsafeOn,       ///< RC link lost: failsafe landing.
        FailsafeOff,      ///< RC link back.
        NumericalReset,   ///< NaN/Inf detected: state reset (FR-PHY-17).
        Rollover,         ///< Ground: tipped onto its side or roof (FR-GND-07).
        Stuck,            ///< Ground: no progress under power (value: seconds; FR-GND-08).
    };
    Kind kind = Kind::Collision;  ///< What happened.
    double value = 0.0;           ///< Kind-specific value.
    std::string text;             ///< Kind-specific text.
};

/// @brief A point of the collision shape (body frame, relative to the centre of mass).
struct CollisionPoint {
    Vec3 body{0.0};     ///< Position.
    bool gear = false;  ///< Landing gear (meant to touch the ground) vs. arms/props/top.
};

/// @brief Battery read-out.
struct BatteryTelemetry {
    double voltage = 0.0;                           ///< Terminal voltage, V.
    double current = 0.0;                           ///< Current, A.
    double stateOfCharge = 1.0;                     ///< 0–1.
    double temperatureC = 25.0;                     ///< Pack temperature.
    double consumedMah = 0.0;                       ///< Charge used.
    double consumedWh = 0.0;                        ///< Energy used.
    int cells = 0;                                  ///< Cells in series.
    BatteryWarning warning = BatteryWarning::None;  ///< Warning state.
};

/// @brief Most wheels of any ground drone layout.
inline constexpr int kMaxWheels = 6;

/// @brief Ground-drone part of the telemetry (FR-GFX-20).
struct GroundTelemetry {
    DriveMode mode = DriveMode::Manual;            ///< Drive mode.
    int wheelCount = 0;                            ///< Wheels in use (0 for aerial).
    std::array<double, kMaxWheels> wheelSpeed{};   ///< Wheel speeds, rad/s.
    std::array<double, kMaxWheels> wheelAngle{};   ///< Wheel rotation angles (visuals).
    std::array<double, kMaxWheels> compression{};  ///< Suspension compression, m.
    std::array<double, kMaxWheels> wheelLoad{};    ///< Normal load, N.
    std::array<double, kMaxWheels> slip{};         ///< Combined slip (0 = grip).
    std::array<bool, kMaxWheels> contact{};        ///< Wheel on the ground.
    double leftCommand = 0.0;                      ///< Left motors, −1 … 1.
    double rightCommand = 0.0;                     ///< Right motors, −1 … 1.
    bool braking = false;                          ///< Brake applied.
    double speedMs = 0.0;                          ///< Forward ground speed (truth).
    double odometrySpeedMs = 0.0;                  ///< Speed from the wheel encoders.
    double odometerM = 0.0;                        ///< Distance from the wheel encoders.
    double cruiseSetpointMs = 0.0;                 ///< Speed-control target.
    double slopeDeg = 0.0;                         ///< Slope of the terrain under it.
    /// Lateral load transfer (N_right − N_left) / ΣN: ±1 = one side's wheels lifting.
    double loadTransfer = 0.0;
    double maxSlip = 0.0;                              ///< Largest wheel slip.
    SurfaceMaterial surface = SurfaceMaterial::Grass;  ///< Material under the vehicle.
    bool rolledOver = false;                           ///< On its side or roof.
    double stuckS = 0.0;                               ///< Time without progress under power.
    bool inWater = false;                              ///< Deeper than the wading depth.
    double waterDepthM = 0.0;                          ///< Water depth at the vehicle.
};

/// @brief Telemetry of one tick (shown in the HUD and the Simulation tab).
struct VehicleTelemetry {
    VehicleType type = VehicleType::Aerial;  ///< Type.
    bool armed = false;                      ///< Motors armed.
    bool crashed = false;                    ///< Crashed (reset needed).
    bool onGround = false;                   ///< Landing gear touching the ground.
    bool failsafe = false;                   ///< RC-loss failsafe active.
    FlightMode mode = FlightMode::Angle;     ///< Flight mode.
    ArmBlock armBlock = ArmBlock::None;      ///< Last arming refusal.
    double throttle = 0.0;                   ///< Pilot throttle 0–1.
    std::array<double, 4> motorCommand{};    ///< ESC commands 0–1.
    std::array<double, 4> motorRpm{};        ///< Motor speeds.
    std::array<double, 4> propAngleRad{};    ///< Propeller angles (visuals).
    double motorSpeedFraction = 0.0;         ///< Mean speed / maximum speed.
    BatteryTelemetry battery;                ///< Battery.
    EstimatedState estimate;                 ///< Flight-controller estimate.
    GpsSample gps;                           ///< Last GPS sample.
    double airspeedMs = 0.0;                 ///< Speed relative to the air.
    Vec3 wind{0.0};                          ///< Wind at the vehicle.
    double armedTimeS = 0.0;                 ///< Time since arming.
    double totalThrustN = 0.0;               ///< Sum of rotor thrust.
    GroundTelemetry ground;                  ///< Ground drones only.
};

/// @brief A simulated vehicle. Methods are called by the simulation loop in the order of
/// FR-LOOP-05: sensors → controller → integrate → collisions.
class Vehicle {
public:
    Vehicle() = default;
    virtual ~Vehicle() = default;
    Vehicle(const Vehicle&) = delete;
    Vehicle& operator=(const Vehicle&) = delete;
    Vehicle(Vehicle&&) = delete;
    Vehicle& operator=(Vehicle&&) = delete;

    /// @brief Aerial or ground.
    [[nodiscard]] virtual VehicleType type() const noexcept = 0;
    /// @brief Key of the profile it was built from.
    [[nodiscard]] virtual const std::string& profileKey() const noexcept = 0;
    /// @brief Places the vehicle at rest on the ground under @p position with heading
    /// @p headingRad; full battery at the ambient temperature; sensors powered on.
    virtual void reset(const Vec3& position, double headingRad, const EnvironmentSample& env) = 0;
    /// @brief Samples the sensors and runs the estimator.
    virtual void updateSensors(const EnvironmentSample& env, double dt) = 0;
    /// @brief Runs the onboard controller with the pilot's commands.
    virtual void updateController(const PilotCommand& command, double dt) = 0;
    /// @brief Integrates the physics over @p dt.
    virtual void integrate(const EnvironmentSample& env, double dt) = 0;
    /// @brief Contact bookkeeping: impacts, crashes, water (forces are part of integrate()).
    virtual void resolveCollisions(const EnvironmentSample& env, double dt) = 0;
    /// @brief Takes the events since the last call.
    [[nodiscard]] virtual std::vector<VehicleEvent> takeEvents() = 0;
    /// @brief Ground-truth rigid-body state.
    [[nodiscard]] virtual const RigidBodyState& bodyState() const noexcept = 0;
    /// @brief Telemetry.
    [[nodiscard]] virtual VehicleTelemetry telemetry() const = 0;
    /// @brief Collision shape.
    [[nodiscard]] virtual std::span<const CollisionPoint> collisionPoints() const noexcept = 0;
    /// @brief Height of the centre of mass above the landing gear's contact plane at rest.
    [[nodiscard]] virtual double restHeightM() const noexcept = 0;
    /// @brief Contacts with obstacles (trees, masts, buildings, gate frames, boxes): every
    /// collision point inside one is pushed out along the contact normal and its velocity into
    /// the obstacle removed; hard hits are Collision events or crashes. Call after
    /// resolveCollisions().
    virtual void resolveObstacles(const ObstacleProbe& probe) = 0;
    /// @brief True while the motors are armed.
    [[nodiscard]] virtual bool armed() const noexcept = 0;
    /// @brief Battery.
    [[nodiscard]] virtual const Battery& battery() const noexcept = 0;
    /// @brief Mass properties (including payload).
    [[nodiscard]] virtual const MassProperties& mass() const noexcept = 0;
    /// @brief Onboard (FPV) camera.
    [[nodiscard]] virtual const CameraParams& camera() const noexcept = 0;
    /// @brief Last sensor readings.
    [[nodiscard]] virtual const SensorReadings& sensors() const noexcept = 0;
    /// @brief Overrides the state (moving onto new terrain; tests).
    virtual void setBodyState(const RigidBodyState& state) noexcept = 0;
    /// @brief Attaches a payload of @p massKg at @p offsetBody with drag area @p dragAreaM2
    /// (FR-PHY-08).
    virtual void attachPayload(double massKg, const Vec3& offsetBody, double dragAreaM2) = 0;
    /// @brief Releases the payload.
    virtual void releasePayload() = 0;
};

}  // namespace ds::physics
