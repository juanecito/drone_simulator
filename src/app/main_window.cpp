// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "ds/app/asset_paths.hpp"
#include "ds/app/controls_panel.hpp"
#include "ds/app/environment_panel.hpp"
#include "ds/app/history_panel.hpp"
#include "ds/app/items_controller.hpp"
#include "ds/app/items_panel.hpp"
#include "ds/app/mission_dialogs.hpp"
#include "ds/app/mission_setup.hpp"
#include "ds/app/missions_controller.hpp"
#include "ds/app/missions_panel.hpp"
#include "ds/app/scene_controller.hpp"
#include "ds/app/settings_controller.hpp"
#include "ds/app/settings_panel.hpp"
#include "ds/app/simulation_controller.hpp"
#include "ds/app/simulation_panel.hpp"
#include "ds/app/terrain_panel.hpp"
#include "ds/app/vehicle_panel.hpp"
#include "ds/app/viewport.hpp"
#include "ds/common/version.hpp"
#include "ds/geo/utm.hpp"

namespace ds::app {
namespace {

struct PlaceholderTab {
    const char* title;
    const char* milestone;
};

// Tabs from SPECIFICATION §4.11, in display order, with the milestone that fills them.
constexpr std::array<PlaceholderTab, 8> kTabs{{
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Missions"), "M5"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Simulation"), "M3"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Vehicle"), "M3"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Terrain"), "M2"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Items"), "M5"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Environment"), "M2"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "Settings"), "M1"},
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "History"), "M7"},
}};

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

MainWindow::MainWindow(SettingsController* settings, MissionsController* missions,
                       QWidget* parent)
    : QMainWindow(parent), settings_(settings), missions_(missions) {
    setWindowTitle(fromView(common::kApplicationName));
    setObjectName(QStringLiteral("MainWindow"));
    resize(1280, 800);
    const std::filesystem::path assets = findAssetDirectory();
    if (missions_ == nullptr) {
        // Built-in missions only: no user folder is touched.
        missions_ = new MissionsController(
            MissionFolders{.assetDir = assets, .userDir = {}, .libraryDir = {}},
            settings_ != nullptr ? &settings_->storage() : nullptr, this);
    }
    scene_ = new SceneController(settings_, assets, this);
    const char* noJoysticks = std::getenv("DS_NO_JOYSTICKS");
    simulation_ = new SimulationController(*scene_, settings_, assets,
                                           noJoysticks == nullptr || *noJoysticks == '\0', this);
    items_ = new ItemsController(*scene_, settings_, this);
    simulation_->setFreeItems(items_->items());
    connect(items_, &ItemsController::changed, this,
            [this] { simulation_->setFreeItems(items_->items()); });
    createViewport();
    createPanel();
    createMenus();
    createStatusBar();
    updateSimulationActions();
    updateMissionActions();
    restoreLayout();
    connect(simulation_, &SimulationController::missionActiveChanged, this,
            &MainWindow::updateMissionActions);
    connect(simulation_, &SimulationController::missionStateChanged, this,
            &MainWindow::updateMissionActions);
    connect(simulation_, &SimulationController::missionFinished, this,
            &MainWindow::onMissionFinished);
}

MainWindow::~MainWindow() {
    // Children are deleted in creation order, which would destroy the scene first; the
    // simulation (its thread, the last flight record) must go before it.
    delete std::exchange(simulation_, nullptr);
}

