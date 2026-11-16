// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/items/registry.hpp"
#include "ds/items/triggers.hpp"
#include "ds/missions/flight_rules.hpp"
#include "ds/missions/mission.hpp"
#include "ds/missions/mission_world.hpp"

/// @file
/// @brief Running a mission (FR-MIS-04 … FR-MIS-11): lifecycle, objectives, failures, flight
/// rules, failure injection and scoring. Headless and deterministic: the simulation calls
/// update() once per tick with the vehicle's ground-truth state.
/// @ingroup missions

namespace ds::missions {

/// @brief Lifecycle (FR-MIS-08).
enum class MissionState { Briefing, PreFlight, InProgress, Succeeded, Failed, Aborted, Debrief };
/// @brief "briefing", "preFlight", "inProgress", "succeeded", "failed", "aborted", "debrief".
[[nodiscard]] std::string_view missionStateName(MissionState state) noexcept;

/// @brief State of one objective.
enum class ObjectiveState {
    Locked,     ///< Waiting for the objectives it depends on.
    Active,     ///< Can be achieved now.
    Completed,  ///< Achieved.
    Failed,     ///< Cannot be achieved any more.
};
/// @brief "locked", "active", "completed", "failed".
[[nodiscard]] std::string_view objectiveStateName(ObjectiveState state) noexcept;

/// @brief Progress of one objective (HUD, Missions tab, debrief).
struct ObjectiveStatus {
    std::string id;                        ///< Objective id.
    ObjectiveType type = ObjectiveType::ReachAltitude;  ///< Type.
    std::string title;                     ///< Display text.
    bool optional = false;                 ///< Optional (bonus).
    ObjectiveState state = ObjectiveState::Locked;  ///< State.
    double progress = 0.0;                 ///< 0–1.
    std::optional<double> completedAtS;    ///< Mission time of completion.
    std::optional<double> errorM;          ///< Position error, for precision objectives.
    std::optional<double> maxErrorM;       ///< The error that scores zero precision.
    std::string detail;                    ///< e.g. "gate 3 of 8", "hard landing: 1.4 m/s".
};

/// @brief Vehicle state of one tick (ground truth from the simulation).
struct VehicleSample {
    double timeS = 0.0;                     ///< Simulation time.
    items::Vec3 position{0.0};              ///< Centre of mass, world frame.
    items::Vec3 velocity{0.0};              ///< World frame.
    double headingDeg = 0.0;                ///< True heading.
    double aglM = 0.0;                      ///< Landing gear / wheels above the terrain.
    bool armed = false;                     ///< Motors armed.
    bool onGround = true;                   ///< Gear or wheels touching the ground.
    bool crashed = false;                   ///< Destroyed.
    double batteryPct = 100.0;              ///< State of charge, %.
    bool lightsOn = true;                   ///< Vehicle lights.
    items::Vec3 cameraForward{0.0, 0.0, -1.0};  ///< FPV camera axis (unit, world frame).
    double sunElevationDeg = 45.0;          ///< Apparent sun elevation.
    double visibilityM = 1e9;               ///< Weather visibility.
    bool rolledOver = false;                ///< Ground: on its side or roof.
    double stuckS = 0.0;                    ///< Ground: time without progress under power.
    bool inWater = false;                   ///< In water.
};

/// @brief Something that happened to the vehicle (from the simulation's events).
struct VehicleIncident {
    /// @brief Kinds.
    enum class Kind { Collision, Crash, WaterEntered, Rollover };
    Kind kind = Kind::Collision;  ///< Kind.
    double speedMs = 0.0;         ///< Impact speed.
    std::string text;             ///< Reason (crash).
};

/// @brief Something the mission reports (HUD messages, sounds, the simulation).
struct MissionEvent {
    /// @brief Kinds.
    enum class Kind {
        Started,             ///< PreFlight → InProgress.
        ObjectiveActive,     ///< An objective was unlocked.
        ObjectiveCompleted,  ///< id = objective.
        ObjectiveFailed,     ///< id = objective, text = reason.
        GatePassed,          ///< id = gate, value = gates passed so far.
        WrongGate,           ///< id = gate.
        PayloadAttached,     ///< id = objective, value = cargo mass (kg): add it to the vehicle.
        PayloadReleased,     ///< id = objective: remove the cargo mass.
        FailureInjected,     ///< text = failure type, params = its values (FR-MIS-07).
        Succeeded,           ///< Mission succeeded.
        Failed,              ///< text = reason.
        Aborted,             ///< Aborted by the pilot.
    };
    Kind kind = Kind::Started;  ///< Kind.
    double timeS = 0.0;         ///< Mission time.
    std::string id;             ///< Objective or item id.
    std::string text;           ///< Message.
    double value = 0.0;         ///< Kind-specific number.
    common::Json params;        ///< FailureInjected parameters.
};

/// @brief Score breakdown (FR-MIS-10); see docs/MISSIONS.md for the formulas.
struct ScoreBreakdown {
    double completion = 0.0;  ///< Required objectives (≤ 500) + success (500).
    double time = 0.0;        ///< ≤ 300.
    double precision = 0.0;   ///< ≤ 200.
    double battery = 0.0;     ///< ≤ 100.
    double smoothness = 0.0;  ///< ≤ 100.
    double bonus = 0.0;       ///< Optional objectives.
    double penalties = 0.0;   ///< ≤ 0.
    double total = 0.0;       ///< Sum, at least 0.
};

/// @brief Outcome of an attempt (debrief, `mission_attempts`).
struct MissionResult {
    MissionState outcome = MissionState::Failed;  ///< Succeeded, Failed or Aborted.
    std::string failureReason;                    ///< Empty on success.
    double durationS = 0.0;                       ///< Mission time.
    double batteryPct = 0.0;                      ///< Charge at the end.
    std::optional<double> worstErrorM;            ///< Largest precision error.
    double meanJerk = 0.0;                        ///< Smoothness measure, m/s³.
    int collisions = 0;                           ///< Collisions.
    int ruleWarnings = 0;                         ///< Rule warning episodes.
    int wrongGates = 0;                           ///< Gates passed in the wrong order.
    ScoreBreakdown score;                         ///< Score.
    int stars = 0;                                ///< 0–3.
    std::vector<ObjectiveStatus> objectives;      ///< Per objective.
    std::vector<std::string> penalties;           ///< Penalty descriptions.

