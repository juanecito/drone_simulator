// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <vector>

#include "ds/items/registry.hpp"

/// @file
/// @brief Trigger detection (FR-ITM-08): gate pass-through (direction-aware), zone enter/exit
/// and waypoint reach, from the vehicle's movement during one tick.
/// @ingroup items

namespace ds::items {

/// @brief One trigger.
struct TriggerEvent {
    /// @brief Kinds of trigger.
    enum class Kind {
        GatePassed,       ///< Crossed a gate opening (see `reversed`).
        ZoneEntered,      ///< Entered a zone, pad or waypoint volume.
        ZoneExited,       ///< Left it.
        WaypointReached,  ///< Entered a waypoint (also reported as ZoneEntered).
    };
    Kind kind = Kind::ZoneEntered;  ///< Kind.
    ItemId id;                      ///< Item.
    bool reversed = false;          ///< GatePassed: crossed against the gate's direction.
    Vec3 point{0.0};                ///< Where it happened (gate crossing point or position).
};

/// @brief Follows one moving point (the vehicle) through the items of a registry.
class TriggerDetector {
public:
    /// @brief Starts at @p position: zones already containing it count as entered, without
    /// events.
    void reset(const ItemRegistry& registry, const Vec3& position);
    /// @brief Movement from the previous position to @p position; returns the triggers in the
    /// order they happened (gates by crossing point along the segment, then exits, then
    /// entries).
    std::vector<TriggerEvent> update(const ItemRegistry& registry, const Vec3& position);
    /// @brief Zones currently containing the point.
    [[nodiscard]] const std::vector<ItemId>& inside() const noexcept { return inside_; }
    /// @brief True if the point is inside @p id.
    [[nodiscard]] bool isInside(ItemId id) const noexcept;

private:
    Vec3 last_{0.0};
    bool started_ = false;
    std::vector<ItemId> inside_;
};

/// @brief Where the segment @p a → @p b crosses the gate @p gate, if it does: the parameter
/// along the segment (0–1) and whether the crossing is against the gate's forward direction.
struct GateCrossing {
    double t = 0.0;         ///< Fraction of the segment.
    bool reversed = false;  ///< Against the gate's direction.
};
/// @brief Gate crossing test (exposed for tests).
[[nodiscard]] std::optional<GateCrossing> crossGate(const Item& gate, const Vec3& a, const Vec3& b);

}  // namespace ds::items
