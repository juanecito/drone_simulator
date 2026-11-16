// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "ds/items/registry.hpp"
#include "ds/missions/mission.hpp"
#include "ds/missions/mission_world.hpp"

/// @file
/// @brief Flight rules (FR-MIS-06b, FR-MIS-06a, FR-MIS-06d): 120 m AGL height limit, visual line
/// of sight (distance, hidden time, lights at night), required lights and daylight-only.
/// Used by the mission runner (rules fail the mission) and in free mode (warnings only).
/// @ingroup missions

namespace ds::missions {

/// @brief Height of the pilot's eye above the ground (FR-MIS-06b).
inline constexpr double kPilotEyeHeightM = 1.7;
/// @brief Sun elevation at sunset/sunrise (upper limb with refraction), degrees.
inline constexpr double kSunsetElevationDeg = -0.833;
/// @brief Sun elevation at the end of civil twilight, degrees.
inline constexpr double kCivilTwilightElevationDeg = -6.0;

/// @brief Which rule.
enum class FlightRule { Height, VlosDistance, VlosHidden, VlosLights, Lights, Daylight };

/// @brief Short id ("height", "vlosDistance", …).
[[nodiscard]] std::string_view flightRuleName(FlightRule rule) noexcept;

/// @brief A rule close to being or being broken.
struct RuleWarning {
    FlightRule rule = FlightRule::Height;  ///< Rule.
    std::string message;                   ///< For the HUD.
    bool broken = false;                   ///< Broken (counting towards failure), not just close.
    double brokenForS = 0.0;               ///< How long it has been broken.
};

/// @brief Inputs of one tick.
struct FlightRuleSample {
    double timeS = 0.0;              ///< Simulation time.
    items::Vec3 position{0.0};       ///< Vehicle position (world).
    double aglM = 0.0;               ///< Height above the terrain.
    bool armed = false;              ///< Rules apply only while armed.
    bool lightsOn = true;            ///< Vehicle lights.
    double sunElevationDeg = 45.0;   ///< Apparent sun elevation at the vehicle.
    double visibilityM = 1e9;        ///< Weather visibility.
};

/// @brief Tracks the flight rules of a mission (or of free mode).
class FlightRulesMonitor {
public:
    /// @brief Rules @p rules for a vehicle of @p type flown from @p pilotPosition (on the
    /// ground; the eye is kPilotEyeHeightM above it). @p items (may be null) and
    /// @p terrainLineOfSight (may be empty) hide the drone.
    FlightRulesMonitor(RulesSpec rules, physics::VehicleType type, items::Vec3 pilotPosition,
                       const items::ItemRegistry* items, LineOfSightFunction terrainLineOfSight);

    /// @brief Restarts all timers.
    void reset();
    /// @brief Advances by one tick. Returns the rule broken for longer than allowed, if any.
    std::optional<RuleWarning> update(const FlightRuleSample& sample);
    /// @brief Current warnings (HUD).
    [[nodiscard]] const std::vector<RuleWarning>& warnings() const noexcept { return warnings_; }
    /// @brief Number of separate warning episodes so far (scoring penalties).
    [[nodiscard]] int warningEpisodes() const noexcept { return episodes_; }
    /// @brief Horizontal distance limit right now (rules and visibility).
    [[nodiscard]] double vlosLimitM(double visibilityM) const noexcept;
    /// @brief True if the pilot's eye sees @p target (terrain and solid items).
    [[nodiscard]] bool visible(const items::Vec3& target) const;
    /// @brief The pilot's eye.
    [[nodiscard]] const items::Vec3& eye() const noexcept { return eye_; }

private:
    struct Track {
        bool warned = false;
        double brokenSinceS = -1.0;
    };
    void check(FlightRule rule, bool warn, bool broken, double allowedS, std::string message,
               double now, std::optional<RuleWarning>& failure);

    RulesSpec rules_;
    physics::VehicleType type_;
    items::Vec3 eye_;
    const items::ItemRegistry* items_;
    LineOfSightFunction terrainLos_;
    std::vector<RuleWarning> warnings_;
    std::array<Track, 6> tracks_{};
    int episodes_ = 0;
    double lastVisibilityCheckS_ = -1.0;
    bool lastVisible_ = true;
};

}  // namespace ds::missions
