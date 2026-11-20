// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/missions_controller.hpp"

#include <QCoreApplication>

#include <algorithm>
#include <format>
#include <map>
#include <tuple>
#include <utility>

#include "ds/app/mission_setup.hpp"
#include "ds/common/log.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/storage/storage_service.hpp"

namespace ds::app {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("ds::app::MissionsController", text);
}

std::map<std::string, storage::MissionProgress> readProgress(storage::StorageService* storage) {
    std::map<std::string, storage::MissionProgress> out;
    if (storage == nullptr) {
        return out;
    }
    auto list = storage->submit([](storage::DbConnection& c) {
                           return storage::MissionStore(c).list();
                       }).get();
    if (!list) {
        log::warn("Cannot read the mission progress: {}", list.error().message);
        return out;
    }
    for (const storage::MissionListEntry& e : *list) {
        out.emplace(e.mission.missionKey, e.progress);
    }
    return out;
}

}  // namespace

QString chapterTitle(std::string_view chapter) {
    if (chapter.empty()) {
        return tr("Free missions");
    }
    QString title = QString::fromUtf8(chapter.data(), static_cast<qsizetype>(chapter.size()));
    title.replace(QLatin1Char('-'), QLatin1Char(' ')).replace(QLatin1Char('_'), QLatin1Char(' '));
    title[0] = title[0].toUpper();
    return title;
}

MissionsController::MissionsController(MissionFolders folders, storage::StorageService* storage,
                                       QObject* parent)
    : QObject(parent), folders_(std::move(folders)), storage_(storage) {
    reload();
}

const MissionRow* MissionsController::find(std::string_view key) const noexcept {
    const auto it = std::ranges::find(rows_, key, &MissionRow::key);
    return it != rows_.end() ? &*it : nullptr;
}

const missions::CatalogEntry& MissionsController::entry(const MissionRow& row) const {
    return library_->entries.at(row.entry);
}

void MissionsController::reload() {
    rows_.clear();
    library_.reset();
    loadError_.clear();
    auto library = loadMissionLibrary(folders_, storage_);
    if (!library) {
        loadError_ = QString::fromStdString(library.error().message);
        log::error("Cannot load the missions: {}", library.error().message);
        emit missionsChanged();
        return;
    }
    library_ = std::move(*library);
    const auto progress = readProgress(storage_);

    std::map<std::string, int> bestStars;
    std::vector<missions::CampaignMission> campaign;
    for (std::size_t i = 0; i < library_->entries.size(); ++i) {
        const missions::CatalogEntry& e = library_->entries[i];
        MissionRow row;
        row.key = e.key;
        row.title = e.key;
        row.builtIn = e.origin == missions::MissionOrigin::BuiltIn;
        row.entry = i;
        if (const auto it = progress.find(e.key); it != progress.end()) {
            row.attempts = it->second.attempts;
            row.bestStars = it->second.bestStars;
            row.bestTimeS = it->second.bestTimeS;
        }
        if (const auto& d = e.load.definition; d && e.load.valid()) {
            row.title = d->title;
            row.chapter = d->chapter;
            row.order = d->order;
            row.ground = d->vehicle.type == physics::VehicleType::Ground;
            row.difficulty = d->difficulty;
            row.status = MissionStatus::Available;
            if (!e.load.unavailable.empty()) {
                row.status = MissionStatus::Unavailable;
                row.statusDetail = e.load.unavailable.front();
            } else if (auto reason = unplayableReason(*d)) {
                row.status = MissionStatus::Unavailable;
                row.statusDetail = std::move(*reason);
            }
            bestStars[row.key] = row.bestStars;
            campaign.push_back({d->id, d->chapter, d->order, d->unlock});
        } else {
            row.status = MissionStatus::Invalid;
            row.statusDetail = e.load.errors.empty() ? std::string() : e.load.errors.front().text();
        }
        rows_.push_back(std::move(row));
    }

    // Campaign order, then lock state with the reason (FR-MIS-12).
    const auto rank = [](const MissionRow& r) {
        return std::tuple(r.status == MissionStatus::Invalid, r.chapter.empty(), r.chapter, r.order,
                          r.key);
    };
    std::ranges::stable_sort(rows_, {}, rank);
    const auto unlocked = missions::unlockedMissions(campaign, bestStars);
    const MissionRow* previous = nullptr;
    for (MissionRow& row : rows_) {
        if (row.status == MissionStatus::Invalid) {
            continue;
        }
        const bool sameChapter = previous != nullptr && previous->chapter == row.chapter;
        const auto it = unlocked.find(row.key);
        if (it != unlocked.end() && !it->second && row.status == MissionStatus::Available) {
            row.status = MissionStatus::Locked;
            const auto& unlock = library_->entries[row.entry].load.definition->unlock;
            if (unlock) {
                std::string needs;
                for (const std::string& key : unlock->missions) {
                    const MissionRow* other = find(key);
                    needs += (needs.empty() ? "" : ", ") + (other != nullptr ? other->title : key);
                }
                row.statusDetail = std::format("{} {} on {}", unlock->minStars,
                                               unlock->minStars == 1 ? "star" : "stars", needs);
            } else if (sameChapter) {
                row.statusDetail = std::format("1 star on {}", previous->title);
            }
        }
        previous = &row;
    }
    emit missionsChanged();
}

