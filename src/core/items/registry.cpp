// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/items/registry.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace ds::items {
namespace {

constexpr std::int64_t kMaxCellsPerItem = 1024;

// Ray against the local axis-aligned box [lo, hi]; returns the entry distance.
std::optional<double> rayBox(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi) {
    double tMin = 0.0;
    double tMax = std::numeric_limits<double>::max();
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-12) {
            if (o[a] < lo[a] || o[a] > hi[a]) {
                return std::nullopt;
            }
            continue;
        }
        double t0 = (lo[a] - o[a]) / d[a];
        double t1 = (hi[a] - o[a]) / d[a];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return std::nullopt;
        }
    }
    return tMin;
}

std::optional<double> raySphere(const Vec3& o, const Vec3& d, const Vec3& c, double r) {
    const Vec3 oc = o - c;
    const double a = glm::dot(d, d);
    const double b = glm::dot(oc, d);
    const double cc = glm::dot(oc, oc) - r * r;
    if (cc <= 0.0) {
        return 0.0;  // Starts inside.
    }
    const double disc = b * b - a * cc;
    if (disc < 0.0 || b > 0.0) {
        return std::nullopt;
    }
    return (-b - std::sqrt(disc)) / a;
}

// Vertical cylinder of radius r between y0 and y1 around the local origin.
std::optional<double> rayCylinder(const Vec3& o, const Vec3& d, double r, double y0, double y1) {
    double tMin = 0.0;
    double tMax = std::numeric_limits<double>::max();
    // Slab in y.
    if (std::abs(d.y) < 1e-12) {
        if (o.y < y0 || o.y > y1) {
            return std::nullopt;
        }
    } else {
        double t0 = (y0 - o.y) / d.y;
        double t1 = (y1 - o.y) / d.y;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
    }
    // Infinite cylinder in x–z.
    const double a = d.x * d.x + d.z * d.z;
    const double b = o.x * d.x + o.z * d.z;
    const double c = o.x * o.x + o.z * o.z - r * r;
    if (a < 1e-12) {
        if (c > 0.0) {
            return std::nullopt;
        }
    } else {
        const double disc = b * b - a * c;
        if (disc < 0.0) {
            return std::nullopt;
        }
        const double s = std::sqrt(disc);
        tMin = std::max(tMin, (-b - s) / a);
        tMax = std::min(tMax, (-b + s) / a);
    }
    if (tMin > tMax) {
        return std::nullopt;
    }
    return tMin;
}

}  // namespace

ItemRegistry::ItemRegistry(double cellSizeM) : cellSizeM_(cellSizeM > 0.0 ? cellSizeM : 64.0) {}

// ----- storage helpers -----------------------------------------------------------------

Item* ItemRegistry::mutableItem(ItemId id) noexcept {
    if (!id.valid() || id.index >= slots_.size()) {
        return nullptr;
    }
    Slot& slot = slots_[id.index];
    if (slot.generation != id.generation || !slot.item) {
        return nullptr;
    }
    return &*slot.item;
}

const Item* ItemRegistry::get(ItemId id) const noexcept {
    return const_cast<ItemRegistry*>(this)->mutableItem(id);
}

const Item* ItemRegistry::find(std::string_view key) const noexcept {
    if (key.empty()) {
        return nullptr;
    }
    const auto it = keys_.find(std::string(key));
    return it == keys_.end() ? nullptr : get(it->second);
}

common::Result<ItemId> ItemRegistry::insert(Item item, std::optional<ItemId> at) {
    if (!item.key.empty()) {
        if (const auto it = keys_.find(item.key); it != keys_.end() && !(at && it->second == *at)) {
            return common::makeError(std::format("duplicate item id \"{}\"", item.key));
        }
    }
    ItemId id;
    if (at) {
        // Restoring a removed item (undo/redo) in its own slot and generation.
        if (at->index >= slots_.size()) {
            slots_.resize(at->index + 1);
        }
        Slot& slot = slots_[at->index];
        if (slot.item) {
            return common::makeError("item slot is in use");
        }
        std::erase(freeSlots_, at->index);
        slot.generation = at->generation;
        id = *at;
    } else if (!freeSlots_.empty()) {
        id.index = freeSlots_.back();
        freeSlots_.pop_back();
        id.generation = slots_[id.index].generation;
    } else {
        id.index = static_cast<std::uint32_t>(slots_.size());
        id.generation = 1;
        slots_.push_back(Slot{});
    }
    item.id = id;
    if (!item.key.empty()) {
        keys_[item.key] = id;
    }
    slots_[id.index].item = std::move(item);
    ++count_;
    indexItem(*slots_[id.index].item);
    return id;
}

