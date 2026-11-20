// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/disk_cache.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <iterator>
#include <map>

#include "ds/common/log.hpp"
#include "ds/common/sha256.hpp"

namespace ds::net {
namespace {

namespace fs = std::filesystem;

constexpr auto kPrivateDir = fs::perms::owner_all;
constexpr auto kPrivateFile = fs::perms::owner_read | fs::perms::owner_write;

std::unexpected<NetError> cacheError(std::string message) {
    return netError(NetErrorCode::Cache, std::move(message));
}

template <typename T>
NetResult<T> fromCommon(common::Result<T> r) {
    if (!r) {
        return cacheError("cache index: " + r.error().message);
    }
    if constexpr (std::is_void_v<T>) {
        return {};
    } else {
        return std::move(*r);
    }
}

bool safeProviderName(std::string_view provider) {
    return !provider.empty() && provider != "." && provider != ".." &&
           std::ranges::all_of(provider, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.';
           });
}

NetResult<void> makePrivateDirs(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return cacheError(std::format("cannot create \"{}\": {}", dir.string(), ec.message()));
    }
    fs::permissions(dir, kPrivateDir, fs::perm_options::replace, ec);
    return {};
}

}  // namespace

// --- InMemoryCacheIndex ---------------------------------------------------------------------

common::Result<std::optional<CacheEntry>> InMemoryCacheIndex::find(std::string_view provider,
                                                                   std::string_view key) {
    const std::scoped_lock lock(mutex_);
    const auto it = entries_.find(std::pair{std::string(provider), std::string(key)});
    return it == entries_.end() ? std::optional<CacheEntry>{} : std::optional{it->second};
}

common::Result<void> InMemoryCacheIndex::put(const CacheEntry& entry) {
    const std::scoped_lock lock(mutex_);
    entries_[{entry.provider, entry.key}] = entry;
    return {};
}

common::Result<void> InMemoryCacheIndex::touch(std::string_view provider, std::string_view key,
                                               std::int64_t unixS) {
    const std::scoped_lock lock(mutex_);
    if (const auto it = entries_.find(std::pair{std::string(provider), std::string(key)});
        it != entries_.end()) {
        it->second.lastAccessUnixS = unixS;
    }
    return {};
}

common::Result<bool> InMemoryCacheIndex::setPinned(std::string_view provider, std::string_view key,
                                                   bool pinned) {
    const std::scoped_lock lock(mutex_);
    const auto it = entries_.find(std::pair{std::string(provider), std::string(key)});
    if (it == entries_.end()) {
        return false;
    }
    it->second.pinned = pinned;
    return true;
}

common::Result<void> InMemoryCacheIndex::remove(std::string_view provider, std::string_view key) {
    const std::scoped_lock lock(mutex_);
    entries_.erase(std::pair{std::string(provider), std::string(key)});
    return {};
}

common::Result<std::vector<CacheEntry>> InMemoryCacheIndex::list(std::string_view provider) {
    const std::scoped_lock lock(mutex_);
    std::vector<CacheEntry> out;
    for (const auto& [k, e] : entries_) {
        if (provider.empty() || e.provider == provider) {
            out.push_back(e);
        }
    }
    std::ranges::stable_sort(out, {}, &CacheEntry::lastAccessUnixS);
    return out;
}

// --- DiskCache --------------------------------------------------------------------------------

DiskCache::DiskCache(fs::path root, CacheIndex& index, std::uint64_t limitBytes)
    : root_(std::move(root)), index_(&index), limitBytes_(limitBytes) {}

NetResult<std::optional<CachedPayload>> DiskCache::get(std::string_view provider,
                                                       std::string_view key,
                                                       std::int64_t nowUnixS) {
    const std::scoped_lock lock(mutex_);
    auto found = fromCommon(index_->find(provider, key));
    if (!found) {
        return std::unexpected(found.error());
    }
    if (!*found) {
        return std::optional<CachedPayload>{};
    }
    CachedPayload payload{**found, {}, false};
    std::ifstream in(root_ / payload.entry.relativePath, std::ios::binary);
    payload.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        payload.body.clear();
    }
    if (payload.body.size() != payload.entry.sizeBytes ||
        common::sha256Hex(payload.body) != payload.entry.contentHash) {
        log::warn("cache: dropping damaged entry {}/{}", provider, payload.entry.relativePath);
        if (auto removed = removeLocked(payload.entry); !removed) {
            return std::unexpected(removed.error());
        }
        return std::optional<CachedPayload>{};
    }
    payload.stale = payload.entry.expiresAtUnixS && nowUnixS >= *payload.entry.expiresAtUnixS;
    payload.entry.lastAccessUnixS = nowUnixS;
    if (auto touched = fromCommon(index_->touch(provider, key, nowUnixS)); !touched) {
        return std::unexpected(touched.error());
    }
    return std::optional{std::move(payload)};
}

