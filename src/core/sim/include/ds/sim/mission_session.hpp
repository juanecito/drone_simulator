// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/physics/failures.hpp"

/// @file
/// @brief A mission flown in the simulation (FR-MIS-08, FR-LOOP-05): couples a
/// missions::MissionRunner with the World on the simulation thread — builds the vehicle sample
/// of each tick, feeds the incidents, applies payload mass, publishes mission events and the
/// mission status of the snapshot. Headless: no GUI, no OpenGL.
/// @ingroup sim

namespace ds::sim {

class World;

/// @brief The failure a mission's `failures[]` entry injects (FR-MIS-07): its type and the
/// parameters it gives (the others keep defaultFailure()). Empty for an unknown type.
[[nodiscard]] std::optional<physics::Failure> missionFailure(const missions::FailureSpec& spec);

/// @brief Interval between two points of the recorded flight path (debrief), seconds.
inline constexpr double kPathSampleS = 0.5;
/// @brief Most points kept in the recorded flight path.
inline constexpr std::size_t kMaxPathPoints = 7200;

/// @brief Mission part of the snapshot (HUD, debrief).
struct MissionStatus {
    bool active = false;                                              ///< A mission is loaded.
    std::string missionId;                                            ///< Mission id.
    std::string title;                                                ///< Mission title.
    missions::MissionState state = missions::MissionState::Briefing;  ///< Lifecycle.
    double elapsedS = 0.0;                                            ///< Mission time.
    std::optional<double> timeLimitS;                                 ///< Time limit, if any.
    std::vector<missions::ObjectiveStatus> objectives;  ///< Objectives, in mission order.
    std::vector<std::string> preflightProblems;         ///< What prevents the start (PreFlight).
    double payloadKg = 0.0;                             ///< Cargo carried.
    std::optional<missions::MissionResult> result;      ///< Set once the mission has ended.
    std::vector<glm::dvec3> path;  ///< Flight path; filled once the mission has ended.
    std::uint64_t attempt = 0;     ///< Incremented by every retry.
};

/// @brief Runs a mission inside a World.
class MissionSession {
public:
    /// @brief Takes the prepared @p runner (in Briefing) and moves it to PreFlight.
    explicit MissionSession(std::unique_ptr<missions::MissionRunner> runner);
    ~MissionSession();
    MissionSession(const MissionSession&) = delete;
    MissionSession& operator=(const MissionSession&) = delete;
    MissionSession(MissionSession&&) = delete;
    MissionSession& operator=(MissionSession&&) = delete;

    /// @brief One tick, after the world moved the vehicle (FR-LOOP-05: items, missions).
    /// @param world The world, after the vehicle moved.
    /// @param incidents Collisions, crashes and water entries of this tick.
    void tick(World& world, std::span<const missions::VehicleIncident> incidents);
    /// @brief Ends a running mission as Aborted (Missions › Abort).
    void abort(World& world);
    /// @brief Back to PreFlight with a clean mission state; the world must be reset by the
    /// caller (the vehicle back at the spawn point).
    void retry(World& world);
    /// @brief Fills the snapshot's mission part.
    void fill(MissionStatus& status) const;
    /// @brief The vehicle sample for the current world state (also used for tests).
    [[nodiscard]] static missions::VehicleSample sample(const World& world);

    /// @brief The runner.
    [[nodiscard]] const missions::MissionRunner& runner() const noexcept { return *runner_; }

private:
    void handleMissionEvents(World& world);
    void recordPath(const World& world);

    std::unique_ptr<missions::MissionRunner> runner_;
    std::vector<std::string> preflightProblems_;
    std::vector<glm::dvec3> path_;
    double nextPathS_ = 0.0;
    std::uint64_t attempt_ = 0;
};

}  // namespace ds::sim
