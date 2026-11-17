// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/app/mission_library.hpp"
#include "ds/astro/time.hpp"
#include "ds/common/error.hpp"
#include "ds/missions/mission_runner.hpp"
#include "ds/storage/mission_store.hpp"

/// @file
/// @brief GUI-side access to the list of missions (FR-MIS-01, FR-MIS-01a, FR-MIS-12, FR-UI-06).
/// @ingroup app

namespace ds::storage {
class StorageService;
}  // namespace ds::storage

namespace ds::app {

/// @brief Whether a mission can be started.
enum class MissionStatus {
    Available,    ///< Valid, playable and unlocked.
    Locked,       ///< Valid, but the campaign rule is not met yet (FR-MIS-12).
    Unavailable,  ///< Valid, but data is missing, e.g. an elevation file (FR-MIS-02).
    Invalid,      ///< The file has errors (FR-MIS-01).
};

/// @brief One mission of the list, with the user's progress.
struct MissionRow {
    std::string key;                  ///< Mission id.
    std::string title;                ///< Title (the key if the file is invalid).
    std::string chapter;              ///< Campaign chapter ("" ⇒ free mission).
    int order = 0;                    ///< Position in the chapter.
    bool ground = false;              ///< Ground vehicle (else aerial).
    bool builtIn = false;             ///< Built-in pack (else the user's folder).
    int difficulty = 0;               ///< 1–5 (0 if the file is invalid).
    MissionStatus status = MissionStatus::Invalid;  ///< See MissionStatus.
    std::string statusDetail;         ///< Why it is locked / unavailable / invalid.
    int attempts = 0;                 ///< Attempts recorded.
    int bestStars = 0;                ///< Best stars (0 if none).
    std::optional<double> bestTimeS;  ///< Fastest success.
    std::size_t entry = 0;            ///< Index in MissionLibrary::entries.
};

/// @brief Owns the mission library and the list shown in the Missions tab.
/// @details The list is ordered by chapter ("" last), order and key, with invalid missions at
/// the end. Progress comes from the database when a storage service is given.
class MissionsController final : public QObject {
    Q_OBJECT

public:
    /// @brief Loads the missions of @p folders (see loadMissionLibrary()). @p storage may be
    /// null (no index, no progress); if given it must outlive the controller.
    MissionsController(MissionFolders folders, storage::StorageService* storage,
                       QObject* parent = nullptr);

    /// @brief Rescans the folders and re-reads the progress (FR-MIS-01a); emits missionsChanged().
    void reload();

    /// @brief The missions, in display order.
    [[nodiscard]] const std::vector<MissionRow>& missions() const noexcept { return rows_; }
    /// @brief The mission with id @p key, or nullptr.
    [[nodiscard]] const MissionRow* find(std::string_view key) const noexcept;
    /// @brief The catalogue entry (validation result, definition) of @p row.
    [[nodiscard]] const missions::CatalogEntry& entry(const MissionRow& row) const;
    /// @brief The folders scanned.
    [[nodiscard]] const MissionFolders& folders() const noexcept { return folders_; }
    /// @brief Why no mission could be loaded at all (e.g. the schema is missing), else empty.
    [[nodiscard]] const QString& loadError() const noexcept { return loadError_; }

    /// @brief The first available mission after @p key in display order, or nullptr
    /// (Debrief › Next mission).
    [[nodiscard]] const MissionRow* nextMission(std::string_view key) const noexcept;
    /// @brief Saves an attempt of mission @p key (FR-MIS-11, FR-MIS-12) and reloads the list
    /// (progress and unlocks change). Returns the attempt's id.
    common::Result<std::int64_t> recordAttempt(const std::string& key,
                                               const missions::MissionResult& result,
                                               astro::UtcTime startedAt);
    /// @brief The personal best of @p key (most stars, then highest score), ignoring the
    /// attempt @p excluding; nullopt if there is none or no database.
    [[nodiscard]] std::optional<storage::MissionAttemptRecord> bestAttempt(
        const std::string& key, std::optional<std::int64_t> excluding = std::nullopt) const;
    /// @brief Copies a mission file or mission folder into the user's mission folder
    /// (Missions › Import mission, FR-MIS-01) and reloads the list.
    common::Result<std::filesystem::path> importMission(const std::filesystem::path& source,
                                                        bool overwrite = false);

signals:
    /// @brief The list was rebuilt.
    void missionsChanged();

private:
    MissionFolders folders_;
    storage::StorageService* storage_;
    std::optional<MissionLibrary> library_;
    std::vector<MissionRow> rows_;
    QString loadError_;
};

/// @brief "Basics" for chapter "basics", "Free missions" for "".
[[nodiscard]] QString chapterTitle(std::string_view chapter);

}  // namespace ds::app
