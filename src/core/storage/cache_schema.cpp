// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// Schema of the tile-cache index `cache.db` (SPECIFICATION §4.10, FR-NET-04). It is a separate
// file so the cache can be wiped without touching user data. Never edit a released migration.

#include <array>
#include <string_view>

#include "ds/storage/migrations.hpp"

namespace ds::storage {
namespace {

constexpr std::array<std::string_view, 3> kCacheVersion1{
    R"sql(CREATE TABLE IF NOT EXISTS tiles (
    provider      TEXT NOT NULL,
    key           TEXT NOT NULL,             -- request URL without credentials
    path          TEXT NOT NULL,             -- relative to the cache root
    size          INTEGER NOT NULL,
    content_type  TEXT NOT NULL DEFAULT '',
    etag          TEXT NOT NULL DEFAULT '',
    last_modified TEXT NOT NULL DEFAULT '',
    content_hash  TEXT NOT NULL,             -- SHA-256 (FR-TER-O14)
    stored_at     INTEGER NOT NULL,          -- Unix seconds
    last_access   INTEGER NOT NULL,          -- Unix seconds (LRU)
    expires_at    INTEGER,                   -- Unix seconds; NULL = never
    pinned        INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (provider, key)
))sql",
    R"sql(CREATE INDEX IF NOT EXISTS tiles_by_access ON tiles (last_access))sql",
    R"sql(CREATE INDEX IF NOT EXISTS tiles_by_provider ON tiles (provider, last_access))sql",
};

constexpr std::array<Migration, 1> kCacheMigrations{{
    {1, "tile cache index", kCacheVersion1},
}};

}  // namespace

std::span<const Migration> cacheSchemaMigrations() noexcept { return kCacheMigrations; }

}  // namespace ds::storage
