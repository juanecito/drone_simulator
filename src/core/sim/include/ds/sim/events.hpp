// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "ds/common/event_bus.hpp"

/// @file
/// @brief Simulation events (FR-LOOP-07) and the event bus that carries them.
/// @ingroup sim

namespace ds::sim {

/// @brief The vehicle hit something without breaking.
struct CollisionEvent {
    double speedMs = 0.0;  ///< Impact speed.
};
/// @brief The vehicle crashed.
struct CrashEvent {
    std::string reason;    ///< E.g. "hard landing".
    double speedMs = 0.0;  ///< Impact speed.
};
/// @brief A gate was passed (missions, M5).
struct GatePassedEvent {
    std::uint64_t itemId = 0;  ///< Gate.
};
/// @brief The vehicle entered a zone (missions, M5).
struct ZoneEnteredEvent {
    std::uint64_t itemId = 0;  ///< Zone.
};
/// @brief The vehicle left a zone (missions, M5).
struct ZoneExitedEvent {
    std::uint64_t itemId = 0;  ///< Zone.
};
/// @brief A payload was attached.
struct PayloadAttachedEvent {
    double massKg = 0.0;  ///< Payload mass.
};
/// @brief The payload was released.
struct PayloadReleasedEvent {};
/// @brief A mission objective was completed (M5).
struct ObjectiveCompletedEvent {
    std::string objectiveId;  ///< Objective.
};
/// @brief A mission objective failed (M5).
struct ObjectiveFailedEvent {
    std::string objectiveId;  ///< Objective.
    std::string reason;       ///< Why.
};
/// @brief The mission succeeded (M5).
struct MissionSucceededEvent {};
/// @brief The mission failed (M5).
struct MissionFailedEvent {
    std::string reason;  ///< Why.
};
/// @brief Battery warning level changed.
struct BatteryWarningEvent {
    /// @brief Warning level.
    enum class Level { Low, Critical, Depleted };
    Level level = Level::Low;  ///< New level.
};
/// @brief A ground drone rolled over (M6).
struct RolloverEvent {};
/// @brief A ground drone is stuck (M6).
struct VehicleStuckEvent {};
/// @brief The vehicle fell into water.
struct WaterEnteredEvent {};
/// @brief The simulation was reset.
struct SimResetEvent {};
/// @brief The motors were armed or disarmed.
struct ArmingEvent {
    bool armed = false;  ///< New state.
};
/// @brief Arming was refused by a pre-arm check.
struct ArmRefusedEvent {
    std::string reason;  ///< Message.
};
/// @brief RC-loss failsafe started or ended.
struct FailsafeEvent {
    bool active = false;  ///< New state.
};
/// @brief Invalid numbers were detected and the vehicle state was reset (FR-PHY-17).
struct NumericalResetEvent {};

/// @brief Event bus of the simulation.
using EventBus =
    common::EventBus<CollisionEvent, CrashEvent, GatePassedEvent, ZoneEnteredEvent, ZoneExitedEvent,
                     PayloadAttachedEvent, PayloadReleasedEvent, ObjectiveCompletedEvent,
                     ObjectiveFailedEvent, MissionSucceededEvent, MissionFailedEvent,
                     BatteryWarningEvent, RolloverEvent, VehicleStuckEvent, WaterEnteredEvent,
                     SimResetEvent, ArmingEvent, ArmRefusedEvent, FailsafeEvent,
                     NumericalResetEvent>;
/// @brief Any simulation event.
using Event = EventBus::Event;

/// @brief An event with the simulation time it happened at.
struct TimedEvent {
    double timeS = 0.0;  ///< Simulation time.
    Event event;         ///< The event.
};

/// @brief Short human-readable description (status bar, HUD messages, log).
[[nodiscard]] std::string describe(const Event& event);

}  // namespace ds::sim
