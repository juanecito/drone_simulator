// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/missions/mission_runner.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

namespace ds::missions {
namespace {

using items::ItemId;
using items::ItemType;
using items::Vec3;
using physics::VehicleType;

constexpr double kSettleSpeedMs = 0.3;     // "Stopped" for landing / return home.
constexpr double kSettleTimeS = 1.0;       // Time stopped before a landing counts.
constexpr double kPickupMaxAglM = 1.0;     // Aerial pickup / drop height.
constexpr double kPickupMaxSpeedMs = 0.5;  // Speed during pickup.
constexpr double kPickupTimeS = 1.0;       // Time in the zone to attach / release cargo.
constexpr double kSpawnToleranceM = 3.0;   // Pre-flight: distance from the spawn point.
constexpr double kDefaultMaxStuckS = 30.0;
constexpr double kDefaultGroundCollisionMs = 2.0;
constexpr double kNoFlyWarningM = 10.0;
constexpr double kJerkPeriodS = 0.1;
constexpr std::size_t kMaxSurveyCells = 250'000;

double horizontal(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

double headingDifference(double a, double b) {
    double d = std::fmod(std::abs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

// Runtime state of one objective.
struct Tracker {
    const ObjectiveSpec* spec = nullptr;
    ObjectiveStatus status;
    std::vector<std::size_t> deps;  // Indices of the objectives it waits for.
    // Resolved geometry.
    ItemId target;
    ItemId pickup;
    ItemId drop;
    std::vector<ItemId> gates;
    Vec3 point{0.0};
    // Timers and counters.
    double holdS = 0.0;
    double accumulatedS = 0.0;
    double errorSum = 0.0;
    double errorTime = 0.0;
    std::size_t gatesPassed = 0;
    int wrongGates = 0;
    bool carrying = false;
    bool airborneSeen = false;
    bool departed = false;
    bool touchedDown = false;
    // Survey grid.
    Vec3 gridOrigin{0.0};
    double cellM = 1.0;
    std::size_t cols = 0;
    std::size_t rows = 0;
    std::vector<std::uint8_t> cells;  // 0 outside, 1 to cover, 2 covered.
    std::size_t toCover = 0;
    std::size_t covered = 0;
};

}  // namespace

std::string_view missionStateName(MissionState state) noexcept {
    switch (state) {
        case MissionState::Briefing: return "briefing";
        case MissionState::PreFlight: return "preFlight";
        case MissionState::InProgress: return "inProgress";
        case MissionState::Succeeded: return "succeeded";
        case MissionState::Failed: return "failed";
        case MissionState::Aborted: return "aborted";
        case MissionState::Debrief: return "debrief";
    }
    return "unknown";
}

std::string_view objectiveStateName(ObjectiveState state) noexcept {
    switch (state) {
        case ObjectiveState::Locked: return "locked";
        case ObjectiveState::Active: return "active";
        case ObjectiveState::Completed: return "completed";
        case ObjectiveState::Failed: return "failed";
    }
    return "unknown";
}

common::Json MissionResult::toJson() const {
    common::Json objectivesJson = common::Json::array();
    for (const ObjectiveStatus& o : objectives) {
        common::Json j{{"id", o.id},
                       {"type", objectiveTypeName(o.type)},
                       {"state", objectiveStateName(o.state)},
                       {"optional", o.optional},
                       {"progress", o.progress}};
        if (o.completedAtS) {
            j["completedAtS"] = *o.completedAtS;
        }
        if (o.errorM) {
            j["errorM"] = *o.errorM;
        }
        if (!o.detail.empty()) {
            j["detail"] = o.detail;
        }
        objectivesJson.push_back(std::move(j));
    }
    common::Json j{
        {"outcome", missionStateName(outcome)},
        {"durationS", durationS},
        {"batteryPct", batteryPct},
        {"meanJerk", meanJerk},
        {"collisions", collisions},
        {"ruleWarnings", ruleWarnings},
        {"wrongGates", wrongGates},
        {"stars", stars},
        {"score",
         {{"completion", score.completion},
          {"time", score.time},
          {"precision", score.precision},
          {"battery", score.battery},
          {"smoothness", score.smoothness},
          {"bonus", score.bonus},
          {"penalties", score.penalties},
          {"total", score.total}}},
        {"objectives", std::move(objectivesJson)},
        {"penalties", penalties},
    };
    if (!failureReason.empty()) {
        j["failureReason"] = failureReason;
    }
    if (worstErrorM) {
        j["worstErrorM"] = *worstErrorM;
    }
    return j;
}

void scoreResult(const MissionDefinition& mission, MissionResult& r) {
    const bool success = r.outcome == MissionState::Succeeded;
    std::size_t required = 0;
    std::size_t done = 0;
    double precisionSum = 0.0;
    int precisionCount = 0;
    r.worstErrorM.reset();
    ScoreBreakdown s;
    for (const ObjectiveStatus& o : r.objectives) {
        const bool completed = o.state == ObjectiveState::Completed;
        if (!o.optional) {
            ++required;
            done += completed ? 1U : 0U;
        } else if (completed) {
            const ObjectiveSpec* spec = mission.objective(o.id);
            s.bonus += spec != nullptr ? spec->bonus : 0.0;
        }
        if (completed && o.errorM) {
            r.worstErrorM = std::max(r.worstErrorM.value_or(0.0), *o.errorM);
            if (o.maxErrorM && *o.maxErrorM > 0.0) {
                precisionSum += std::clamp(1.0 - *o.errorM / *o.maxErrorM, 0.0, 1.0);
                ++precisionCount;
            }
        }
    }
    const double fraction = required == 0 ? 0.0 : static_cast<double>(done) / static_cast<double>(required);
    s.completion = 500.0 * fraction + (success ? 500.0 : 0.0);
    if (success) {
        double reference = 600.0;
        if (mission.rules.timeLimitS) {
            reference = *mission.rules.timeLimitS;
        } else if (!mission.scoring.stars.empty() && mission.scoring.stars.front().maxTimeS) {
            reference = 2.0 * *mission.scoring.stars.front().maxTimeS;
        }
        s.time = 300.0 * std::clamp(1.0 - r.durationS / reference, 0.0, 1.0);
        s.precision = precisionCount > 0 ? 200.0 * precisionSum / precisionCount : 200.0;
        s.battery = std::clamp(r.batteryPct, 0.0, 100.0);
        s.smoothness = 100.0 * std::exp(-r.meanJerk / 30.0);
    } else {
        s.precision = precisionCount > 0 ? 200.0 * precisionSum / precisionCount * fraction : 0.0;
    }
    r.penalties.clear();
    if (r.collisions > 0) {
        r.penalties.push_back(std::format("{} collision(s): −{}", r.collisions, 25 * r.collisions));
    }
    if (r.ruleWarnings > 0) {
        r.penalties.push_back(
            std::format("{} rule warning(s): −{}", r.ruleWarnings, 10 * r.ruleWarnings));
    }
    if (r.wrongGates > 0) {
        r.penalties.push_back(std::format("{} wrong gate(s): −{}", r.wrongGates, 25 * r.wrongGates));
    }
    s.penalties = -(25.0 * r.collisions + 10.0 * r.ruleWarnings + 25.0 * r.wrongGates);
    s.total = std::max(0.0, s.completion + s.time + s.precision + s.battery + s.smoothness +
                                s.bonus + s.penalties);
    r.score = s;

    r.stars = 0;
    if (success) {
        r.stars = 1;
        for (const StarThreshold& t : mission.scoring.stars) {
            const bool met = (!t.maxTimeS || r.durationS <= *t.maxTimeS) &&
                             (!t.maxErrorM || !r.worstErrorM || *r.worstErrorM <= *t.maxErrorM) &&
                             (!t.minBatteryPct || r.batteryPct >= *t.minBatteryPct) &&
                             (!t.minScore || s.total >= *t.minScore);
            if (!met || r.stars >= 3) {
                break;
            }
            ++r.stars;
        }
    }
}

// =======================================================================================

struct MissionRunner::Impl {
    MissionDefinition mission;
    MissionPlacement placement;
    LineOfSightFunction terrainLos;
    items::ItemRegistry registry;
    Vec3 spawn{0.0};
    Vec3 pilot{0.0};
    FlightRulesMonitor rules;
    items::TriggerDetector triggers;
    std::vector<Tracker> trackers;
    std::vector<ObjectiveStatus> statuses;  // Mirror of trackers[i].status for span access.
    MissionState state = MissionState::Briefing;
    std::vector<MissionEvent> events;
    std::optional<MissionResult> result;
    double startS = 0.0;
    double nowS = 0.0;
    double payloadKg = 0.0;
    std::size_t nextFailure = 0;
    std::vector<std::size_t> failureOrder;
    int collisions = 0;
    double lastBatteryPct = 100.0;
    bool wasOnGround = true;
    double lastDescentMs = 0.0;
    // Smoothness.
    double jerkClockS = -1.0;
    Vec3 jerkVelocity{0.0};
    std::optional<Vec3> jerkAccel;
    double jerkSum = 0.0;
    int jerkSamples = 0;

    Impl(MissionDefinition m, MissionPlacement p, LineOfSightFunction los, items::ItemRegistry r,
         Vec3 spawnPos, Vec3 pilotPos)
        : mission(std::move(m)),
          placement(std::move(p)),
          terrainLos(std::move(los)),
          registry(std::move(r)),
          spawn(spawnPos),
          pilot(pilotPos),
          rules(mission.rules, mission.vehicle.type, pilotPos, &registry, terrainLos) {
        failureOrder.resize(mission.failures.size());
        for (std::size_t i = 0; i < failureOrder.size(); ++i) {
            failureOrder[i] = i;
        }
        std::ranges::stable_sort(failureOrder, {}, [&](std::size_t i) { return mission.failures[i].atS; });
        resetTrackers();
    }

    [[nodiscard]] bool aerial() const { return mission.vehicle.type == VehicleType::Aerial; }

    void emit(MissionEvent::Kind kind, std::string id = {}, std::string text = {},
              double value = 0.0, common::Json params = {}) {
        events.push_back(MissionEvent{kind, nowS - startS, std::move(id), std::move(text), value,
                                      std::move(params)});
    }

    // ----- set-up ----------------------------------------------------------------------

    void resetTrackers() {
        trackers.clear();
        for (const ObjectiveSpec& o : mission.objectives) {
            Tracker t;
            t.spec = &o;
            t.status.id = o.id;
            t.status.type = o.type;
            t.status.title = objectiveTitle(mission, o);
            t.status.optional = o.optional;
            const auto findItem = [&](const std::string& key) {
                const items::Item* item = registry.find(key);
                return item != nullptr ? item->id : ItemId{};
            };
            t.target = findItem(o.target);
            t.pickup = findItem(o.pickup);
            t.drop = findItem(o.drop);
            for (const std::string& g : o.gates) {
                t.gates.push_back(findItem(g));
            }
            if (const items::Item* item = registry.get(t.target)) {
                t.point = item->transform.toWorld(item->shape.offsetM);
            } else if (o.position) {
                if (auto p = worldPosition(*o.position, mission, placement)) {
                    t.point = *p;
                }
            }
            switch (o.type) {
                case ObjectiveType::Land:
                case ObjectiveType::Park: t.status.maxErrorM = o.maxErrorM.value_or(0.5); break;
                case ObjectiveType::Hover: t.status.maxErrorM = o.toleranceM.value_or(1.0); break;
                case ObjectiveType::ReachWaypoint: t.status.maxErrorM = waypointRadius(t); break;
                case ObjectiveType::ReturnHome: t.status.maxErrorM = o.radiusM.value_or(2.0); break;
                case ObjectiveType::Deliver: {
                    const items::Item* d = registry.get(t.drop);
                    t.status.maxErrorM = d != nullptr && d->shape.radiusM > 0.0
                                             ? d->shape.radiusM
                                             : std::max(1.0, d != nullptr ? d->shape.halfExtentsM.x : 1.0);
                    break;
                }
                default: break;
            }
            if (o.type == ObjectiveType::Survey) {
                buildSurveyGrid(t);
            }
            trackers.push_back(std::move(t));
        }
        // Dependencies: `after`, plus the earlier required objectives in sequential order.
        for (std::size_t i = 0; i < trackers.size(); ++i) {
            for (const std::string& dep : trackers[i].spec->after) {
                for (std::size_t k = 0; k < trackers.size(); ++k) {
                    if (trackers[k].spec->id == dep) {
                        trackers[i].deps.push_back(k);
                    }
                }
            }
            if (mission.objectiveOrder == ObjectiveOrder::Sequential) {
                for (std::size_t k = 0; k < i; ++k) {
                    if (!trackers[k].spec->optional) {
                        trackers[i].deps.push_back(k);
                    }
                }
            }
        }
        syncStatuses();
    }

    double waypointRadius(const Tracker& t) const {
        if (t.spec->radiusM) {
            return *t.spec->radiusM;
        }
        const items::Item* item = registry.get(t.target);
        return item != nullptr ? item->shape.radiusM : 2.0;
    }

    void buildSurveyGrid(Tracker& t) {
        const items::Item* area = registry.get(t.target);
        if (area == nullptr) {
            return;
        }
        Vec3 lo;
        Vec3 hi;
        area->worldBounds(lo, hi);
        const double swath = t.spec->swathWidthM.value_or(aerial() ? 20.0 : 2.0);
        t.cellM = std::max(swath / 4.0, 0.5);
        const double w = hi.x - lo.x;
        const double l = hi.z - lo.z;
        while ((w / t.cellM + 1.0) * (l / t.cellM + 1.0) > static_cast<double>(kMaxSurveyCells)) {
            t.cellM *= 1.5;
        }
        t.cols = static_cast<std::size_t>(std::ceil(w / t.cellM)) + 1U;
        t.rows = static_cast<std::size_t>(std::ceil(l / t.cellM)) + 1U;
        t.gridOrigin = lo;
        t.cells.assign(t.cols * t.rows, 0);
        const double y = std::clamp(0.0, area->shape.bottomM, area->shape.topM);
        for (std::size_t r = 0; r < t.rows; ++r) {
            for (std::size_t c = 0; c < t.cols; ++c) {
                const Vec3 world(lo.x + (static_cast<double>(c) + 0.5) * t.cellM, 0.0,
                                 lo.z + (static_cast<double>(r) + 0.5) * t.cellM);
                Vec3 local = area->transform.toLocal(world);
                local.y = y;
                if (area->shape.contains(local)) {
                    t.cells[r * t.cols + c] = 1;
                    ++t.toCover;
                }
            }
        }
    }

    void syncStatuses() {
        statuses.clear();
        for (const Tracker& t : trackers) {
            statuses.push_back(t.status);
        }
    }

    // ----- lifecycle -------------------------------------------------------------------

    void start(const VehicleSample& s) {
        state = MissionState::InProgress;
        startS = s.timeS;
        nowS = s.timeS;
        triggers.reset(registry, s.position);
        rules.reset();
        wasOnGround = s.onGround;
        lastBatteryPct = s.batteryPct;
        emit(MissionEvent::Kind::Started);
        unlockObjectives();
    }

    void unlockObjectives() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (Tracker& t : trackers) {
                if (t.status.state != ObjectiveState::Locked) {
                    continue;
                }
                bool ready = true;
                bool blocked = false;
                for (const std::size_t d : t.deps) {
                    const ObjectiveState ds = trackers[d].status.state;
                    ready = ready && ds == ObjectiveState::Completed;
                    blocked = blocked || ds == ObjectiveState::Failed;
                }
                if (blocked) {
                    failObjective(t, "a required earlier objective failed");
                    changed = true;
                } else if (ready) {
                    t.status.state = ObjectiveState::Active;
                    emit(MissionEvent::Kind::ObjectiveActive, t.spec->id, t.status.title);
                    changed = true;
                }
            }
        }
    }

    void completeObjective(Tracker& t, std::optional<double> errorM = std::nullopt) {
        t.status.state = ObjectiveState::Completed;
        t.status.progress = 1.0;
        t.status.completedAtS = nowS - startS;
        if (errorM) {
            t.status.errorM = errorM;
        }
        emit(MissionEvent::Kind::ObjectiveCompleted, t.spec->id, t.status.title);
    }

    void failObjective(Tracker& t, std::string reason) {
        t.status.state = ObjectiveState::Failed;
        t.status.detail = reason;
        emit(MissionEvent::Kind::ObjectiveFailed, t.spec->id, std::move(reason));
        if (t.carrying) {
            t.carrying = false;
            payloadKg = 0.0;
            emit(MissionEvent::Kind::PayloadReleased, t.spec->id);
        }
    }

    void finish(MissionState outcome, std::string reason, const VehicleSample& s) {
        state = outcome;
        for (Tracker& t : trackers) {
            if (t.carrying) {
                t.carrying = false;
                payloadKg = 0.0;
            }
        }
        syncStatuses();
        MissionResult r;
        r.outcome = outcome;
        r.failureReason = std::move(reason);
        r.durationS = nowS - startS;
        r.batteryPct = s.batteryPct;
        r.meanJerk = jerkSamples > 0 ? jerkSum / jerkSamples : 0.0;
        r.collisions = collisions;
        r.ruleWarnings = rules.warningEpisodes();
        for (const Tracker& t : trackers) {
            r.wrongGates += t.wrongGates;
        }
        r.objectives = statuses;
        scoreResult(mission, r);
        result = std::move(r);
        switch (outcome) {
            case MissionState::Succeeded: emit(MissionEvent::Kind::Succeeded); break;
            case MissionState::Aborted: emit(MissionEvent::Kind::Aborted); break;
            default: emit(MissionEvent::Kind::Failed, {}, result->failureReason); break;
        }
    }

    // ----- one tick --------------------------------------------------------------------

    void tick(const VehicleSample& s, std::span<const VehicleIncident> incidents) {
        const double dt = std::max(0.0, s.timeS - nowS);
        nowS = s.timeS;
        const double t = nowS - startS;
        const double speed = glm::length(s.velocity);

        updateSmoothness(s);
        injectFailures(t);

        // Incidents.
        for (const VehicleIncident& i : incidents) {
            switch (i.kind) {
                case VehicleIncident::Kind::Collision:
                    ++collisions;
                    if (!aerial() && i.speedMs > mission.rules.maxCollisionSpeedMs.value_or(
                                                     kDefaultGroundCollisionMs)) {
                        return finish(MissionState::Failed,
                                      std::format("collision at {:.1f} m/s", i.speedMs), s);
                    }
                    break;
                case VehicleIncident::Kind::Crash:
                    return finish(MissionState::Failed,
                                  i.text.empty() ? std::string("crashed")
                                                 : std::format("crashed: {}", i.text),
                                  s);
                case VehicleIncident::Kind::WaterEntered:
                    return finish(MissionState::Failed, "the vehicle went into the water", s);
                case VehicleIncident::Kind::Rollover:
                    return finish(MissionState::Failed, "the vehicle rolled over", s);
            }
        }
        if (auto reason = vehicleFailure(s)) {
            return finish(MissionState::Failed, std::move(*reason), s);
        }
        if (auto reason = zoneFailure(s)) {
            return finish(MissionState::Failed, std::move(*reason), s);
        }
        const FlightRuleSample rs{nowS, s.position, s.aglM, s.armed, s.lightsOn, s.sunElevationDeg,
                                  s.visibilityM};
        if (const auto broken = rules.update(rs)) {
            return finish(MissionState::Failed, std::format("rule broken: {}", broken->message), s);
        }
        if (mission.rules.timeLimitS && t > *mission.rules.timeLimitS) {
            return finish(MissionState::Failed, "time limit exceeded", s);
        }

        // Objectives.
        const auto gateEvents = triggers.update(registry, s.position);
        for (Tracker& tr : trackers) {
            if (tr.status.state == ObjectiveState::Active) {
                evaluate(tr, s, speed, dt, gateEvents);
            }
        }
        unlockObjectives();
        if (!s.onGround) {
            lastDescentMs = -s.velocity.y;
        }
        wasOnGround = s.onGround;
        lastBatteryPct = s.batteryPct;
        syncStatuses();

        for (const Tracker& tr : trackers) {
            if (!tr.spec->optional && tr.status.state == ObjectiveState::Failed) {
                return finish(MissionState::Failed,
                              std::format("objective \"{}\" failed: {}", tr.status.title,
                                          tr.status.detail),
                              s);
            }
        }
        const bool allDone = std::ranges::all_of(trackers, [](const Tracker& tr) {
            return tr.spec->optional || tr.status.state == ObjectiveState::Completed;
        });
        if (allDone) {
            return finish(MissionState::Succeeded, {}, s);
        }
        // Mission end at sunset / sunrise (FR-MIS-06a).
        if (mission.rules.endAt == EndAt::Sunset && s.sunElevationDeg < kSunsetElevationDeg) {
            return finish(MissionState::Failed, "the sun has set", s);
        }
        if (mission.rules.endAt == EndAt::Sunrise && s.sunElevationDeg > kSunsetElevationDeg) {
            return finish(MissionState::Failed, "the sun has risen", s);
        }
    }

    std::optional<std::string> vehicleFailure(const VehicleSample& s) const {
        if (s.crashed) {
            return "crashed";
        }
        if (s.inWater) {
            return "the vehicle went into the water";
        }
        if (s.batteryPct <= 0.0) {
            return "battery depleted";
        }
        if (mission.rules.minBatteryPct > 0.0 && s.batteryPct < mission.rules.minBatteryPct) {
            return std::format("battery below {:g} %", mission.rules.minBatteryPct);
        }
        if (!aerial()) {
            if (s.rolledOver) {
                return "the vehicle rolled over";
            }
            if (s.stuckS > mission.rules.maxStuckS.value_or(kDefaultMaxStuckS)) {
                return std::format("stuck for more than {:g} s",
                                   mission.rules.maxStuckS.value_or(kDefaultMaxStuckS));
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> zoneFailure(const VehicleSample& s) const {
        bool anyFence = false;
        bool insideFence = false;
        for (const items::Item* item : registry.itemsOfType(ItemType::Geofence)) {
            if (!item->enabled) {
                continue;
            }
            anyFence = true;
            insideFence = insideFence || item->contains(s.position);
        }
        if (anyFence && !insideFence) {
            return "left the geofence";
        }
        for (const items::Item* item : registry.itemsOfType(ItemType::NoFlyZone)) {
            if (item->enabled && item->contains(s.position)) {
                return std::format("entered the no-fly zone \"{}\"", item->name);
            }
        }
        if (mission.terrain.extent && !mission.terrain.extent->contains(s.position.x, s.position.z)) {
            return "left the mission area";
        }
        return std::nullopt;
    }

    void injectFailures(double t) {
        while (nextFailure < failureOrder.size() &&
               mission.failures[failureOrder[nextFailure]].atS <= t) {
            const FailureSpec& f = mission.failures[failureOrder[nextFailure]];
            emit(MissionEvent::Kind::FailureInjected, {}, f.type, 0.0, f.params);
            ++nextFailure;
        }
    }

    void updateSmoothness(const VehicleSample& s) {
        if (s.onGround) {
            jerkAccel.reset();
            jerkClockS = -1.0;
            return;
        }
        if (jerkClockS < 0.0) {
            jerkClockS = s.timeS;
            jerkVelocity = s.velocity;
            return;
        }
        const double dt = s.timeS - jerkClockS;
        if (dt < kJerkPeriodS) {
            return;
        }
        const Vec3 accel = (s.velocity - jerkVelocity) / dt;
        if (jerkAccel) {
            jerkSum += glm::length(accel - *jerkAccel) / dt;
            ++jerkSamples;
        }
        jerkAccel = accel;
        jerkVelocity = s.velocity;
        jerkClockS = s.timeS;
    }

    // ----- objectives ------------------------------------------------------------------

    void hold(Tracker& t, bool condition, double dt, double needS) {
        t.holdS = condition ? t.holdS + dt : 0.0;
        t.status.progress = needS > 0.0 ? std::min(t.holdS / needS, 0.99) : 0.0;
    }

    void evaluate(Tracker& t, const VehicleSample& s, double speed, double dt,
                  const std::vector<items::TriggerEvent>& triggerEvents) {
        const ObjectiveSpec& o = *t.spec;
        if (!s.onGround) {
            t.airborneSeen = true;
        }
        switch (o.type) {
            case ObjectiveType::ReachAltitude: {
                const double target = o.altitudeAglM.value_or(0.0);
                const double need = o.holdS.value_or(0.0);
                const bool ok = s.aglM >= target - o.toleranceM.value_or(0.5);
                hold(t, ok, dt, need);
                if (ok && t.holdS >= need) {
                    completeObjective(t);
                } else if (!ok) {
                    t.status.progress = std::clamp(s.aglM / target, 0.0, 0.99);
                }
                break;
            }
            case ObjectiveType::ReachWaypoint: {
                const double radius = waypointRadius(t);
                const double distance = aerial() ? glm::length(s.position - t.point)
                                                 : horizontal(s.position, t.point);
                const bool inBand = (!o.minAltitudeAglM || s.aglM >= *o.minAltitudeAglM) &&
                                    (!o.maxAltitudeAglM || s.aglM <= *o.maxAltitudeAglM);
                const bool slow = !o.maxSpeedMs || speed <= *o.maxSpeedMs;
                const bool ok = distance <= radius && inBand && slow;
                const double need = o.holdS.value_or(0.0);
                hold(t, ok, dt, need);
                t.status.detail = distance <= radius && !inBand ? "outside the altitude band"
                                  : distance <= radius && !slow ? "too fast"
                                                                : std::format("{:.0f} m", distance);
                if (ok && t.holdS >= need) {
                    completeObjective(t, distance);
                }
                break;
            }
            case ObjectiveType::Hover: {
                const double error = horizontal(s.position, t.point);
                const bool posOk = error <= o.toleranceM.value_or(1.0);
                const bool altOk =
                    o.altitudeAglM
                        ? std::abs(s.aglM - *o.altitudeAglM) <= o.altitudeToleranceM.value_or(1.0)
                        : s.aglM > 0.5;
                const bool ok = posOk && altOk && !s.onGround && s.armed;
                if (!ok) {
                    t.errorSum = 0.0;
                    t.errorTime = 0.0;
                } else {
                    t.errorSum += error * dt;
                    t.errorTime += dt;
                }
                const double need = o.durationS.value_or(5.0);
                hold(t, ok, dt, need);
                if (ok && t.holdS >= need) {
                    completeObjective(t, t.errorTime > 0.0 ? t.errorSum / t.errorTime : error);
                }
                break;
            }
            case ObjectiveType::Land: evaluateLanding(t, s, speed, dt); break;
            case ObjectiveType::GateCourse: evaluateGates(t, triggerEvents); break;
            case ObjectiveType::Deliver: evaluateDelivery(t, s, speed, dt); break;
            case ObjectiveType::Inspect: {
                const items::Item* target = registry.get(t.target);
                if (target == nullptr) {
                    break;
                }
                const Vec3 d = t.point - s.position;
                const double distance = glm::length(d);
                const double maxAngle = o.maxAngleDeg.value_or(20.0);
                bool ok = distance <= o.maxDistanceM.value_or(30.0) && distance > 1e-6;
                if (ok) {
                    const double cosAngle = glm::dot(d / distance, glm::normalize(s.cameraForward));
                    ok = std::acos(std::clamp(cosAngle, -1.0, 1.0)) * 180.0 / std::numbers::pi <= maxAngle;
                }
                if (ok && terrainLos) {
                    ok = terrainLos(s.position, t.point);
                }
                if (ok) {
                    const auto hit = registry.raycast(s.position, d / distance, distance);
                    ok = !hit || hit->id == t.target || hit->distanceM > distance - 0.5;
                }
                if (ok) {
                    t.accumulatedS += dt;
                }
                const double need = o.durationS.value_or(5.0);
                t.status.progress = std::min(t.accumulatedS / need, 0.99);
                t.status.detail = std::format("{:.1f} of {:g} s in view", t.accumulatedS, need);
                if (t.accumulatedS >= need) {
                    completeObjective(t);
                }
                break;
            }
            case ObjectiveType::Survey: evaluateSurvey(t, s); break;
            case ObjectiveType::ReturnHome: {
                const double radius = o.radiusM.value_or(2.0);
                const double distance = horizontal(s.position, spawn);
                if (distance > radius || !s.onGround) {
                    t.departed = true;
                }
                const bool settled = s.onGround && speed < (aerial() ? kSettleSpeedMs : 0.1);
                const bool ok = t.departed && distance <= radius && settled;
                hold(t, ok, dt, kSettleTimeS);
                t.status.detail = std::format("{:.0f} m from home", distance);
                if (ok && t.holdS >= kSettleTimeS) {
                    completeObjective(t, distance);
                }
                break;
            }
            case ObjectiveType::Park: {
                const items::Item* zone = registry.get(t.target);
                if (zone == nullptr) {
                    break;
                }
                const double error = horizontal(s.position, t.point);
                const double headingError =
                    headingDifference(s.headingDeg, items::headingOf(zone->transform.rotation));
                Vec3 local = zone->transform.toLocal(s.position);
                local.y = zone->shape.offsetM.y;
                const bool inside = zone->shape.contains(local);
                const bool ok = inside && error <= o.maxErrorM.value_or(0.5) &&
                                headingError <= o.maxHeadingErrorDeg.value_or(10.0) &&
                                speed <= o.maxSpeedMs.value_or(0.1);
                const double need = o.holdS.value_or(1.0);
                hold(t, ok, dt, need);
                t.status.detail = inside ? std::format("{:.2f} m, {:.0f}° off", error, headingError)
                                         : std::format("{:.0f} m away", error);
                if (ok && t.holdS >= need) {
                    completeObjective(t, error);
                }
                break;
            }
        }
    }

    void evaluateLanding(Tracker& t, const VehicleSample& s, double speed, double dt) {
        const ObjectiveSpec& o = *t.spec;
        const items::Item* pad = registry.get(t.target);
        if (pad == nullptr) {
            return;
        }
        const double error = horizontal(s.position, t.point);
        const bool onPad = error <= pad->shape.radiusM;
        // Touchdown: the first ground contact after being airborne.
        if (s.onGround && !wasOnGround && t.airborneSeen && onPad) {
            t.touchedDown = true;
            const double limit = o.maxTouchdownSpeedMs.value_or(1.0);
            if (lastDescentMs > limit) {
                failObjective(t, std::format("hard landing: {:.1f} m/s (limit {:g} m/s)",
                                             lastDescentMs, limit));
                return;
            }
        }
        if (!s.onGround) {
            t.touchedDown = false;
        }
        const double maxError = o.maxErrorM.value_or(0.5);
        bool headingOk = true;
        if (o.maxHeadingErrorDeg) {
            headingOk = headingDifference(s.headingDeg, items::headingOf(pad->transform.rotation)) <=
                        *o.maxHeadingErrorDeg;
        }
        const bool ok = t.touchedDown && s.onGround && speed < kSettleSpeedMs && error <= maxError &&
                        headingOk;
        hold(t, ok, dt, kSettleTimeS);
        if (t.touchedDown && s.onGround) {
            t.status.detail = error > maxError ? std::format("{:.2f} m from the centre (max {:g} m)",
                                                             error, maxError)
                              : !headingOk     ? std::string("wrong heading")
                                               : std::format("{:.2f} m from the centre", error);
        } else {
            t.status.detail = std::format("{:.0f} m to the pad", error);
        }
        if (ok && t.holdS >= kSettleTimeS) {
            completeObjective(t, error);
        }
    }

    void evaluateGates(Tracker& t, const std::vector<items::TriggerEvent>& triggerEvents) {
        const ObjectiveSpec& o = *t.spec;
        const std::size_t total = t.gates.size() * static_cast<std::size_t>(o.laps);
        for (const items::TriggerEvent& e : triggerEvents) {
            if (e.kind != items::TriggerEvent::Kind::GatePassed || t.gatesPassed >= total) {
                continue;
            }
            const ItemId expected = t.gates[t.gatesPassed % t.gates.size()];
            if (e.id == expected && !e.reversed) {
                ++t.gatesPassed;
                emit(MissionEvent::Kind::GatePassed, registry.get(e.id)->key, {},
                     static_cast<double>(t.gatesPassed));
            } else if (std::ranges::find(t.gates, e.id) != t.gates.end()) {
                ++t.wrongGates;
                emit(MissionEvent::Kind::WrongGate, registry.get(e.id)->key,
                     e.reversed ? "gate passed in the wrong direction" : "wrong gate");
                if (o.strictOrder) {
                    failObjective(t, e.reversed ? "gate passed in the wrong direction"
                                                : "gates passed out of order");
                    return;
                }
            }
        }
        t.status.progress = total == 0 ? 0.0 : static_cast<double>(t.gatesPassed) / static_cast<double>(total);
        t.status.detail = std::format("gate {} of {}", std::min(t.gatesPassed + 1, total), total);
        if (t.gatesPassed >= total) {
            completeObjective(t);
        }
    }

    void evaluateDelivery(Tracker& t, const VehicleSample& s, double speed, double dt) {
        const ObjectiveSpec& o = *t.spec;
        const items::Item* pickup = registry.get(t.pickup);
        const items::Item* drop = registry.get(t.drop);
        if (pickup == nullptr || drop == nullptr) {
            return;
        }
        const bool low = aerial() ? s.aglM <= kPickupMaxAglM : true;
        if (!t.carrying) {
            // Another delivery may already be carrying cargo: one payload at a time.
            if (payloadKg > 0.0) {
                return;
            }
            const bool ok = pickup->contains(s.position) && low &&
                            speed <= (aerial() ? kPickupMaxSpeedMs : kSettleSpeedMs);
            hold(t, ok, dt, 2.0 * kPickupTimeS);
            t.status.detail = ok ? std::string("loading…") : std::format("go to {}", pickup->name);
            if (ok && t.holdS >= kPickupTimeS) {
                t.carrying = true;
                t.holdS = 0.0;
                payloadKg = o.cargoKg;
                t.status.progress = 0.5;
                emit(MissionEvent::Kind::PayloadAttached, o.id, {}, o.cargoKg);
            }
            return;
        }
        const Vec3 dropCentre = drop->transform.toWorld(drop->shape.offsetM);
        Vec3 local = drop->transform.toLocal(s.position);
        if (drop->type == ItemType::ParkingZone || drop->type == ItemType::LandingPad) {
            local.y = drop->shape.kind == items::Shape::Kind::Box ? drop->shape.offsetM.y : 0.0;
        }
        const bool inside = drop->shape.contains(local);
        const bool ok = inside && low && speed <= o.maxDropSpeedMs.value_or(kPickupMaxSpeedMs);
        hold(t, ok, dt, 2.0 * kPickupTimeS);
        t.status.progress = 0.5 + 0.5 * std::min(t.holdS / kPickupTimeS, 0.99);
        t.status.detail = ok ? std::string("unloading…") : std::format("carry it to {}", drop->name);
        if (ok && t.holdS >= kPickupTimeS) {
            t.carrying = false;
            payloadKg = 0.0;
            emit(MissionEvent::Kind::PayloadReleased, o.id);
            completeObjective(t, horizontal(s.position, dropCentre));
        }
    }

    void evaluateSurvey(Tracker& t, const VehicleSample& s) {
        const ObjectiveSpec& o = *t.spec;
        if (t.toCover == 0) {
            return;
        }
        const bool inBand = (!o.minAltitudeAglM || s.aglM >= *o.minAltitudeAglM) &&
                            (!o.maxAltitudeAglM || s.aglM <= *o.maxAltitudeAglM) &&
                            (!aerial() || !s.onGround);
        if (inBand) {
            const double half = 0.5 * o.swathWidthM.value_or(aerial() ? 20.0 : 2.0);
            const auto toCell = [&](double v, double origin) {
                return static_cast<std::int64_t>(std::floor((v - origin) / t.cellM));
            };
            const auto c0 = std::max<std::int64_t>(0, toCell(s.position.x - half, t.gridOrigin.x));
            const auto c1 = std::min<std::int64_t>(static_cast<std::int64_t>(t.cols) - 1,
                                                   toCell(s.position.x + half, t.gridOrigin.x));
            const auto r0 = std::max<std::int64_t>(0, toCell(s.position.z - half, t.gridOrigin.z));
            const auto r1 = std::min<std::int64_t>(static_cast<std::int64_t>(t.rows) - 1,
                                                   toCell(s.position.z + half, t.gridOrigin.z));
            for (std::int64_t r = r0; r <= r1; ++r) {
                for (std::int64_t c = c0; c <= c1; ++c) {
                    const double cx = t.gridOrigin.x + (static_cast<double>(c) + 0.5) * t.cellM;
                    const double cz = t.gridOrigin.z + (static_cast<double>(r) + 0.5) * t.cellM;
                    std::uint8_t& cell = t.cells[static_cast<std::size_t>(r) * t.cols +
                                                 static_cast<std::size_t>(c)];
                    if (cell == 1 && std::hypot(cx - s.position.x, cz - s.position.z) <= half) {
                        cell = 2;
                        ++t.covered;
                    }
                }
            }
        }
        const double fraction = static_cast<double>(t.covered) / static_cast<double>(t.toCover);
        const double need = o.coveragePct.value_or(90.0) / 100.0;
        t.status.progress = std::min(fraction / need, 1.0);
        t.status.detail = std::format("{:.0f} % covered", 100.0 * fraction);
        if (fraction >= need) {
            completeObjective(t);
        }
    }
};

// =======================================================================================

common::Result<std::unique_ptr<MissionRunner>> MissionRunner::create(
    MissionDefinition mission, const MissionPlacement& placement,
    LineOfSightFunction terrainLineOfSight) {
    auto registry = buildItems(mission, placement);
    if (!registry) {
        return std::unexpected(registry.error());
    }
    MissionPosition spawnOnGround = mission.spawn;
    spawnOnGround.altitudeRef = items::AltitudeRef::Agl;
    spawnOnGround.altitudeM = 0.0;
    auto spawn = worldPosition(spawnOnGround, mission, placement);
    if (!spawn) {
        return common::makeError(std::format("spawn: {}", spawn.error().message));
    }
    Vec3 pilot = *spawn;
    if (const auto pilots = registry->itemsOfType(ItemType::PilotPosition); !pilots.empty()) {
        pilot = pilots.front()->transform.position;
        if (placement.ground) {
            pilot.y = placement.ground(pilot.x, pilot.z);
        }
    }
    auto impl = std::make_unique<Impl>(std::move(mission), placement, std::move(terrainLineOfSight),
                                       std::move(*registry), *spawn, pilot);
    return std::unique_ptr<MissionRunner>(new MissionRunner(std::move(impl)));
}

MissionRunner::MissionRunner(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MissionRunner::~MissionRunner() = default;

const MissionDefinition& MissionRunner::mission() const noexcept { return impl_->mission; }
const items::ItemRegistry& MissionRunner::items() const noexcept { return impl_->registry; }
Vec3 MissionRunner::spawnPosition() const noexcept { return impl_->spawn; }
double MissionRunner::spawnHeadingDeg() const noexcept { return impl_->mission.spawnHeadingDeg; }
Vec3 MissionRunner::pilotPosition() const noexcept { return impl_->pilot; }
MissionState MissionRunner::state() const noexcept { return impl_->state; }

void MissionRunner::beginPreFlight() {
    if (impl_->state == MissionState::Briefing) {
        impl_->state = MissionState::PreFlight;
    }
}

std::vector<std::string> MissionRunner::preflightProblems(const VehicleSample& s) const {
    std::vector<std::string> problems;
    if (s.crashed) {
        problems.emplace_back("the vehicle is damaged: reset it");
    }
    if (!s.onGround) {
        problems.emplace_back("the vehicle must be on the ground");
    }
    if (const double d = horizontal(s.position, impl_->spawn); d > kSpawnToleranceM) {
        problems.push_back(std::format("the vehicle is {:.0f} m from the start point", d));
    }
    if (s.batteryPct < 99.0) {
        problems.emplace_back("the battery is not full");
    }
    return problems;
}

void MissionRunner::update(const VehicleSample& sample, std::span<const VehicleIncident> incidents) {
    Impl& m = *impl_;
    if (m.state == MissionState::PreFlight) {
        const bool started = sample.armed || (!m.aerial() && glm::length(sample.velocity) > 0.2);
        if (!started) {
            return;
        }
        m.start(sample);
    }
    if (m.state == MissionState::InProgress) {
        m.tick(sample, incidents);
    }
}

void MissionRunner::abort() {
    Impl& m = *impl_;
    if (m.state == MissionState::InProgress || m.state == MissionState::PreFlight) {
        VehicleSample s;
        s.timeS = m.nowS;
        s.batteryPct = m.lastBatteryPct;
        m.finish(MissionState::Aborted, "aborted by the pilot", s);
    }
}

void MissionRunner::enterDebrief() {
    Impl& m = *impl_;
    if (m.state == MissionState::Succeeded || m.state == MissionState::Failed ||
        m.state == MissionState::Aborted) {
        m.state = MissionState::Debrief;
    }
}

void MissionRunner::retry() {
    Impl& m = *impl_;
    m.state = MissionState::Briefing;
    m.events.clear();
    m.result.reset();
    m.startS = m.nowS = 0.0;
    m.payloadKg = 0.0;
    m.nextFailure = 0;
    m.collisions = 0;
    m.lastBatteryPct = 100.0;
    m.wasOnGround = true;
    m.lastDescentMs = 0.0;
    m.jerkClockS = -1.0;
    m.jerkAccel.reset();
    m.jerkSum = 0.0;
    m.jerkSamples = 0;
    m.triggers = items::TriggerDetector{};
    m.rules.reset();
    m.resetTrackers();
}

std::span<const ObjectiveStatus> MissionRunner::objectives() const noexcept {
    return impl_->statuses;
}
const std::vector<RuleWarning>& MissionRunner::warnings() const noexcept {
    return impl_->rules.warnings();
}
std::vector<MissionEvent> MissionRunner::takeEvents() { return std::exchange(impl_->events, {}); }
double MissionRunner::elapsedS() const noexcept {
    return impl_->state == MissionState::Briefing || impl_->state == MissionState::PreFlight
               ? 0.0
               : impl_->nowS - impl_->startS;
}
double MissionRunner::payloadKg() const noexcept { return impl_->payloadKg; }

std::optional<double> MissionRunner::surveyCoverage(std::string_view objectiveId) const {
    for (const Tracker& t : impl_->trackers) {
        if (t.spec->id == objectiveId && t.spec->type == ObjectiveType::Survey && t.toCover > 0) {
            return static_cast<double>(t.covered) / static_cast<double>(t.toCover);
        }
    }
    return std::nullopt;
}

const std::optional<MissionResult>& MissionRunner::result() const noexcept { return impl_->result; }

}  // namespace ds::missions
