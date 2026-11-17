// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>

#include "ds/common/error.hpp"
#include "ds/storage/mission_store.hpp"

class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;

/// @defgroup app Application
/// @brief Qt6 Widgets GUI: main window, 3D viewport, tab panel and command line (SPECIFICATION
/// §4.11).

/// @file
/// @brief Main window: 3D viewport, tab panel, menus and status bar (SPECIFICATION §4.11).
/// @ingroup app

namespace ds::app {

class EnvironmentPanel;
class MissionBriefingDialog;
class MissionDebriefDialog;
class MissionsController;
class MissionsPanel;
class ItemsController;
class SceneController;
class SettingsController;
class SimulationController;
class Viewport;

/// @brief Top-level window of the application.
/// @details Tabs without content yet show which milestone fills them (SPECIFICATION §10).
/// The joystick service is started unless the environment variable `DS_NO_JOYSTICKS` is set
/// (tests, machines where SDL misbehaves).
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// @brief Builds the window, menus, tab panel and status bar.
    /// @param settings User settings, or null to run without persistence (tests). If given, it
    ///        must outlive the window: the saved geometry and dock layout are restored here and
    ///        saved again when the window closes (FR-UI-03).
    /// @param missions The mission list for the Missions tab, or null: the window then lists
    ///        the built-in missions only (tests). If given, it must outlive the window.
    /// @param parent Parent widget, normally null.
    explicit MainWindow(SettingsController* settings = nullptr,
                        MissionsController* missions = nullptr, QWidget* parent = nullptr);
    /// @brief Stops the simulation before the scene it depends on is destroyed.
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    /// @brief The 3D viewport (central widget).
    [[nodiscard]] Viewport* viewport() const noexcept { return viewport_; }
    /// @brief The free-mode scene (terrain, time, camera).
    [[nodiscard]] SceneController* scene() const noexcept { return scene_; }
    /// @brief The mission list shown in the Missions tab.
    [[nodiscard]] MissionsController* missions() const noexcept { return missions_; }
    /// @brief The flight simulation (vehicle, loop, input).
    [[nodiscard]] SimulationController* simulation() const noexcept { return simulation_; }
    /// @brief Items of the free-flight scene (Items tab).
    [[nodiscard]] ItemsController* items() const noexcept { return items_; }
    /// @brief The dockable tab panel.
    [[nodiscard]] QDockWidget* panelDock() const noexcept { return panelDock_; }
    /// @brief The tab widget inside the panel.
    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    /// @brief Toggles full-screen mode (`F11`).
    [[nodiscard]] QAction* fullScreenAction() const noexcept { return fullScreenAction_; }
    /// @brief Shows or hides the tab panel (`F9`).
    [[nodiscard]] QAction* panelAction() const noexcept { return panelAction_; }

    // --- Mission flow (FR-UI-07) -----------------------------------------------------------

    /// @brief Opens the briefing of mission @p key; *Fly* starts it (launchMission()).
    void startMission(const std::string& key);
    /// @brief Starts mission @p key with the profile @p profileKey ("" ⇒ the mission's own)
    /// without a briefing: hides the panel (FR-UI-04) and locks the scene tabs.
    common::Result<void> launchMission(const std::string& key, const QString& profileKey = {});
    /// @brief Leaves the mission and goes back to free flight.
    void endMission();
    /// @brief Missions › Import Mission: copies @p source (a mission file or folder) into the
    /// user's mission folder and selects it. Asks before replacing an existing mission.
    void importMission(const QString& source);
    /// @brief The open briefing, or null.
    [[nodiscard]] MissionBriefingDialog* briefingDialog() const noexcept;
    /// @brief The open debrief, or null.
    [[nodiscard]] MissionDebriefDialog* debriefDialog() const noexcept;
    /// @brief The Missions tab.
    [[nodiscard]] MissionsPanel* missionsPanel() const noexcept { return missionsPanel_; }
    /// @brief Delay between the end of an attempt and its debrief, milliseconds.
    static constexpr int kDebriefDelayMs = 1500;

    /// @brief Rich text of the Help › About dialog (name, version, copyright, licences).
    [[nodiscard]] static QString aboutText();

signals:
    /// @brief The application cannot continue (e.g. OpenGL failed, FR-GFX-01).
    /// @details The window does not show dialogs or exit by itself: main() reports
    /// @p message to the user and quits, which keeps the window testable.
    void fatalError(const QString& message);

protected:
    /// @brief Saves the window layout before closing.
    void closeEvent(QCloseEvent* event) override;
    /// @brief Keeps the full-screen action in step with the window state.
    void changeEvent(QEvent* event) override;

private:
    void restoreLayout();
    void createViewport();
    void createPanel();
    void createMenus();
    void createStatusBar();
    void setFullScreen(bool enabled);
    void showAbout();
    void onGlReady(const QString& summary);
    void onGlFailed(const QString& message);
    void updatePositionReadout();
    void updateCameraReadout();
    void updateSimulationActions();
    void updateMissionActions();
    void onMissionFinished();
    void showDebrief();
    void closeDebrief();
    void chooseImport(bool folder);

    SettingsController* settings_ = nullptr;
    SceneController* scene_ = nullptr;
    SimulationController* simulation_ = nullptr;
    MissionsController* missions_ = nullptr;
    Viewport* viewport_ = nullptr;
    EnvironmentPanel* environmentPanel_ = nullptr;
    MissionsPanel* missionsPanel_ = nullptr;
    QWidget* terrainPanel_ = nullptr;
    QWidget* vehiclePanel_ = nullptr;
    ItemsController* items_ = nullptr;
    QWidget* itemsPanel_ = nullptr;
    QPointer<MissionBriefingDialog> briefing_;
    QPointer<MissionDebriefDialog> debrief_;
    std::string missionKey_;
    std::optional<std::int64_t> lastAttemptId_;
    std::optional<storage::MissionAttemptRecord> bestBefore_;
    bool panelWasVisible_ = true;
    QAction* startMissionAction_ = nullptr;
    QAction* retryMissionAction_ = nullptr;
    QAction* abortMissionAction_ = nullptr;
    QAction* endMissionAction_ = nullptr;
    QLabel* missionLabel_ = nullptr;
    QDockWidget* panelDock_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QAction* fullScreenAction_ = nullptr;
    QAction* panelAction_ = nullptr;
    QAction* startAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* stepAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    QLabel* positionLabel_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* glLabel_ = nullptr;
};

}  // namespace ds::app
