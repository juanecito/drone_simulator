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
/// @details Milestone M1 provides the display units, coordinate display and the Data section
/// (data folder, database file, schema version). Graphics, controls, data providers and cache
/// settings arrive with the milestones that implement them.
class SettingsPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Builds the panel for @p settings, which must outlive it.
    explicit SettingsPanel(SettingsController& settings, QWidget* parent = nullptr);

    /// @brief Units selector (object name `UnitsCombo`).
    [[nodiscard]] QComboBox* unitsCombo() const noexcept { return units_; }
    /// @brief Coordinate display selector (object name `CoordinatesCombo`).
    [[nodiscard]] QComboBox* coordinatesCombo() const noexcept { return coordinates_; }
    /// @brief Free-mode rule enforcement (object name `EnforceRulesCheck`).
    [[nodiscard]] QCheckBox* enforceRulesCheck() const noexcept { return enforceRules_; }
    /// @brief Text shown for the database file (object name `DatabaseLabel`).
    [[nodiscard]] QLabel* databaseLabel() const noexcept { return database_; }

private:
    void openDataFolder();

    SettingsController* settings_;
    QComboBox* units_ = nullptr;
    QComboBox* coordinates_ = nullptr;
    QCheckBox* enforceRules_ = nullptr;
    QLabel* database_ = nullptr;
    QPushButton* openFolder_ = nullptr;
};

}  // namespace ds::app
