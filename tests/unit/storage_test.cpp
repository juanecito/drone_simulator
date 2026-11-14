// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <QSqlQuery>
#include <QVariant>

#include <gtest/gtest.h>

#include <array>
#include <future>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "ds/storage/connection.hpp"
#include "ds/storage/input_profile_store.hpp"
#include "ds/storage/migrations.hpp"
#include "ds/storage/session_store.hpp"
#include "ds/storage/settings_store.hpp"
#include "ds/storage/storage_service.hpp"
#include "ds/storage/terrain_preset_store.hpp"
#include "ds/storage/vehicle_profile_store.hpp"

namespace {

using namespace ds::storage;

DbConnection memoryDb() {
    auto db = DbConnection::openInMemory();
    if (!db) {
        throw std::runtime_error(db.error().message);
    }
    return std::move(*db);
}

DbConnection migratedDb() {
    DbConnection db = memoryDb();
    if (auto ok = migrate(db); !ok) {
        throw std::runtime_error(ok.error().message);
    }
    return db;
}

QVariant scalar(DbConnection& db, std::string_view sql) {
    auto query = db.prepare(sql);
    EXPECT_TRUE(query.has_value()) << (query ? "" : query.error().message);
    if (!query || !execute(*query) || !query->next()) {
        return {};
    }
    return query->value(0);
}

bool run(DbConnection& db, std::string_view sql) { return db.execute(sql).has_value(); }

// --- Connection (FR-DB-04, FR-DB-06) ------------------------------------------------------------

TEST(StorageConnection, EnablesForeignKeys) {
    DbConnection db = memoryDb();
    EXPECT_TRUE(db.inMemory());
    EXPECT_EQ(scalar(db, "PRAGMA foreign_keys").toInt(), 1);
}

TEST(StorageConnection, ConnectionsAreIndependentAndRemovedOnDestruction) {
    std::string name;
    {
        DbConnection a = memoryDb();
        DbConnection b = memoryDb();
        EXPECT_NE(a.connectionName(), b.connectionName());
        ASSERT_TRUE(run(a, "CREATE TABLE t (x)"));
        EXPECT_FALSE(run(b, "SELECT * FROM t"));  // Separate in-memory databases.
        name = a.connectionName();
    }
    EXPECT_FALSE(QSqlDatabase::contains(QString::fromStdString(name)));
}

TEST(StorageConnection, SqlErrorsAreMapped) {
    DbConnection db = memoryDb();
    const auto result = db.execute("SELEKT 1");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StorageErrorCode::Query);
    EXPECT_NE(result.error().message.find("SELEKT"), std::string::npos);
}

TEST(StorageTransaction, RollsBackWhenNotCommitted) {
    DbConnection db = memoryDb();
    ASSERT_TRUE(run(db, "CREATE TABLE t (x INTEGER)"));
    {
        auto tx = Transaction::begin(db);
        ASSERT_TRUE(tx.has_value());
        ASSERT_TRUE(run(db, "INSERT INTO t VALUES (1)"));
    }
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM t").toInt(), 0);
    {
        auto tx = Transaction::begin(db);
        ASSERT_TRUE(tx.has_value());
        ASSERT_TRUE(run(db, "INSERT INTO t VALUES (2)"));
        ASSERT_TRUE(tx->commit().has_value());
    }
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM t").toInt(), 1);
}

// --- Migrations (FR-DB-03) ---------------------------------------------------------------------

TEST(StorageMigrations, EmptyDatabaseGoesToLatest) {
    DbConnection db = memoryDb();
    const auto report = migrate(db);
    ASSERT_TRUE(report.has_value()) << report.error().message;
    EXPECT_EQ(report->fromVersion, 0);
    EXPECT_EQ(report->toVersion, latestSchemaVersion());
    EXPECT_FALSE(report->backup.has_value());  // Nothing to back up in memory.
    EXPECT_EQ(*schemaVersion(db), latestSchemaVersion());
    for (const char* table : {"settings", "vehicle_profiles", "data_providers", "provider_calls",
                              "provider_credentials", "terrain_presets", "downloaded_areas",
                              "scenes", "scene_items", "missions", "input_profiles", "sessions",
                              "mission_attempts", "telemetry_samples", "schema_version"}) {
        auto query = db.prepare("SELECT COUNT(*) FROM sqlite_master WHERE name = :name");
        ASSERT_TRUE(query.has_value());
        query->bindValue(QStringLiteral(":name"), QString::fromLatin1(table));
        ASSERT_TRUE(execute(*query).has_value());
        ASSERT_TRUE(query->next());
        EXPECT_EQ(query->value(0).toInt(), 1) << table;
    }
}

