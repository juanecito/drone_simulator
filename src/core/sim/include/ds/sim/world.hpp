// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "ds/astro/time.hpp"
#include "ds/geo/coordinates.hpp"
#include "ds/input/control.hpp"
#include "ds/items/registry.hpp"
#include "ds/missions/flight_rules.hpp"
#include "ds/physics/aerial_vehicle.hpp"
#include "ds/physics/ground_vehicle.hpp"
#include "ds/physics/vehicle_factory.hpp"
#include "ds/physics/wind.hpp"
#include "ds/sim/events.hpp"
#include "ds/sim/mission_session.hpp"
#include "ds/sim/world_snapshot.hpp"
#include "ds/weather/wind_levels.hpp"

/// @file
/// @brief The simulated world, advanced one fixed step at a time — runnable headless, without
/// GUI or OpenGL (FR-LOOP-05, FR-LOOP-08). Deterministic for a given configuration and input
/// sequence (FR-X-05).
/// @ingroup sim

namespace ds::sim {

/// @brief Terrain lookup: height, normal, friction and material at world (x, z). Must be
/// thread-safe.
using GroundFunction = physics::GroundQuery;

/// @brief Flat ground at height 0 (tests).
[[nodiscard]] GroundFunction flatGround(double heightM = 0.0);

/// @brief Line of sight over the terrain @p ground: true unless the terrain rises above the
/// straight line between the two points (sampled every ≤ 5 m, at most 400 samples).
[[nodiscard]] missions::LineOfSightFunction terrainLineOfSight(GroundFunction ground);

/// @brief Configuration of a world.
struct WorldConfig {
    physics::VehicleProfile profile;    ///< Vehicle (aerial or ground).
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
    /// Geographic position of world (0, 0, 0) (sun position for the flight rules).
    geo::Geodetic geoOrigin{};
    double visibilityM = 60'000.0;     ///< Meteorological visibility (VLOS limit).
};

/// @brief Flight rules in free mode (FR-MIS-06d): the default rules, checked from the pilot's
/// position, shown as warnings; with @ref enforce a rule broken for longer than allowed ends
/// the flight (FlightRuleBrokenEvent).
struct FreeFlightRules {
    physics::Vec3 pilotPosition{0.0};  ///< Pilot on the ground (the eye is 1.7 m above).
    bool enforce = false;              ///< End the flight when a rule is broken.
};

/// @brief Maps a transmitter flight-mode index to the vehicle's mode.
[[nodiscard]] physics::FlightMode toFlightMode(int index) noexcept;

/// @brief Maps a transmitter mode index to the ground drone's drive mode: 0 and 1 (the
/// default) = Manual, 2 and above = Speed control.
[[nodiscard]] physics::DriveMode toDriveMode(int index) noexcept;

/// @brief Converts device controls into the vehicle's pilot command.
[[nodiscard]] physics::PilotCommand toPilotCommand(const input::ControlInputs& controls) noexcept;

/// @brief One vehicle in its environment.
class World {
public:
    /// @brief Builds the world and places the vehicle at the start position.
    explicit World(WorldConfig config);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

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
    /// @brief Replaces the vehicle (new profile, either type) and resets.
    void setProfile(const physics::VehicleProfile& profile);
    /// @brief Changes the visibility (VLOS limit).
    void setVisibility(double visibilityM) noexcept { config_.visibilityM = visibilityM; }
    /// @brief Changes the geographic origin (sun position).
    void setGeoOrigin(const geo::Geodetic& origin) noexcept;

    /// @brief Flies @p session (null ends the mission). Free-mode rules are not checked while
    /// a mission runs: the mission checks its own.
    void setMission(std::unique_ptr<MissionSession> session);
    /// @brief The mission being flown, or null.
    [[nodiscard]] MissionSession* mission() noexcept { return mission_.get(); }
    /// @brief Free-mode flight rules; nullopt switches them off.
    void setFreeFlightRules(std::optional<FreeFlightRules> rules);
    /// @brief Items of the free-flight scene (Items tab): obstacles outside missions. During a
    /// mission the mission's own items are the obstacles.
    void setFreeItems(const std::vector<items::Item>& freeItems);
    /// @brief The free-flight scene's items.
    [[nodiscard]] const items::ItemRegistry& freeItems() const noexcept { return freeItems_; }
    /// @brief Apparent sun elevation at the origin, degrees (updated every simulated second).
    [[nodiscard]] double sunElevationDeg() const noexcept { return sunElevationDeg_; }

    /// @brief Surface a vehicle point at @p point stands on (FR-GND-03, FR-ITM-13): the terrain,
    /// or the top of a ramp or bridge deck under the point that is not above it (by more than
    /// 2 cm). Decks are paved.
    [[nodiscard]] physics::GroundSample surfaceAt(const physics::Vec3& point) const;

    /// @brief Fills @p snapshot (allocation-free for a reused snapshot).
    void fillSnapshot(WorldSnapshot& snapshot) const;
    /// @brief Event bus (handlers run on the simulation thread).
    [[nodiscard]] EventBus& events() noexcept { return events_; }
    /// @brief The vehicle.
    [[nodiscard]] physics::Vehicle& vehicle() noexcept { return *vehicle_; }
    /// @brief The vehicle.
    [[nodiscard]] const physics::Vehicle& vehicle() const noexcept { return *vehicle_; }
    /// @brief The vehicle if it is an aerial drone, else null.
    [[nodiscard]] physics::AerialVehicle* aerialVehicle() noexcept;
    /// @brief The vehicle if it is an aerial drone, else null.
    [[nodiscard]] const physics::AerialVehicle* aerialVehicle() const noexcept;
    /// @brief The vehicle if it is a ground drone, else null.
    [[nodiscard]] physics::GroundVehicle* groundVehicle() noexcept;
    /// @brief The vehicle if it is a ground drone, else null.
    [[nodiscard]] const physics::GroundVehicle* groundVehicle() const noexcept;
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
    /// @brief Pilot controls of the last tick.
    [[nodiscard]] const input::ControlInputs& controls() const noexcept { return controls_; }

private:
    physics::EnvironmentSample sampleEnvironment(const physics::Vec3& position) const;
    [[nodiscard]] const items::ItemRegistry& activeItems() const noexcept;
    void restartWind();
    void publishVehicleEvents();
    void resolveObstacles();
    void updateSun();
    void checkFreeRules();

    WorldConfig config_;
    std::unique_ptr<physics::Vehicle> vehicle_;
    physics::WindModel wind_;
    physics::EnvironmentSample env_;
    physics::SurfaceQuery surfaceQuery_;  ///< surfaceAt(), for the vehicle's contacts.
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
    std::unique_ptr<MissionSession> mission_;
    items::ItemRegistry freeItems_;
    std::vector<missions::VehicleIncident> incidents_;
    std::optional<FreeFlightRules> freeRules_;
    std::unique_ptr<missions::FlightRulesMonitor> freeMonitor_;
    double sunElevationDeg_ = 45.0;
    double sunUpdatedAtS_ = -1e9;
};

}  // namespace ds::sim
