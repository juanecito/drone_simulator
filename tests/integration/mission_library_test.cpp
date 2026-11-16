// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

// The list of available missions built at start-up (FR-MIS-01, FR-MIS-01b, FR-DB-09).

#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "ds/app/mission_library.hpp"
#include "ds/storage/mission_store.hpp"
#include "ds/storage/storage_service.hpp"

namespace {

namespace fs = std::filesystem;
using ds::app::loadMissionLibrary;
using ds::app::MissionFolders;

std::vector<ds::storage::MissionListEntry> index(ds::storage::StorageService& service) {
    auto list = service
                    .submit([](ds::storage::DbConnection& c) {
                        return ds::storage::MissionStore(c).list();
                    })
                    .get();
    EXPECT_TRUE(list.has_value()) << (list ? "" : list.error().message);
    return list ? *list : std::vector<ds::storage::MissionListEntry>{};
}

TEST(MissionLibrary, DefaultMissionIsInstalledAndListedAtStart) {
    const QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    const fs::path data = fs::path(home.path().toStdString()) / ".drone_simulation";
    auto service = ds::storage::StorageService::start({.databaseFile = std::nullopt});
    ASSERT_TRUE(service) << service.error().message;
    const MissionFolders folders{.assetDir = DS_TEST_ASSETS_DIR,
                                 .userDir = data / "missions",
                                 .libraryDir = data / "library"};

    auto library = loadMissionLibrary(folders, service->get());
    ASSERT_TRUE(library) << library.error().message;
    const fs::path installed = data / "missions" / "first-flight.mission.json";
    ASSERT_EQ(library->installed, std::vector<fs::path>{installed});
    const auto entry = std::ranges::find(library->entries, "first-flight",
                                         &ds::missions::CatalogEntry::key);
    ASSERT_NE(entry, library->entries.end());
    EXPECT_EQ(entry->origin, ds::missions::MissionOrigin::User);
    EXPECT_TRUE(entry->load.valid()) << entry->load.report();

    auto rows = index(**service);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].mission.missionKey, "first-flight");
    EXPECT_EQ(rows[0].mission.title, "First flight");
    EXPECT_EQ(rows[0].mission.origin, "user");
    EXPECT_EQ(rows[0].mission.status, "valid");
    EXPECT_EQ(rows[0].mission.filePath, installed.string());
    EXPECT_EQ(rows[0].mission.fileSha256.size(), 64U);

    // Next start: nothing to install; a copy under another name is invalid (duplicate id) and
    // does not replace the row; a broken file is listed as invalid with its errors.
    fs::copy_file(installed, data / "missions" / "other.mission.json");  // Sorts after it.
    std::ofstream(data / "missions" / "broken.mission.json") << "{ \"id\": \"broken\" }";
    library = loadMissionLibrary(folders, service->get());
    ASSERT_TRUE(library);
    EXPECT_TRUE(library->installed.empty());
    EXPECT_EQ(library->entries.size(), 3U);
    rows = index(**service);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].mission.missionKey, "broken");
    EXPECT_EQ(rows[0].mission.status, "invalid");
    EXPECT_NE(rows[0].mission.validationErrorsJson.find("missing required property"),
              std::string::npos);
    EXPECT_EQ(rows[1].mission.filePath, installed.string());

    // Without a user folder only the built-in missions are listed (none yet), and the user's
    // rows are kept as missing.
    library = loadMissionLibrary({.assetDir = DS_TEST_ASSETS_DIR, .userDir = {}, .libraryDir = {}},
                                 service->get());
    ASSERT_TRUE(library);
    EXPECT_TRUE(library->entries.empty());
    rows = index(**service);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[1].mission.status, "missing");
}

TEST(MissionLibrary, MissingSchemaIsAnError) {
    const QTemporaryDir assets;
    EXPECT_FALSE(loadMissionLibrary({.assetDir = assets.path().toStdString(), .userDir = {},
                                     .libraryDir = {}},
                                    nullptr));
}

}  // namespace