TEST(StorageMigrations, SecondRunIsANoOp) {
    DbConnection db = migratedDb();
    const auto again = migrate(db);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->fromVersion, latestSchemaVersion());
    EXPECT_EQ(again->toVersion, latestSchemaVersion());
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM schema_version").toInt(), 1);
}

TEST(StorageMigrations, FailedMigrationLeavesDatabaseUnchanged) {
    DbConnection db = migratedDb();
    constexpr std::array<std::string_view, 2> v2{"CREATE TABLE extra (x INTEGER)",
                                                 "THIS IS NOT SQL"};
    std::vector<Migration> migrations(schemaMigrations().begin(), schemaMigrations().end());
    migrations.push_back({latestSchemaVersion() + 1, "broken", v2});
    const auto result = migrate(db, migrations);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StorageErrorCode::Migration);
    EXPECT_EQ(*schemaVersion(db), latestSchemaVersion());
    EXPECT_EQ(scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'extra'").toInt(), 0);
}

TEST(StorageMigrations, NewerDatabaseIsRefused) {
    DbConnection db = migratedDb();
    ASSERT_TRUE(run(db, "UPDATE schema_version SET version = 99"));
    const auto result = migrate(db);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StorageErrorCode::NewerSchema);
    EXPECT_EQ(*schemaVersion(db), 99);
}

TEST(StorageMigrations, VersionsMustBeContiguous) {
    DbConnection db = memoryDb();
    constexpr std::array<std::string_view, 1> sql{"CREATE TABLE a (x)"};
    const std::array<Migration, 1> gap{{{2, "starts at 2", sql}}};
    const auto result = migrate(db, gap);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StorageErrorCode::Migration);
}

// --- Schema constraints -----------------------------------------------------------------------

TEST(StorageSchema, TerrainPresetChecks) {
    DbConnection db = migratedDb();
    // Procedural needs a seed.
    EXPECT_FALSE(run(db,
                     "INSERT INTO terrain_presets (preset_key, name, source_type, params_json) "
                     "VALUES ('p1', 'P1', 'procedural', '{}')"));
    EXPECT_TRUE(run(db,
                    "INSERT INTO terrain_presets (preset_key, name, source_type, seed, "
                    "params_json) VALUES ('p2', 'P2', 'procedural', 42, '{}')"));
    // Online needs a provider.
    EXPECT_FALSE(run(db,
                     "INSERT INTO terrain_presets (preset_key, name, source_type, params_json) "
                     "VALUES ('o1', 'O1', 'online', '{}')"));
    // File needs files.
    EXPECT_FALSE(run(db,
                     "INSERT INTO terrain_presets (preset_key, name, source_type, params_json) "
                     "VALUES ('f1', 'F1', 'file', '{}')"));
    // Extent: both or neither.
    EXPECT_FALSE(run(db,
                     "INSERT INTO terrain_presets (preset_key, name, source_type, seed, "
                     "extent_width_m, params_json) VALUES ('p3', 'P3', 'procedural', 1, "
                     "1000, '{}')"));
    EXPECT_TRUE(run(db,
                    "INSERT INTO terrain_presets (preset_key, name, source_type, seed, "
                    "extent_width_m, extent_length_m, params_json) VALUES ('p4', 'P4', "
                    "'procedural', 1, 1000, 2000, '{}')"));
    // Unknown source type.
    EXPECT_FALSE(run(db,
                     "INSERT INTO terrain_presets (preset_key, name, source_type, seed, "
                     "params_json) VALUES ('x', 'X', 'magic', 1, '{}')"));
}