    /// @brief `results_json` of `mission_attempts` (per-objective results, score breakdown).
    [[nodiscard]] common::Json toJson() const;
};

/// @brief Runs one mission.
class MissionRunner {
public:
    /// @brief Places the mission (items, spawn, pilot) and prepares the objectives.
    /// @param terrainLineOfSight Terrain visibility test for VLOS (empty ⇒ terrain never hides).
    [[nodiscard]] static common::Result<std::unique_ptr<MissionRunner>> create(
        MissionDefinition mission, const MissionPlacement& placement,
        LineOfSightFunction terrainLineOfSight = {});

    MissionRunner(const MissionRunner&) = delete;
    MissionRunner& operator=(const MissionRunner&) = delete;
    MissionRunner(MissionRunner&&) = delete;
    MissionRunner& operator=(MissionRunner&&) = delete;
    ~MissionRunner();

    /// @brief The mission.
    [[nodiscard]] const MissionDefinition& mission() const noexcept;
    /// @brief The mission's items, placed in the world.
    [[nodiscard]] const items::ItemRegistry& items() const noexcept;
    /// @brief Start position (on the terrain) and heading.
    [[nodiscard]] items::Vec3 spawnPosition() const noexcept;
    /// @brief Heading at the start, degrees.
    [[nodiscard]] double spawnHeadingDeg() const noexcept;
    /// @brief Pilot position on the ground (the pilot item, or the spawn point).
    [[nodiscard]] items::Vec3 pilotPosition() const noexcept;

    /// @brief Current state.
    [[nodiscard]] MissionState state() const noexcept;
    /// @brief Briefing → PreFlight.
    void beginPreFlight();
    /// @brief What prevents the start (vehicle away from the spawn, airborne, crashed).
    [[nodiscard]] std::vector<std::string> preflightProblems(const VehicleSample& sample) const;
    /// @brief One tick. In PreFlight the mission starts when the vehicle is armed; in
    /// InProgress objectives, rules and failures are evaluated. Ignored in other states.
    void update(const VehicleSample& sample, std::span<const VehicleIncident> incidents = {});
    /// @brief Ends a running mission as Aborted.
    void abort();
    /// @brief Succeeded / Failed / Aborted → Debrief.
    void enterDebrief();
    /// @brief Back to Briefing with a clean state (FR-MIS-08).
    void retry();

    /// @brief Objectives, in mission order.
    [[nodiscard]] std::span<const ObjectiveStatus> objectives() const noexcept;
    /// @brief Active rule warnings (HUD).
    [[nodiscard]] const std::vector<RuleWarning>& warnings() const noexcept;
    /// @brief Events since the last call.
    [[nodiscard]] std::vector<MissionEvent> takeEvents();
    /// @brief Mission time (0 before the start).
    [[nodiscard]] double elapsedS() const noexcept;
    /// @brief Mass of the cargo currently carried (0 if none).
    [[nodiscard]] double payloadKg() const noexcept;
    /// @brief Covered fraction (0–1) of the survey objective @p objectiveId, or nullopt.
    [[nodiscard]] std::optional<double> surveyCoverage(std::string_view objectiveId) const;
    /// @brief The result once the mission has ended.
    [[nodiscard]] const std::optional<MissionResult>& result() const noexcept;

private:
    struct Impl;
    explicit MissionRunner(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// @brief Score and stars of an attempt from its figures (exposed for tests and the debrief).
void scoreResult(const MissionDefinition& mission, MissionResult& result);

}  // namespace ds::missions
