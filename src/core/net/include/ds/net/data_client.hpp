// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/net/disk_cache.hpp"
#include "ds/net/geo_rect.hpp"
#include "ds/net/http_client.hpp"
#include "ds/net/net_error.hpp"
#include "ds/net/provider.hpp"
#include "ds/net/quota.hpp"

/// @file
/// @brief Provider-aware fetching: credentials, quota, disk cache, offline mode and payload
/// checks on top of HttpClient (§4.6).
/// @ingroup net

namespace ds::net {

/// @brief A bounding-box request (FR-TER-O01).
struct BBoxRequest {
    std::string dataset;  ///< Dataset id; empty ⇒ provider default.
    GeoRect area;         ///< Requested area (EPSG:4326).
};

/// @brief A payload ready for decoding.
struct Payload {
    std::string body;         ///< Raw bytes (e.g. a GeoTIFF).
    std::string contentType;  ///< Content type.
    std::string contentHash;  ///< SHA-256 of body (FR-TER-O14).
    std::string cacheKey;     ///< Key in the disk cache.
    bool fromCache = false;   ///< Served from the cache.
    bool stale = false;       ///< Served from the cache past its expiry (offline or quota).
};

/// @brief Receives a fetch result, on the network thread (or the caller's thread when it is
/// answered at once, e.g. from the cache or with an error).
using PayloadCallback = std::move_only_function<void(NetResult<Payload>)>;

/// @brief Progress of an area download (FR-TER-O09).
struct DownloadProgress {
    int requestsDone = 0;     ///< Finished requests (including cached ones).
    int requestsTotal = 0;    ///< All requests.
    std::uint64_t bytes = 0;  ///< Bytes received or read from the cache.
};

/// @brief A running area download. Destroying the handle does not cancel it.
class DownloadJob {
public:
    /// @brief Cancels the remaining requests; the callback then reports Cancelled.
    void cancel();
    /// @brief Current progress.
    [[nodiscard]] DownloadProgress progress() const;
    /// @brief True once the completion callback has run.
    [[nodiscard]] bool finished() const noexcept { return finished_.load(); }

private:
    friend class DataClient;
    mutable std::mutex mutex_;
    DownloadProgress progress_;
    std::vector<RequestId> requests_;
    std::function<void(RequestId)> cancelRequest_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
    std::vector<std::string> cacheKeys_;
    std::optional<NetError> firstError_;
    std::move_only_function<void(NetResult<std::vector<std::string>>)> done_;
    std::function<void(const DownloadProgress&)> onProgress_;
};

/// @brief Estimate shown before a download (FR-TER-O09, FR-TER-O17).
struct DownloadEstimate {
    std::vector<GeoRect> requests;     ///< Planned bounding boxes.
    int uncachedRequests = 0;          ///< Calls that will be made.
    std::uint64_t estimatedBytes = 0;  ///< Approximate download size.
    std::optional<QuotaStatus> quota;  ///< For quota-limited providers.
};

/// @brief Fetches provider data through the cache (FR-NET-01 … FR-NET-07, FR-TER-O16/O17).
/// @details Order of checks for a fetch: fresh cache entry ⇒ served; manual offline or quota
/// exhausted ⇒ stale cache entry if any, else Offline / QuotaExceeded; no API key ⇒
/// MissingKey; otherwise one HTTP request (conditional if a stale entry exists), whose payload
/// is validated, cached and returned. Network failures fall back to a stale entry. Every
/// request that reaches the provider is recorded in the ledger. Thread-safe. The DataClient may
/// be destroyed while requests are in flight; the HttpClient, DiskCache and UsageLedger it uses
/// must outlive them (they are cancelled when the HttpClient is destroyed).
class DataClient {
public:
    /// @brief Uses @p http; @p cache and @p ledger may be null (no cache / no quota records).
    /// @p clock returns the current UTC time in Unix seconds (injectable for tests).
    DataClient(HttpClient& http, DiskCache* cache, UsageLedger* ledger,
               std::function<std::int64_t()> clock = {});

    /// @brief Sets (or clears, with an empty string) the API key of @p providerKey. Keys are kept
    /// in memory only (the app layer loads them from the OS keyring, FR-NET-07).
    void setApiKey(std::string_view providerKey, std::string key);
    /// @brief True if a key is set for @p providerKey.
    [[nodiscard]] bool hasApiKey(std::string_view providerKey) const;

    /// @brief Fetches one bounding box. Returns 0 if the request was answered at once.
    RequestId fetch(const ProviderConfig& provider, const BBoxRequest& request, double priority,
                    PayloadCallback callback);
    /// @brief The cached payload of one bounding box, without any request (fresh or stale; the
    /// cache checks its SHA-256). nullopt if it is not cached.
    [[nodiscard]] NetResult<std::optional<Payload>> cached(const ProviderConfig& provider,
                                                           const BBoxRequest& request);
    /// @brief Cancels a fetch.
    void cancel(RequestId id) { http_->cancel(id); }

    /// @brief Plans the requests for @p area and counts those not cached yet.
    [[nodiscard]] NetResult<DownloadEstimate> estimateDownload(const ProviderConfig& provider,
                                                               std::string_view dataset,
                                                               const GeoRect& area);
    /// @brief Downloads and pins @p area (FR-TER-O09, FR-NET-05). Fails at once with
    /// QuotaExceeded if the uncached requests need more calls than remain.
    /// @param provider Provider to download from.
    /// @param dataset Dataset id; empty ⇒ provider default.
    /// @param area Area to download (EPSG:4326).
    /// @param onProgress Called after each request (network thread).
    /// @param done Receives the cache keys of all pieces, or the first error.
    [[nodiscard]] NetResult<std::shared_ptr<DownloadJob>> downloadArea(
        const ProviderConfig& provider, std::string_view dataset, const GeoRect& area,
        std::function<void(const DownloadProgress&)> onProgress,
        std::move_only_function<void(NetResult<std::vector<std::string>>)> done);

    /// @brief Current budget of a quota-limited provider (nullopt if it has no limit).
    [[nodiscard]] NetResult<std::optional<QuotaStatus>> quota(const ProviderConfig& provider);

    /// @brief Cache key of a request: the request URL without credentials.
    [[nodiscard]] static NetResult<std::string> cacheKey(const ProviderConfig& provider,
                                                         const DatasetConfig& dataset,
                                                         const GeoRect& area);

    /// @brief The underlying HTTP client.
    [[nodiscard]] HttpClient& http() noexcept { return *http_; }

private:
    [[nodiscard]] std::string apiKey(std::string_view providerKey) const;

    HttpClient* http_;
    DiskCache* cache_;
    UsageLedger* ledger_;
    std::function<std::int64_t()> clock_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string, std::less<>> keys_;  // Guarded by mutex_.
};

/// @brief Cheap structural check of a payload (FR-TER-O15): non-empty, and a TIFF/BigTIFF
/// signature for TIFF content. Full decoding checks happen in `core/terrain` (GDAL).
[[nodiscard]] NetResult<void> checkPayload(std::string_view body, std::string_view format);

}  // namespace ds::net
