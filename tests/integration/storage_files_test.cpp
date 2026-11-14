// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <vector>

#include "ds/storage/connection.hpp"
#include "ds/storage/data_location.hpp"
#include "ds/storage/migrations.hpp"
#include "ds/storage/settings_store.hpp"
#include "ds/storage/storage_service.hpp"

#include <unistd.h>

namespace {

using namespace ds::storage;
namespace fs = std::filesystem;

fs::perms modeOf(const fs::path& path) { return fs::status(path).permissions() & fs::perms::mask; }

constexpr fs::perms kMode0700 = fs::perms::owner_all;
constexpr fs::perms kMode0600 = fs::perms::owner_read | fs::perms::owner_write;

class TempHome : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(dir_.isValid()); }
    [[nodiscard]] fs::path home() const { return fs::path(dir_.path().toStdString()); }

private:
    QTemporaryDir dir_;
};

// --- First launch (§8.2, FR-DB-01, NFR-SEC-01) --------------------------------------------------

TEST_F(TempHome, FirstLaunchCreatesPrivateDirectoryAndDatabase) {
    const auto directory = prepareDataDirectory(home());
    ASSERT_TRUE(directory.has_value()) << directory.error().message;
    EXPECT_EQ(*directory, home() / ".drone_simulation");
    EXPECT_EQ(modeOf(*directory), kMode0700);

    const fs::path dbFile = *directory / kDatabaseFileName;
    {
        auto service = StorageService::start({dbFile});
        ASSERT_TRUE(service.has_value()) << service.error().message;
        EXPECT_FALSE((*service)->info().inMemory);
        EXPECT_EQ((*service)->info().migration.fromVersion, 0);
        EXPECT_EQ((*service)->info().migration.toVersion, latestSchemaVersion());
        auto saved = (*service)->submit(
            [](DbConnection& db) { return SettingsStore(db).setString("k", "v").has_value(); });
        EXPECT_TRUE(saved.get());
    }
    EXPECT_EQ(modeOf(dbFile), kMode0600);
    for (const char* suffix : {"-wal", "-shm"}) {  // SQLite copies the database file's mode.
        const fs::path side = fs::path(dbFile.string() + suffix);
        if (fs::exists(side)) {
            EXPECT_EQ(modeOf(side), kMode0600) << side;
        }
    }

    // Second launch: data persisted, nothing to migrate, no backup.
    auto again = StorageService::start({dbFile});
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ((*again)->info().migration.fromVersion, latestSchemaVersion());
    EXPECT_FALSE((*again)->info().migration.backup.has_value());
    auto value = (*again)->submit([](DbConnection& db) { return SettingsStore(db).string("k"); });
    EXPECT_EQ(value.get().value(), "v");
}

TEST_F(TempHome, ExistingDirectoryAndFileAreTightened) {
    const fs::path directory = home() / ".drone_simulation";
    fs::create_directory(directory);
    fs::permissions(directory, fs::perms::owner_all | fs::perms::group_read |
                                   fs::perms::group_exec | fs::perms::others_read |
                                   fs::perms::others_exec);
    const fs::path file = directory / "x.db";
    {
        std::ofstream(file) << "";
    }
    fs::permissions(file, kMode0600 | fs::perms::group_read | fs::perms::others_read);

    ASSERT_TRUE(prepareDataDirectory(home()).has_value());
    EXPECT_EQ(modeOf(directory), kMode0700);
    ASSERT_TRUE(ensurePrivateFile(file).has_value());
    EXPECT_EQ(modeOf(file), kMode0600);
}

TEST_F(TempHome, UnusableHomeIsReported) {
    EXPECT_FALSE(prepareDataDirectory({}).has_value());
    EXPECT_FALSE(prepareDataDirectory("relative/home").has_value());
    EXPECT_FALSE(prepareDataDirectory(home() / "does-not-exist").has_value());
    if (::geteuid() != 0) {  // root ignores permission bits.
        fs::permissions(home(), fs::perms::owner_read | fs::perms::owner_exec);
        const auto result = prepareDataDirectory(home());
        fs::permissions(home(), fs::perms::owner_all);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, StorageErrorCode::DataDirectory);
    }
}

// --- Migration with backup (§8.2, FR-DB-08) ----------------------------------------------------

TEST_F(TempHome, MigrationBacksUpAndPreservesData) {
    const fs::path dbFile = home() / "user.db";
    {
        auto db = DbConnection::open(dbFile);
        ASSERT_TRUE(db.has_value());
        ASSERT_TRUE(migrate(*db).has_value());
        ASSERT_TRUE(SettingsStore(*db).setString("pilot.callsign", "Halcón").has_value());
    }

    constexpr std::array<std::string_view, 2> next{
        "CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, text TEXT NOT NULL)",
        "INSERT INTO notes (text) VALUES ('migrated')"};
    std::vector<Migration> migrations(schemaMigrations().begin(), schemaMigrations().end());
    const int newVersion = latestSchemaVersion() + 1;
    migrations.push_back({newVersion, "test step", next});

    auto db = DbConnection::open(dbFile);
    ASSERT_TRUE(db.has_value());
    const auto report = migrate(*db, migrations);
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_EQ(report->fromVersion, latestSchemaVersion());
    EXPECT_EQ(report->toVersion, newVersion);
    ASSERT_TRUE(report->backup.has_value());
    EXPECT_EQ(report->backup->filename().string(),
              std::format("user.db.bak-v{}", latestSchemaVersion()));
    EXPECT_EQ(modeOf(*report->backup), kMode0600);

    EXPECT_EQ(SettingsStore(*db).string("pilot.callsign").value(), "Halcón");  // Preserved.
    EXPECT_EQ(*schemaVersion(*db), newVersion);

    // The backup is a complete database at the old version.
    auto old = DbConnection::open(*report->backup);
    ASSERT_TRUE(old.has_value());
    EXPECT_EQ(*schemaVersion(*old), latestSchemaVersion());
    EXPECT_EQ(SettingsStore(*old).string("pilot.callsign").value(), "Halcón");
}

TEST_F(TempHome, NewerDatabaseIsLeftUntouched) {
    const fs::path dbFile = home() / "future.db";
    {
        auto db = DbConnection::open(dbFile);
        ASSERT_TRUE(db.has_value());
        ASSERT_TRUE(migrate(*db).has_value());
        ASSERT_TRUE(db->execute("UPDATE schema_version SET version = 1000").has_value());
    }
    auto service = StorageService::start({dbFile});
    ASSERT_FALSE(service.has_value());
    EXPECT_EQ(service.error().code, StorageErrorCode::NewerSchema);
    EXPECT_FALSE(fs::exists(dbFile.string() + ".bak-v1000"));
}

}  // namespace