TEST(StorageSchema, ForeignKeysAreEnforced) {
    DbConnection db = migratedDb();
    ASSERT_TRUE(run(db,
                    "INSERT INTO data_providers (id, provider_key, name, kind, protocol, "
                    "url_template, params_json, attribution, origin) VALUES (1, 'otp', "
                    "'OpenTopography', 'elevation', 'rest_bbox', 'https://example.org', "
                    "'{}', 'OpenTopography', 'builtin')"));
    ASSERT_TRUE(run(db,
                    "INSERT INTO terrain_presets (preset_key, name, source_type, "
                    "elevation_provider_id, params_json) VALUES ('o', 'O', 'online', 1, "
                    "'{}')"));
    EXPECT_FALSE(run(db, "DELETE FROM data_providers WHERE id = 1"));  // ON DELETE RESTRICT
    EXPECT_FALSE(run(db,
                     "INSERT INTO provider_calls (provider_id, called_at) VALUES "
                     "(7, '2026-01-01')"));  // No provider 7.
}

TEST(StorageSchema, MissionProgressView) {
    DbConnection db = migratedDb();
    ASSERT_TRUE(run(db,
                    "INSERT INTO missions (id, mission_key, title, vehicle_type, origin, "
                    "file_path, file_sha256, schema_version, status) VALUES "
                    "(1, 'm1', 'M1', 'aerial', 'builtin', 'a.mission.json', 'x', 1, 'valid'), "
                    "(2, 'm2', 'M2', 'ground', 'builtin', 'b.mission.json', 'y', 1, 'valid')"));
    ASSERT_TRUE(run(db,
                    "INSERT INTO mission_attempts (mission_id, started_at, outcome, score, "
                    "stars, duration_s, results_json) VALUES "
                    "(1, 't1', 'failed', 10, 0, 50, '{}'), "
                    "(1, 't2', 'succeeded', 80, 2, 120, '{}'), "
                    "(1, 't3', 'succeeded', 95, 3, 100, '{}')"));
    auto query = db.prepare(
        "SELECT attempts, successes, best_stars, best_score, best_time_s "
        "FROM mission_progress WHERE mission_id = :id");
    ASSERT_TRUE(query.has_value());
    query->bindValue(QStringLiteral(":id"), 1);
    ASSERT_TRUE(execute(*query).has_value());
    ASSERT_TRUE(query->next());
    EXPECT_EQ(query->value(0).toInt(), 3);
    EXPECT_EQ(query->value(1).toInt(), 2);
    EXPECT_EQ(query->value(2).toInt(), 3);
    EXPECT_DOUBLE_EQ(query->value(3).toDouble(), 95.0);
    EXPECT_DOUBLE_EQ(query->value(4).toDouble(), 100.0);

    query->bindValue(QStringLiteral(":id"), 2);  // No attempts yet.
    ASSERT_TRUE(execute(*query).has_value());
    ASSERT_TRUE(query->next());
    EXPECT_EQ(query->value(0).toInt(), 0);
    EXPECT_EQ(query->value(1).toInt(), 0);
    EXPECT_TRUE(query->value(2).isNull());
}

TEST(StorageSchema, StarsAreLimitedToThree) {
    DbConnection db = migratedDb();
    ASSERT_TRUE(run(db,
                    "INSERT INTO missions (id, mission_key, title, vehicle_type, origin, "
                    "file_path, file_sha256, schema_version, status) VALUES (1, 'm', 'M', "
                    "'aerial', 'user', 'm.mission.json', 'x', 1, 'valid')"));
    EXPECT_FALSE(run(db,
                     "INSERT INTO mission_attempts (mission_id, started_at, outcome, stars, "
                     "results_json) VALUES (1, 't', 'succeeded', 4, '{}')"));
}

// --- Settings ----------------------------------------------------------------------------------

