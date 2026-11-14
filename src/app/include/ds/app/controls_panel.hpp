// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>
#include <QWidget>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ds/input/calibration.hpp"
#include "ds/input/mapping.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

/// @file
/// @brief Settings › Controls (FR-IN-01…05): input device, stick mapping presets, calibration
/// wizard, expo and deadzones, saved per device in `input_profiles`.
/// @ingroup app

namespace ds::input {
class JoystickService;
}

namespace ds::app {

class SettingsController;
class SimulationController;

/// @brief Guided calibration of a joystick (sticks centred, extremes, then one movement per
/// channel to detect the channel order, the stick mode and inverted axes).
class CalibrationDialog final : public QDialog {
    Q_OBJECT

public:
    /// @brief Calibrates device @p guid (with @p axes axes) starting from @p base.
    CalibrationDialog(const input::JoystickService& joysticks, std::string guid, int axes,
                      input::InputMapping base, QWidget* parent = nullptr);

    /// @brief The calibrated mapping (after accept()).
    [[nodiscard]] const input::InputMapping& result() const noexcept { return wizard_.result(); }

private:
    void poll();
    void next();
    void updatePrompt();

    const input::JoystickService* joysticks_;
    std::string guid_;
    input::CalibrationWizard wizard_;
    QLabel* prompt_ = nullptr;
    QLabel* error_ = nullptr;
    std::vector<QProgressBar*> bars_;
    QPushButton* next_ = nullptr;
    QTimer* timer_ = nullptr;
};

/// @brief Chooses the flying device and edits its mapping.
class ControlsPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Edits the input of @p simulation; @p settings may be null (nothing is saved).
    ControlsPanel(SimulationController& simulation, SettingsController* settings,
                  QWidget* parent = nullptr);

private:
    void buildUi();
    void refreshDevices();
    void onDeviceChosen();
    void reloadMappings();
    void onMappingChosen();
    void showMapping();
    void readChannels();
    void poll();
    void use();
    void calibrate();
    void save();
    void remove();
    void updateStatus();
    [[nodiscard]] std::optional<std::string> selectedGuid() const;

    struct ChannelRow {
        QLabel* axis = nullptr;
        QCheckBox* inverted = nullptr;
        QDoubleSpinBox* expo = nullptr;
        QDoubleSpinBox* deadzone = nullptr;
        QProgressBar* bar = nullptr;
    };

    SimulationController* simulation_;
    SettingsController* settings_;
    QComboBox* device_ = nullptr;
    QComboBox* mapping_ = nullptr;
    QPushButton* use_ = nullptr;
    QPushButton* calibrate_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* delete_ = nullptr;
    QLabel* status_ = nullptr;
    std::array<ChannelRow, input::kChannelCount> rows_{};
    QTimer* timer_ = nullptr;
    std::uint64_t devicesVersion_ = ~std::uint64_t{0};
    input::InputMapping working_;
    input::RawJoystickState previousRaw_;
    input::ControlInputs previousControls_;
};

}  // namespace ds::app