const MissionRow* MissionsController::nextMission(std::string_view key) const noexcept {
    const auto it = std::ranges::find(rows_, key, &MissionRow::key);
    if (it == rows_.end()) {
        return nullptr;
    }
    const auto next = std::find_if(std::next(it), rows_.end(), [](const MissionRow& r) {
        return r.status == MissionStatus::Available;
    });
    return next != rows_.end() ? &*next : nullptr;
}

common::Result<std::int64_t> MissionsController::recordAttempt(
    const std::string& key, const missions::MissionResult& result, astro::UtcTime startedAt,
    const std::string& terrainDataHash) {
    if (storage_ == nullptr) {
        return common::makeError("results cannot be saved without a database");
    }
    storage::MissionAttemptRecord r;
    r.missionKey = key;
    r.startedAt = astro::formatUtc(startedAt);
    r.outcome = std::string(missions::missionStateName(result.outcome));
    r.failureReason = result.failureReason;
    r.score = result.score.total;
    r.stars = result.stars;
    r.durationS = result.durationS;
    r.resultsJson = result.toJson().dump();
    r.terrainDataHash = terrainDataHash;
    auto id = storage_->submit([r](storage::DbConnection& c) {
                         return storage::MissionStore(c).recordAttempt(r);
                     }).get();
    if (!id) {
        log::warn("Cannot save the attempt of {}: {}", key, id.error().message);
        return common::makeError(id.error().message);
    }
    reload();  // Progress, best stars and unlocks changed.
    return *id;
}

std::optional<storage::MissionAttemptRecord> MissionsController::bestAttempt(
    const std::string& key, std::optional<std::int64_t> excluding,
    const std::string& terrainDataHash) const {
    if (storage_ == nullptr) {
        return std::nullopt;
    }
    auto attempts = storage_->submit([key](storage::DbConnection& c) {
                               return storage::MissionStore(c).attempts(key, 1000);
                           }).get();
    if (!attempts) {
        log::warn("Cannot read the attempts of {}: {}", key, attempts.error().message);
        return std::nullopt;
    }
    // Same order as MissionStore::bestAttempt(): most stars, then highest score.
    std::optional<storage::MissionAttemptRecord> best;
    for (const storage::MissionAttemptRecord& a : *attempts) {
        if ((excluding && a.id == excluding) || a.terrainDataHash != terrainDataHash) {
            continue;
        }
        if (!best || a.stars > best->stars || (a.stars == best->stars && a.score > best->score)) {
            best = a;
        }
    }
    return best;
}

common::Result<std::filesystem::path> MissionsController::importMission(
    const std::filesystem::path& source, bool overwrite) {
    if (folders_.userDir.empty()) {
        return common::makeError("there is no user mission folder");
    }
    auto imported = missions::importMission(source, folders_.userDir, overwrite);
    if (!imported) {
        return std::unexpected(imported.error());
    }
    log::info("Imported mission {}", imported->string());
    reload();
    return imported;
}

}  // namespace ds::app
