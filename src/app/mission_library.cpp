// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/mission_library.hpp"

#include <set>
#include <string>

#include "ds/common/json.hpp"
#include "ds/common/log.hpp"

namespace ds::app {

storage::MissionIndexRecord toIndexRecord(const missions::CatalogEntry& entry) {
    storage::MissionIndexRecord r;
    r.missionKey = entry.key;
    r.origin = entry.origin == missions::MissionOrigin::BuiltIn ? "builtin" : "user";
    r.filePath = entry.load.file.string();
    r.fileSha256 = entry.load.sha256;
    r.title = entry.key;
    if (const auto& d = entry.load.definition) {
        r.title = d->title;
        r.vehicleType = d->vehicle.type == physics::VehicleType::Ground ? "ground" : "aerial";
        r.chapter = d->chapter;
        r.sortOrder = d->order;
        r.schemaVersion = d->schemaVersion;
    }
    if (entry.load.valid()) {
        r.status = "valid";
    } else {
        r.status = "invalid";
        common::Json errors = common::Json::array();
        for (const missions::MissionIssue& e : entry.load.errors) {
            errors.push_back(e.text());
        }
        r.validationErrorsJson = errors.dump();
    }
    return r;
}

common::Result<MissionLibrary> loadMissionLibrary(const MissionFolders& folders,
                                                  storage::StorageService* storage) {
    auto validation = MissionValidation::create(folders.assetDir, folders.libraryDir);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    MissionLibrary library;
    library.validation = std::move(*validation);

    if (!folders.userDir.empty()) {
        const auto installed = missions::installDefaultMissions(
            folders.assetDir / std::string(kDefaultMissionsDirName), folders.userDir);
        if (!installed) {
            log::warn("Cannot install the default missions: {}", installed.error().message);
        } else {
            for (const auto& file : *installed) {
                log::info("Installed the default mission {}", file.string());
            }
            library.installed = *installed;
        }
    }

    library.entries = missions::scanMissions(folders.assetDir / "missions", folders.userDir,
                                             library.validation->context);
    int valid = 0;
    std::vector<storage::MissionIndexRecord> builtIn;
    std::vector<storage::MissionIndexRecord> user;
    std::set<std::string> keys;
    for (const missions::CatalogEntry& e : library.entries) {
        if (e.load.valid()) {
            ++valid;
        } else {
            log::warn("Mission {} is invalid:\n{}", e.load.file.string(), e.load.report());
        }
        if (!keys.insert(e.key).second) {
            continue;  // Duplicate id: the first file keeps the row.
        }
        (e.origin == missions::MissionOrigin::BuiltIn ? builtIn : user).push_back(toIndexRecord(e));
    }
    log::info("Missions: {} available, {} invalid", valid, library.entries.size() - static_cast<std::size_t>(valid));

    if (storage != nullptr) {
        auto synced = storage
                          ->submit([builtIn, user](storage::DbConnection& c) -> storage::StorageResult<void> {
                              storage::MissionStore store(c);
                              if (auto ok = store.sync("builtin", builtIn); !ok) {
                                  return ok;
                              }
                              return store.sync("user", user);
                          })
                          .get();
        if (!synced) {
            log::warn("Cannot update the mission index: {}", synced.error().message);
        }
    }
    return library;
}

}  // namespace ds::app
