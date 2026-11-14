// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "ds/astro/time.hpp"
#include "ds/input/control.hpp"
#include "ds/physics/aerial_vehicle.hpp"
#include "ds/physics/wind.hpp"
#include "ds/sim/events.hpp"
#include "ds/sim/world_snapshot.hpp"
#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief The simulated world, advanced one fixed step at a time — runnable headless, without
/// GUI or OpenGL (FR-LOOP-05, FR-LOOP-08). Deterministic for a given configuration and input
/// sequence (FR-X-05).
/// @ingroup sim

namespace ds::sim {

/// @brief Terrain lookup: height, normal and friction at world (x, z). Must be thread-safe.
using GroundFunction = std::function<physics::GroundSample(double x, double z)>;

/// @brief Flat ground at height 0 (tests).
[[nodiscard]] GroundFunction flatGround(double heightM = 0.0);

/// @brief Configuration of a world.
struct WorldConfig {
    physics::AerialProfile profile;     ///< Vehicle.
    std::uint64_t seed = 1;             ///< Session seed (sensors, wind).
    GroundFunction ground;              ///< Terrain (flat ground at 0 if empty).
    std::optional<double> waterLevelM;  ///< Water surface, if any.
    double originAltitudeM = 0.0;       ///< AMSL altitude of world height 0.
    weather::AirConditions air;         ///< Air at the ground.
    weather::WindSettings wind;         ///< Wind level and direction.
    weather::WindLevelTable windTable = weather::WindLevelTable::builtIn();  ///< Levels.
    double roughnessM = 0.03;          ///< Surface roughness for the wind profile.
    physics::Vec3 startPosition{0.0};  ///< Take-off point (x, z; placed on the ground).
    double startHeadingRad = 0.0;      ///< Initial heading.
    astro::UtcTime startUtc{};         ///< Simulation clock at the start.
};

/// @brief Maps a transmitter flight-mode index to the vehicle's mode.
[[nodiscard]] physics::FlightMode toFlightMode(int index) noexcept;

/// @brief Converts device controls into the vehicle's pilot command.
[[nodiscard]] physics::PilotCommand toPilotCommand(const input::ControlInputs& controls) noexcept;

/// @brief One vehicle in its environment.
class World {
public:
    /// @brief Builds the world and places the vehicle at the start position.
    explicit World(WorldConfig config);

    /// @brief Puts the vehicle back at the start, refills the battery, restarts the wind and
    /// the statistics, and publishes SimResetEvent.
    void reset();
    /// @brief Moves the take-off point and resets.
    void resetAt(const physics::Vec3& position, double headingRad);
    /// @brief Advances one step of @p dt seconds with @p controls (FR-LOOP-05 order).
    void tick(const input::ControlInputs& controls, double dt);

    /// @brief Replaces the terrain (the vehicle is lifted onto it if it ended up below).
    void setGround(GroundFunction ground, std::optional<double> waterLevelM,
                   double originAltitudeM);
    /// @brief Changes the air at the ground.
    void setAir(const weather::AirConditions& air) noexcept { config_.air = air; }
    /// @brief Changes the wind (restarts the wind model with the session seed).
    void setWind(const weather::WindSettings& wind);
    /// @brief Sets the simulation clock.
    void setUtc(astro::UtcTime utc) noexcept;
    /// @brief Replaces the vehicle (new profile) and resets.
    void setProfile(const physics::AerialProfile& profile);

    /// @brief Fills @p snapshot (allocation-free for a reused snapshot).
    void fillSnapshot(WorldSnapshot& snapshot) const;
    /// @brief Event bus (handlers run on the simulation thread).
    [[nodiscard]] EventBus& events() noexcept { return events_; }
    /// @brief The vehicle.
    [[nodiscard]] physics::AerialVehicle& vehicle() noexcept { return *vehicle_; }
    /// @brief The vehicle.
    [[nodiscard]] const physics::AerialVehicle& vehicle() const noexcept { return *vehicle_; }
    /// @brief Wind model.
    [[nodiscard]] const physics::WindModel& wind() const noexcept { return wind_; }
    /// @brief Environment of the last tick.
    [[nodiscard]] const physics::EnvironmentSample& environment() const noexcept { return env_; }
    /// @brief Configuration.
    [[nodiscard]] const WorldConfig& config() const noexcept { return config_; }
    /// @brief Simulated seconds since the start.
    [[nodiscard]] double time() const noexcept { return time_; }
    /// @brief Ticks since the start.
    [[nodiscard]] std::uint64_t tickCount() const noexcept { return ticks_; }
    /// @brief Simulation clock.
    [[nodiscard]] astro::UtcTime utc() const noexcept;
    /// @brief Flight statistics.
    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }

private:
    physics::EnvironmentSample sampleEnvironment(const physics::Vec3& position) const;
    void restartWind();
    void publishVehicleEvents();

    WorldConfig config_;
    std::unique_ptr<physics::AerialVehicle> vehicle_;
    physics::WindModel wind_;
    physics::EnvironmentSample env_;
    EventBus events_;
    double time_ = 0.0;
    std::uint64_t ticks_ = 0;
    std::uint64_t resets_ = 0;
    astro::UtcTime clockBase_{};
    double clockBaseTime_ = 0.0;
    physics::Vec3 home_{0.0};
    SessionStats stats_;
    physics::Vec3 lastPosition_{0.0};
    input::ControlInputs controls_;
};

}  // namespace ds::sim