void ItemRegistry::erase(ItemId id) {
    Slot& slot = slots_[id.index];
    unindexItem(id);
    if (!slot.item->key.empty()) {
        keys_.erase(slot.item->key);
    }
    slot.item.reset();
    // A generation never issued before, so every old handle goes stale (an undo may have
    // restored an older generation); skip 0, which marks invalid handles.
    slot.maxGeneration = std::max(slot.maxGeneration, slot.generation);
    slot.generation = slot.maxGeneration == std::numeric_limits<std::uint32_t>::max()
                          ? 1
                          : slot.maxGeneration + 1;
    slot.maxGeneration = std::max(slot.maxGeneration, slot.generation);
    freeSlots_.push_back(id.index);
    --count_;
}

void ItemRegistry::modify(ItemId id, const std::function<void(Item&)>& change) {
    Item* item = mutableItem(id);
    Item before = *item;
    change(*item);
    unindexItem(id);
    indexItem(*item);
    record(Edit{id, std::move(before), *item});
    notify({ItemChange::Kind::Modified, id});
}

// ----- CRUD ----------------------------------------------------------------------------

common::Result<ItemId> ItemRegistry::add(Item item) {
    auto id = insert(std::move(item), std::nullopt);
    if (id) {
        record(Edit{*id, std::nullopt, *get(*id)});
        notify({ItemChange::Kind::Added, *id});
    }
    return id;
}

bool ItemRegistry::remove(ItemId id) {
    const Item* item = get(id);
    if (item == nullptr) {
        return false;
    }
    Item before = *item;
    erase(id);
    record(Edit{id, std::move(before), std::nullopt});
    notify({ItemChange::Kind::Removed, id});
    return true;
}

common::Result<ItemId> ItemRegistry::duplicate(ItemId id) {
    const Item* item = get(id);
    if (item == nullptr) {
        return common::makeError("no such item");
    }
    Item copy = *item;
    copy.key.clear();
    copy.name = std::format("{} (copy)", item->name);
    return add(std::move(copy));
}

bool ItemRegistry::rename(ItemId id, std::string name) {
    if (get(id) == nullptr) {
        return false;
    }
    modify(id, [&](Item& i) { i.name = std::move(name); });
    return true;
}

bool ItemRegistry::setTransform(ItemId id, const Transform& transform) {
    if (get(id) == nullptr) {
        return false;
    }
    modify(id, [&](Item& i) { i.transform = transform; });
    return true;
}

bool ItemRegistry::setEnabled(ItemId id, bool enabled) {
    if (get(id) == nullptr) {
        return false;
    }
    modify(id, [&](Item& i) { i.enabled = enabled; });
    return true;
}

bool ItemRegistry::setVisible(ItemId id, bool visible) {
    if (get(id) == nullptr) {
        return false;
    }
    modify(id, [&](Item& i) { i.visible = visible; });
    return true;
}

common::Result<void> ItemRegistry::replace(const Item& item) {
    const Item* current = get(item.id);
    if (current == nullptr) {
        return common::makeError("no such item");
    }
    if (item.key != current->key && !item.key.empty() && keys_.contains(item.key)) {
        return common::makeError(std::format("duplicate item id \"{}\"", item.key));
    }
    const std::string oldKey = current->key;
    modify(item.id, [&](Item& i) { i = item; });
    if (oldKey != item.key) {
        keys_.erase(oldKey);
        if (!item.key.empty()) {
            keys_[item.key] = item.id;
        }
    }
    return {};
}

void ItemRegistry::clear() {
    slots_.clear();
    freeSlots_.clear();
    keys_.clear();
    grid_.clear();
    cellsOf_.clear();
    large_.clear();
    count_ = 0;
    clearHistory();
    notify({ItemChange::Kind::Cleared, {}});
}

std::vector<const Item*> ItemRegistry::items() const {
    std::vector<const Item*> out;
    out.reserve(count_);
    for (const Slot& s : slots_) {
        if (s.item) {
            out.push_back(&*s.item);
        }
    }
    return out;
}

std::vector<const Item*> ItemRegistry::itemsOfType(ItemType type) const {
    std::vector<const Item*> out;
    for (const Slot& s : slots_) {
        if (s.item && s.item->type == type) {
            out.push_back(&*s.item);
        }
    }
    return out;
}

// ----- spatial index -------------------------------------------------------------------

ItemRegistry::CellKey ItemRegistry::cellKey(std::int64_t cx, std::int64_t cz) const noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32U) |
           static_cast<std::uint32_t>(cz);
}

