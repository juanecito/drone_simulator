// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/settings_controller.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/log.hpp"
#include "ds/storage/settings_store.hpp"
#include "ds/storage/storage_service.hpp"

namespace ds::app {
namespace {

constexpr const char* kMetric = "metric";
constexpr const char* kImperial = "imperial";
constexpr const char* kUtm = "utm";
constexpr const char* kUtmLatLon = "utm+latlon";

struct Loaded {
    std::optional<std::vector<std::uint8_t>> geometry;
    std::optional<std::vector<std::uint8_t>> state;
    std::optional<std::string> units;
    std::optional<std::string> coordinates;
    std::optional<std::string> enforceRules;
    std::optional<std::string> chaseTopView;
    std::optional<std::string> chaseBehindView;
};

template <typename T>
std::optional<T> valueOrLog(storage::StorageResult<std::optional<T>> result, std::string_view key) {
    if (!result) {
        log::warn("Ignoring setting '{}': {}", key, result.error().message);
        return std::nullopt;
    }
    return std::move(*result);
}

std::optional<QByteArray> toQByteArray(const std::optional<std::vector<std::uint8_t>>& bytes) {
    if (!bytes) {
        return std::nullopt;
    }
    return QByteArray(reinterpret_cast<const char*>(bytes->data()),
                      static_cast<qsizetype>(bytes->size()));
}

std::vector<std::uint8_t> toBytes(const QByteArray& data) {
    return {reinterpret_cast<const std::uint8_t*>(data.constData()),
            reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()};
}

}  // namespace

SettingsController::SettingsController(storage::StorageService& storage, QObject* parent)
    : QObject(parent), storage_(&storage) {
    load();
}

SettingsController::~SettingsController() { flush(); }

void SettingsController::load() {
    using namespace settings_keys;
    Loaded loaded =
        storage_
            ->submit([](storage::DbConnection& connection) {
                storage::SettingsStore store(connection);
                return Loaded{
                    .geometry = valueOrLog(store.bytes(kWindowGeometry), kWindowGeometry),
                    .state = valueOrLog(store.bytes(kWindowState), kWindowState),
                    .units = valueOrLog(store.string(kUnits), kUnits),
                    .coordinates = valueOrLog(store.string(kCoordinates), kCoordinates),
                    .enforceRules = valueOrLog(store.string(kEnforceFreeRules), kEnforceFreeRules),
                    .chaseTopView = valueOrLog(store.string(kChaseTopView), kChaseTopView),
                    .chaseBehindView = valueOrLog(store.string(kChaseBehindView), kChaseBehindView),
                };
            })
            .get();
    windowGeometry_ = toQByteArray(loaded.geometry);
    windowState_ = toQByteArray(loaded.state);
    if (loaded.units) {
        if (*loaded.units == kImperial) {
            units_ = UnitSystem::Imperial;
        } else if (*loaded.units != kMetric) {
            log::warn("Ignoring unknown unit system '{}'", *loaded.units);
        }
    }
    enforceFreeRules_ = loaded.enforceRules && *loaded.enforceRules == "true";
    chaseTopView_ = !loaded.chaseTopView || *loaded.chaseTopView != "false";
    chaseBehindView_ = !loaded.chaseBehindView || *loaded.chaseBehindView != "false";
    if (loaded.coordinates) {
        if (*loaded.coordinates == kUtmLatLon) {
            coordinates_ = CoordinateDisplay::UtmAndLatLon;
        } else if (*loaded.coordinates != kUtm) {
            log::warn("Ignoring unknown coordinate display '{}'", *loaded.coordinates);
        }
    }
}

void SettingsController::setWindowLayout(const QByteArray& geometry, const QByteArray& state) {
    windowGeometry_ = geometry;
    windowState_ = state;
    // Fire and forget: dropping a packaged_task future does not block.
    static_cast<void>(storage_->submit(
        [geometry = toBytes(geometry), state = toBytes(state)](storage::DbConnection& c) {
            storage::SettingsStore store(c);
            for (const auto& [key, value] : {std::pair{settings_keys::kWindowGeometry, &geometry},
                                             std::pair{settings_keys::kWindowState, &state}}) {
                if (const auto result = store.setBytes(key, *value); !result) {
                    log::error("Cannot save setting '{}': {}", key, result.error().message);
                }
            }
        }));
}

void SettingsController::setUnits(UnitSystem units) {
    if (units == units_) {
        return;
    }
    units_ = units;
    writeString(settings_keys::kUnits, units == UnitSystem::Imperial ? kImperial : kMetric);
    emit unitsChanged(units);
}

void SettingsController::setCoordinateDisplay(CoordinateDisplay display) {
    if (display == coordinates_) {
        return;
    }
    coordinates_ = display;
    writeString(settings_keys::kCoordinates,
                display == CoordinateDisplay::UtmAndLatLon ? kUtmLatLon : kUtm);
    emit coordinateDisplayChanged(display);
}

void SettingsController::setEnforceFreeFlightRules(bool enforce) {
    if (enforce == enforceFreeRules_) {
        return;
    }
    enforceFreeRules_ = enforce;
    writeString(settings_keys::kEnforceFreeRules, enforce ? "true" : "false");
    emit enforceFreeFlightRulesChanged(enforce);
}

void SettingsController::setChaseTopView(bool shown) {
    if (shown == chaseTopView_) {
        return;
    }
    chaseTopView_ = shown;
    writeString(settings_keys::kChaseTopView, shown ? "true" : "false");
    emit chaseInsetsChanged(chaseTopView_, chaseBehindView_);
}

void SettingsController::setChaseBehindView(bool shown) {
    if (shown == chaseBehindView_) {
        return;
    }
    chaseBehindView_ = shown;
    writeString(settings_keys::kChaseBehindView, shown ? "true" : "false");
    emit chaseInsetsChanged(chaseTopView_, chaseBehindView_);
}

void SettingsController::writeString(const char* key, const char* value) {
    static_cast<void>(storage_->submit([key, value](storage::DbConnection& c) {
        storage::SettingsStore store(c);
        if (const auto result = store.setString(key, value); !result) {
            log::error("Cannot save setting '{}': {}", key, result.error().message);
        }
    }));
}

std::optional<QByteArray> SettingsController::readJson(const char* key) const {
    auto value = storage_
                     ->submit([key](storage::DbConnection& c) {
                         storage::SettingsStore store(c);
                         return valueOrLog(store.json(key), key);
                     })
                     .get();
    if (!value) {
        return std::nullopt;
    }
    return QByteArray::fromStdString(*value);
}

void SettingsController::writeJson(const char* key, const QByteArray& json) {
    static_cast<void>(storage_->submit([key, json = json.toStdString()](storage::DbConnection& c) {
        storage::SettingsStore store(c);
        if (const auto result = store.setJson(key, json); !result) {
            log::error("Cannot save setting '{}': {}", key, result.error().message);
        }
    }));
}

const storage::StorageInfo& SettingsController::storageInfo() const noexcept {
    return storage_->info();
}

void SettingsController::flush() {
    storage_->submit([](storage::DbConnection&) {}).wait();
}

}  // namespace ds::app
