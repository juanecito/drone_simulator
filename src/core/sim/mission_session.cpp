// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/mission_session.hpp"

#include <cmath>
#include <format>
#include <utility>

#include "ds/common/math.hpp"
#include "ds/sim/world.hpp"

namespace ds::sim {

std::optional<physics::Failure> missionFailure(const missions::FailureSpec& spec) {
    const auto type = physics::parseFailureType(spec.type);
    if (!type) {
        return std::nullopt;
    }
    physics::Failure f = physics::defaultFailure(*type);
    const common::Json& p = spec.params;
    const auto number = [&p](const char* key, double fallback) {
        return p.contains(key) && p[key].is_number() ? p[key].get<double>() : fallback;
    };
    if (p.contains("durationS") && p["durationS"].is_number()) {
        f.durationS = p["durationS"].get<double>();
    }
    f.magnitudeM = number("magnitudeM", f.magnitudeM);
    f.driftMs = number("driftMs", f.driftMs);
    f.motor = static_cast<int>(std::lround(number("motor", f.motor)));
    f.efficiency = number("efficiency", f.efficiency);
    f.fraction = number("fraction", f.fraction);
    f.offsetDeg = number("offsetDeg", f.offsetDeg);
    return f;
}
namespace {

using missions::MissionEvent;
using missions::MissionState;

// Payload hung just under the body: offset (FRD, metres) and a drag area growing with size.
constexpr physics::Vec3 kPayloadOffsetBody{0.0, 0.0, 0.06};

double payloadDragArea(double massKg) { return 0.01 + 0.02 * std::cbrt(massKg * massKg); }

}  // namespace

MissionSession::MissionSession(std::unique_ptr<missions::MissionRunner> runner)
    : runner_(std::move(runner)) {
    runner_->beginPreFlight();
}

MissionSession::~MissionSession() = default;

missions::VehicleSample MissionSession::sample(const World& world) {
    const physics::Vehicle& v = world.vehicle();
    const physics::RigidBodyState& body = v.bodyState();
    const physics::VehicleTelemetry t = v.telemetry();
    missions::VehicleSample s;
    s.timeS = world.time();
    s.position = body.position;
    s.velocity = body.velocity;
    s.headingDeg = common::normalizeDegrees(
        common::radToDeg(physics::eulerFromOrientation(body.orientation).yaw));
    const double ground = world.config().ground(body.position.x, body.position.z).heightM;
    s.aglM = body.position.y - ground - v.restHeightM();
    s.armed = t.armed;
    s.onGround = t.onGround;
    s.crashed = t.crashed;
    s.batteryPct = 100.0 * t.battery.stateOfCharge;
    s.lightsOn = world.controls().lights;
    // FPV camera axis: forward, tilted up (body FRD: up is −z).
    const double uptilt = common::degToRad(v.camera().uptiltDeg);
    s.cameraForward = glm::normalize(
        physics::bodyToWorld(body.orientation, {std::cos(uptilt), 0.0, -std::sin(uptilt)}));
    s.sunElevationDeg = world.sunElevationDeg();
    s.visibilityM = world.visibilityAtVehicle();
    if (const auto water = world.config().waterLevelM) {
        s.inWater = body.position.y < *water;
    }
    if (t.type == physics::VehicleType::Ground) {
        s.rolledOver = t.ground.rolledOver;
        s.stuckS = t.ground.stuckS;
        s.inWater = s.inWater || t.ground.inWater;
    }
    return s;
}

void MissionSession::tick(World& world, std::span<const missions::VehicleIncident> incidents) {
    const MissionState before = runner_->state();
    if (before != MissionState::PreFlight && before != MissionState::InProgress) {
        return;
    }
    const missions::VehicleSample s = sample(world);
    if (before == MissionState::PreFlight) {
        preflightProblems_ = runner_->preflightProblems(s);
    }
    runner_->update(s, incidents);
    if (runner_->state() != MissionState::PreFlight) {
        preflightProblems_.clear();
    }
    handleMissionEvents(world);
    if (before == MissionState::PreFlight && runner_->state() == MissionState::InProgress) {
        path_.clear();
        nextPathS_ = 0.0;
    }
    if (runner_->state() == MissionState::InProgress ||
        (before == MissionState::InProgress && runner_->result())) {
        recordPath(world);
    }
}

void MissionSession::recordPath(const World& world) {
    if (world.time() < nextPathS_ && runner_->state() == MissionState::InProgress) {
        return;
    }
    nextPathS_ = world.time() + kPathSampleS;
    if (path_.size() >= kMaxPathPoints) {
        // Keep the whole flight: drop every other point and sample half as often.
        std::vector<glm::dvec3> thinned;
        thinned.reserve(path_.size() / 2 + 1);
        for (std::size_t i = 0; i < path_.size(); i += 2) {
            thinned.push_back(path_[i]);
        }
        path_ = std::move(thinned);
    }
    path_.push_back(world.vehicle().bodyState().position);
}

void MissionSession::handleMissionEvents(World& world) {
    const auto titleOf = [this](const std::string& id) {
        for (const missions::ObjectiveStatus& o : runner_->objectives()) {
            if (o.id == id) {
                return o.title;
            }
        }
        return id;
    };
    const auto itemName = [this](const std::string& key) {
        const items::Item* item = runner_->items().find(key);
        return item == nullptr || item->name.empty() ? key : item->name;
    };
    EventBus& bus = world.events();
    for (MissionEvent& e : runner_->takeEvents()) {
        switch (e.kind) {
            case MissionEvent::Kind::Started:
                bus.publish(MissionMessageEvent{"Mission started"});
                break;
            case MissionEvent::Kind::ObjectiveActive: break;  // The HUD lists it.
            case MissionEvent::Kind::ObjectiveCompleted:
                bus.publish(ObjectiveCompletedEvent{e.id, titleOf(e.id)});
                break;
            case MissionEvent::Kind::ObjectiveFailed:
                bus.publish(ObjectiveFailedEvent{e.id, e.text, titleOf(e.id)});
                break;
            case MissionEvent::Kind::GatePassed:
                bus.publish(MissionMessageEvent{
                    std::format("Gate passed: {} ({:.0f})", itemName(e.id), e.value)});
                break;
            case MissionEvent::Kind::WrongGate:
                bus.publish(MissionMessageEvent{"Wrong gate: " + itemName(e.id)});
                break;
            case MissionEvent::Kind::PayloadAttached:
                world.vehicle().attachPayload(e.value, kPayloadOffsetBody,
                                              payloadDragArea(e.value));
                bus.publish(PayloadAttachedEvent{e.value});
                break;
            case MissionEvent::Kind::PayloadReleased:
                world.vehicle().releasePayload();
                bus.publish(PayloadReleasedEvent{});
                break;
            case MissionEvent::Kind::FailureInjected:
                // World::injectFailure() publishes the FailureEvent.
                if (auto failure = missionFailure({0.0, e.text, e.params})) {
                    world.injectFailure(*failure);
                } else {
                    bus.publish(MissionMessageEvent{"Unknown failure: " + e.text});
                }
                break;
            case MissionEvent::Kind::Succeeded: bus.publish(MissionSucceededEvent{}); break;
            case MissionEvent::Kind::Failed: bus.publish(MissionFailedEvent{e.text}); break;
            case MissionEvent::Kind::Aborted:
                bus.publish(MissionMessageEvent{"Mission aborted"});
                break;
        }
    }
}

void MissionSession::abort(World& world) {
    const bool running = runner_->state() == MissionState::InProgress;
    runner_->abort();
    if (running) {
        recordPath(world);
    }
    handleMissionEvents(world);
}

void MissionSession::retry(World& world) {
    runner_->retry();
    runner_->beginPreFlight();
    (void)runner_->takeEvents();
    world.vehicle().releasePayload();
    path_.clear();
    nextPathS_ = 0.0;
    preflightProblems_.clear();
    ++attempt_;
}

void MissionSession::fill(MissionStatus& s) const {
    const missions::MissionRunner& r = *runner_;
    s.active = true;
    s.missionId = r.mission().id;
    s.title = r.mission().title;
    s.state = r.state();
    s.elapsedS = r.elapsedS();
    s.timeLimitS = r.mission().rules.timeLimitS;
    s.objectives.assign(r.objectives().begin(), r.objectives().end());
    s.preflightProblems = preflightProblems_;
    s.payloadKg = r.payloadKg();
    s.result = r.result();
    if (r.result()) {
        s.path = path_;
    } else {
        s.path.clear();
    }
    s.attempt = attempt_;
}

}  // namespace ds::sim
