// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
#include "ds/storage/provider_store.hpp"
#include "ds/terrain/online_terrain.hpp"

class QWidget;

namespace ds::storage {
class StorageService;
}

/// @file
/// @brief The application's data services (§4.6): providers, HTTP client, disk cache, call
/// ledger and API keys (OS keyring), and the download of online terrain before use
/// (FR-TER-O09).
/// @ingroup app

namespace ds::app {

class Keyring;

/// @brief How the data services are set up.
struct DataServicesOptions {
    /// Default cache size limit (FR-NET-05).
    static constexpr std::uint64_t kDefaultCacheLimitBytes = std::uint64_t{5} << 30U;

    /// Folder holding `cache/` and `cache.db` (normally `~/.drone_simulation`).
    std::filesystem::path dataDirectory;
    /// Built-in provider presets (`assets/providers`).
    std::filesystem::path providerPresets;
    /// User database: providers table, call ledger, network settings, fallback API keys.
    /// Null ⇒ presets only, calls counted in memory, nothing saved.
    storage::StorageService* userStorage = nullptr;
    /// HTTP client settings; the proxy saved in Settings › Cache replaces an empty `proxy`.
    net::HttpClientOptions http;
    /// Cache size limit; the limit saved in Settings › Cache replaces it.
    std::uint64_t cacheLimitBytes = kDefaultCacheLimitBytes;
    /// OS keyring for the API keys (FR-NET-07). Null ⇒ none: keys are stored in the user
    /// database (with a warning), or for the session only without one.
    std::shared_ptr<Keyring> keyring;
    /// Read API keys from `DS_API_KEY_<PROVIDER>` environment variables first (they win over
    /// stored keys and cannot be changed in the UI).
    bool keysFromEnvironment = true;
};

/// @brief A download the user is asked to confirm (FR-TER-O17).
struct DownloadRequest {
    std::string providerName;        ///< e.g. "OpenTopography".
    std::string datasetName;         ///< e.g. "Copernicus GLO-30 (30 m)".
    net::GeoRect area;               ///< Requested area.
    net::DownloadEstimate estimate;  ///< Requests, size and quota.
};

/// @brief Where the API key of a provider comes from.
enum class KeyLocation {
    None,         ///< No key.
    Environment,  ///< `DS_API_KEY_<PROVIDER>`.
    Keyring,      ///< The OS keyring.
    Database,     ///< The fallback copy in the user database (file mode 0600).
    Session,      ///< Memory only, lost at exit (no keyring and no database).
};

/// @brief Usage of a provider (Settings › Data providers, FR-NET-09).
struct ProviderUsageInfo {
    std::optional<net::QuotaStatus> quota;  ///< Daily budget, for quota-limited providers.
    storage::ProviderUsage last24h;         ///< Calls, errors and bytes over the last 24 h.
    net::ClientStats session;               ///< Cache hits and requests this session.
    net::CacheUsage cache;                  ///< Cached data.
    bool circuitOpen = false;               ///< Requests are paused after repeated failures.
};

/// @brief Outcome of *Test connection* (FR-NET-11).
struct ConnectionTest {
    bool ok = false;                       ///< A valid elevation raster came back.
    std::string message;                   ///< What happened, for the user.
    std::chrono::milliseconds latency{0};  ///< From the request to the answer.
    std::uint64_t bytes = 0;               ///< Response size.
    net::GeoRect area;                     ///< Requested bounding box.
};

/// @brief Providers, network client, cache and ledger of the application.
/// @details The net classes are thread-safe; the provider list, keys and settings are changed
/// on the GUI thread only (Settings › Data providers and Cache).
class DataServices final : public QObject {
    Q_OBJECT

public:
    /// @brief Asks whether to make a download; false cancels it.
    using ConfirmDownload = std::function<bool(QWidget* parent, const DownloadRequest& request)>;

    /// @brief Opens the cache index (`cache.db`, in memory if it cannot be used), syncs the
    /// built-in providers into the user database, reads the network settings and loads the
    /// API keys (environment, then keyring, then the database copy).
    [[nodiscard]] static common::Result<std::unique_ptr<DataServices>> create(
        const DataServicesOptions& options);
    ~DataServices() override;
    DataServices(const DataServices&) = delete;
    DataServices& operator=(const DataServices&) = delete;
    DataServices(DataServices&&) = delete;
    DataServices& operator=(DataServices&&) = delete;

    // --- Providers -------------------------------------------------------------------------

    /// @brief Every provider (built-in and user-defined), sorted by name.
    [[nodiscard]] const std::vector<net::ProviderConfig>& providers() const noexcept {
        return providers_;
    }
    /// @brief The elevation providers with bounding-box requests (the ones the terrain uses).
    [[nodiscard]] std::vector<const net::ProviderConfig*> elevationProviders() const;
    /// @brief The provider with @p key, or nullptr.
    [[nodiscard]] const net::ProviderConfig* provider(std::string_view key) const noexcept;
    /// @brief Adds or replaces the user-defined provider @p provider (FR-TER-O02), after
    /// validating it like a provider file; built-in keys are refused. Saved in the user
    /// database (session only without one). Emits providersChanged().
    common::Result<void> saveProvider(net::ProviderConfig provider);
    /// @brief Removes the user-defined provider @p key, its stored API key and, if
    /// @p deleteCache, its cached data. Emits providersChanged().
    common::Result<void> removeProvider(std::string_view key, bool deleteCache);

