// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QDate>
#include <QWidget>

#include "ds/app/scene_config.hpp"

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTimer;

/// @file
/// @brief Environment tab (SPECIFICATION §4.11): date and time, sun and moon read-outs,
/// rise/set and twilight times, exposure, visibility, wind level and air. The weather
/// generator follows in M7.
/// @ingroup app

namespace ds::app {

class SceneController;

/// @brief Edits the scene's date, time, exposure and visibility.
class EnvironmentPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Shows and edits the environment of @p scene (which must outlive the panel).
    explicit EnvironmentPanel(SceneController& scene, QWidget* parent = nullptr);

    /// @brief Shows the measured exposure (from the viewport).
    void setMeasuredExposure(double ev100, double averageLuminance);
    /// @brief Name of the phase for an elongation (0 new, 180 full) and waxing flag.
    [[nodiscard]] static QString phaseName(double elongationDeg);
    /// @brief Refreshes all widgets from the scene.
    void refresh();

private:
    void buildUi();
    void apply(const EnvironmentConfig& config);
    void onDateTimeEdited();
    void onSliderMoved(int minute);
    void onTimeZoneChosen();
    void updateReadouts();
    void updateEvents();
    void updateEnabled();
    void updateWindInfo();
    [[nodiscard]] QString formatLocal(astro::UtcTime time, bool withDate = false) const;

    SceneController* scene_;
    QComboBox* timeZone_ = nullptr;
    QDateTimeEdit* dateTime_ = nullptr;
    QSlider* slider_ = nullptr;
    QCheckBox* animate_ = nullptr;
    QComboBox* rate_ = nullptr;
    QLabel* sun_ = nullptr;
    QLabel* moon_ = nullptr;
    QLabel* events_ = nullptr;
    QComboBox* exposureMode_ = nullptr;
    QDoubleSpinBox* manualEv_ = nullptr;
    QDoubleSpinBox* compensation_ = nullptr;
    QLabel* measured_ = nullptr;
    QDoubleSpinBox* visibility_ = nullptr;
    QSpinBox* windLevel_ = nullptr;
    QLabel* windInfo_ = nullptr;
    QSpinBox* windFrom_ = nullptr;
    QCheckBox* windRandom_ = nullptr;
    QDoubleSpinBox* temperature_ = nullptr;
    QDoubleSpinBox* qnh_ = nullptr;
    QDoubleSpinBox* humidity_ = nullptr;
    QTimer* timer_ = nullptr;
    QDate eventsDate_;
    std::string eventsZone_;
    double eventsLat_ = 1000.0;
};

}  // namespace ds::app
