// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

#include <array>

class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

/// @file
/// @brief Simulation tab (SPECIFICATION §4.11): run control, time scale, camera, live
/// controls, telemetry, flight statistics and events.
/// @ingroup app

namespace ds::app {

class SimulationController;

/// @brief Controls and watches the simulation.
class SimulationPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Shows @p simulation, which must outlive the panel.
    explicit SimulationPanel(SimulationController& simulation, QWidget* parent = nullptr);

    /// @brief Refreshes the read-outs from the latest snapshot.
    void refresh();

private:
    void buildUi();
    void updateButtons();
    void addEvent(const QString& text);

    SimulationController* simulation_;
    QPushButton* start_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* step_ = nullptr;
    QPushButton* reset_ = nullptr;
    QComboBox* timeScale_ = nullptr;
    QComboBox* camera_ = nullptr;
    QLabel* state_ = nullptr;
    QLabel* input_ = nullptr;
    QProgressBar* throttle_ = nullptr;
    QProgressBar* roll_ = nullptr;
    QProgressBar* pitch_ = nullptr;
    QProgressBar* yaw_ = nullptr;
    std::array<QLabel*, 4> barLabels_{};  // Throttle/drive, roll/steering, pitch, yaw.
    QLabel* keys_ = nullptr;
    bool groundShown_ = false;
    QLabel* telemetry_ = nullptr;
    QLabel* stats_ = nullptr;
    QListWidget* events_ = nullptr;
};

}  // namespace ds::app
