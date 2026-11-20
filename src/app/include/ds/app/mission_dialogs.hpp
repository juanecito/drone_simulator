// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>
#include <QImage>
#include <QString>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/items/item.hpp"
#include "ds/missions/catalog.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/terrain/height_source.hpp"

/// @file
/// @brief Mission flow dialogs (FR-UI-07, FR-MIS-11): the briefing (map, objectives, rules,
/// weather, day/night situation, vehicle) and the debrief (results, stars, score breakdown,
/// flight path over a top-down terrain view, comparison with the personal best).
/// @ingroup app

class QComboBox;
class QLabel;
class QPushButton;
class QTextBrowser;

namespace ds::app {

class DataServices;

struct MissionRow;
struct VehicleProfileInfo;

/// @brief What the top-down mission map shows.
struct MissionMapData {
    std::shared_ptr<const terrain::HeightSource> terrain;  ///< Heights (world frame).
    std::optional<terrain::Extent> extent;                 ///< Bounded terrain, if any.
    std::vector<items::Item> items;                        ///< Placed items.
    glm::dvec3 spawn{0.0};                                 ///< Take-off point.
    double spawnHeadingDeg = 0.0;                          ///< Heading at the start.
    glm::dvec3 pilot{0.0};                                 ///< Pilot position.
    std::vector<glm::dvec3> path;                          ///< Flight path (debrief).
};

/// @brief Builds the map data of @p mission (opens file terrain, and online terrain from the
/// cache of @p data; see unplayableReason()).
[[nodiscard]] common::Result<MissionMapData> missionMapData(
    const missions::MissionDefinition& mission, DataServices* data = nullptr);

/// @brief Top-down map: shaded terrain, items, take-off point, pilot and flight path; north up.
class MissionMapWidget final : public QWidget {
    Q_OBJECT

public:
    /// @brief Empty map.
    explicit MissionMapWidget(QWidget* parent = nullptr);
    /// @brief Shows @p data (the shaded terrain is computed here).
    void setData(MissionMapData mapData);
    /// @brief World rectangle shown (x: west → east, y: north → south), metres.
    [[nodiscard]] const QRectF& worldRect() const noexcept { return world_; }
    /// @brief Preferred size.
    [[nodiscard]] QSize sizeHint() const override { return {360, 360}; }

protected:
    /// @brief Draws the map.
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QPointF toScreen(const QRectF& target, double x, double z) const;

    MissionMapData data_;
    QRectF world_;
    QImage shaded_;
};

/// @brief Day/night situation at the mission's start (FR-MIS-06a), as HTML: local time, sun
/// elevation, sunrise and sunset, the Moon's phase and its next rise or set, and a warning for
/// night missions.
[[nodiscard]] QString daylightHtml(const missions::MissionDefinition& mission);

/// @brief Results of an attempt as HTML (FR-MIS-11): outcome, per-objective results, score
/// breakdown, penalties and the comparison with @p best (the personal best before it).
[[nodiscard]] QString debriefHtml(const missions::MissionDefinition& mission,
                                  const missions::MissionResult& result,
                                  const std::optional<storage::MissionAttemptRecord>& best);

/// @brief Briefing (FR-UI-07): the mission, its map and the vehicle to fly; *Fly* starts it.
class MissionBriefingDialog final : public QDialog {
    Q_OBJECT

public:
    /// @param row Mission row.
    /// @param entry Its catalogue entry (a valid mission).
    /// @param profiles Profiles the pilot may choose when the mission allows it.
    /// @param keyboardOnly True if no controller is set up: a mission without
    ///        `allowKeyboard` cannot be flown then.
    /// @param services Data services for the map of online terrain (may be null).
    /// @param parent Parent widget.
    MissionBriefingDialog(const MissionRow& row, const missions::CatalogEntry& entry,
                          const QList<VehicleProfileInfo>& profiles, bool keyboardOnly,
                          DataServices* services = nullptr, QWidget* parent = nullptr);

    /// @brief Profile chosen ("" ⇒ the mission's own).
    [[nodiscard]] QString chosenProfile() const;
    /// @brief The *Fly* button (object name `FlyButton`).
    [[nodiscard]] QPushButton* flyButton() const noexcept { return fly_; }
    /// @brief The text pane (object name `BriefingText`).
    [[nodiscard]] QTextBrowser* text() const noexcept { return text_; }

private:
    QComboBox* profile_ = nullptr;
    QPushButton* fly_ = nullptr;
    QTextBrowser* text_ = nullptr;
};

/// @brief Debrief (FR-UI-07, FR-MIS-11): results, stars, path; *Retry*, *Next mission* or
/// *Back to free flight*.
class MissionDebriefDialog final : public QDialog {
    Q_OBJECT

public:
    /// @brief What the pilot chose.
    enum class Choice { Back, Retry, Next };

    /// @param mission The mission.
    /// @param result The attempt's result.
    /// @param path Its flight path.
    /// @param best The personal best before this attempt.
    /// @param hasNext Enables *Next mission*.
    /// @param services Data services for the map of online terrain (may be null).
    /// @param parent Parent widget.
    MissionDebriefDialog(const missions::MissionDefinition& mission,
                         const missions::MissionResult& result, std::vector<glm::dvec3> path,
                         const std::optional<storage::MissionAttemptRecord>& best, bool hasNext,
                         DataServices* services = nullptr, QWidget* parent = nullptr);

    /// @brief The button pressed (Back if the dialog was closed).
    [[nodiscard]] Choice choice() const noexcept { return choice_; }
    /// @brief The text pane (object name `DebriefText`).
    [[nodiscard]] QTextBrowser* text() const noexcept { return text_; }
    /// @brief Button for @p choice (object names `RetryButton`, `NextButton`, `BackButton`).
    [[nodiscard]] QPushButton* button(Choice choice) const noexcept;

private:
    Choice choice_ = Choice::Back;
    QTextBrowser* text_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* back_ = nullptr;
};

}  // namespace ds::app