void ItemRegistry::indexItem(const Item& item) {
    Vec3 lo;
    Vec3 hi;
    item.worldBounds(lo, hi);
    const auto cx0 = static_cast<std::int64_t>(std::floor(lo.x / cellSizeM_));
    const auto cx1 = static_cast<std::int64_t>(std::floor(hi.x / cellSizeM_));
    const auto cz0 = static_cast<std::int64_t>(std::floor(lo.z / cellSizeM_));
    const auto cz1 = static_cast<std::int64_t>(std::floor(hi.z / cellSizeM_));
    if ((cx1 - cx0 + 1) * (cz1 - cz0 + 1) > kMaxCellsPerItem) {
        large_.push_back(item.id);
        return;
    }
    auto& cells = cellsOf_[item.id.value()];
    for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
        for (std::int64_t cz = cz0; cz <= cz1; ++cz) {
            const CellKey key = cellKey(cx, cz);
            grid_[key].push_back(item.id);
            cells.push_back(key);
        }
    }
}

void ItemRegistry::unindexItem(ItemId id) {
    std::erase(large_, id);
    const auto it = cellsOf_.find(id.value());
    if (it == cellsOf_.end()) {
        return;
    }
    for (const CellKey key : it->second) {
        auto cell = grid_.find(key);
        if (cell != grid_.end()) {
            std::erase(cell->second, id);
            if (cell->second.empty()) {
                grid_.erase(cell);
            }
        }
    }
    cellsOf_.erase(it);
}

std::vector<ItemId> ItemRegistry::candidates(const Vec3& lo, const Vec3& hi) const {
    std::vector<ItemId> out = large_;
    const auto cx0 = static_cast<std::int64_t>(std::floor(lo.x / cellSizeM_));
    const auto cx1 = static_cast<std::int64_t>(std::floor(hi.x / cellSizeM_));
    const auto cz0 = static_cast<std::int64_t>(std::floor(lo.z / cellSizeM_));
    const auto cz1 = static_cast<std::int64_t>(std::floor(hi.z / cellSizeM_));
    if ((cx1 - cx0 + 1) * (cz1 - cz0 + 1) > kMaxCellsPerItem * 16) {
        // Huge query: scan everything.
        for (const Slot& s : slots_) {
            if (s.item) {
                out.push_back(s.item->id);
            }
        }
    } else {
        for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
            for (std::int64_t cz = cz0; cz <= cz1; ++cz) {
                if (const auto it = grid_.find(cellKey(cx, cz)); it != grid_.end()) {
                    out.insert(out.end(), it->second.begin(), it->second.end());
                }
            }
        }
    }
    std::ranges::sort(out, {}, &ItemId::value);
    const auto dup = std::ranges::unique(out);
    out.erase(dup.begin(), dup.end());
    return out;
}