void MainWindow::restoreLayout() {
    if (settings_ == nullptr) {
        return;
    }
    // Invalid or stale blobs are rejected by Qt and leave the defaults in place.
    if (const auto& geometry = settings_->windowGeometry()) {
        restoreGeometry(*geometry);
    }
    if (const auto& state = settings_->windowState()) {
        restoreState(*state);
    }
    // The saved state may float the panel again after the geometry made the window full
    // screen: apply the full-screen rule to the restored layout.
    if (panelDockedForFullScreen_) {
        panelDock_->setFeatures(panelDock_->features() | QDockWidget::DockWidgetFloatable);
        panelDockedForFullScreen_ = false;
    }
    keepPanelDocked(isFullScreen() || windowState().testFlag(Qt::WindowFullScreen));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (settings_ != nullptr) {
        settings_->setWindowLayout(saveGeometry(), saveState());
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange && fullScreenAction_ != nullptr) {
        const QSignalBlocker blocker(fullScreenAction_);
        fullScreenAction_->setChecked(isFullScreen());
        keepPanelDocked(isFullScreen());
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::createViewport() {
    viewport_ = new Viewport(scene_, simulation_, this);
    viewport_->setObjectName(QStringLiteral("Viewport"));
    setCentralWidget(viewport_);
    connect(viewport_, &Viewport::glReady, this, &MainWindow::onGlReady);
    // Queued: the error dialog must not be opened from inside initializeGL() (FR-GFX-01).
    connect(viewport_, &Viewport::glFailed, this, &MainWindow::onGlFailed, Qt::QueuedConnection);
    connect(viewport_, &Viewport::statsUpdated, this, &MainWindow::updatePositionReadout);
    // The mouse is only captured to look around with the free camera.
    connect(simulation_, &SimulationController::cameraModeChanged, this, [this] {
        if (simulation_->vehicleView()) {
            viewport_->releaseMouse();
        }
    });
    connect(simulation_, &SimulationController::simulationEvent, this,
            [this](const QString& text) { statusBar()->showMessage(text, 5000); });
    connect(simulation_, &SimulationController::stateChanged, this,
            &MainWindow::updateSimulationActions);
}

void MainWindow::createPanel() {
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("PanelTabs"));
    for (const auto& tab : kTabs) {
        const std::string_view title(tab.title);
        if (title == "Missions") {
            missionsPanel_ = new MissionsPanel(*missions_);
            missionsPanel_->setObjectName(QStringLiteral("MissionsPanel"));
            connect(missionsPanel_, &MissionsPanel::startRequested, this,
                    &MainWindow::startMission);
            if (settings_ != nullptr) {
                // The last mission selected comes back on the next start (FR-UI-03).
                if (const auto saved = settings_->readJson(settings_keys::kLastMission)) {
                    const QJsonDocument doc = QJsonDocument::fromJson("[" + *saved + "]");
                    missionsPanel_->selectMission(doc.array().at(0).toString().toStdString());
                }
                connect(missionsPanel_, &MissionsPanel::missionSelected, this,
                        [this](const std::string& key) {
                            settings_->writeJson(settings_keys::kLastMission,
                                                 QJsonDocument(QJsonArray{QString::fromStdString(key)})
                                                     .toJson(QJsonDocument::Compact)
                                                     .mid(1)
                                                     .chopped(1));
                        });
            }
            tabs_->addTab(missionsPanel_, tr(tab.title));
            continue;
        }
        if (title == "Terrain") {
            auto* panel = new TerrainPanel(*scene_);
            panel->setObjectName(QStringLiteral("TerrainPanel"));
            terrainPanel_ = panel;
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Simulation") {
            auto* panel = new SimulationPanel(*simulation_);
            panel->setObjectName(QStringLiteral("SimulationPanel"));
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Items") {
            auto* panel = new ItemsPanel(*items_, *simulation_, *scene_);
            panel->setObjectName(QStringLiteral("ItemsPanel"));
            itemsPanel_ = panel;
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Vehicle") {
            auto* panel = new VehiclePanel(*simulation_);
            panel->setObjectName(QStringLiteral("VehiclePanel"));
            vehiclePanel_ = panel;
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Environment") {
            environmentPanel_ = new EnvironmentPanel(*scene_);
            environmentPanel_->setObjectName(QStringLiteral("EnvironmentPanel"));
            tabs_->addTab(environmentPanel_, tr(tab.title));
            continue;
        }
        if (title == "History") {
            historyPanel_ = new HistoryPanel(settings_ != nullptr ? &settings_->storage() : nullptr,
                                             *simulation_);
            historyPanel_->setObjectName(QStringLiteral("HistoryPanel"));
            connect(historyPanel_, &HistoryPanel::replayRequested, this,
                    [this](const storage::ReplayRecord& replay) {
                        if (auto played = playReplay(replay); !played) {
                            QMessageBox::warning(
                                this, tr("Replay"),
                                tr("The replay cannot be played: %1")
                                    .arg(QString::fromStdString(played.error().message)));
                        }
                    });
            connect(missions_, &MissionsController::missionsChanged, historyPanel_,
                    &HistoryPanel::refresh);
            // In a scroll area: the tables and the plot do not widen the panel.
            auto* scroll = new QScrollArea;
            scroll->setWidget(historyPanel_);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            tabs_->addTab(scroll, tr(tab.title));
            continue;
        }
        if (title == "Settings") {
            // Display and data settings, then the controls (FR-IN-01…05).
            auto* content = new QWidget;
            auto* layout = new QVBoxLayout(content);
            if (settings_ != nullptr) {
                auto* panel = new SettingsPanel(*settings_);
                panel->setObjectName(QStringLiteral("SettingsPanel"));
                layout->addWidget(panel);
            } else {
                auto* label = new QLabel(tr("Settings are not saved without a database."));
                label->setEnabled(false);
                layout->addWidget(label);
            }
            auto* controls = new ControlsPanel(*simulation_, settings_);
            controls->setObjectName(QStringLiteral("ControlsPanel"));
            layout->addWidget(controls);
            layout->addStretch();
            auto* scroll = new QScrollArea;
            scroll->setWidget(content);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            tabs_->addTab(scroll, tr(tab.title));
            continue;
        }
        auto* label =
            new QLabel(tr("Coming in milestone %1.").arg(QLatin1StringView(tab.milestone)));
        label->setAlignment(Qt::AlignCenter);
        label->setEnabled(false);
        tabs_->addTab(label, tr(tab.title));
    }

    panelDock_ = new QDockWidget(tr("Panel"), this);
    panelDock_->setObjectName(QStringLiteral("PanelDock"));
    panelDock_->setWidget(tabs_);
    panelDock_->setMinimumWidth(320);
    addDockWidget(Qt::RightDockWidgetArea, panelDock_);

    panelAction_ = panelDock_->toggleViewAction();
    panelAction_->setText(tr("Show &Panel"));
    panelAction_->setShortcut(QKeySequence(Qt::Key_F9));
}

void MainWindow::createMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);
    quitAction->setMenuRole(QAction::QuitRole);

    QMenu* missionsMenu = menuBar()->addMenu(tr("&Missions"));
    missionsMenu->addAction(tr("&Show Missions"), this, [this] {
        panelDock_->show();
        tabs_->setCurrentIndex(0);  // Missions tab.
    });
    startMissionAction_ = missionsMenu->addAction(tr("S&tart Mission…"), this, [this] {
        startMission(missionsPanel_->selectedMission());
    });
    retryMissionAction_ = missionsMenu->addAction(tr("Re&try Mission"), this, [this] {
        closeDebrief();
        simulation_->retryMission();
        viewport_->setFocus();
    });
    abortMissionAction_ = missionsMenu->addAction(tr("&Abort Mission"), simulation_,
                                                  &SimulationController::abortMission);
    endMissionAction_ =
        missionsMenu->addAction(tr("&Back to Free Flight"), this, &MainWindow::endMission);
    missionsMenu->addSeparator();
    missionsMenu->addAction(tr("&Reload Missions"), missions_, &MissionsController::reload);
    missionsMenu->addAction(tr("&Import Mission…"), this, [this] { chooseImport(false); });
    missionsMenu->addAction(tr("Import Mission &Folder…"), this, [this] { chooseImport(true); });

    // Function keys: letters are flight and camera keys in the viewport.
    QMenu* simulationMenu = menuBar()->addMenu(tr("&Simulation"));
    startAction_ =
        simulationMenu->addAction(tr("&Start"), simulation_, &SimulationController::start);
    startAction_->setShortcut(QKeySequence(Qt::Key_F5));
    pauseAction_ =
        simulationMenu->addAction(tr("&Pause"), simulation_, &SimulationController::pause);
    pauseAction_->setShortcut(QKeySequence(Qt::Key_F6));
    stepAction_ = simulationMenu->addAction(tr("S&tep"), simulation_, &SimulationController::step);
    stepAction_->setShortcut(QKeySequence(Qt::Key_F7));
    resetAction_ =
        simulationMenu->addAction(tr("&Reset Vehicle"), simulation_, &SimulationController::reset);
    resetAction_->setShortcut(QKeySequence(Qt::Key_F8));

    QMenu* vehicleMenu = menuBar()->addMenu(tr("&Vehicle"));
    vehicleMenu->addAction(tr("&Profiles…"), this, [this] {
        panelDock_->show();
        tabs_->setCurrentIndex(2);  // Vehicle tab.
    });

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    fullScreenAction_ = viewMenu->addAction(tr("&Full Screen"));
    fullScreenAction_->setCheckable(true);
    fullScreenAction_->setShortcut(QKeySequence(Qt::Key_F11));
    connect(fullScreenAction_, &QAction::toggled, this, &MainWindow::setFullScreen);
    viewMenu->addAction(panelAction_);
    QAction* cameraAction = viewMenu->addAction(tr("Next &Camera"), simulation_,
                                                &SimulationController::cycleCameraMode);
    cameraAction->setToolTip(tr("C in the 3D view"));

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction* aboutAction =
        helpMenu->addAction(tr("&About Drone Simulator"), this, &MainWindow::showAbout);
    aboutAction->setMenuRole(QAction::AboutRole);
    QAction* aboutQtAction = helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);
    aboutQtAction->setMenuRole(QAction::AboutQtRole);
}

