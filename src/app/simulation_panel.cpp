// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/simulation_panel.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <array>
#include <cmath>
#include <numbers>

#include "ds/app/simulation_controller.hpp"
#include "ds/physics/flight_controller.hpp"

namespace ds::app {
namespace {

constexpr std::array<double, 6> kTimeScales{0.25, 0.5, 1.0, 2.0, 3.0, 4.0};
constexpr int kMaxEvents = 100;

QString stateName(sim::RunState state) {
    switch (state) {
        case sim::RunState::Stopped: return SimulationPanel::tr("Not started");
        case sim::RunState::Running: return SimulationPanel::tr("Running");
        case sim::RunState::Paused: return SimulationPanel::tr("Paused");
        case sim::RunState::SingleStep: return SimulationPanel::tr("Stepping");
    }
    return {};
}

QProgressBar* stickBar(const QString& name, bool bipolar) {
    auto* bar = new QProgressBar;
    bar->setObjectName(name);
    bar->setRange(bipolar ? -100 : 0, 100);
    bar->setValue(0);
    bar->setFormat(QStringLiteral("%v %"));
    bar->setMaximumHeight(16);
    return bar;
}

QString fromStd(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

SimulationPanel::SimulationPanel(SimulationController& simulation, QWidget* parent)
    : QWidget(parent), simulation_(&simulation) {
    buildUi();
    connect(simulation_, &SimulationController::telemetryUpdated, this, &SimulationPanel::refresh);
    connect(simulation_, &SimulationController::stateChanged, this,
            &SimulationPanel::updateButtons);
    connect(simulation_, &SimulationController::cameraModeChanged, this, [this] {
        const QSignalBlocker block(camera_);
        camera_->setCurrentIndex(static_cast<int>(simulation_->cameraMode()));
    });
    connect(simulation_, &SimulationController::simulationEvent, this, &SimulationPanel::addEvent);
    connect(simulation_, &SimulationController::missionActiveChanged, this,
            &SimulationPanel::updateFailureControls);
    updateButtons();
    refresh();
}

void SimulationPanel::buildUi() {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    // --- Run control ---------------------------------------------------------------------
    auto* runBox = new QGroupBox(tr("Simulation"));
    auto* runLayout = new QVBoxLayout(runBox);
    auto* buttons = new QHBoxLayout;
    start_ = new QPushButton(tr("Start"));
    start_->setObjectName(QStringLiteral("StartButton"));
    pause_ = new QPushButton(tr("Pause"));
    pause_->setObjectName(QStringLiteral("PauseButton"));
    step_ = new QPushButton(tr("Step"));
    step_->setObjectName(QStringLiteral("StepButton"));
    step_->setToolTip(tr("Runs a single simulation tick."));
    reset_ = new QPushButton(tr("Reset"));
    reset_->setObjectName(QStringLiteral("ResetButton"));
    reset_->setToolTip(tr("Back to the take-off point, disarmed, full battery (R)."));
    for (QPushButton* b : {start_, pause_, step_, reset_}) {
        buttons->addWidget(b);
    }
    runLayout->addLayout(buttons);
    connect(start_, &QPushButton::clicked, simulation_, &SimulationController::start);
    connect(pause_, &QPushButton::clicked, simulation_, &SimulationController::pause);
    connect(step_, &QPushButton::clicked, simulation_, &SimulationController::step);
    connect(reset_, &QPushButton::clicked, simulation_, &SimulationController::reset);

    auto* form = new QFormLayout;
    state_ = new QLabel;
    state_->setObjectName(QStringLiteral("SimStateLabel"));
    form->addRow(tr("State:"), state_);
    timeScale_ = new QComboBox;
    timeScale_->setObjectName(QStringLiteral("TimeScaleCombo"));
    for (const double scale : kTimeScales) {
        timeScale_->addItem(QStringLiteral("%1×").arg(scale), scale);
    }
    timeScale_->setCurrentIndex(2);
    connect(timeScale_, &QComboBox::activated, this, [this](int index) {
        if (auto set = simulation_->setTimeScale(timeScale_->itemData(index).toDouble()); !set) {
            addEvent(tr("Time scale: %1").arg(fromStd(set.error().message)));
        }
    });
    form->addRow(tr("Time scale:"), timeScale_);
    camera_ = new QComboBox;
    camera_->setObjectName(QStringLiteral("CameraCombo"));
    for (int i = 0; i < graphics::kCameraModeCount; ++i) {
        const auto name = graphics::cameraModeName(static_cast<graphics::CameraMode>(i));
        camera_->addItem(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
    }
    camera_->setCurrentIndex(static_cast<int>(simulation_->cameraMode()));
    camera_->setToolTip(tr("C cycles through the cameras."));
    connect(camera_, &QComboBox::activated, this, [this](int index) {
        simulation_->setCameraMode(static_cast<graphics::CameraMode>(index));
    });
    form->addRow(tr("Camera:"), camera_);
    auto* home = new QPushButton(tr("Take Off From Free-Camera Position"));
    home->setObjectName(QStringLiteral("HomeButton"));
    home->setToolTip(tr("Moves the take-off point under the free camera and resets."));
    connect(home, &QPushButton::clicked, simulation_, &SimulationController::moveHomeToCamera);
    form->addRow(home);
    runLayout->addLayout(form);
    layout->addWidget(runBox);

    // --- Pilot controls ------------------------------------------------------------------
    auto* controlsBox = new QGroupBox(tr("Controls"));
    auto* grid = new QGridLayout(controlsBox);
    input_ = new QLabel;
    input_->setObjectName(QStringLiteral("InputSourceLabel"));
    grid->addWidget(input_, 0, 0, 1, 2);
    throttle_ = stickBar(QStringLiteral("ThrottleBar"), false);
    roll_ = stickBar(QStringLiteral("RollBar"), true);
    pitch_ = stickBar(QStringLiteral("PitchBar"), true);
    yaw_ = stickBar(QStringLiteral("YawBar"), true);
    int row = 1;
    for (QProgressBar* bar : {throttle_, roll_, pitch_, yaw_}) {
        auto* label = new QLabel;
        barLabels_[static_cast<std::size_t>(row - 1)] = label;
        grid->addWidget(label, row, 0);
        grid->addWidget(bar, row, 1);
        ++row;
    }
    keys_ = new QLabel;
    keys_->setObjectName(QStringLiteral("KeyboardHelpLabel"));
    keys_->setWordWrap(true);
    grid->addWidget(keys_, row, 0, 1, 2);
    groundShown_ = simulation_->vehicleType() != physics::VehicleType::Ground;  // Forces a set-up.
    layout->addWidget(controlsBox);

    // --- Telemetry and statistics ----------------------------------------------------------
    auto* telemetryBox = new QGroupBox(tr("Telemetry"));
    auto* telemetryLayout = new QVBoxLayout(telemetryBox);
    telemetry_ = new QLabel;
    telemetry_->setObjectName(QStringLiteral("TelemetryLabel"));
    telemetry_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    telemetryLayout->addWidget(telemetry_);
    layout->addWidget(telemetryBox);

    // --- Failure injection (FR-PHY-15) -------------------------------------------------------
    failureBox_ = new QGroupBox(tr("Failure injection"));
    failureBox_->setObjectName(QStringLiteral("FailureBox"));
    auto* failureForm = new QFormLayout(failureBox_);
    failureType_ = new QComboBox;
    failureType_->setObjectName(QStringLiteral("FailureTypeCombo"));
    for (const physics::FailureType t : physics::kFailureTypes) {
        failureType_->addItem(tr(physics::failureTitle(t).data()), static_cast<int>(t));
    }
    failureType_->setToolTip(
        tr("GPS drift: 0.5 m/s up to 20 m. Battery cell fault: 30 % of the capacity lost. "
           "Compass interference: 30° off. Barometer drift: 0.2 m/s up to 15 m."));
    failureForm->addRow(tr("Failure:"), failureType_);
    failureMotor_ = new QSpinBox;
    failureMotor_->setObjectName(QStringLiteral("FailureMotorSpin"));
    failureMotor_->setRange(1, static_cast<int>(physics::kMaxFailureMotors));
    failureMotor_->setToolTip(tr("Motor (aerial) or driven wheel (ground)."));
    failurePower_ = new QSpinBox;
    failurePower_->setObjectName(QStringLiteral("FailurePowerSpin"));
    failurePower_->setRange(0, 100);
    failurePower_->setSuffix(tr(" % left"));
    failurePower_->setToolTip(tr("Thrust (rotor) or drive (wheel) left: 0 % = the motor stops."));
    auto* motorRow = new QHBoxLayout;
    motorRow->addWidget(failureMotor_);
    motorRow->addWidget(failurePower_, 1);
    failureForm->addRow(tr("Motor:"), motorRow);
    failureDuration_ = new QSpinBox;
    failureDuration_->setObjectName(QStringLiteral("FailureDurationSpin"));
    failureDuration_->setRange(0, 3600);
    failureDuration_->setSuffix(tr(" s"));
    failureDuration_->setSpecialValueText(tr("until reset"));
    failureForm->addRow(tr("Duration:"), failureDuration_);
    auto* inject = new QPushButton(tr("Inject"));
    inject->setObjectName(QStringLiteral("InjectFailureButton"));
    auto* clear = new QPushButton(tr("Clear all"));
    clear->setObjectName(QStringLiteral("ClearFailuresButton"));
    auto* failureButtons = new QHBoxLayout;
    failureButtons->addWidget(inject);
    failureButtons->addWidget(clear);
    failureForm->addRow(failureButtons);
    activeFailures_ = new QLabel;
    activeFailures_->setObjectName(QStringLiteral("ActiveFailuresLabel"));
    activeFailures_->setWordWrap(true);
    failureForm->addRow(tr("Active:"), activeFailures_);
    connect(failureType_, &QComboBox::currentIndexChanged, this,
            &SimulationPanel::updateFailureControls);
    connect(inject, &QPushButton::clicked, this, [this] {
        physics::Failure f = physics::defaultFailure(
            static_cast<physics::FailureType>(failureType_->currentData().toInt()));
        f.motor = failureMotor_->value();
        f.efficiency = failurePower_->value() / 100.0;
        if (failureDuration_->value() > 0) {
            f.durationS = failureDuration_->value();
        }
        simulation_->injectFailure(f);
    });
    connect(clear, &QPushButton::clicked, this, [this] { simulation_->clearFailures(); });
    layout->addWidget(failureBox_);
    updateFailureControls();

    auto* statsBox = new QGroupBox(tr("This flight"));
    auto* statsLayout = new QVBoxLayout(statsBox);
    stats_ = new QLabel;
    stats_->setObjectName(QStringLiteral("FlightStatsLabel"));
    statsLayout->addWidget(stats_);
    layout->addWidget(statsBox);

    auto* eventsBox = new QGroupBox(tr("Events"));
    auto* eventsLayout = new QVBoxLayout(eventsBox);
    events_ = new QListWidget;
    events_->setObjectName(QStringLiteral("EventList"));
    events_->setMinimumHeight(100);
    eventsLayout->addWidget(events_);
    layout->addWidget(eventsBox);
    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void SimulationPanel::updateFailureControls() {
    // Missions schedule their own failures.
    failureBox_->setEnabled(!simulation_->missionActive());
    const bool motor = static_cast<physics::FailureType>(failureType_->currentData().toInt()) ==
                       physics::FailureType::MotorFailure;
    failureMotor_->setEnabled(motor);
    failurePower_->setEnabled(motor);
    const bool once = static_cast<physics::FailureType>(failureType_->currentData().toInt()) ==
                      physics::FailureType::BatteryDrop;
    failureDuration_->setEnabled(!once);
}

void SimulationPanel::updateButtons() {
    const sim::RunState state = simulation_->state();
    const bool running = state == sim::RunState::Running;
    start_->setText(state == sim::RunState::Paused ? tr("Resume") : tr("Start"));
    start_->setEnabled(!running);
    pause_->setEnabled(running);
    state_->setText(stateName(state));
}

void SimulationPanel::addEvent(const QString& text) {
    const sim::WorldSnapshot& s = simulation_->snapshot();
    const auto t = static_cast<int>(s.simTimeS);
    events_->insertItem(0, QStringLiteral("%1:%2  %3")
                               .arg(t / 60, 2, 10, QLatin1Char('0'))
                               .arg(t % 60, 2, 10, QLatin1Char('0'))
                               .arg(text));
    while (events_->count() > kMaxEvents) {
        delete events_->takeItem(events_->count() - 1);
    }
}

void SimulationPanel::refresh() {
    const sim::WorldSnapshot& s = simulation_->snapshot();
    {
        QStringList names;
        for (const std::string& f : s.activeFailures) {
            names << tr(f.c_str());
        }
        activeFailures_->setText(names.isEmpty() ? tr("none") : names.join(QStringLiteral(", ")));
    }
    const sim::HudModel& hud = simulation_->hud();
    const auto percent = [](double v) { return static_cast<int>(std::lround(100.0 * v)); };
    const input::ControlInputs& c = s.controls;
    const bool ground = simulation_->vehicleType() == physics::VehicleType::Ground;
    if (ground != groundShown_) {
        groundShown_ = ground;
        const std::array<QString, 4> names =
            ground ? std::array<QString, 4>{tr("Drive"), tr("Steering"), QString(), QString()}
                   : std::array<QString, 4>{tr("Throttle"), tr("Roll"), tr("Pitch"), tr("Yaw")};
        for (std::size_t i = 0; i < names.size(); ++i) {
            barLabels_[i]->setText(names[i]);
        }
        throttle_->setRange(ground ? -100 : 0, 100);
        for (QWidget* w :
             std::initializer_list<QWidget*>{pitch_, yaw_, barLabels_[2], barLabels_[3]}) {
            w->setVisible(!ground);
        }
        keys_->setText(
            ground ? tr("<small>Keyboard (vehicle cameras): W/S forward and reverse, A/D steer, "
                        "Space brake, Enter arm, 1–2 Manual, 3–4 Speed control, L lights. "
                        "C camera, P pause, R reset.</small>")
                   : tr("<small>Keyboard (vehicle cameras): W/S throttle, A/D yaw, arrows pitch "
                        "and roll, Space or Enter arm, 1–4 Acro/Angle/Alt hold/Pos hold, "
                        "H return to home, L lights. C camera, P pause, R reset.</small>"));
    }
    throttle_->setValue(percent(ground ? c.drive : c.throttle));
    roll_->setValue(percent(ground ? c.steering : c.roll));
    pitch_->setValue(percent(c.pitch));
    yaw_->setValue(percent(c.yaw));
    switch (simulation_->activeInputSource()) {
        case input::InputSource::Joystick:
            input_->setText(tr("Input: %1")
                                .arg(simulation_->joystickMapping()
                                         ? fromStd(simulation_->joystickMapping()->deviceName)
                                         : tr("joystick")));
            break;
        case input::InputSource::Keyboard: input_->setText(tr("Input: keyboard")); break;
        case input::InputSource::None: input_->setText(tr("Input: none")); break;
    }

    const physics::VehicleTelemetry& t = s.telemetry;
    const auto mode = t.type == physics::VehicleType::Ground ? physics::driveModeName(t.ground.mode)
                                                             : physics::flightModeName(t.mode);
    QStringList lines;
    lines << tr("Vehicle: %1")
                 .arg(fromStd(s.profileName) +
                      (simulation_->profileModified() ? tr(" (edited)") : QString()));
    lines << tr("%1   %2").arg(fromStd(hud.armed.text),
                               QString::fromUtf8(mode.data(), static_cast<qsizetype>(mode.size())));
    lines << fromStd(hud.battery.text);
    if (ground) {
        const physics::GroundTelemetry& g = t.ground;
        const std::string surface = physics::surfaceDescription(g.surface, g.cover);
        lines << tr("%1   odometry %2 m/s   %3 m")
                     .arg(fromStd(hud.speed.text))
                     .arg(g.odometrySpeedMs, 0, 'f', 2)
                     .arg(g.odometerM, 0, 'f', 0);
        lines << tr("Pitch %1°   Roll %2°   Slope %3° on %4")
                     .arg(s.attitude.pitch * 180.0 / std::numbers::pi, 0, 'f', 0)
                     .arg(s.attitude.roll * 180.0 / std::numbers::pi, 0, 'f', 0)
                     .arg(g.slopeDeg, 0, 'f', 0)
                     .arg(
                         QString::fromUtf8(surface.data(), static_cast<qsizetype>(surface.size())));
        lines << tr("Wheel slip up to %1 %   motors L %2 % R %3 %%4")
                     .arg(100.0 * g.maxSlip, 0, 'f', 0)
                     .arg(100.0 * g.leftCommand, 0, 'f', 0)
                     .arg(100.0 * g.rightCommand, 0, 'f', 0)
                     .arg(g.braking ? tr("   braking") : QString());
    } else {
        lines << tr("%1   %2").arg(fromStd(hud.altitudeAgl.text), fromStd(hud.altitudeAmsl.text));
        lines << tr("%1   %2").arg(fromStd(hud.speed.text), fromStd(hud.verticalSpeed.text));
    }
    lines << fromStd(hud.gps.text);
    if (!hud.wind.text.empty()) {
        lines << fromStd(hud.wind.text);
    }
    lines << tr("Sim time %1 s   %2 Hz   %3 ms/tick")
                 .arg(s.simTimeS, 0, 'f', 1)
                 .arg(s.tickRateHz, 0, 'f', 0)
                 .arg(s.tickCostMs, 0, 'f', 3);
    telemetry_->setText(lines.join(QLatin1Char('\n')));

    const sim::SessionStats& st = s.stats;
    stats_->setText(tr("Flight time %1 s   Distance %2 m\nMax AGL %3 m   Max speed %4 m/s\n"
                       "Energy %5 Wh   Crashes %6")
                        .arg(st.flightTimeS, 0, 'f', 0)
                        .arg(st.distanceM, 0, 'f', 0)
                        .arg(st.maxAglM, 0, 'f', 1)
                        .arg(st.maxSpeedMs, 0, 'f', 1)
                        .arg(st.energyWh, 0, 'f', 2)
                        .arg(st.crashes));
    if (state_->text() != stateName(simulation_->state())) {
        updateButtons();
    }
}

}  // namespace ds::app
