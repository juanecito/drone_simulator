// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <vector>

#include "ds/storage/mission_store.hpp"
#include "ds/storage/recording_store.hpp"
#include "ds/storage/session_store.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace ds::storage {
class StorageService;
}  // namespace ds::storage

/// @file
/// @brief History tab (M7 step 7): flight sessions and mission attempts with their figures, the
/// telemetry of the selected session plotted with QPainter, and its replay (FR-X-06). Every
/// read and write runs on the storage thread; the panel never waits for it (FR-DB-07).
/// @ingroup app

namespace ds::app {

class SimulationController;
class TelemetryPlot;

/// @brief Lists the flights and plays their replays.
class HistoryPanel final : public QWidget {
    Q_OBJECT
public:
    /// @brief Shows the history of @p storage (null: no database, the tab says so) for
    /// @p simulation; both must outlive the panel.
    HistoryPanel(storage::StorageService* storage, SimulationController& simulation,
                 QWidget* parent = nullptr);
    /// @brief Reloads the sessions and attempts.
    void refresh();
    /// @brief Selects the session @p sessionId (its telemetry is loaded); false if not listed.
    bool selectSession(std::int64_t sessionId);
    /// @brief The session selected, if any.
    [[nodiscard]] std::optional<std::int64_t> selectedSession() const;
    /// @brief Deletes session @p sessionId with its telemetry and replay (after the user
    /// confirmed).
    void removeSession(std::int64_t sessionId);
    /// @brief True while a storage request is pending (tests).
    [[nodiscard]] bool busy() const noexcept { return !pending_.empty(); }
    /// @brief Sessions listed.
    [[nodiscard]] const std::vector<storage::SessionRecord>& sessions() const noexcept {
        return sessions_;
    }

signals:
    /// @brief The user asked to play the replay @p replay (the main window finds its mission).
    void replayRequested(const ds::storage::ReplayRecord& replay);

private:
    void buildUi();
    void fillSessions();
    void fillAttempts();
    void loadTelemetry(std::int64_t sessionId);
    void requestReplay();
    void confirmRemove();
    void updateButtons();
    template <typename T>
    void whenReady(std::future<T> future, std::function<void(T)> done);
    void poll();

    storage::StorageService* storage_;
    SimulationController* simulation_;
    std::vector<storage::SessionRecord> sessions_;
    std::vector<storage::MissionAttemptRecord> attempts_;
    std::uint64_t telemetryRequest_ = 0;  // Latest telemetry request (older answers dropped).
    std::vector<std::function<bool()>> pending_;
    QTimer* poll_ = nullptr;

    QTableWidget* sessionsTable_ = nullptr;
    QTableWidget* attemptsTable_ = nullptr;
    QComboBox* quantity_ = nullptr;
    TelemetryPlot* plot_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* replay_ = nullptr;
    QPushButton* stopReplay_ = nullptr;
    QPushButton* delete_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* summary_ = nullptr;
};

}  // namespace ds::app