void MainWindow::createStatusBar() {
    // Drone position and elevation (FR-UI-08a / FR-GFX-14a); filled in once a drone exists.
    positionLabel_ = new QLabel(tr("UTM —   Elev —"));
    positionLabel_->setObjectName(QStringLiteral("PositionLabel"));
    stateLabel_ = new QLabel(tr("Idle"));
    stateLabel_->setObjectName(QStringLiteral("StateLabel"));
    glLabel_ = new QLabel(tr("OpenGL: starting…"));
    glLabel_->setObjectName(QStringLiteral("GlLabel"));
    statusBar()->addWidget(positionLabel_, 1);
    fpsLabel_ = new QLabel;
    fpsLabel_->setObjectName(QStringLiteral("FpsLabel"));
    missionLabel_ = new QLabel;
    missionLabel_->setObjectName(QStringLiteral("MissionLabel"));
    statusBar()->addPermanentWidget(missionLabel_);
    statusBar()->addPermanentWidget(stateLabel_);
    statusBar()->addPermanentWidget(fpsLabel_);
    statusBar()->addPermanentWidget(glLabel_);
}

void MainWindow::setFullScreen(bool enabled) {
    if (enabled) {
        showFullScreen();
    } else {
        showNormal();
    }
}

void MainWindow::keepPanelDocked(bool fullScreen) {
    // A floating panel is a window of its own: next to a full-screen window the window
    // manager keeps handing the focus and the top of the stack from one to the other, and
    // the panel flickers. In full screen it is docked and cannot be undocked.
    if (panelDock_ == nullptr || fullScreen == panelDockedForFullScreen_) {
        return;
    }
    panelDockedForFullScreen_ = fullScreen;
    if (fullScreen) {
        panelWasFloating_ = panelDock_->isFloating();
        if (panelWasFloating_) {
            panelFloatingGeometry_ = panelDock_->geometry();
            panelDock_->setFloating(false);
        }
        panelDock_->setFeatures(panelDock_->features() & ~QDockWidget::DockWidgetFloatable);
        return;
    }
    panelDock_->setFeatures(panelDock_->features() | QDockWidget::DockWidgetFloatable);
    if (panelWasFloating_) {
        panelDock_->setFloating(true);
        panelDock_->setGeometry(panelFloatingGeometry_);
    }
}

