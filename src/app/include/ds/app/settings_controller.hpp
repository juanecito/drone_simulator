// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <optional>

namespace ds::storage {
class StorageService;
struct StorageInfo;
}  // namespace ds::storage

/// @file
/// @brief Adapter between the GUI and the persisted user settings (FR-UI-03, FR-UI-06).
/// @ingroup app

namespace ds::app {

/// @brief How lengths, heights and speeds are displayed (SPECIFICATION §4.11, Settings tab).
enum class UnitSystem {
    Metric,    ///< m, km, m/s, km/h.
    Imperial,  ///< ft, mi, ft/s, mph.
};

/// @brief Which coordinates are shown next to the always-visible UTM position (decision 7).
enum class CoordinateDisplay {
    Utm,           ///< UTM only.
    UtmAndLatLon,  ///< UTM plus latitude/longitude.
};

/// @brief Settings keys in the `settings` table.
namespace settings_keys {
inline constexpr const char* kWindowGeometry = "ui.mainWindow.geometry";  ///< QByteArray.
inline constexpr const char* kWindowState = "ui.mainWindow.state";        ///< QByteArray.
inline constexpr const char* kUnits = "display.units";                    ///< "metric"/"imperial".
inline constexpr const char* kCoordinates = "display.coordinates";        ///< "utm"/"utm+latlon".
inline constexpr const char* kSceneTerrain = "scene.terrain";  ///< Object (scene_config.hpp).
inline constexpr const char* kSceneEnvironment = "scene.environment";  ///< Object.
/// "true"/"false": the chase camera's top-view inset is shown (default true).
inline constexpr const char* kChaseTopView = "display.chaseTopView";
/// "true"/"false": the chase camera's behind-view inset is shown (default true).
inline constexpr const char* kChaseBehindView = "display.chaseBehindView";
/// "true"/"false": free mode ends the flight when a flight rule is broken (FR-MIS-06d).
inline constexpr const char* kEnforceFreeRules = "rules.enforceInFreeMode";
/// Mission id selected last in the Missions tab (FR-UI-03).
inline constexpr const char* kLastMission = "missions.lastSelected";
/// Items of the free-flight scene (Items tab): array of items_controller.hpp objects.
inline constexpr const char* kSceneItems = "scene.items";
}  // namespace settings_keys

/// @brief GUI-side access to user settings, backed by the storage thread.
/// @details Values are loaded once at construction (a short blocking read during start-up)
/// and cached; setters update the cache, emit a change signal and queue the write, so the GUI
/// never waits for the database. Write failures are logged. Lives on the GUI thread.
class SettingsController final : public QObject {
    Q_OBJECT

public:
    /// @brief Loads the settings from @p storage, which must outlive this object.
    explicit SettingsController(storage::StorageService& storage, QObject* parent = nullptr);
    ~SettingsController() override;

    SettingsController(const SettingsController&) = delete;
    SettingsController& operator=(const SettingsController&) = delete;
    SettingsController(SettingsController&&) = delete;
    SettingsController& operator=(SettingsController&&) = delete;

    /// @brief Saved `QMainWindow::saveGeometry()` blob, if any.
    [[nodiscard]] const std::optional<QByteArray>& windowGeometry() const noexcept {
        return windowGeometry_;
    }
    /// @brief Saved `QMainWindow::saveState()` blob (docks and toolbars), if any.
    [[nodiscard]] const std::optional<QByteArray>& windowState() const noexcept {
        return windowState_;
    }
    /// @brief Stores the main-window geometry and dock state.
    void setWindowLayout(const QByteArray& geometry, const QByteArray& state);

    /// @brief Display unit system (default metric).
    [[nodiscard]] UnitSystem units() const noexcept { return units_; }
    /// @brief Changes and persists the unit system.
    void setUnits(UnitSystem units);

    /// @brief Coordinate display (default UTM only).
    [[nodiscard]] CoordinateDisplay coordinateDisplay() const noexcept { return coordinates_; }
    /// @brief Changes and persists the coordinate display.
    void setCoordinateDisplay(CoordinateDisplay display);

    /// @brief True if the chase camera shows its top-view inset (default true).
    [[nodiscard]] bool chaseTopView() const noexcept { return chaseTopView_; }
    /// @brief Shows or hides, and persists, the chase camera's top-view inset.
    void setChaseTopView(bool shown);
    /// @brief True if the chase camera shows its behind-view inset (default true).
    [[nodiscard]] bool chaseBehindView() const noexcept { return chaseBehindView_; }
    /// @brief Shows or hides, and persists, the chase camera's behind-view inset.
    void setChaseBehindView(bool shown);

    /// @brief True if free mode ends the flight when a flight rule is broken (default false:
    /// warnings only, FR-MIS-06d).
    [[nodiscard]] bool enforceFreeFlightRules() const noexcept { return enforceFreeRules_; }
    /// @brief Changes and persists the free-mode rule enforcement.
    void setEnforceFreeFlightRules(bool enforce);

    /// @brief Facts about the open database (file, schema version, in-memory flag).
    [[nodiscard]] const storage::StorageInfo& storageInfo() const noexcept;

    /// @brief Reads the raw JSON value of @p key (blocks until the storage thread answers).
    [[nodiscard]] std::optional<QByteArray> readJson(const char* key) const;
    /// @brief Stores the JSON value @p json under @p key (asynchronous).
    void writeJson(const char* key, const QByteArray& json);
    /// @brief The storage service (for stores other than settings, e.g. terrain presets).
    [[nodiscard]] storage::StorageService& storage() const noexcept { return *storage_; }

    /// @brief Waits until all queued writes have reached the database.
    void flush();

signals:
    /// @brief The unit system changed.
    void unitsChanged(ds::app::UnitSystem units);
    /// @brief The coordinate display changed.
    void coordinateDisplayChanged(ds::app::CoordinateDisplay display);
    /// @brief The free-mode rule enforcement changed.
    void enforceFreeFlightRulesChanged(bool enforce);
    /// @brief The chase insets shown changed: @p top view and @p behind view.
    void chaseInsetsChanged(bool top, bool behind);

private:
    void load();
    void writeString(const char* key, const char* value);

    storage::StorageService* storage_;
    std::optional<QByteArray> windowGeometry_;
    std::optional<QByteArray> windowState_;
    UnitSystem units_ = UnitSystem::Metric;
    CoordinateDisplay coordinates_ = CoordinateDisplay::Utm;
    bool enforceFreeRules_ = false;
    bool chaseTopView_ = true;
    bool chaseBehindView_ = true;
};

}  // namespace ds::app
