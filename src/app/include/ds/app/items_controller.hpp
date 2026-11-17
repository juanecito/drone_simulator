// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/common/json.hpp"
#include "ds/items/item.hpp"
#include "ds/items/registry.hpp"

/// @file
/// @brief Items of the free-flight scene (Items tab, FR-ITM-04, FR-ITM-07, FR-ITM-09,
/// FR-ITM-11): add, edit, duplicate, delete, snap to the terrain, undo/redo, gate order; saved
/// with the scene in the user's settings.
/// @ingroup app

namespace ds::app {

class SceneController;
class SettingsController;

/// @brief What the Items tab can add (the aerial palette; ground items come with M6).
struct ItemTemplate {
    items::ItemType type = items::ItemType::Obstacle;  ///< Type.
    std::string shape;                                 ///< Obstacle shape ("" for others).
    std::string label;                                 ///< Menu text ("Tree").
};

/// @brief The add-item palette, in menu order.
[[nodiscard]] const std::vector<ItemTemplate>& itemPalette();

/// @brief Scene item as JSON (`type`, `key`, `name`, `local` [x, altitude, z], `altRef`,
/// `headingDeg`, `enabled`, `properties`).
[[nodiscard]] common::Json itemToJson(const items::Item& item);
/// @brief Item from itemToJson()'s form; the shape follows from the type and properties.
[[nodiscard]] common::Result<items::Item> itemFromJson(const common::Json& json);

/// @brief The free-flight scene's items.
class ItemsController final : public QObject {
    Q_OBJECT

public:
    /// @brief Loads the saved items of @p settings (may be null: nothing is saved) and places
    /// them on @p scene's terrain; both must outlive the controller.
    ItemsController(SceneController& scene, SettingsController* settings,
                    QObject* parent = nullptr);

    /// @brief The items (read only; edit through this controller).
    [[nodiscard]] const items::ItemRegistry& registry() const noexcept { return registry_; }
    /// @brief Copies of every item, in registry order.
    [[nodiscard]] std::vector<items::Item> items() const;

    /// @brief Adds an item from @p tmpl on the ground at world (@p x, @p z), facing
    /// @p headingDeg; it gets a unique key ("gate-2") and a default name ("Gate 2").
    common::Result<items::ItemId> add(const ItemTemplate& tmpl, double x, double z,
                                      double headingDeg);
    /// @brief Replaces an item (same id): name, position, altitude, heading, properties. The
    /// shape is rebuilt from the properties and the item put back on the terrain.
    common::Result<void> update(items::Item item);
    /// @brief Copies an item 3 m east of it.
    common::Result<items::ItemId> duplicate(items::ItemId id);
    /// @brief Deletes an item.
    bool remove(items::ItemId id);
    /// @brief Puts an item on the ground (AGL 0).
    bool snapToTerrain(items::ItemId id);
    /// @brief Undoes the last edit.
    bool undo();
    /// @brief Redoes the last undone edit.
    bool redo();
    /// @brief Deletes every item (undoable).
    void clear();

    /// @brief Gates in flying order (`order` property, then registry order).
    [[nodiscard]] std::vector<items::ItemId> gateOrder() const;
    /// @brief Moves gate @p id @p delta places in the flying order.
    bool moveGate(items::ItemId id, int delta);

signals:
    /// @brief The items changed (any edit, undo, redo or terrain change).
    void changed();

private:
    void placeAll();
    void save();
    void edited();
    [[nodiscard]] std::string uniqueKey(std::string_view base) const;

    SceneController* scene_;
    SettingsController* settings_;
    items::ItemRegistry registry_;
};

}  // namespace ds::app