QString MainWindow::aboutText() {
    return tr("<h3>%1 %2</h3>"
              "<p>Realistic 3D simulator for aerial and ground drones.</p>"
              "<p>%3<br>"
              "Released under the MIT License.</p>"
              "<p>Built with Qt %4 (LGPLv3). Third-party notices: "
              "<i>/usr/share/doc/drone-simulator/THIRD_PARTY_NOTICES.md</i>.</p>")
        .arg(fromView(common::kApplicationName), fromView(common::kVersion),
             fromView(common::kCopyright).toHtmlEscaped(), QString::fromLatin1(qVersion()));
}

void MainWindow::showAbout() { QMessageBox::about(this, tr("About Drone Simulator"), aboutText()); }

void MainWindow::onGlReady(const QString& summary) { glLabel_->setText(summary); }

void MainWindow::updateSimulationActions() {
    const sim::RunState state = simulation_->state();
    startAction_->setText(state == sim::RunState::Paused ? tr("&Resume") : tr("&Start"));
    startAction_->setEnabled(state != sim::RunState::Running);
    pauseAction_->setEnabled(state == sim::RunState::Running);
    switch (state) {
        case sim::RunState::Stopped: stateLabel_->setText(tr("Idle")); break;
        case sim::RunState::Running: stateLabel_->setText(tr("Running")); break;
        case sim::RunState::Paused: stateLabel_->setText(tr("Paused")); break;
        case sim::RunState::SingleStep: stateLabel_->setText(tr("Stepping")); break;
    }
}

