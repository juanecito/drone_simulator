// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ds/net/disk_cache.hpp"
#include "ds/net/provider.hpp"
#include "ds/net/quota.hpp"
#include "ds/storage/connection.hpp"

/// @file
/// @brief Data providers, their call log and fallback credentials (`data_providers`,
/// `provider_calls`, `provider_credentials`; FR-TER-O02, FR-TER-O17, FR-NET-07, FR-NET-09),
/// and the tile-cache index of `cache.db` (FR-NET-04).
/// @ingroup storage

namespace ds::storage {

/// @brief Formats Unix seconds as `YYYY-MM-DDTHH:MM:SSZ` (sortable text, UTC).
[[nodiscard]] std::string unixToIsoUtc(std::int64_t unixS);
/// @brief Parses the format of unixToIsoUtc(); nullopt if malformed.
[[nodiscard]] std::optional<std::int64_t> isoUtcToUnix(std::string_view text);

/// @brief Usage of a provider over a period (FR-NET-09).
struct ProviderUsage {
    int calls = 0;            ///< Requests that reached the provider.
    int errors = 0;           ///< Of which failed (status 0 or ≥ 400).
    std::uint64_t bytes = 0;  ///< Bytes received.
};

/// @brief Reads and writes provider rows on a connection of the storage thread.
/// @details The full configuration is kept in `params_json` (format of net::toJson()); the
/// other columns mirror it for queries and constraints.
class ProviderStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit ProviderStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief Inserts or updates the built-in @p presets by key (FR-DB-09). A user-defined
    /// provider with the same key is left untouched. Runs in one transaction.
    [[nodiscard]] StorageResult<void> syncBuiltin(std::span<const net::ProviderConfig> presets);
    /// @brief All providers, sorted by name. Rows that no longer parse are skipped with a log
    /// warning.
    [[nodiscard]] StorageResult<std::vector<net::ProviderConfig>> list();
    /// @brief The provider with @p key, if any.
    [[nodiscard]] StorageResult<std::optional<net::ProviderConfig>> find(std::string_view key);
    /// @brief Inserts or replaces a user-defined provider (origin forced to `user`). A built-in
    /// key cannot be overwritten (InvalidValue).
    [[nodiscard]] StorageResult<std::int64_t> saveUser(net::ProviderConfig provider);
    /// @brief Deletes a user-defined provider. Returns false if there was none.
    [[nodiscard]] StorageResult<bool> removeUser(std::string_view key);

    /// @brief Appends a call to `provider_calls`. Unknown providers are an InvalidValue error.
    [[nodiscard]] StorageResult<void> recordCall(const net::ProviderCall& call);
    /// @brief Times of the calls to @p key at or after @p sinceUnixS, ascending.
    [[nodiscard]] StorageResult<std::vector<std::int64_t>> callTimesSince(std::string_view key,
                                                                          std::int64_t sinceUnixS);
    /// @brief Usage of @p key at or after @p sinceUnixS.
    [[nodiscard]] StorageResult<ProviderUsage> usageSince(std::string_view key,
                                                          std::int64_t sinceUnixS);
    /// @brief Deletes calls older than @p beforeUnixS (housekeeping). Returns the count.
    [[nodiscard]] StorageResult<int> pruneCalls(std::int64_t beforeUnixS);

    /// @brief Stores the fallback API key (only when no OS keyring is available, FR-NET-07).
    [[nodiscard]] StorageResult<void> setCredential(std::string_view key, std::string_view apiKey);
    /// @brief The fallback API key, if stored.
    [[nodiscard]] StorageResult<std::optional<std::string>> credential(std::string_view key);
    /// @brief Removes the fallback API key.
    [[nodiscard]] StorageResult<void> clearCredential(std::string_view key);

private:
    StorageResult<std::optional<std::int64_t>> idOf(std::string_view key);
    StorageResult<std::int64_t> upsert(const net::ProviderConfig& provider);

    DbConnection* connection_;
};

/// @brief The `tiles` table of `cache.db` (connection migrated with cacheSchemaMigrations()).
class CacheIndexStore {
public:
    /// @brief Uses @p connection, which must outlive the store and stay on its thread.
    explicit CacheIndexStore(DbConnection& connection) noexcept : connection_(&connection) {}

    /// @brief See net::CacheIndex::find().
    [[nodiscard]] StorageResult<std::optional<net::CacheEntry>> find(std::string_view provider,
                                                                     std::string_view key);
    /// @brief See net::CacheIndex::put().
    [[nodiscard]] StorageResult<void> put(const net::CacheEntry& entry);
    /// @brief See net::CacheIndex::touch().
    [[nodiscard]] StorageResult<void> touch(std::string_view provider, std::string_view key,
                                            std::int64_t unixS);
    /// @brief See net::CacheIndex::setPinned().
    [[nodiscard]] StorageResult<bool> setPinned(std::string_view provider, std::string_view key,
                                                bool pinned);
    /// @brief See net::CacheIndex::remove().
    [[nodiscard]] StorageResult<void> remove(std::string_view provider, std::string_view key);
    /// @brief See net::CacheIndex::list().
    [[nodiscard]] StorageResult<std::vector<net::CacheEntry>> list(std::string_view provider);

private:
    DbConnection* connection_;
};

class StorageService;

/// @brief net::UsageLedger backed by `provider_calls` through a StorageService (thread-safe;
/// each call waits for the storage thread).
class StorageUsageLedger final : public net::UsageLedger {
public:
    /// @brief Uses @p service (the user database), which must outlive the ledger.
    explicit StorageUsageLedger(StorageService& service) noexcept : service_(&service) {}
    [[nodiscard]] common::Result<std::vector<std::int64_t>> callTimesSince(
        std::string_view providerKey, std::int64_t sinceUnixS) override;
    [[nodiscard]] common::Result<void> record(const net::ProviderCall& call) override;

private:
    StorageService* service_;
};

/// @brief net::CacheIndex backed by `cache.db` through its own StorageService (thread-safe).
class StorageCacheIndex final : public net::CacheIndex {
public:
    /// @brief Uses @p service, started with cacheSchemaMigrations(); it must outlive the index.
    explicit StorageCacheIndex(StorageService& service) noexcept : service_(&service) {}
    [[nodiscard]] common::Result<std::optional<net::CacheEntry>> find(
        std::string_view provider, std::string_view key) override;
    [[nodiscard]] common::Result<void> put(const net::CacheEntry& entry) override;
    [[nodiscard]] common::Result<void> touch(std::string_view provider, std::string_view key,
                                             std::int64_t unixS) override;
    [[nodiscard]] common::Result<bool> setPinned(std::string_view provider, std::string_view key,
                                                 bool pinned) override;
    [[nodiscard]] common::Result<void> remove(std::string_view provider,
                                              std::string_view key) override;
    [[nodiscard]] common::Result<std::vector<net::CacheEntry>> list(
        std::string_view provider) override;

private:
    StorageService* service_;
};

}  // namespace ds::storage
