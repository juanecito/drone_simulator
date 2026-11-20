// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include "ds/astro/time.hpp"
#include "ds/input/control.hpp"
#include "ds/missions/flight_rules.hpp"
#include "ds/physics/vehicle.hpp"
#include "ds/sim/mission_session.hpp"
#include "ds/weather/weather.hpp"

/// @file
/// @brief Immutable state published after every tick (FR-LOOP-06).
/// @ingroup sim

namespace ds::sim {

/// @brief Loop states (FR-LOOP-03).
enum class RunState { Stopped, Running, Paused, SingleStep };

/// @brief Statistics of the current flight (written to the `sessions` table).
struct SessionStats {
    astro::UtcTime startedAt{};  ///< Start (simulation UTC).
    double durationS = 0.0;      ///< Simulated time since the start.
    double flightTimeS = 0.0;    ///< Time armed.
    double maxAglM = 0.0;        ///< Highest height above ground.
    double maxSpeedMs = 0.0;     ///< Highest ground speed.
    double distanceM = 0.0;      ///< Distance travelled while armed.
    double energyWh = 0.0;       ///< Battery energy used.
    int crashes = 0;             ///< Number of crashes.
};

/// @brief Everything the GUI and the renderer need from one tick.
struct WorldSnapshot {
    std::uint64_t tick = 0;               ///< Tick counter.
    double simTimeS = 0.0;                ///< Simulated time since the start.
    astro::UtcTime utc{};                 ///< Simulation clock (FR-LOOP-04a).
    RunState state = RunState::Stopped;   ///< Loop state.
    double timeScale = 1.0;               ///< Time scale.
    double tickRateHz = 500.0;            ///< Tick rate.
    double tickCostMs = 0.0;              ///< Average CPU time per tick.
    bool hasVehicle = false;              ///< A vehicle exists.
    std::string profileKey;               ///< Vehicle profile.
    std::string profileName;              ///< Vehicle profile display name.
    physics::RigidBodyState body;         ///< Ground-truth state.
    physics::Euler attitude;              ///< Ground-truth attitude.
    physics::VehicleTelemetry telemetry;  ///< Vehicle telemetry.
    input::ControlInputs controls;        ///< Pilot controls of the last tick (sticks, switches).
    double groundHeightM = 0.0;           ///< World height of the terrain under the vehicle.
    double aglM = 0.0;                    ///< Height of the landing gear above the terrain.
    double originAltitudeM = 0.0;         ///< AMSL altitude of world height 0.
    physics::Vec3 home{0.0};              ///< Take-off position.
    int windLevel = 0;                    ///< Wind level 0–10.
    double windFromDeg = 0.0;             ///< Current mean wind direction (from), degrees true.
    double windMeanSpeedMs = 0.0;         ///< Level's mean speed at 10 m.
    double windGustMs = 0.0;              ///< Level's gust peak.
    double weatherTimeS = 0.0;            ///< Weather clock (restarts with the session).
    weather::WeatherConditions weather;   ///< Weather now (FR-WX-02).
    std::array<weather::Drift, weather::kMaxCloudLayers> cloudDrift{};  ///< Cloud motion.
    double visibilityM = 50'000.0;        ///< Visibility at the vehicle's height.
    SessionStats stats;                   ///< Flight statistics.
    double sunElevationDeg = 45.0;        ///< Apparent sun elevation at the origin.
    /// Flight-rule warnings (the mission's, or free mode's, FR-MIS-06b/06d).
    std::vector<missions::RuleWarning> ruleWarnings;
    MissionStatus mission;                ///< Mission being flown (mission.active).
    /// Wall-clock time of publication (the renderer extrapolates from it).
    std::chrono::steady_clock::time_point publishedAt{};
    std::uint64_t resetCount = 0;  ///< Incremented on every reset.

    /// @brief Altitude above mean sea level of the centre of mass.
    [[nodiscard]] double amslM() const noexcept { return originAltitudeM + body.position.y; }
};

}  // namespace ds::sim