void MainWindow::updatePositionReadout() {
    if (simulation_->vehicleView()) {
        // The drone's position (FR-UI-08a), as in the HUD position panel.
        const sim::HudModel& hud = simulation_->hud();
        QString text = QString::fromStdString(hud.positionUtm);
        if (!hud.positionLatLon.empty()) {
            text += QStringLiteral("  ") + QString::fromStdString(hud.positionLatLon);
        }
        text += QStringLiteral("   ") + QString::fromStdString(hud.positionElevation);
        positionLabel_->setText(text);
    } else {
        updateCameraReadout();
    }
    const auto& stats = viewport_->frameStats();
    fpsLabel_->setText(tr("%1 FPS  %2 chunks")
                           .arg(viewport_->framesPerSecond(), 0, 'f', 0)
                           .arg(stats.chunksDrawn));
    if (environmentPanel_ != nullptr) {
        environmentPanel_->setMeasuredExposure(viewport_->exposure().ev100,
                                               viewport_->exposure().averageLuminance);
    }
}

void MainWindow::updateCameraReadout() {
    const glm::dvec3 pos = scene_->camera().position;
    const geo::Geodetic geodetic = scene_->toGeodetic(pos);
    const double amsl = scene_->terrain().origin.heightM + pos.y;
    const double agl = pos.y - scene_->query().height(pos.x, pos.z);
    const bool imperial = settings_ != nullptr && settings_->units() == UnitSystem::Imperial;
    const auto length = [imperial](double metres) {
        return imperial ? tr("%1 ft").arg(metres / 0.3048, 0, 'f', 0)
                        : tr("%1 m").arg(metres, 0, 'f', 1);
    };
    QString text;
    if (auto utm = geo::toUtm(geodetic.latitudeDeg, geodetic.longitudeDeg)) {
        text = tr("Camera  UTM %1").arg(QString::fromStdString(geo::formatUtm(*utm)));
    } else {
        text = tr("Camera  (outside UTM)");
    }
    if (settings_ != nullptr && settings_->coordinateDisplay() == CoordinateDisplay::UtmAndLatLon) {
        text += QStringLiteral("  (%1°, %2°)")
                    .arg(geodetic.latitudeDeg, 0, 'f', 6)
                    .arg(geodetic.longitudeDeg, 0, 'f', 6);
    }
    text += tr("   Elev %1 AMSL   AGL %2").arg(length(amsl), length(agl));
    positionLabel_->setText(text);
}

void MainWindow::onGlFailed(const QString& message) {
    glLabel_->setText(tr("OpenGL: unavailable"));
    emit fatalError(message);
}

// --- Mission flow ----------------------------------------------------------------------------

MissionBriefingDialog* MainWindow::briefingDialog() const noexcept { return briefing_.data(); }
MissionDebriefDialog* MainWindow::debriefDialog() const noexcept { return debrief_.data(); }

void MainWindow::startMission(const std::string& key) {
    const MissionRow* row = missions_->find(key);
    if (row == nullptr || row->status != MissionStatus::Available || briefing_ ||
        simulation_->missionActive()) {
        return;
    }
    // The briefing offers the profiles and checks the controls of the mission's vehicle type.
    const physics::VehicleType type =
        row->ground ? physics::VehicleType::Ground : physics::VehicleType::Aerial;
    const bool keyboardOnly = !simulation_->joystickMapping(type).has_value();
    briefing_ = new MissionBriefingDialog(*row, missions_->entry(*row),
                                          simulation_->profiles(type), keyboardOnly, this);
    briefing_->setAttribute(Qt::WA_DeleteOnClose);
    connect(briefing_, &QDialog::finished, this, [this, key](int result) {
        // Closed: no longer "the open briefing", even before Qt deletes it.
        const QString profile = briefing_ ? briefing_->chosenProfile() : QString();
        briefing_.clear();
        if (result != QDialog::Accepted) {
            return;
        }
        if (auto launched = launchMission(key, profile); !launched) {
            QMessageBox::warning(this, tr("Mission"),
                                 tr("The mission cannot be started: %1")
                                     .arg(QString::fromStdString(launched.error().message)));
        }
    });
    briefing_->open();
}