TEST(StorageSettings, TypedRoundTrips) {
    DbConnection db = migratedDb();
    SettingsStore settings(db);
    ASSERT_TRUE(settings.setString("display.units", "metric"));
    ASSERT_TRUE(settings.setInteger("history.limit", -9007199254740991));
    ASSERT_TRUE(settings.setNumber("graphics.fovDeg", 92.5));
    ASSERT_TRUE(settings.setBoolean("graphics.vsync", true));
    ASSERT_TRUE(settings.setBytes("ui.state", {0, 1, 2, 250, 255}));
    ASSERT_TRUE(settings.setJson("controls.map", R"({"roll":1,"pitch":[2,3]})"));

    EXPECT_EQ(settings.string("display.units").value(), "metric");
    EXPECT_EQ(settings.integer("history.limit").value(), -9007199254740991);
    EXPECT_DOUBLE_EQ(*settings.number("graphics.fovDeg").value(), 92.5);
    EXPECT_EQ(settings.boolean("graphics.vsync").value(), true);
    EXPECT_EQ(settings.bytes("ui.state").value(), (std::vector<std::uint8_t>{0, 1, 2, 250, 255}));
    EXPECT_EQ(settings.json("controls.map").value(), R"({"pitch":[2,3],"roll":1})");
}

TEST(StorageSettings, StringsAreJsonEscaped) {
    DbConnection db = migratedDb();
    SettingsStore settings(db);
    const std::string tricky = "quote \" backslash \\ newline \n ñ ✈";
    ASSERT_TRUE(settings.setString("k", tricky));
    EXPECT_EQ(settings.string("k").value(), tricky);
}

TEST(StorageSettings, MissingKeyIsNullopt) {
    DbConnection db = migratedDb();
    SettingsStore settings(db);
    const auto value = settings.string("nothing.here");
    ASSERT_TRUE(value.has_value());
    EXPECT_FALSE(value->has_value());
}

TEST(StorageSettings, WrongTypeIsAnError) {
    DbConnection db = migratedDb();
    SettingsStore settings(db);
    ASSERT_TRUE(settings.setString("k", "text"));
    const auto asInt = settings.integer("k");
    ASSERT_FALSE(asInt.has_value());
    EXPECT_EQ(asInt.error().code, StorageErrorCode::InvalidValue);
    ASSERT_TRUE(settings.setNumber("n", 1.5));
    EXPECT_FALSE(settings.integer("n").has_value());  // Not a whole number.
}

TEST(StorageSettings, RejectsInvalidInput) {
    DbConnection db = migratedDb();
    SettingsStore settings(db);
    EXPECT_FALSE(settings.setJson("k", "{not json").has_value());
    EXPECT_FALSE(settings.setJson("k", "1 2").has_value());
    EXPECT_FALSE(settings.setJson("", "1").has_value());
    EXPECT_FALSE(settings.setNumber("k", std::numeric_limits<double>::infinity()).has_value());
}

TEST(StorageSettings, OverwriteRemoveAndKeys) {
    DbConnection db = migratedDb();
    SettingsStore settings(db);
    ASSERT_TRUE(settings.setInteger("b", 1));
    ASSERT_TRUE(settings.setInteger("b", 2));
    ASSERT_TRUE(settings.setInteger("a", 3));
    EXPECT_EQ(settings.integer("b").value(), 2);
    EXPECT_EQ(settings.keys().value(), (std::vector<std::string>{"a", "b"}));
    ASSERT_TRUE(settings.remove("b"));
    ASSERT_TRUE(settings.remove("does-not-exist"));
    EXPECT_EQ(settings.keys().value(), (std::vector<std::string>{"a"}));
}

// --- Storage thread (FR-DB-07) -----------------------------------------------------------------

TEST(StorageService, RunsTasksInOrderOnItsOwnThread) {
    auto service = StorageService::start({});
    ASSERT_TRUE(service.has_value()) << service.error().message;
    EXPECT_TRUE((*service)->info().inMemory);
    EXPECT_EQ((*service)->info().migration.toVersion, latestSchemaVersion());

    const auto callerThread = std::this_thread::get_id();
    std::vector<std::future<bool>> writes;
    for (int i = 0; i < 50; ++i) {
        writes.push_back((*service)->submit([i, callerThread](DbConnection& db) {
            SettingsStore settings(db);
            return std::this_thread::get_id() != callerThread &&
                   settings.setInteger("counter", i).has_value();
        }));
    }
    for (auto& write : writes) {
        EXPECT_TRUE(write.get());
    }
    auto last = (*service)->submit(
        [](DbConnection& db) { return SettingsStore(db).integer("counter").value(); });
    EXPECT_EQ(last.get(), 49);
}