std::vector<ItemId> ItemRegistry::queryBox(const Vec3& lo, const Vec3& hi) const {
    std::vector<ItemId> out;
    for (const ItemId id : candidates(lo, hi)) {
        const Item* item = get(id);
        if (item == nullptr || !item->enabled) {
            continue;
        }
        Vec3 ilo;
        Vec3 ihi;
        item->worldBounds(ilo, ihi);
        if (ilo.x <= hi.x && ihi.x >= lo.x && ilo.y <= hi.y && ihi.y >= lo.y && ilo.z <= hi.z &&
            ihi.z >= lo.z) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<ItemId> ItemRegistry::queryRadius(const Vec3& centre, double radiusM) const {
    std::vector<ItemId> out;
    for (const ItemId id : queryBox(centre - Vec3(radiusM), centre + Vec3(radiusM))) {
        Vec3 lo;
        Vec3 hi;
        get(id)->worldBounds(lo, hi);
        const Vec3 nearest = glm::clamp(centre, lo, hi);
        const Vec3 d = nearest - centre;
        if (glm::dot(d, d) <= radiusM * radiusM) {
            out.push_back(id);
        }
    }
    return out;
}

std::vector<ItemId> ItemRegistry::containing(const Vec3& point) const {
    std::vector<ItemId> out;
    for (const ItemId id : queryBox(point, point)) {
        if (get(id)->contains(point)) {
            out.push_back(id);
        }
    }
    return out;
}

std::optional<RayHit> ItemRegistry::raycast(const Vec3& origin, const Vec3& direction,
                                            double maxDistanceM) const {
    const Vec3 end = origin + direction * maxDistanceM;
    std::optional<RayHit> best;
    for (const ItemId id : queryBox(glm::min(origin, end), glm::max(origin, end))) {
        const Item& item = *get(id);
        if (!item.solid) {
            continue;
        }
        // Into the local frame (the distance scale is preserved: direction is not normalised
        // after the inverse scale, so t stays a world distance).
        const Vec3 o = item.transform.toLocal(origin);
        const Vec3 d = (glm::conjugate(item.transform.rotation) * direction) / item.transform.scale;
        const Shape& s = item.shape;
        std::optional<double> t;
        switch (s.kind) {
            case Shape::Kind::Box: t = rayBox(o, d, s.offsetM - s.halfExtentsM, s.offsetM + s.halfExtentsM); break;
            case Shape::Kind::Sphere: t = raySphere(o, d, s.offsetM, s.radiusM); break;
            case Shape::Kind::Cylinder: t = rayCylinder(o, d, s.radiusM, s.bottomM, s.topM); break;
            default: break;
        }
        if (t && *t <= maxDistanceM && (!best || *t < best->distanceM)) {
            best = RayHit{id, *t};
        }
    }
    return best;
}

// ----- terrain placement ---------------------------------------------------------------

void ItemRegistry::placeOnTerrain(const HeightFunction& ground, double originAltitudeM) {
    for (Slot& s : slots_) {
        if (!s.item) {
            continue;
        }
        Item& item = *s.item;
        const Vec3& p = item.transform.position;
        const double y = item.altitudeRef == AltitudeRef::Agl
                             ? (ground ? ground(p.x, p.z) : 0.0) + item.altitudeM
                             : item.altitudeM - originAltitudeM;
        if (y != p.y) {
            item.transform.position.y = y;
            unindexItem(item.id);
            indexItem(item);
            notify({ItemChange::Kind::Modified, item.id});
        }
    }
}

bool ItemRegistry::snapToTerrain(ItemId id, const HeightFunction& ground) {
    const Item* item = get(id);
    if (item == nullptr || !ground) {
        return false;
    }
    const Vec3 p = item->transform.position;
    modify(id, [&](Item& i) {
        i.altitudeRef = AltitudeRef::Agl;
        i.altitudeM = 0.0;
        i.transform.position.y = ground(p.x, p.z);
    });
    return true;
}

// ----- notifications -------------------------------------------------------------------

std::uint64_t ItemRegistry::subscribe(Listener listener) {
    const std::uint64_t token = nextToken_++;
    listeners_.emplace_back(token, std::move(listener));
    return token;
}

void ItemRegistry::unsubscribe(std::uint64_t token) {
    std::erase_if(listeners_, [token](const auto& l) { return l.first == token; });
}

void ItemRegistry::notify(const ItemChange& change) {
    // Copy: a listener may unsubscribe while being called.
    const auto listeners = listeners_;
    for (const auto& [token, listener] : listeners) {
        listener(change);
    }
}

// ----- undo / redo ---------------------------------------------------------------------

void ItemRegistry::record(Edit edit) {
    history_.resize(undoPos_);  // Drop the redo branch.
    history_.push_back(std::move(edit));
    if (history_.size() > historyLimit_) {
        history_.erase(history_.begin());
    }
    undoPos_ = history_.size();
}

bool ItemRegistry::apply(ItemId id, const std::optional<Item>& state) {
    const bool exists = get(id) != nullptr;
    if (!state) {
        if (!exists) {
            return false;
        }
        erase(id);
        notify({ItemChange::Kind::Removed, id});
        return true;
    }
    if (!exists) {
        if (!insert(*state, id)) {
            return false;
        }
        notify({ItemChange::Kind::Added, id});
        return true;
    }
    Item* item = mutableItem(id);
    if (item->key != state->key) {
        if (!state->key.empty() && keys_.contains(state->key)) {
            return false;
        }
        keys_.erase(item->key);
        if (!state->key.empty()) {
            keys_[state->key] = id;
        }
    }
    *item = *state;
    unindexItem(id);
    indexItem(*item);
    notify({ItemChange::Kind::Modified, id});
    return true;
}

bool ItemRegistry::undo() {
    if (!canUndo()) {
        return false;
    }
    const Edit& e = history_[undoPos_ - 1];
    if (!apply(e.id, e.before)) {
        return false;
    }
    --undoPos_;
    return true;
}

bool ItemRegistry::redo() {
    if (!canRedo()) {
        return false;
    }
    const Edit& e = history_[undoPos_];
    if (!apply(e.id, e.after)) {
        return false;
    }
    ++undoPos_;
    return true;
}

void ItemRegistry::clearHistory() noexcept {
    history_.clear();
    undoPos_ = 0;
}

void ItemRegistry::setHistoryLimit(std::size_t limit) {
    historyLimit_ = std::max<std::size_t>(limit, 1);
    while (history_.size() > historyLimit_) {
        history_.erase(history_.begin());
        undoPos_ = undoPos_ > 0 ? undoPos_ - 1 : 0;
    }
}

}  // namespace ds::items
