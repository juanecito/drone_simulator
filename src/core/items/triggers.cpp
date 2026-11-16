// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/items/triggers.hpp"

#include <algorithm>
#include <cmath>

namespace ds::items {
namespace {

std::vector<ItemId> zonesContaining(const ItemRegistry& registry, const Vec3& p) {
    std::vector<ItemId> out;
    for (const ItemId id : registry.containing(p)) {
        if (isZoneType(registry.get(id)->type)) {
            out.push_back(id);
        }
    }
    std::ranges::sort(out, {}, &ItemId::value);
    return out;
}

}  // namespace

std::optional<GateCrossing> crossGate(const Item& gate, const Vec3& a, const Vec3& b) {
    if (gate.shape.kind != Shape::Kind::Gate) {
        return std::nullopt;
    }
    // Local frame: the opening lies in the x–y plane; forward is −z.
    const Vec3 la = gate.transform.toLocal(a);
    const Vec3 lb = gate.transform.toLocal(b);
    const double fa = -la.z;  // Signed distance along forward.
    const double fb = -lb.z;
    const bool forward = fa < 0.0 && fb >= 0.0;
    const bool backward = fa > 0.0 && fb <= 0.0;
    if (!forward && !backward) {
        return std::nullopt;
    }
    const double t = fa / (fa - fb);
    const Vec3 p = la + (lb - la) * t;
    const Vec3& half = gate.shape.halfExtentsM;
    if (std::abs(p.x) > half.x || std::abs(p.y) > half.y) {
        return std::nullopt;
    }
    return GateCrossing{t, backward};
}

void TriggerDetector::reset(const ItemRegistry& registry, const Vec3& position) {
    last_ = position;
    started_ = true;
    inside_ = zonesContaining(registry, position);
}

bool TriggerDetector::isInside(ItemId id) const noexcept {
    return std::ranges::find(inside_, id) != inside_.end();
}

std::vector<TriggerEvent> TriggerDetector::update(const ItemRegistry& registry,
                                                  const Vec3& position) {
    if (!started_) {
        reset(registry, position);
        return {};
    }
    std::vector<TriggerEvent> events;

    // Gates: the whole segment, so fast movement cannot jump through a gate.
    struct Hit {
        double t;
        TriggerEvent event;
    };
    std::vector<Hit> gates;
    for (const ItemId id : registry.queryBox(glm::min(last_, position), glm::max(last_, position))) {
        const Item& item = *registry.get(id);
        if (item.type != ItemType::Gate) {
            continue;
        }
        if (const auto c = crossGate(item, last_, position)) {
            gates.push_back({c->t, TriggerEvent{TriggerEvent::Kind::GatePassed, id, c->reversed,
                                                last_ + (position - last_) * c->t}});
        }
    }
    std::ranges::sort(gates, {}, &Hit::t);
    for (auto& g : gates) {
        events.push_back(g.event);
    }

    // Zones: compare containment before and after.
    std::vector<ItemId> now = zonesContaining(registry, position);
    for (const ItemId id : inside_) {
        if (!std::ranges::binary_search(now, id.value(), {}, &ItemId::value)) {
            events.push_back({TriggerEvent::Kind::ZoneExited, id, false, position});
        }
    }
    for (const ItemId id : now) {
        if (!std::ranges::binary_search(inside_, id.value(), {}, &ItemId::value)) {
            events.push_back({TriggerEvent::Kind::ZoneEntered, id, false, position});
            if (registry.get(id)->type == ItemType::Waypoint) {
                events.push_back({TriggerEvent::Kind::WaypointReached, id, false, position});
            }
        }
    }
    inside_ = std::move(now);
    last_ = position;
    return events;
}

}  // namespace ds::items
