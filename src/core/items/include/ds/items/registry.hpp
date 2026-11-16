// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/items/item.hpp"

/// @file
/// @brief The items manager (§4.8): entity registry with generational ids (FR-ITM-01), CRUD
/// (FR-ITM-04), spatial queries on a uniform grid (FR-ITM-05), terrain placement (FR-ITM-07),
/// change notifications (FR-ITM-10) and undo/redo (FR-ITM-09).
/// @details Not thread-safe: one owner thread (the simulation thread during a mission, the GUI
/// thread in the editor) uses a registry; other threads get copies.
/// @ingroup items

namespace ds::items {

/// @brief What changed.
struct ItemChange {
    /// @brief Kinds of change.
    enum class Kind { Added, Removed, Modified, Cleared };
    Kind kind = Kind::Added;  ///< Kind.
    ItemId id;                ///< Item (invalid for Cleared).
};

/// @brief Nearest hit of a ray.
struct RayHit {
    ItemId id;              ///< Item hit.
    double distanceM = 0.0; ///< Distance along the ray.
};

/// @brief Terrain height at world (x, z), relative to the terrain origin.
using HeightFunction = std::function<double(double x, double z)>;

/// @brief Owner of all items of a scene or mission.
class ItemRegistry {
public:
    /// @brief Change callback.
    using Listener = std::function<void(const ItemChange&)>;

    /// @brief Creates an empty registry; @p cellSizeM is the spatial grid's cell size.
    explicit ItemRegistry(double cellSizeM = 64.0);

    // ----- CRUD (FR-ITM-04) ----------------------------------------------------------
    /// @brief Adds @p item (its id is ignored) and returns the new id. Fails if the key is
    /// not empty and already used.
    common::Result<ItemId> add(Item item);
    /// @brief Removes the item. False for a stale or invalid id.
    bool remove(ItemId id);
    /// @brief Copies the item; the copy gets an empty key and the name "<name> (copy)".
    common::Result<ItemId> duplicate(ItemId id);
    /// @brief Renames the item. False for a stale id.
    bool rename(ItemId id, std::string name);
    /// @brief Moves / rotates / scales the item. False for a stale id.
    bool setTransform(ItemId id, const Transform& transform);
    /// @brief Enables or disables the item. False for a stale id.
    bool setEnabled(ItemId id, bool enabled);
    /// @brief Shows or hides the item. False for a stale id.
    bool setVisible(ItemId id, bool visible);
    /// @brief Replaces the whole item (same id; key must stay unique). False for a stale id.
    common::Result<void> replace(const Item& item);
    /// @brief Removes everything and clears the undo history.
    void clear();

    // ----- access --------------------------------------------------------------------
    /// @brief The item, or nullptr for a stale or invalid id.
    [[nodiscard]] const Item* get(ItemId id) const noexcept;
    /// @brief The item with mission key @p key, or nullptr.
    [[nodiscard]] const Item* find(std::string_view key) const noexcept;
    /// @brief Number of items.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    /// @brief All items, in slot order.
    [[nodiscard]] std::vector<const Item*> items() const;
    /// @brief Items of @p type.
    [[nodiscard]] std::vector<const Item*> itemsOfType(ItemType type) const;

    // ----- spatial queries (FR-ITM-05); only enabled items --------------------------
    /// @brief Items whose bounds come within @p radiusM of @p centre.
    [[nodiscard]] std::vector<ItemId> queryRadius(const Vec3& centre, double radiusM) const;
    /// @brief Items whose bounds overlap the box [@p lo, @p hi].
    [[nodiscard]] std::vector<ItemId> queryBox(const Vec3& lo, const Vec3& hi) const;
    /// @brief Items whose shape contains @p point.
    [[nodiscard]] std::vector<ItemId> containing(const Vec3& point) const;
    /// @brief Nearest solid item hit by the ray from @p origin along @p direction (unit) within
    /// @p maxDistanceM.
    [[nodiscard]] std::optional<RayHit> raycast(const Vec3& origin, const Vec3& direction,
                                                double maxDistanceM) const;

    // ----- terrain placement (FR-ITM-07) ---------------------------------------------
    /// @brief Sets the vertical position of every item from its altitude reference: AGL items
    /// to `ground(x, z) + altitudeM`, AMSL items to `altitudeM − originAltitudeM`. Not undoable
    /// (it follows the terrain, not an edit).
    void placeOnTerrain(const HeightFunction& ground, double originAltitudeM);
    /// @brief Puts one item on the terrain (AGL 0) as an undoable edit.
    bool snapToTerrain(ItemId id, const HeightFunction& ground);

    // ----- notifications (FR-ITM-10) -------------------------------------------------
    /// @brief Registers @p listener; returns a token for unsubscribe().
    std::uint64_t subscribe(Listener listener);
    /// @brief Removes a listener.
    void unsubscribe(std::uint64_t token);

    // ----- undo / redo (FR-ITM-09) ---------------------------------------------------
    /// @brief True if an edit can be undone.
    [[nodiscard]] bool canUndo() const noexcept { return undoPos_ > 0; }
    /// @brief True if an undone edit can be redone.
    [[nodiscard]] bool canRedo() const noexcept { return undoPos_ < history_.size(); }
    /// @brief Undoes the last edit. False if there is none or it cannot be applied.
    bool undo();
    /// @brief Redoes the last undone edit.
    bool redo();
    /// @brief Forgets the history.
    void clearHistory() noexcept;
    /// @brief Maximum number of remembered edits (default 200).
    void setHistoryLimit(std::size_t limit);

private:
    struct Slot {
        std::optional<Item> item;
        std::uint32_t generation = 1;     // Of the current / next item in the slot.
        std::uint32_t maxGeneration = 1;  // Highest ever issued (undo can restore older ones).
    };
    struct Edit {
        ItemId id;
        std::optional<Item> before;  // Empty ⇒ the edit added the item.
        std::optional<Item> after;   // Empty ⇒ the edit removed the item.
    };
    using CellKey = std::uint64_t;

    Item* mutableItem(ItemId id) noexcept;
    common::Result<ItemId> insert(Item item, std::optional<ItemId> at);
    void erase(ItemId id);
    void modify(ItemId id, const std::function<void(Item&)>& change);
    void record(Edit edit);
    bool apply(ItemId id, const std::optional<Item>& state);
    void notify(const ItemChange& change);
    void indexItem(const Item& item);
    void unindexItem(ItemId id);
    [[nodiscard]] std::vector<ItemId> candidates(const Vec3& lo, const Vec3& hi) const;
    [[nodiscard]] CellKey cellKey(std::int64_t cx, std::int64_t cz) const noexcept;

    double cellSizeM_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::size_t count_ = 0;
    std::unordered_map<std::string, ItemId> keys_;
    std::unordered_map<CellKey, std::vector<ItemId>> grid_;
    std::unordered_map<std::uint64_t, std::vector<CellKey>> cellsOf_;
    std::vector<ItemId> large_;  // Items spanning too many cells: always candidates.
    std::vector<std::pair<std::uint64_t, Listener>> listeners_;
    std::uint64_t nextToken_ = 1;
    std::vector<Edit> history_;
    std::size_t undoPos_ = 0;
    std::size_t historyLimit_ = 200;
};

}  // namespace ds::items
