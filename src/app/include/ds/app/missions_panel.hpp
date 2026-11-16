// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QWidget>

#include <string>

class QLabel;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

/// @file
/// @brief Missions tab (SPECIFICATION §4.11): the list of missions grouped by campaign chapter,
/// with vehicle type, stars, best time and state, and the details of the selected mission.
/// @ingroup app

namespace ds::missions {
struct CatalogEntry;
}  // namespace ds::missions

namespace ds::app {

class MissionsController;
struct MissionRow;

/// @brief Lists the missions of a MissionsController.
class MissionsPanel final : public QWidget {
    Q_OBJECT

public:
    /// @brief Shows the missions of @p missions, which must outlive the panel.
    explicit MissionsPanel(MissionsController& missions, QWidget* parent = nullptr);

    /// @brief Id of the selected mission ("" if none).
    [[nodiscard]] std::string selectedMission() const;
    /// @brief Selects the mission with id @p key; returns false if it is not listed.
    bool selectMission(const std::string& key);

private:
    void rebuild();
    void showDetails();

    MissionsController* missions_;
    QLabel* summary_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTextBrowser* details_ = nullptr;
    QPushButton* reload_ = nullptr;
    QPushButton* openFolder_ = nullptr;
    QPushButton* start_ = nullptr;
};

/// @brief Rich-text description of a mission for the details pane: briefing, objectives,
/// conditions (terrain, time, weather, vehicle), rules, star thresholds, progress, and the
/// errors of an invalid file.
[[nodiscard]] QString missionDetailsHtml(const MissionRow& row, const missions::CatalogEntry& entry);

/// @brief "★★☆" for 2 of 3 stars, "—" for none.
[[nodiscard]] QString starsText(int stars);

/// @brief "1:05.3" for 65.3 s.
[[nodiscard]] QString durationText(double seconds);

}  // namespace ds::app