common::Result<void> MainWindow::launchMission(const std::string& key, const QString& profileKey) {
    if (simulation_->missionActive()) {
        return common::makeError("another mission is being flown");
    }
    const MissionRow* row = missions_->find(key);
    if (row == nullptr || row->status != MissionStatus::Available) {
        return common::makeError("the mission is not available");
    }
    const missions::MissionDefinition& mission = *missions_->entry(*row).load.definition;
    auto profile = profileKey.isEmpty()
                       ? missionProfile(mission,
                                        [this](const std::string& k) {
                                            return simulation_->profileFor(QString::fromStdString(k));
                                        })
                       : simulation_->profileFor(profileKey);
    if (!profile) {
        return std::unexpected(profile.error());
    }
    const std::filesystem::path missionDir = missions_->entry(*row).load.file.parent_path();
    if (auto started = simulation_->startMission(mission, *profile, missionDir); !started) {
        return started;
    }
    missionKey_ = key;
    lastAttemptId_.reset();
    // Fly full view (FR-UI-04): the panel comes back after the mission.
    panelWasVisible_ = panelDock_->isVisible();
    panelDock_->hide();
    viewport_->setFocus();
    statusBar()->showMessage(mission.vehicle.type == physics::VehicleType::Ground
                                 ? tr("Arm the rover (Enter) to start the mission")
                                 : tr("Arm the drone to start the mission"),
                             8000);
    return {};
}

void MainWindow::closeDebrief() {
    if (debrief_) {
        debrief_->disconnect(this);  // Not a choice of the pilot's.
        debrief_->close();
        debrief_.clear();
    }
}

void MainWindow::endMission() {
    closeDebrief();
    if (!simulation_->missionActive()) {
        return;
    }
    simulation_->endMission();
    missionKey_.clear();
    if (panelWasVisible_) {
        panelDock_->show();
    }
}

common::Result<void> MainWindow::playReplay(const storage::ReplayRecord& replay) {
    if (simulation_->missionActive()) {
        return common::makeError("end the mission first");
    }
    if (replay.missionKey.empty()) {
        return simulation_->startReplay(replay);
    }
    const MissionRow* row = missions_->find(replay.missionKey);
    if (row == nullptr || !missions_->entry(*row).load.definition) {
        return common::makeError("the mission \"" + replay.missionKey + "\" is not installed");
    }
    const missions::CatalogEntry& entry = missions_->entry(*row);
    return simulation_->startReplay(replay, &*entry.load.definition, entry.load.file.parent_path());
}

void MainWindow::onMissionFinished() {
    const sim::MissionStatus& status = simulation_->missionStatus();
    if (!status.result || missionKey_.empty()) {
        return;
    }
    // The personal best before this attempt, then the attempt itself (FR-MIS-11, FR-MIS-12).
    bestBefore_ = missions_->bestAttempt(missionKey_);
    const astro::UtcTime started =
        simulation_->snapshot().utc -
        std::chrono::milliseconds(std::llround(status.elapsedS * 1000.0));
    auto id = missions_->recordAttempt(missionKey_, *status.result, started);
    lastAttemptId_ = id ? std::optional<std::int64_t>(*id) : std::nullopt;
    if (id) {
        simulation_->linkNextSessionToAttempt(*id);  // History: the attempt's replay.
    }
    const std::uint64_t attempt = status.attempt;
    QTimer::singleShot(kDebriefDelayMs, this, [this, attempt] {
        if (simulation_->missionActive() && simulation_->missionStatus().attempt == attempt &&
            simulation_->missionStatus().result) {
            simulation_->pause();
            showDebrief();
        }
    });
}

