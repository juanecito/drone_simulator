// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/net/data_client.hpp"
#include "ds/net/disk_cache.hpp"
#include "ds/net/http_client.hpp"
#include "ds/net/provider.hpp"
#include "ds/net/quota.hpp"
#include "ds/terrain/online_terrain.hpp"

class QWidget;

namespace ds::storage {
class StorageService;
}

/// @file
/// @brief The application's data services (§4.6): providers, HTTP client, disk cache, call
/// ledger and API keys, and the download of online terrain before use (FR-TER-O09).
/// @ingroup app

namespace ds::app {

/// @brief How the data services are set up.
struct DataServicesOptions {
    /// Folder holding `cache/` and `cache.db` (normally `~/.drone_simulation`).
    std::filesystem::path dataDirectory;
    /// Built-in provider presets (`assets/providers`).
    std::filesystem::path providerPresets;
    /// User database: providers table, call ledger, fallback API keys. Null ⇒ presets only,
    /// calls counted in memory.
    storage::StorageService* userStorage = nullptr;
    net::HttpClientOptions http;                              ///< HTTP client settings.
    std::uint64_t cacheLimitBytes = std::uint64_t{5} << 30U;  ///< FR-NET-05 (5 GB).
    /// Read API keys from `DS_API_KEY_<PROVIDER>` environment variables (FR-NET-07 until the
    /// keyring of Settings › Data providers arrives).
    bool keysFromEnvironment = true;
};

/// @brief A download the user is asked to confirm (FR-TER-O17).
struct DownloadRequest {
    std::string providerName;        ///< e.g. "OpenTopography".
    std::string datasetName;         ///< e.g. "Copernicus GLO-30 (30 m)".
    net::GeoRect area;               ///< Requested area.
    net::DownloadEstimate estimate;  ///< Requests, size and quota.
};

/// @brief Providers, network client, cache and ledger of the application. Thread-safe where
/// the underlying net classes are; providers are read once at start-up.
class DataServices {
public:
    /// @brief Asks whether to make a download; false cancels it.
    using ConfirmDownload = std::function<bool(QWidget* parent, const DownloadRequest& request)>;

    /// @brief Opens the cache index (`cache.db`, in memory if it cannot be used), syncs the
    /// built-in providers into the user database and loads the API keys.
    [[nodiscard]] static common::Result<std::unique_ptr<DataServices>> create(
        const DataServicesOptions& options);
    ~DataServices();
    DataServices(const DataServices&) = delete;
    DataServices& operator=(const DataServices&) = delete;
    DataServices(DataServices&&) = delete;
    DataServices& operator=(DataServices&&) = delete;

    /// @brief Every provider (built-in and user-defined), sorted by name.
    [[nodiscard]] const std::vector<net::ProviderConfig>& providers() const noexcept {
        return providers_;
    }
    /// @brief The elevation providers with bounding-box requests (the ones the terrain uses).
    [[nodiscard]] std::vector<const net::ProviderConfig*> elevationProviders() const;
    /// @brief The provider with @p key, or nullptr.
    [[nodiscard]] const net::ProviderConfig* provider(std::string_view key) const noexcept;

    /// @brief The data client (cache, quota and offline rules).
    [[nodiscard]] net::DataClient& client() noexcept { return *client_; }
    /// @brief The HTTP client (offline mode).
    [[nodiscard]] net::HttpClient& http() noexcept { return *http_; }

    /// @brief Sets the API key of @p providerKey for this session (memory only).
    void setApiKey(std::string_view providerKey, std::string key);
    /// @brief True if a key is set for @p providerKey.
    [[nodiscard]] bool hasApiKey(std::string_view providerKey) const;
    /// @brief Daily budget of @p provider (nullopt if it has none or it cannot be read).
    [[nodiscard]] std::optional<net::QuotaStatus> quota(const net::ProviderConfig& provider);

    /// @brief Replaces the confirmation question (tests answer it themselves).
    void setConfirmDownload(ConfirmDownload confirm) { confirm_ = std::move(confirm); }
    /// @brief Asks the user (a message box unless replaced).
    [[nodiscard]] bool confirmDownload(QWidget* parent, const DownloadRequest& request) const;

    /// @brief Environment variable holding the API key of @p providerKey:
    /// `DS_API_KEY_` + the key in capitals, other characters as `_`.
    [[nodiscard]] static std::string keyVariable(std::string_view providerKey);

private:
    DataServices() = default;

    std::unique_ptr<storage::StorageService> cacheStorage_;
    std::unique_ptr<net::CacheIndex> cacheIndex_;
    std::unique_ptr<net::DiskCache> cache_;
    std::unique_ptr<net::UsageLedger> ledger_;
    std::unique_ptr<net::HttpClient> http_;
    std::unique_ptr<net::DataClient> client_;
    std::vector<net::ProviderConfig> providers_;
    ConfirmDownload confirm_;
};

/// @brief Makes sure the data of an online terrain is cached before it is used (FR-TER-O09):
/// asks before spending provider calls (FR-TER-O17), then downloads with a progress dialog
/// that can be cancelled. The downloaded pieces are pinned in the cache (FR-NET-05).
/// @return true when everything is cached, false if the user declined or cancelled; an error
///         for a missing key, a used-up quota, offline mode or a failed download.
[[nodiscard]] common::Result<bool> ensureOnlineData(QWidget* parent, DataServices& data,
                                                    const terrain::OnlineTerrainRequest& request);

/// @brief Text of the confirmation question for @p request.
[[nodiscard]] std::string downloadQuestion(const DownloadRequest& request);

}  // namespace ds::app
