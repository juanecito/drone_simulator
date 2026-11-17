// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ds/graphics/mesh.hpp"
#include "ds/items/registry.hpp"

/// @file
/// @brief How world items look (FR-ITM-02, FR-GFX-12): simple procedural meshes per item type
/// — landing pads with an "H", gate frames, translucent waypoint spheres and zone volumes,
/// obstacles (boxes, cylinders, trees, buildings, power lines, wind turbines), payloads,
/// inspection targets and the pilot. GPU-independent: produces graphics::DrawItem lists.
/// @ingroup sim

namespace ds::sim {

/// @brief Height drawn for zone volumes without a top (open prisms and cylinders), metres.
inline constexpr double kOpenZoneHeightM = 150.0;

/// @brief Draw items for the items of a registry; meshes are built once.
class ItemVisuals {
public:
    /// @brief Builds the meshes for every visible item of @p registry.
    /// @param registry Items to draw (copied: the registry may change afterwards).
    /// @param showMarkers Draw markers (waypoint spheres, pickup/drop/parking zones, survey
    ///        areas); physical items, no-fly zones and geofences are always drawn
    ///        (`rules.showMarkers`, §4.9.1).
    ItemVisuals(const items::ItemRegistry& registry, bool showMarkers);

    /// @brief Appends the items to @p out. Items whose key is in @p highlighted (the current
    /// objective targets) glow, pulsing with @p timeS.
    void appendDrawItems(double timeS, std::span<const std::string> highlighted,
                         std::vector<graphics::DrawItem>& out) const;

    /// @brief Number of items drawn.
    [[nodiscard]] std::size_t size() const noexcept { return parts_.size(); }

    /// @brief The mesh for @p item, or null if it is not drawn (exposed for tests).
    [[nodiscard]] static std::shared_ptr<const graphics::Mesh> meshFor(const items::Item& item,
                                                                       bool showMarkers);

private:
    struct Part {
        std::string key;
        std::shared_ptr<const graphics::Mesh> mesh;
        glm::dvec3 position{0.0};
        glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
        glm::vec3 scale{1.0F};
    };
    std::vector<Part> parts_;
};

}  // namespace ds::sim
