// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

/// @file
/// @brief Settings tab: display preferences and user data location (SPECIFICATION §4.11).
/// @ingroup app

namespace ds::app {

class SettingsController;

/// @brief Contents of the Settings tab.
/// @details Display units, coordinate display, chase camera insets, flight rules and the Data
/// section (data folder, database file, schema version). The Settings tab adds the data providers
/// and cache (DataProvidersPanel) and the controls (ControlsPanel) below it.
class SettingsPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Builds the panel for @p settings, which must outlive it.
    explicit SettingsPanel(SettingsController& settings, QWidget* parent = nullptr);

    /// @brief Units selector (object name `UnitsCombo`).
    [[nodiscard]] QComboBox* unitsCombo() const noexcept { return units_; }
    /// @brief Coordinate display selector (object name `CoordinatesCombo`).
    [[nodiscard]] QComboBox* coordinatesCombo() const noexcept { return coordinates_; }
    /// @brief Chase camera top-view inset shown (object name `ChaseTopViewCheck`).
    [[nodiscard]] QCheckBox* chaseTopViewCheck() const noexcept { return chaseTopView_; }
    /// @brief Chase camera behind-view inset shown (object name `ChaseBehindViewCheck`).
    [[nodiscard]] QCheckBox* chaseBehindViewCheck() const noexcept { return chaseBehindView_; }
    /// @brief Free-mode rule enforcement (object name `EnforceRulesCheck`).
    [[nodiscard]] QCheckBox* enforceRulesCheck() const noexcept { return enforceRules_; }
    /// @brief Text shown for the database file (object name `DatabaseLabel`).
    [[nodiscard]] QLabel* databaseLabel() const noexcept { return database_; }

private:
    void openDataFolder();

    SettingsController* settings_;
    QComboBox* units_ = nullptr;
    QComboBox* coordinates_ = nullptr;
    QCheckBox* chaseTopView_ = nullptr;
    QCheckBox* chaseBehindView_ = nullptr;
    QCheckBox* enforceRules_ = nullptr;
    QLabel* database_ = nullptr;
    QPushButton* openFolder_ = nullptr;
};

}  // namespace ds::app
