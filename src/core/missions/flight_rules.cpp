// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/flight_rules.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace ds::missions {
namespace {

// The height warning comes at 100 m for the default 120 m limit (FR-MIS-06b).
constexpr double kHeightWarningFraction = 100.0 / 120.0;
constexpr double kDistanceWarningFraction = 0.9;
// Line-of-sight ray tests are done at this rate (they are the costly part).
constexpr double kVisibilityCheckPeriodS = 0.05;

double horizontalDistance(const items::Vec3& a, const items::Vec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

}  // namespace

std::string_view flightRuleName(FlightRule rule) noexcept {
    switch (rule) {
        case FlightRule::Height: return "height";
        case FlightRule::VlosDistance: return "vlosDistance";
        case FlightRule::VlosHidden: return "vlosHidden";
        case FlightRule::VlosLights: return "vlosLights";
        case FlightRule::Lights: return "lights";
        case FlightRule::Daylight: return "daylight";
    }
    return "unknown";
}

FlightRulesMonitor::FlightRulesMonitor(RulesSpec rules, physics::VehicleType type,
                                       items::Vec3 pilotPosition, const items::ItemRegistry* items,
                                       LineOfSightFunction terrainLineOfSight)
    : rules_(std::move(rules)),
      type_(type),
      eye_(pilotPosition + items::Vec3(0.0, kPilotEyeHeightM, 0.0)),
      items_(items),
      terrainLos_(std::move(terrainLineOfSight)) {}

void FlightRulesMonitor::reset() {
    warnings_.clear();
    tracks_ = {};
    episodes_ = 0;
    lastVisibilityCheckS_ = -1.0;
    lastVisible_ = true;
}

double FlightRulesMonitor::vlosLimitM(double visibilityM) const noexcept {
    return std::min(rules_.vlos.maxDistanceM, visibilityM);
}

bool FlightRulesMonitor::visible(const items::Vec3& target) const {
    if (terrainLos_ && !terrainLos_(eye_, target)) {
        return false;
    }
    if (items_ != nullptr) {
        const items::Vec3 d = target - eye_;
        const double length = glm::length(d);
        if (length > 1e-6) {
            // Stop short of the target so the vehicle's own surroundings do not count.
            if (const auto hit = items_->raycast(eye_, d / length, length - 0.5)) {
                return false;
            }
        }
    }
    return true;
}

void FlightRulesMonitor::check(FlightRule rule, bool warn, bool broken, double allowedS,
                               std::string message, double now,
                               std::optional<RuleWarning>& failure) {
    Track& t = tracks_[static_cast<std::size_t>(rule)];
    const auto it = std::ranges::find(warnings_, rule, &RuleWarning::rule);
    if (!warn && !broken) {
        t = {};
        if (it != warnings_.end()) {
            warnings_.erase(it);
        }
        return;
    }
    if (!t.warned) {
        t.warned = true;
        ++episodes_;
    }
    if (broken && t.brokenSinceS < 0.0) {
        t.brokenSinceS = now;
    } else if (!broken) {
        t.brokenSinceS = -1.0;
    }
    RuleWarning w{rule, std::move(message), broken, broken ? now - t.brokenSinceS : 0.0};
    if (broken && w.brokenForS > allowedS && !failure) {
        failure = w;
    }
    if (it != warnings_.end()) {
        *it = std::move(w);
    } else {
        warnings_.push_back(std::move(w));
    }
}

std::optional<RuleWarning> FlightRulesMonitor::update(const FlightRuleSample& s) {
    std::optional<RuleWarning> failure;
    const bool active = s.armed;
    const double now = s.timeS;
    const double grace = rules_.graceS;

    // (1) Height above the terrain (aerial only).
    if (type_ == physics::VehicleType::Aerial && rules_.maxAltitudeAglM) {
        const double limit = *rules_.maxAltitudeAglM;
        const bool broken = active && s.aglM > limit;
        const bool warn = active && s.aglM >= limit * kHeightWarningFraction;
        check(FlightRule::Height, warn, broken, grace,
              std::format("Height {:.0f} m AGL — limit {:.0f} m", s.aglM, limit), now, failure);
    }

    // (2) Visual line of sight.
    if (rules_.vlos.enabled) {
        const double distance = horizontalDistance(eye_, s.position);
        const double limit = vlosLimitM(s.visibilityM);
        check(FlightRule::VlosDistance, active && distance >= kDistanceWarningFraction * limit,
              active && distance > limit, grace,
              std::format("Line of sight: {:.0f} m from the pilot — limit {:.0f} m", distance,
                          limit),
              now, failure);

        if (active && (lastVisibilityCheckS_ < 0.0 ||
                       now - lastVisibilityCheckS_ >= kVisibilityCheckPeriodS)) {
            lastVisible_ = visible(s.position);
            lastVisibilityCheckS_ = now;
        }
        const bool hidden = active && !lastVisible_;
        check(FlightRule::VlosHidden, hidden, hidden, rules_.vlos.maxHiddenS,
              "Line of sight: the drone is hidden from the pilot", now, failure);

        const bool night = s.sunElevationDeg < kSunsetElevationDeg;
        const bool dark = active && night && !s.lightsOn;
        check(FlightRule::VlosLights, dark, dark, grace,
              "Line of sight at night needs the vehicle lights on", now, failure);
    }

    // Mission-specific rules (FR-MIS-06a).
    if (rules_.requireLightsOn) {
        const bool off = active && !s.lightsOn;
        check(FlightRule::Lights, off, off, grace, "Vehicle lights must be on", now, failure);
    }
    if (rules_.daylightOnly) {
        const bool dark = active && s.sunElevationDeg < kCivilTwilightElevationDeg;
        const bool dusk = active && s.sunElevationDeg < kCivilTwilightElevationDeg + 1.0;
        check(FlightRule::Daylight, dusk, dark, grace,
              "Flight is only allowed in daylight and civil twilight", now, failure);
    }
    return failure;
}

}  // namespace ds::missions