void MainWindow::showDebrief() {
    const sim::MissionStatus& status = simulation_->missionStatus();
    const missions::MissionDefinition* mission = simulation_->mission();
    if (debrief_ || mission == nullptr || !status.result) {
        return;
    }
    const MissionRow* next = missions_->nextMission(missionKey_);
    debrief_ = new MissionDebriefDialog(*mission, *status.result, status.path, bestBefore_,
                                        next != nullptr, this);
    debrief_->setAttribute(Qt::WA_DeleteOnClose);
    const std::string nextKey = next != nullptr ? next->key : std::string();
    connect(debrief_, &QDialog::finished, this, [this, nextKey] {
        const auto choice = debrief_ ? debrief_->choice() : MissionDebriefDialog::Choice::Back;
        debrief_.clear();  // Closed: a later attempt gets its own debrief.
        switch (choice) {
            case MissionDebriefDialog::Choice::Retry:
                simulation_->retryMission();
                viewport_->setFocus();
                break;
            case MissionDebriefDialog::Choice::Next:
                endMission();
                missionsPanel_->selectMission(nextKey);
                startMission(nextKey);
                break;
            case MissionDebriefDialog::Choice::Back: endMission(); break;
        }
    });
    debrief_->open();
}

void MainWindow::chooseImport(bool folder) {
    const QString source =
        folder ? QFileDialog::getExistingDirectory(this, tr("Import Mission Folder"))
               : QFileDialog::getOpenFileName(this, tr("Import Mission"), QString(),
                                              tr("Mission files (*.json)"));
    if (!source.isEmpty()) {
        importMission(source);
    }
}

void MainWindow::importMission(const QString& source) {
    auto imported = missions_->importMission(source.toStdString());
    if (!imported && imported.error().message.ends_with("already exists")) {
        const auto answer = QMessageBox::question(
            this, tr("Import Mission"),
            tr("%1\n\nReplace the mission in your folder?")
                .arg(QString::fromStdString(imported.error().message)));
        if (answer != QMessageBox::Yes) {
            return;
        }
        imported = missions_->importMission(source.toStdString(), true);
    }
    if (!imported) {
        QMessageBox::warning(this, tr("Import Mission"),
                             tr("The mission cannot be imported: %1")
                                 .arg(QString::fromStdString(imported.error().message)));
        return;
    }
    // Select it: its row is the one whose file is the imported one.
    for (const MissionRow& row : missions_->missions()) {
        if (missions_->entry(row).load.file == *imported) {
            missionsPanel_->selectMission(row.key);
            const QString state = row.status == MissionStatus::Invalid
                                      ? tr("it has errors, see the Missions tab")
                                      : tr("ready");
            statusBar()->showMessage(
                tr("Imported %1 — %2").arg(QString::fromStdString(row.title), state), 8000);
            break;
        }
    }
    panelDock_->show();
    tabs_->setCurrentWidget(missionsPanel_);
}

void MainWindow::updateMissionActions() {
    const bool active = simulation_->missionActive();
    const auto state = simulation_->missionStatus().state;
    const bool running = active && (state == missions::MissionState::InProgress ||
                                    state == missions::MissionState::PreFlight);
    startMissionAction_->setEnabled(!active);
    retryMissionAction_->setEnabled(active);
    abortMissionAction_->setEnabled(running);
    endMissionAction_->setEnabled(active);
    missionsPanel_->setMissionActive(active);
    // Mission rules: the scene and the vehicle are the mission's (FR-MIS-09).
    // The Items tab shows the mission's items, read only.
    for (QWidget* w : {terrainPanel_, vehiclePanel_, static_cast<QWidget*>(environmentPanel_)}) {
        if (w != nullptr) {
            w->setEnabled(!active);
        }
    }
    resetAction_->setEnabled(!active);
    if (!active) {
        missionLabel_->clear();
        return;
    }
    const missions::MissionDefinition* mission = simulation_->mission();
    QString text;
    switch (state) {
        case missions::MissionState::Briefing:
        case missions::MissionState::PreFlight: text = tr("pre-flight"); break;
        case missions::MissionState::InProgress: text = tr("in progress"); break;
        case missions::MissionState::Succeeded: text = tr("succeeded"); break;
        case missions::MissionState::Failed: text = tr("failed"); break;
        case missions::MissionState::Aborted: text = tr("aborted"); break;
        case missions::MissionState::Debrief: text = tr("debrief"); break;
    }
    missionLabel_->setText(tr("Mission: %1 — %2")
                               .arg(mission != nullptr ? QString::fromStdString(mission->title)
                                                       : QString(),
                                    text));
}

}  // namespace ds::app
