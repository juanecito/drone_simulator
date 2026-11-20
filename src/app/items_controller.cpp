// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/items_controller.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/common/log.hpp"

namespace ds::app {
namespace {

using items::ItemType;

// Height of a new item above the ground (gates and waypoints fly; the rest stands).
double defaultAltitude(ItemType type) {
    switch (type) {
        case ItemType::Gate: return 3.0;
        case ItemType::Waypoint: return 10.0;
        case ItemType::InspectionTarget: return 2.0;
        default: return 0.0;
    }
}

}  // namespace

common::Json defaultProperties(const ItemTemplate& t) {
    using common::Json;
    switch (t.type) {
        case ItemType::Obstacle:
            if (t.shape == "house") {
                return {{"shape", "house"}, {"sizeM", Json::array({8.0, 7.0, 10.0})}};
            }
            if (t.shape == "cow") {
                return {{"shape", "cow"}, {"sizeM", Json::array({0.8, 1.5, 2.4})}};
            }
            if (t.shape == "man") {
                return {{"shape", "man"}, {"heightM", 1.75}};
            }
            if (t.shape == "tree") {
                return {{"shape", "tree"}, {"heightM", 9.0}, {"radiusM", 2.5}};
            }
            if (t.shape == "cylinder") {
                return {{"shape", "cylinder"}, {"heightM", 12.0}, {"radiusM", 0.5}};
            }
            if (t.shape == "building") {
                return {{"shape", "building"}, {"sizeM", Json::array({8.0, 6.0, 10.0})}};
            }
            return {{"shape", "box"}, {"sizeM", Json::array({2.0, 2.0, 2.0})}};
        case ItemType::Gate: return {{"widthM", 3.0}, {"heightM", 2.5}};
        case ItemType::Waypoint: return {{"radiusM", 3.0}};
        case ItemType::LandingPad: return {{"radiusM", 1.5}};
        case ItemType::PickupZone:
        case ItemType::DropZone: return {{"radiusM", 3.0}, {"heightM", 10.0}};
        case ItemType::NoFlyZone:
        case ItemType::Geofence: return {{"radiusM", 50.0}};
        case ItemType::InspectionTarget: return {{"radiusM", 0.5}};
        case ItemType::Ramp: return {{"sizeM", Json::array({1.0, 0.25, 1.5})}};
        case ItemType::ParkingZone: return {{"sizeM", Json::array({0.7, 0.9})}};  // Rover bay.
        case ItemType::Bridge: return {{"sizeM", Json::array({1.0, 0.5, 3.0})}};
        default: return Json::object();
    }
}

const std::vector<ItemTemplate>& itemPalette() {
    static const std::vector<ItemTemplate> palette{
        {ItemType::Gate, "", "Gate"},
        {ItemType::Waypoint, "", "Waypoint"},
        {ItemType::LandingPad, "", "Landing pad"},
        {ItemType::Obstacle, "box", "Box"},
        {ItemType::Obstacle, "cylinder", "Pole"},
        {ItemType::Obstacle, "tree", "Tree"},
        {ItemType::Obstacle, "building", "Building"},
        {ItemType::Obstacle, "house", "House"},
        {ItemType::Obstacle, "cow", "Cow"},
        {ItemType::Obstacle, "man", "Man"},
        {ItemType::InspectionTarget, "", "Inspection target"},
        {ItemType::PickupZone, "", "Pickup zone"},
        {ItemType::DropZone, "", "Drop zone"},
        {ItemType::NoFlyZone, "", "No-fly zone"},
        {ItemType::ParkingZone, "", "Parking zone"},
        {ItemType::Ramp, "", "Ramp"},
        {ItemType::Bridge, "", "Bridge"},
    };
    return palette;
}

common::Json itemToJson(const items::Item& item) {
    const items::Vec3& p = item.transform.position;
    return {{"type", std::string(items::itemTypeName(item.type))},
            {"key", item.key},
            {"name", item.name},
            {"local", common::Json::array({p.x, item.altitudeM, p.z})},
            {"altRef", item.altitudeRef == items::AltitudeRef::Amsl ? "AMSL" : "AGL"},
            {"headingDeg", items::headingOf(item.transform.rotation)},
            {"enabled", item.enabled},
            {"properties", item.properties}};
}

common::Result<items::Item> itemFromJson(const common::Json& json) {
    try {
        const auto type = items::parseItemType(json.at("type").get<std::string>());
        if (!type) {
            return common::makeError("unknown item type " + json.at("type").dump());
        }
        items::Item item;
        item.type = *type;
        item.key = json.value("key", std::string());
        item.name = json.value("name", std::string());
        const auto& local = json.at("local");
        item.transform.position = {local.at(0).get<double>(), 0.0, local.at(2).get<double>()};
        item.altitudeM = local.at(1).get<double>();
        item.altitudeRef = json.value("altRef", std::string("AGL")) == "AMSL"
                               ? items::AltitudeRef::Amsl
                               : items::AltitudeRef::Agl;
        item.transform.rotation = items::headingRotation(json.value("headingDeg", 0.0));
        item.enabled = json.value("enabled", true);
        item.properties = json.value("properties", common::Json::object());
        if (!item.properties.is_object()) {
            return common::makeError("item properties must be an object");
        }
        item.shape = items::defaultShape(item.type, item.properties);
        item.solid = items::isSolidType(item.type);
        return item;
    } catch (const common::Json::exception& e) {
        return common::makeError(std::string("invalid item: ") + e.what());
    }
}

ItemsController::ItemsController(SceneController& scene, SettingsController* settings,
                                 QObject* parent)
    : QObject(parent), scene_(&scene), settings_(settings) {
    if (settings_ != nullptr) {
        if (const auto saved = settings_->readJson(settings_keys::kSceneItems)) {
            const auto json = common::Json::parse(saved->toStdString(), nullptr, false);
            for (const common::Json& j : json.is_array() ? json : common::Json::array()) {
                auto item = itemFromJson(j);
                if (!item || !registry_.add(std::move(*item))) {
                    log::warn("Scene item skipped: {}", j.dump());
                }
            }
            registry_.clearHistory();
        }
    }
    placeAll();
    // AGL items follow the terrain (FR-ITM-07).
    connect(scene_, &SceneController::terrainChanged, this, [this] {
        if (!scene_->inMission()) {
            placeAll();
            emit changed();
        }
    });
}

std::vector<items::Item> ItemsController::items() const {
    std::vector<items::Item> out;
    for (const items::Item* item : registry_.items()) {
        out.push_back(*item);
    }
    return out;
}

void ItemsController::placeAll() {
    const terrain::TerrainQuery& q = scene_->query();
    registry_.placeOnTerrain([&q](double x, double z) { return q.height(x, z); },
                             scene_->frame().origin().heightM);
}

void ItemsController::save() {
    if (settings_ == nullptr) {
        return;
    }
    common::Json all = common::Json::array();
    for (const items::Item* item : registry_.items()) {
        all.push_back(itemToJson(*item));
    }
    settings_->writeJson(settings_keys::kSceneItems, QByteArray::fromStdString(all.dump()));
}

void ItemsController::edited() {
    placeAll();
    save();
    emit changed();
}

std::string ItemsController::uniqueKey(std::string_view base) const {
    for (int n = 1;; ++n) {
        std::string key = std::format("{}-{}", base, n);
        if (registry_.find(key) == nullptr) {
            return key;
        }
    }
}

common::Result<items::ItemId> ItemsController::add(const ItemTemplate& tmpl, double x, double z,
                                                   double headingDeg) {
    items::Item item;
    item.type = tmpl.type;
    item.properties = defaultProperties(tmpl);
    const std::string base =
        tmpl.shape.empty() ? std::string(items::itemTypeName(tmpl.type)) : tmpl.shape;
    item.key = uniqueKey(base);
    item.name = std::format("{} {}", tmpl.label, item.key.substr(item.key.rfind('-') + 1));
    item.transform.position = {x, 0.0, z};
    item.transform.rotation = items::headingRotation(headingDeg);
    item.altitudeRef = items::AltitudeRef::Agl;
    item.altitudeM = defaultAltitude(tmpl.type);
    item.shape = items::defaultShape(item.type, item.properties);
    item.solid = items::isSolidType(item.type);
    if (item.type == ItemType::Gate) {
        item.properties["order"] = static_cast<int>(gateOrder().size()) + 1;
    }
    auto id = registry_.add(std::move(item));
    if (id) {
        edited();
    }
    return id;
}

common::Result<void> ItemsController::update(items::Item item) {
    item.shape = items::defaultShape(item.type, item.properties);
    item.solid = items::isSolidType(item.type);
    auto replaced = registry_.replace(item);
    if (replaced) {
        edited();
    }
    return replaced;
}

common::Result<items::ItemId> ItemsController::duplicate(items::ItemId id) {
    const items::Item* source = registry_.get(id);
    if (source == nullptr) {
        return common::makeError("no such item");
    }
    items::Item copy = *source;
    copy.key = uniqueKey(copy.key.substr(0, copy.key.rfind('-')));
    copy.name = source->name + " (copy)";
    copy.transform.position.x += 3.0;
    if (copy.type == ItemType::Gate) {
        copy.properties["order"] = static_cast<int>(gateOrder().size()) + 1;
    }
    auto added = registry_.add(std::move(copy));
    if (added) {
        edited();
    }
    return added;
}

bool ItemsController::remove(items::ItemId id) {
    if (!registry_.remove(id)) {
        return false;
    }
    edited();
    return true;
}

bool ItemsController::snapToTerrain(items::ItemId id) {
    const items::Item* item = registry_.get(id);
    if (item == nullptr) {
        return false;
    }
    items::Item copy = *item;
    copy.altitudeRef = items::AltitudeRef::Agl;
    copy.altitudeM = 0.0;
    return update(std::move(copy)).has_value();
}

bool ItemsController::undo() {
    if (!registry_.undo()) {
        return false;
    }
    edited();
    return true;
}

bool ItemsController::redo() {
    if (!registry_.redo()) {
        return false;
    }
    edited();
    return true;
}

void ItemsController::clear() {
    std::vector<items::ItemId> ids;
    for (const items::Item* item : registry_.items()) {
        ids.push_back(item->id);
    }
    for (const items::ItemId id : ids) {
        registry_.remove(id);
    }
    edited();
}

std::vector<items::ItemId> ItemsController::gateOrder() const {
    std::vector<const items::Item*> gates = registry_.itemsOfType(ItemType::Gate);
    std::ranges::stable_sort(gates, {}, [](const items::Item* g) {
        const auto& p = g->properties;
        return p.contains("order") && p["order"].is_number() ? p["order"].get<double>() : 1e9;
    });
    std::vector<items::ItemId> ids;
    for (const items::Item* g : gates) {
        ids.push_back(g->id);
    }
    return ids;
}

bool ItemsController::moveGate(items::ItemId id, int delta) {
    std::vector<items::ItemId> order = gateOrder();
    const auto it = std::ranges::find(order, id);
    if (it == order.end()) {
        return false;
    }
    const auto from = static_cast<int>(it - order.begin());
    const int to = std::clamp(from + delta, 0, static_cast<int>(order.size()) - 1);
    if (to == from) {
        return false;
    }
    const items::ItemId moved = order[static_cast<std::size_t>(from)];
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, moved);
    // Renumber every gate whose number changes.
    for (std::size_t i = 0; i < order.size(); ++i) {
        items::Item g = *registry_.get(order[i]);
        const int number = static_cast<int>(i) + 1;
        if (!g.properties.contains("order") || g.properties["order"] != number) {
            g.properties["order"] = number;
            (void)registry_.replace(g);
        }
    }
    edited();
    return true;
}

}  // namespace ds::app
