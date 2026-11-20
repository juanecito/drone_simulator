// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ds/graphics/mesh.hpp"
#include "ds/graphics/mesh_loader.hpp"
#include "ds/items/registry.hpp"

/// @file
/// @brief How world items look (FR-ITM-02, FR-GFX-12): simple procedural meshes per item type
/// — landing pads with an "H", gate frames, translucent waypoint spheres and zone volumes,
/// obstacles (boxes, cylinders, buildings, power lines, wind turbines; houses, trees, cows and
/// people drawn from their solid parts), payloads, inspection targets and the pilot. An
/// obstacle with a `model` property is drawn from that mesh file instead (FR-GFX-12), fitted
/// to its shape; its collision shape does not change.
/// GPU-independent: produces graphics::DrawItem lists.
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
    /// @param models Loads the `model` files of obstacles (null: always procedural).
    /// @param modelDir Folder searched first for those files (the mission's folder).
    ItemVisuals(const items::ItemRegistry& registry, bool showMarkers,
                graphics::ModelLibrary* models = nullptr,
                const std::filesystem::path& modelDir = {});

    /// @brief Appends the items to @p out. Items whose key is in @p highlighted (the current
    /// objective targets) glow, pulsing with @p timeS.
    void appendDrawItems(double timeS, std::span<const std::string> highlighted,
                         std::vector<graphics::DrawItem>& out) const;

    /// @brief Number of items drawn.
    [[nodiscard]] std::size_t size() const noexcept { return parts_.size(); }

    /// @brief The mesh for @p item, or null if it is not drawn (exposed for tests).
    [[nodiscard]] static std::shared_ptr<const graphics::Mesh> meshFor(const items::Item& item,
                                                                       bool showMarkers);
    /// @brief The mesh of an obstacle's `model` file, in the item frame and fitted to the
    /// bounds of @p procedural (its procedural mesh, resting on the same bottom); null with
    /// the reason logged if it cannot be loaded, or if @p item has no `model`.
    [[nodiscard]] static std::shared_ptr<const graphics::Mesh> modelMeshFor(
        const items::Item& item, const graphics::Mesh& procedural, graphics::ModelLibrary& models,
        const std::filesystem::path& modelDir = {});

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
