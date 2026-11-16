// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "ds/app/asset_paths.hpp"
#include "ds/app/controls_panel.hpp"
#include "ds/app/environment_panel.hpp"
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
    {QT_TRANSLATE_NOOP("ds::app::MainWindow", "History"), "M5"},
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
    createViewport();
    createPanel();
    createMenus();
    createStatusBar();
    updateSimulationActions();
    restoreLayout();
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
            auto* panel = new MissionsPanel(*missions_);
            panel->setObjectName(QStringLiteral("MissionsPanel"));
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Terrain") {
            auto* panel = new TerrainPanel(*scene_);
            panel->setObjectName(QStringLiteral("TerrainPanel"));
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Simulation") {
            auto* panel = new SimulationPanel(*simulation_);
            panel->setObjectName(QStringLiteral("SimulationPanel"));
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Vehicle") {
            auto* panel = new VehiclePanel(*simulation_);
            panel->setObjectName(QStringLiteral("VehiclePanel"));
            tabs_->addTab(panel, tr(tab.title));
            continue;
        }
        if (title == "Environment") {
            environmentPanel_ = new EnvironmentPanel(*scene_);
            environmentPanel_->setObjectName(QStringLiteral("EnvironmentPanel"));
            tabs_->addTab(environmentPanel_, tr(tab.title));
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

    // Placeholders: enabled as the corresponding milestones land.
    QMenu* missionsMenu = menuBar()->addMenu(tr("&Missions"));
    missionsMenu->addAction(tr("&Show Missions"), this, [this] {
        panelDock_->show();
        tabs_->setCurrentIndex(0);  // Missions tab.
    });
    missionsMenu->addAction(tr("&Reload Missions"), missions_, &MissionsController::reload);
    missionsMenu->addSeparator();
    missionsMenu->addAction(tr("&Import Mission…"))->setEnabled(false);

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

}  // namespace ds::app
