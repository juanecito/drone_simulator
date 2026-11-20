// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// The user database schema (SPECIFICATION §4.10). Each migration is a list of single SQL
// statements (Qt's SQLite driver executes one statement per call). Never edit a released
// migration: add a new one.

#include <array>
#include <string_view>

#include "ds/storage/migrations.hpp"

namespace ds::storage {
namespace {

constexpr std::array<std::string_view, 20> kVersion1{
    R"sql(CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL                     -- JSON-encoded value
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS vehicle_profiles (
    id           INTEGER PRIMARY KEY,
    profile_key  TEXT NOT NULL UNIQUE,
    vehicle_type TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    name         TEXT NOT NULL,
    origin       TEXT NOT NULL CHECK (origin IN ('builtin', 'user')),
    params_json  TEXT NOT NULL,
    created_at   TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at   TEXT NOT NULL DEFAULT (datetime('now'))
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS data_providers (
    id               INTEGER PRIMARY KEY,
    provider_key     TEXT NOT NULL UNIQUE,
    name             TEXT NOT NULL,
    kind             TEXT NOT NULL CHECK (kind IN ('elevation', 'imagery')),
    protocol         TEXT NOT NULL,
    url_template     TEXT NOT NULL,         -- never contains the key itself
    params_json      TEXT NOT NULL,
    attribution      TEXT NOT NULL,
    licence          TEXT,
    requires_key     INTEGER NOT NULL DEFAULT 0,
    allow_cache      INTEGER NOT NULL DEFAULT 1,
    max_age_days     INTEGER,
    rate_limit_rps   REAL,
    daily_call_limit INTEGER,
    insecure_http    INTEGER NOT NULL DEFAULT 0,
    origin           TEXT NOT NULL CHECK (origin IN ('builtin', 'user'))
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS provider_calls (
    id          INTEGER PRIMARY KEY,
    provider_id INTEGER NOT NULL REFERENCES data_providers(id) ON DELETE CASCADE,
    called_at   TEXT NOT NULL,
    dataset     TEXT,
    west REAL, south REAL, east REAL, north REAL,
    http_status INTEGER,
    bytes       INTEGER
))sql",

    R"sql(CREATE INDEX IF NOT EXISTS provider_calls_by_time
    ON provider_calls (provider_id, called_at))sql",

    R"sql(CREATE TABLE IF NOT EXISTS provider_credentials (
    provider_id INTEGER PRIMARY KEY REFERENCES data_providers(id) ON DELETE CASCADE,
    api_key     TEXT NOT NULL
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS terrain_presets (
    id          INTEGER PRIMARY KEY,
    preset_key  TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    source_type TEXT NOT NULL CHECK (source_type IN ('procedural', 'online', 'file')),
    seed        INTEGER,
    elevation_provider_id INTEGER REFERENCES data_providers(id) ON DELETE RESTRICT,
    dem_files   TEXT,
    ortho_files TEXT,
    dem_sha256  TEXT,
    imagery_provider_id   INTEGER REFERENCES data_providers(id) ON DELETE SET NULL,
    origin_lat  REAL NOT NULL DEFAULT 0,
    origin_lon  REAL NOT NULL DEFAULT 0,
    origin_alt  REAL NOT NULL DEFAULT 0,
    extent_width_m  REAL,
    extent_length_m REAL,
    params_json TEXT NOT NULL,
    CHECK ((source_type = 'procedural' AND seed IS NOT NULL) OR
           (source_type = 'online' AND elevation_provider_id IS NOT NULL) OR
           (source_type = 'file' AND dem_files IS NOT NULL)),
    CHECK ((extent_width_m IS NULL) = (extent_length_m IS NULL))
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS missions (
    id                 INTEGER PRIMARY KEY,
    mission_key        TEXT NOT NULL UNIQUE,
    title              TEXT NOT NULL,
    vehicle_type       TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    origin             TEXT NOT NULL CHECK (origin IN ('builtin', 'user')),
    chapter            TEXT,
    sort_order         INTEGER NOT NULL DEFAULT 0,
    file_path          TEXT NOT NULL,
    file_sha256        TEXT NOT NULL,
    schema_version     INTEGER NOT NULL,
    status             TEXT NOT NULL CHECK (status IN ('valid', 'invalid', 'missing')),
    validation_errors  TEXT,
    updated_at         TEXT NOT NULL DEFAULT (datetime('now'))
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS downloaded_areas (
    id            INTEGER PRIMARY KEY,
    provider_id   INTEGER NOT NULL REFERENCES data_providers(id) ON DELETE CASCADE,
    west REAL NOT NULL, south REAL NOT NULL, east REAL NOT NULL, north REAL NOT NULL,
    max_resolution_m REAL NOT NULL,
    mission_id    INTEGER REFERENCES missions(id) ON DELETE CASCADE,
    status        TEXT NOT NULL CHECK (status IN ('pending', 'partial', 'complete')),
    data_hash     TEXT,
    downloaded_at TEXT
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS scenes (
    id                INTEGER PRIMARY KEY,
    name              TEXT NOT NULL UNIQUE,
    terrain_preset_id INTEGER REFERENCES terrain_presets(id) ON DELETE SET NULL,
    environment_json  TEXT NOT NULL,
    created_at        TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at        TEXT NOT NULL DEFAULT (datetime('now'))
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS scene_items (
    id              INTEGER PRIMARY KEY,
    scene_id        INTEGER NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,
    type            TEXT NOT NULL,
    name            TEXT NOT NULL,
    pos_x REAL, pos_y REAL, pos_z REAL,
    rot_w REAL, rot_x REAL, rot_y REAL, rot_z REAL,
    scale_x REAL, scale_y REAL, scale_z REAL,
    properties_json TEXT NOT NULL DEFAULT '{}',
    sort_order      INTEGER NOT NULL DEFAULT 0
))sql",

    R"sql(CREATE INDEX IF NOT EXISTS scene_items_by_scene ON scene_items (scene_id, sort_order))sql",

    R"sql(CREATE TABLE IF NOT EXISTS input_profiles (
    id           INTEGER PRIMARY KEY,
    device_guid  TEXT NOT NULL,
    vehicle_type TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    name         TEXT NOT NULL,
    mapping_json TEXT NOT NULL,
    UNIQUE (device_guid, vehicle_type, name)
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS sessions (
    id                 INTEGER PRIMARY KEY,
    scene_id           INTEGER REFERENCES scenes(id) ON DELETE SET NULL,
    vehicle_profile_id INTEGER REFERENCES vehicle_profiles(id) ON DELETE SET NULL,
    vehicle_type       TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    started_at         TEXT NOT NULL,
    duration_s         REAL,
    max_altitude_agl_m REAL,
    max_speed_ms       REAL,
    distance_m         REAL,
    energy_used_wh     REAL,
    crashes            INTEGER NOT NULL DEFAULT 0
))sql",

    R"sql(CREATE TABLE IF NOT EXISTS mission_attempts (
    id                INTEGER PRIMARY KEY,
    mission_id        INTEGER NOT NULL REFERENCES missions(id) ON DELETE CASCADE,
    session_id        INTEGER REFERENCES sessions(id) ON DELETE SET NULL,
    started_at        TEXT NOT NULL,
    outcome           TEXT NOT NULL CHECK (outcome IN ('succeeded', 'failed', 'aborted')),
    failure_reason    TEXT,
    score             REAL NOT NULL DEFAULT 0,
    stars             INTEGER NOT NULL DEFAULT 0 CHECK (stars BETWEEN 0 AND 3),
    duration_s        REAL,
    terrain_data_hash TEXT,
    results_json      TEXT NOT NULL
))sql",

    R"sql(CREATE INDEX IF NOT EXISTS mission_attempts_by_mission ON mission_attempts (mission_id))sql",

    R"sql(CREATE VIEW IF NOT EXISTS mission_progress AS
SELECT m.id AS mission_id,
       COUNT(a.id)                                                  AS attempts,
       COALESCE(SUM(a.outcome = 'succeeded'), 0)                    AS successes,
       MAX(a.stars)                                                 AS best_stars,
       MAX(a.score)                                                 AS best_score,
       MIN(CASE WHEN a.outcome = 'succeeded' THEN a.duration_s END) AS best_time_s
FROM missions m LEFT JOIN mission_attempts a ON a.mission_id = m.id
GROUP BY m.id)sql",

