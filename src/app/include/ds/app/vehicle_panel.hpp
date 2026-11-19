// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

/// @file
/// @brief Vehicle tab (SPECIFICATION §4.11, FR-PHY-15, FR-GND-13): vehicle type (aerial or
/// ground), profile selection, derived figures (aerial: hover, thrust-to-weight, endurance,
/// top speed, wind limit; ground: top speed, acceleration, climbing limits, rollover angles,
/// range) and the parameter editor.
/// @ingroup app

namespace ds::app {

class SimulationController;

/// @brief Chooses and edits the vehicle profile.
class VehiclePanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Edits the profile of @p simulation, which must outlive the panel.
    explicit VehiclePanel(SimulationController& simulation, QWidget* parent = nullptr);

    /// @brief Reloads the profile list, the parameters and the figures.
    void refresh();

private:
    void buildUi();
    void buildEditor(QWidget* parent, bool ground);
    void reloadProfiles();
    void loadParameters();
    void updateFigures();
    void updateButtons();
    void apply();
    void saveAs();
    void remove();
    void showError(const QString& message);

    SimulationController* simulation_;
    QComboBox* type_ = nullptr;
    QComboBox* profile_ = nullptr;
    QLabel* description_ = nullptr;
    QLabel* figures_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* apply_ = nullptr;
    QPushButton* revert_ = nullptr;
    QPushButton* saveAs_ = nullptr;
    QPushButton* delete_ = nullptr;
    QWidget* aerialEditor_ = nullptr;
    QWidget* groundEditor_ = nullptr;
    std::vector<QDoubleSpinBox*> params_;        // In aerialParameters() order.
    std::vector<QDoubleSpinBox*> groundParams_;  // In groundParameters() order.
    bool edited_ = false;
};

}  // namespace ds::app