TEST(StorageService, TaskExceptionsReachTheCaller) {
    auto service = StorageService::start({});
    ASSERT_TRUE(service.has_value());
    auto failing = (*service)->submit([](DbConnection&) -> int { throw std::runtime_error("x"); });
    EXPECT_THROW(failing.get(), std::runtime_error);
    auto ok = (*service)->submit([](DbConnection&) { return 7; });  // Still running.
    EXPECT_EQ(ok.get(), 7);
}

TEST(StorageService, ReportsOpenErrors) {
    auto service = StorageService::start({std::filesystem::path("/nonexistent-dir/x/y.db")});
    ASSERT_FALSE(service.has_value());
    EXPECT_EQ(service.error().code, StorageErrorCode::CannotOpen);
}

// --- Terrain presets (FR-TER-10) ----------------------------------------------------------------

TEST(TerrainPresetStore, SaveFindListUpdateRemove) {
    DbConnection db = migratedDb();
    TerrainPresetStore store(db);
    TerrainPresetRecord alps{.key = "user.alps",
                             .name = "Alps",
                             .seed = 0xFEDCBA9876543210ULL,  // Above INT64_MAX: bit-cast.
                             .originLatDeg = 46.5,
                             .originLonDeg = 8.0,
                             .originAltM = 1500.0,
                             .extentWidthM = 20000.0,
                             .extentLengthM = 10000.0,
                             .paramsJson = R"({"octaves":8,"amplitudeM":900})"};
    const auto id = store.save(alps);
    ASSERT_TRUE(id.has_value()) << id.error().message;
    alps.id = *id;
    const auto found = store.find("user.alps");
    ASSERT_TRUE(found.has_value());
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(**found, alps);

    TerrainPresetRecord dunes{.key = "user.dunes", .name = "dunes", .seed = 7};
    ASSERT_TRUE(store.save(dunes).has_value());
    const auto all = store.list();
    ASSERT_TRUE(all.has_value());
    ASSERT_EQ(all->size(), 2U);
    EXPECT_EQ((*all)[0].name, "Alps");  // Case-insensitive name order.
    EXPECT_FALSE((*all)[1].extentWidthM.has_value());

    alps.name = "Alps (big)";
    alps.extentWidthM = 50000.0;
    const auto again = store.save(alps);  // Same key ⇒ update, same id.
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *id);
    EXPECT_EQ(store.find("user.alps")->value().name, "Alps (big)");

    EXPECT_TRUE(store.remove("user.alps").value());
    EXPECT_FALSE(store.remove("user.alps").value());
    EXPECT_FALSE(store.find("user.alps")->has_value());
}

TEST(TerrainPresetStore, ConstraintsAndValidation) {
    DbConnection db = migratedDb();
    TerrainPresetStore store(db);
    // Procedural without seed: CHECK constraint.
    const auto noSeed = store.save({.key = "a", .name = "A"});
    ASSERT_FALSE(noSeed.has_value());
    EXPECT_EQ(noSeed.error().code, StorageErrorCode::Query);
    // Only one extent side: CHECK constraint.
    EXPECT_FALSE(
        store.save({.key = "b", .name = "B", .seed = 1, .extentWidthM = 1000.0}).has_value());
    // Unknown source type: CHECK constraint.
    EXPECT_FALSE(
        store.save({.key = "c", .name = "C", .sourceType = "magic", .seed = 1}).has_value());
    // Application-level checks.
    EXPECT_EQ(store.save({.key = "", .name = "D", .seed = 1}).error().code,
              StorageErrorCode::InvalidValue);
    EXPECT_EQ(store.save({.key = "e", .name = "E", .seed = 1, .paramsJson = "[1,2]"}).error().code,
              StorageErrorCode::InvalidValue);
    EXPECT_EQ(store.save({.key = "f", .name = "F", .seed = 1, .paramsJson = "{oops"}).error().code,
              StorageErrorCode::InvalidValue);
    EXPECT_TRUE(store.list()->empty());
}

}  // namespace