NetResult<CacheEntry> DiskCache::put(std::string_view provider, std::string_view key,
                                     std::string_view body, const PayloadMeta& meta,
                                     std::int64_t nowUnixS) {
    if (!safeProviderName(provider)) {
        return cacheError(std::format("invalid provider key \"{}\"", provider));
    }
    const std::scoped_lock lock(mutex_);
    CacheEntry entry;
    entry.provider = std::string(provider);
    entry.key = std::string(key);
    const std::string name = common::sha256Hex(key);
    entry.relativePath = std::format("{}/{}/{}.bin", provider, name.substr(0, 2), name);
    entry.sizeBytes = body.size();
    entry.contentType = meta.contentType;
    entry.etag = meta.etag;
    entry.lastModified = meta.lastModified;
    entry.contentHash = common::sha256Hex(body);
    entry.storedAtUnixS = nowUnixS;
    entry.lastAccessUnixS = nowUnixS;
    entry.expiresAtUnixS = meta.expiresAtUnixS;
    if (auto old = fromCommon(index_->find(provider, key)); old && *old) {
        entry.pinned = (*old)->pinned;
    }

    const fs::path file = root_ / entry.relativePath;
    // Every level gets mode 0700 (create_directories would use the umask for the parents).
    for (const fs::path& dir : {root_, root_ / provider, file.parent_path()}) {
        if (auto ok = makePrivateDirs(dir); !ok) {
            return std::unexpected(ok.error());
        }
    }
    static std::atomic<std::uint64_t> counter{0};
    const fs::path temp = file.parent_path() / std::format(".{}.tmp{}", name, counter++);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        std::error_code ec;
        fs::permissions(temp, kPrivateFile, fs::perm_options::replace, ec);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.close();
        if (!out) {
            fs::remove(temp, ec);
            return cacheError(std::format("cannot write \"{}\"", temp.string()));
        }
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
        fs::remove(temp, ec);
        return cacheError(std::format("cannot store \"{}\": {}", file.string(), ec.message()));
    }
    if (auto ok = fromCommon(index_->put(entry)); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = evictLocked(); !ok) {
        return std::unexpected(ok.error());
    }
    return entry;
}

NetResult<void> DiskCache::revalidated(std::string_view provider, std::string_view key,
                                       std::optional<std::int64_t> expiresAtUnixS,
                                       std::int64_t nowUnixS) {
    const std::scoped_lock lock(mutex_);
    auto found = fromCommon(index_->find(provider, key));
    if (!found) {
        return std::unexpected(found.error());
    }
    if (!*found) {
        return {};
    }
    CacheEntry e = **found;
    e.storedAtUnixS = nowUnixS;
    e.lastAccessUnixS = nowUnixS;
    e.expiresAtUnixS = expiresAtUnixS;
    return fromCommon(index_->put(e));
}

NetResult<bool> DiskCache::setPinned(std::string_view provider, std::string_view key, bool pinned) {
    const std::scoped_lock lock(mutex_);
    auto r = fromCommon(index_->setPinned(provider, key, pinned));
    if (r && !pinned) {
        if (auto ok = evictLocked(); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return r;
}

NetResult<std::uint64_t> DiskCache::totalBytes() {
    const std::scoped_lock lock(mutex_);
    auto all = fromCommon(index_->list({}));
    if (!all) {
        return std::unexpected(all.error());
    }
    std::uint64_t total = 0;
    for (const CacheEntry& e : *all) {
        total += e.sizeBytes;
    }
    return total;
}

NetResult<std::vector<CacheUsage>> DiskCache::usage() {
    const std::scoped_lock lock(mutex_);
    auto all = fromCommon(index_->list({}));
    if (!all) {
        return std::unexpected(all.error());
    }
    std::map<std::string, CacheUsage, std::less<>> by;
    for (const CacheEntry& e : *all) {
        CacheUsage& u = by[e.provider];
        u.provider = e.provider;
        ++u.entries;
        u.bytes += e.sizeBytes;
        if (e.pinned) {
            ++u.pinnedEntries;
            u.pinnedBytes += e.sizeBytes;
        }
    }
    std::vector<CacheUsage> out;
    out.reserve(by.size());
    for (auto& [key, u] : by) {
        out.push_back(std::move(u));
    }
    return out;
}

NetResult<int> DiskCache::releasePinned(std::string_view provider) {
    const std::scoped_lock lock(mutex_);
    auto all = fromCommon(index_->list(provider));
    if (!all) {
        return std::unexpected(all.error());
    }
    int released = 0;
    for (const CacheEntry& e : *all) {
        if (!e.pinned) {
            continue;
        }
        if (auto ok = fromCommon(index_->setPinned(e.provider, e.key, false)); !ok) {
            return std::unexpected(ok.error());
        }
        ++released;
    }
    if (auto ok = evictLocked(); !ok) {
        return std::unexpected(ok.error());
    }
    return released;
}

std::uint64_t DiskCache::limit() const {
    const std::scoped_lock lock(mutex_);
    return limitBytes_;
}

NetResult<void> DiskCache::setLimit(std::uint64_t limitBytes) {
    const std::scoped_lock lock(mutex_);
    limitBytes_ = limitBytes;
    return evictLocked();
}

NetResult<void> DiskCache::clear(std::string_view provider, bool includePinned) {
    const std::scoped_lock lock(mutex_);
    auto all = fromCommon(index_->list(provider));
    if (!all) {
        return std::unexpected(all.error());
    }
    for (const CacheEntry& e : *all) {
        if (!e.pinned || includePinned) {
            if (auto ok = removeLocked(e); !ok) {
                return ok;
            }
        }
    }
    return {};
}

NetResult<void> DiskCache::evictLocked() {
    auto all = fromCommon(index_->list({}));  // Least recently used first.
    if (!all) {
        return std::unexpected(all.error());
    }
    std::uint64_t total = 0;
    for (const CacheEntry& e : *all) {
        total += e.sizeBytes;
    }
    for (const CacheEntry& e : *all) {
        if (total <= limitBytes_) {
            break;
        }
        if (e.pinned) {
            continue;
        }
        if (auto ok = removeLocked(e); !ok) {
            return ok;
        }
        total -= e.sizeBytes;
    }
    return {};
}

NetResult<void> DiskCache::removeLocked(const CacheEntry& entry) {
    std::error_code ec;
    fs::remove(root_ / entry.relativePath, ec);  // A missing file is fine.
    return fromCommon(index_->remove(entry.provider, entry.key));
}

}  // namespace ds::net