    // --- Network ---------------------------------------------------------------------------

    /// @brief The data client (cache, quota and offline rules).
    [[nodiscard]] net::DataClient& client() noexcept { return *client_; }
    /// @brief The HTTP client (offline mode).
    [[nodiscard]] net::HttpClient& http() noexcept { return *http_; }

    // --- API keys (FR-NET-07) --------------------------------------------------------------

    /// @brief Sets the API key of @p providerKey for this session (memory only; tests).
    void setApiKey(std::string_view providerKey, std::string key);
    /// @brief Stores the API key of @p providerKey: in the OS keyring, else in the user
    /// database (keyringProblem() says why), else for the session. An empty @p key forgets
    /// the stored key. A key from the environment cannot be changed. Emits apiKeysChanged().
    common::Result<KeyLocation> storeApiKey(std::string_view providerKey, std::string_view key);
    /// @brief True if a key is set for @p providerKey.
    [[nodiscard]] bool hasApiKey(std::string_view providerKey) const;
    /// @brief Where the key of @p providerKey comes from.
    [[nodiscard]] KeyLocation keyLocation(std::string_view providerKey) const;
    /// @brief Why the OS keyring could not be used (empty if it works or none was tried).
    [[nodiscard]] const std::string& keyringProblem() const noexcept { return keyringProblem_; }
    /// @brief True if an OS keyring is configured (it may still fail, see keyringProblem()).
    [[nodiscard]] bool hasKeyring() const noexcept { return keyring_ != nullptr; }

    // --- Usage and tests -------------------------------------------------------------------

    /// @brief Daily budget of @p provider (nullopt if it has none or it cannot be read).
    [[nodiscard]] std::optional<net::QuotaStatus> quota(const net::ProviderConfig& provider);
    /// @brief Usage of @p provider (FR-NET-09).
    [[nodiscard]] ProviderUsageInfo usage(const net::ProviderConfig& provider);
    /// @brief *Test connection* (FR-NET-11): one uncached request for a ~1 km square inside
    /// the dataset's coverage, decoded as an elevation raster. It is counted against the daily
    /// budget. @p done runs on the GUI thread (not at all if the services are destroyed
    /// first).
    void testConnection(const net::ProviderConfig& provider, const std::string& dataset,
                        std::function<void(const ConnectionTest&)> done);
    /// @brief Bounding box *Test connection* asks for: 0.01° square at the centre of the
    /// dataset's coverage, else in the Alps (46.5° N, 8° E).
    [[nodiscard]] static net::GeoRect testArea(const net::DatasetConfig& dataset);

    // --- Cache and network settings (FR-NET-05, 06, 08) ------------------------------------

    /// @brief Manual offline mode, saved. Emits networkSettingsChanged().
    void setOffline(bool offline);
    /// @brief True in manual offline mode.
    [[nodiscard]] bool offline() const noexcept;
    /// @brief Sets the cache size limit (saved) and evicts if needed.
    common::Result<void> setCacheLimit(std::uint64_t bytes);
    /// @brief Cache size limit.
    [[nodiscard]] std::uint64_t cacheLimit() const;
    /// @brief Cache usage per provider (empty if it cannot be read).
    [[nodiscard]] std::vector<net::CacheUsage> cacheUsage();
    /// @brief Removes the cached data of every provider; pinned areas only if
    /// @p includePinned. Emits networkSettingsChanged().
    common::Result<void> clearCache(bool includePinned);
    /// @brief Unpins every downloaded area so the limit may evict it; returns how many pieces
    /// were pinned. Emits networkSettingsChanged().
    common::Result<int> releasePinnedAreas();
    /// @brief Cache folder.
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept;
    /// @brief Saves the proxy (used from the next start); empty ⇒ the `https_proxy` /
    /// `no_proxy` environment.
    void setProxy(const std::string& proxy);
    /// @brief The saved proxy.
    [[nodiscard]] const std::string& proxy() const noexcept { return proxy_; }

    // --- Downloads -------------------------------------------------------------------------

    /// @brief Replaces the confirmation question (tests answer it themselves).
    void setConfirmDownload(ConfirmDownload confirm) { confirm_ = std::move(confirm); }
    /// @brief Asks the user (a message box unless replaced).
    [[nodiscard]] bool confirmDownload(QWidget* parent, const DownloadRequest& request) const;

    /// @brief Environment variable holding the API key of @p providerKey:
    /// `DS_API_KEY_` + the key in capitals, other characters as `_`.
    [[nodiscard]] static std::string keyVariable(std::string_view providerKey);

signals:
    /// @brief A provider was added, changed or removed.
    void providersChanged();
    /// @brief An API key was stored or forgotten.
    void apiKeysChanged();
    /// @brief Offline mode or the cache limit changed, or the cache was cleared.
    void networkSettingsChanged();

private:
    DataServices() = default;
    void saveSetting(std::string key, std::string value);
    void loadKey(const net::ProviderConfig& provider, bool fromEnvironment);

    storage::StorageService* userStorage_ = nullptr;
    std::shared_ptr<Keyring> keyring_;
    std::string keyringProblem_;
    std::map<std::string, KeyLocation, std::less<>> keyLocations_;
    std::string proxy_;
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

/// @brief "the system keyring", "the user database", … for @p location.
[[nodiscard]] std::string keyLocationName(KeyLocation location);

}  // namespace ds::app
