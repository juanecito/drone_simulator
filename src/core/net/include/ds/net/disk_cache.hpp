// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/net/net_error.hpp"

/// @file
/// @brief Per-user disk cache of provider payloads (FR-NET-04, FR-NET-05, FR-TER-O14).
/// @ingroup net

namespace ds::net {

/// @brief Index entry of one cached payload (one row of the `tiles` table in `cache.db`).
struct CacheEntry {
    std::string provider;              ///< Provider key.
    std::string key;                   ///< Request key (the request URL without credentials).
    std::string relativePath;          ///< File, relative to the cache root.
    std::uint64_t sizeBytes = 0;       ///< File size.
    std::string contentType;           ///< Response content type.
    std::string etag;                  ///< HTTP ETag, for revalidation.
    std::string lastModified;          ///< HTTP Last-Modified, for revalidation.
    std::string contentHash;           ///< SHA-256 of the payload (FR-TER-O14).
    std::int64_t storedAtUnixS = 0;    ///< When downloaded or last revalidated.
    std::int64_t lastAccessUnixS = 0;  ///< Last use (LRU).
    std::optional<std::int64_t> expiresAtUnixS;  ///< Stale after this; empty ⇒ never.
    bool pinned = false;  ///< Kept by eviction (downloaded areas, mission areas).

    /// @brief Equality (tests).
    bool operator==(const CacheEntry&) const = default;
};

/// @brief Index of the cache (FR-NET-04).
/// @details Implemented by `core/storage` over the separate `cache.db`; tests use
/// InMemoryCacheIndex. Implementations must be thread-safe.
class CacheIndex {
public:
    virtual ~CacheIndex() = default;
    /// @brief The entry for (@p provider, @p key), if any.
    [[nodiscard]] virtual common::Result<std::optional<CacheEntry>> find(std::string_view provider,
                                                                         std::string_view key) = 0;
    /// @brief Inserts or replaces the entry with the same provider and key.
    [[nodiscard]] virtual common::Result<void> put(const CacheEntry& entry) = 0;
    /// @brief Sets the last access time.
    [[nodiscard]] virtual common::Result<void> touch(std::string_view provider,
                                                     std::string_view key, std::int64_t unixS) = 0;
    /// @brief Pins or unpins an entry. Returns false if there is none.
    [[nodiscard]] virtual common::Result<bool> setPinned(std::string_view provider,
                                                         std::string_view key, bool pinned) = 0;
    /// @brief Removes an entry (no-op if there is none).
    [[nodiscard]] virtual common::Result<void> remove(std::string_view provider,
                                                      std::string_view key) = 0;
    /// @brief All entries, least recently used first (all providers if @p provider is empty).
    [[nodiscard]] virtual common::Result<std::vector<CacheEntry>> list(
        std::string_view provider = {}) = 0;
};

/// @brief Thread-safe in-memory index (tests).
class InMemoryCacheIndex final : public CacheIndex {
public:
    [[nodiscard]] common::Result<std::optional<CacheEntry>> find(std::string_view provider,
                                                                 std::string_view key) override;
    [[nodiscard]] common::Result<void> put(const CacheEntry& entry) override;
    [[nodiscard]] common::Result<void> touch(std::string_view provider, std::string_view key,
                                             std::int64_t unixS) override;
    [[nodiscard]] common::Result<bool> setPinned(std::string_view provider, std::string_view key,
                                                 bool pinned) override;
    [[nodiscard]] common::Result<void> remove(std::string_view provider,
                                              std::string_view key) override;
    [[nodiscard]] common::Result<std::vector<CacheEntry>> list(std::string_view provider) override;

private:
    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, CacheEntry, std::less<>> entries_;
};

/// @brief A payload read from the cache.
struct CachedPayload {
    CacheEntry entry;    ///< Its index entry.
    std::string body;    ///< The payload.
    bool stale = false;  ///< Past its expiry: usable offline, revalidate when online.
};

/// @brief Metadata stored with a payload.
struct PayloadMeta {
    std::string contentType;                     ///< Response content type.
    std::string etag;                            ///< ETag.
    std::string lastModified;                    ///< Last-Modified.
    std::optional<std::int64_t> expiresAtUnixS;  ///< From maxAgeDays / Cache-Control.
};

/// @brief Payload files under `<root>/<provider>/…`, indexed by a CacheIndex.
/// @details Directories are created with mode 0700 and files with 0600 (NFR-SEC-01). Files are
/// written to a temporary name and renamed, and their SHA-256 is checked on every read, so a
/// crash or a damaged file is never served (the entry is dropped instead). Eviction removes the
/// least recently used unpinned entries until the total size is within the limit. Thread-safe.
class DiskCache {
public:
    /// @brief Cache under @p root (created on first write) with @p index and a size limit.
    DiskCache(std::filesystem::path root, CacheIndex& index, std::uint64_t limitBytes);

    /// @brief Cached payload for (@p provider, @p key), touching its access time.
    [[nodiscard]] NetResult<std::optional<CachedPayload>> get(std::string_view provider,
                                                              std::string_view key,
                                                              std::int64_t nowUnixS);
    /// @brief Stores @p body, then evicts if over the limit. Keeps an existing pin.
    [[nodiscard]] NetResult<CacheEntry> put(std::string_view provider, std::string_view key,
                                            std::string_view body, const PayloadMeta& meta,
                                            std::int64_t nowUnixS);
    /// @brief Marks an existing entry as fresh again after a `304 Not Modified`.
    [[nodiscard]] NetResult<void> revalidated(std::string_view provider, std::string_view key,
                                              std::optional<std::int64_t> expiresAtUnixS,
                                              std::int64_t nowUnixS);
    /// @brief Pins or unpins an entry (FR-NET-05). Returns false if not cached.
    [[nodiscard]] NetResult<bool> setPinned(std::string_view provider, std::string_view key,
                                            bool pinned);
    /// @brief Total size of the indexed files.
    [[nodiscard]] NetResult<std::uint64_t> totalBytes();
    /// @brief Changes the size limit and evicts if needed.
    [[nodiscard]] NetResult<void> setLimit(std::uint64_t limitBytes);
    /// @brief Removes every entry of @p provider (all if empty), pinned ones only if
    /// @p includePinned.
    [[nodiscard]] NetResult<void> clear(std::string_view provider, bool includePinned);
    /// @brief Cache root directory.
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    NetResult<void> evictLocked();
    NetResult<void> removeLocked(const CacheEntry& entry);

    std::filesystem::path root_;
    CacheIndex* index_;
    std::uint64_t limitBytes_;
    std::mutex mutex_;
};

}  // namespace ds::net