// --- Vehicle profiles, input profiles, sessions (M3) -----------------------------------------

namespace {

VehicleProfileRecord profile(std::string key, std::string name, std::string params = "{}") {
    VehicleProfileRecord r;
    r.key = std::move(key);
    r.name = std::move(name);
    r.paramsJson = std::move(params);
    return r;
}

}  // namespace

TEST(StorageVehicleProfiles, BuiltInSyncNeverOverwritesUserProfiles) {
    DbConnection db = migratedDb();
    VehicleProfileStore store(db);
    // A user profile already uses a key that a later release ships.
    ASSERT_TRUE(store.saveUser(profile("camera-250g", "My camera drone", R"({"mine":1})")));
    const std::vector<VehicleProfileRecord> shipped{
        profile("fpv-5in-racer", "5-inch FPV racer", R"({"v":1})"),
        profile("camera-250g", "Sub-250 g camera drone", R"({"v":1})")};
    const auto first = store.syncBuiltIn(shipped);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(*first, (ProfileSyncReport{.inserted = 1, .updated = 0, .skipped = 1}));
    // Second start: nothing changes; a new release changes one profile.
    EXPECT_EQ(*store.syncBuiltIn(shipped), (ProfileSyncReport{.skipped = 1}));
    std::vector<VehicleProfileRecord> next = shipped;
    next[0].paramsJson = R"({"v":2})";
    EXPECT_EQ(*store.syncBuiltIn(next), (ProfileSyncReport{.updated = 1, .skipped = 1}));

    const auto mine = store.find("camera-250g");
    ASSERT_TRUE(mine && *mine);
    EXPECT_EQ((*mine)->origin, "user");
    EXPECT_EQ((*mine)->paramsJson, R"({"mine":1})");
    const auto fpv = store.find("fpv-5in-racer");
    ASSERT_TRUE(fpv && *fpv);
    EXPECT_EQ((*fpv)->origin, "builtin");
    EXPECT_EQ((*fpv)->paramsJson, R"({"v":2})");
    // Built-in first, then by name.
    const auto all = store.list();
    ASSERT_TRUE(all);
    ASSERT_EQ(all->size(), 2U);
    EXPECT_EQ(all->front().key, "fpv-5in-racer");
    EXPECT_TRUE(store.list("ground")->empty());
    EXPECT_EQ(store.list("aerial")->size(), 2U);
}

TEST(StorageVehicleProfiles, BuiltInProfilesAreReadOnly) {
    DbConnection db = migratedDb();
    VehicleProfileStore store(db);
    const std::vector<VehicleProfileRecord> shipped{profile("survey-1500g", "Survey quad")};
    ASSERT_TRUE(store.syncBuiltIn(shipped));
    const auto overwrite = store.saveUser(profile("survey-1500g", "Hacked"));
    ASSERT_FALSE(overwrite);
    EXPECT_EQ(overwrite.error().code, StorageErrorCode::InvalidValue);
    const auto removed = store.remove("survey-1500g");
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, StorageErrorCode::InvalidValue);
    // A copy under a new key is a normal user profile.
    ASSERT_TRUE(store.saveUser(profile("survey-copy", "Survey quad (copy)")));
    EXPECT_TRUE(*store.remove("survey-copy"));
    EXPECT_FALSE(*store.remove("survey-copy"));
}