    R"sql(CREATE TABLE IF NOT EXISTS telemetry_samples (
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    t_s REAL NOT NULL,
    pos_x REAL, pos_y REAL, pos_z REAL,
    roll REAL, pitch REAL, yaw REAL,
    speed REAL, alt_agl REAL, throttle REAL, steering REAL,
    battery_v REAL, battery_a REAL,
    PRIMARY KEY (session_id, t_s)
))sql",

    R"sql(CREATE INDEX IF NOT EXISTS downloaded_areas_by_provider ON downloaded_areas (provider_id))sql",

    R"sql(CREATE INDEX IF NOT EXISTS sessions_by_start ON sessions (started_at))sql",
};

// M7 step 7: replays of flight sessions (FR-X-06).
constexpr std::array<std::string_view, 1> kVersion2{
    R"sql(CREATE TABLE IF NOT EXISTS replays (
    session_id  INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,
    mission_key TEXT,
    replayable  INTEGER NOT NULL DEFAULT 1 CHECK (replayable IN (0, 1)),
    duration_s  REAL NOT NULL DEFAULT 0,
    tick_count  INTEGER NOT NULL DEFAULT 0,
    meta_json   TEXT NOT NULL DEFAULT '{}',
    inputs      BLOB NOT NULL
))sql",
};

constexpr std::array<Migration, 2> kMigrations{{
    {1, "initial schema", kVersion1},
    {2, "flight replays", kVersion2},
}};

}  // namespace

std::span<const Migration> schemaMigrations() noexcept { return kMigrations; }

int latestSchemaVersion() noexcept { return static_cast<int>(kMigrations.size()); }

}  // namespace ds::storage