TEST(StorageVehicleProfiles, RejectsInvalidRecords) {
    DbConnection db = migratedDb();
    VehicleProfileStore store(db);
    EXPECT_FALSE(store.saveUser(profile("", "No key")));
    EXPECT_FALSE(store.saveUser(profile("k", "")));
    EXPECT_FALSE(store.saveUser(profile("k", "Array", "[1, 2]")));
    EXPECT_FALSE(store.saveUser(profile("k", "Broken", "{")));
    VehicleProfileRecord boat = profile("boat", "Boat");
    boat.vehicleType = "marine";
    EXPECT_FALSE(store.saveUser(boat));
    // A sync with one invalid record changes nothing.
    const std::vector<VehicleProfileRecord> bad{profile("a", "A"), profile("b", "B", "nope")};
    EXPECT_FALSE(store.syncBuiltIn(bad));
    EXPECT_TRUE(store.list()->empty());
}

TEST(StorageInputProfiles, SaveFindReplaceAndRemove) {
    DbConnection db = migratedDb();
    InputProfileStore store(db);
    InputProfileRecord radio;
    radio.deviceGuid = "0300aa";
    radio.name = "Radio AETR";
    radio.mappingJson = R"({"schemaVersion":1})";
    const auto id = store.save(radio);
    ASSERT_TRUE(id) << id.error().message;
    InputProfileRecord pad = radio;
    pad.deviceGuid = "0300bb";
    pad.name = "Gamepad mode 2";
    ASSERT_TRUE(store.save(pad));
    // Same device, type and name: replaced, same id.
    radio.mappingJson = R"({"schemaVersion":1,"filterHz":20})";
    EXPECT_EQ(*store.save(radio), *id);

    const auto found = store.find("0300aa", "aerial", "Radio AETR");
    ASSERT_TRUE(found && *found);
    EXPECT_EQ((*found)->mappingJson, radio.mappingJson);
    EXPECT_FALSE(*store.find("0300aa", "ground", "Radio AETR"));
    EXPECT_EQ(store.list()->size(), 2U);
    EXPECT_EQ(store.list(std::string_view("0300bb"))->size(), 1U);
    EXPECT_TRUE(*store.remove(*id));
    EXPECT_FALSE(*store.remove(*id));

    InputProfileRecord invalid = pad;
    invalid.mappingJson = "[]";
    EXPECT_FALSE(store.save(invalid));
    invalid = pad;
    invalid.name.clear();
    EXPECT_FALSE(store.save(invalid));
}

TEST(StorageSessions, InsertUpdateListAndProfileLink) {
    DbConnection db = migratedDb();
    VehicleProfileStore profiles(db);
    const std::vector<VehicleProfileRecord> shipped{profile("camera-250g", "Camera drone")};
    ASSERT_TRUE(profiles.syncBuiltIn(shipped));
    SessionStore store(db);
    SessionRecord s;
    s.vehicleProfileKey = "camera-250g";
    s.startedAt = "2026-06-21T10:00:00Z";
    const auto id = store.insert(s);
    ASSERT_TRUE(id) << id.error().message;
    s.id = *id;
    s.durationS = 312.5;
    s.maxAltitudeAglM = 48.0;
    s.maxSpeedMs = 14.2;
    s.distanceM = 1830.0;
    s.energyUsedWh = 6.1;
    s.crashes = 1;
    EXPECT_TRUE(*store.update(s));

    SessionRecord older;
    older.vehicleProfileKey = "deleted-profile";  // Unknown: stored without a link.
    older.startedAt = "2026-06-20T08:00:00Z";
    ASSERT_TRUE(store.insert(older));
    const auto list = store.recent();
    ASSERT_TRUE(list);
    ASSERT_EQ(list->size(), 2U);
    EXPECT_EQ(list->front(), s);
    EXPECT_EQ(list->back().vehicleProfileKey, "");
    EXPECT_EQ(store.recent(1)->size(), 1U);

    SessionRecord bad = s;
    bad.distanceM = -1.0;
    EXPECT_FALSE(store.update(bad));
    bad = s;
    bad.maxSpeedMs = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(store.insert(bad));
    bad = s;
    bad.startedAt.clear();
    EXPECT_FALSE(store.insert(bad));
    SessionRecord missing = s;
    missing.id = 9999;
    EXPECT_FALSE(*store.update(missing));
    EXPECT_TRUE(*store.remove(*id));
    EXPECT_EQ(store.recent()->size(), 1U);
}
