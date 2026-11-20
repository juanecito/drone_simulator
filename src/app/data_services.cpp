// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/data_services.hpp"

#include <QCoreApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <future>

#include "ds/app/keyring.hpp"
#include "ds/app/progress_task.hpp"
#include "ds/common/log.hpp"
#include "ds/storage/data_location.hpp"
#include "ds/storage/migrations.hpp"
#include "ds/storage/provider_store.hpp"
#include "ds/storage/settings_store.hpp"
#include "ds/storage/storage_service.hpp"
#include "ds/terrain/raster_source.hpp"

namespace ds::app {
namespace {

// Network settings in the user database (Settings › Cache).
constexpr std::string_view kOfflineKey = "net.offline";
constexpr std::string_view kCacheLimitKey = "net.cacheLimitMb";
constexpr std::string_view kProxyKey = "net.proxy";

std::int64_t nowUnixS() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trimmed(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1));
}

struct NetSettings {
    std::optional<bool> offline;
    std::optional<std::int64_t> cacheLimitMb;
    std::string proxy;
};

NetSettings readNetSettings(storage::StorageService& storage) {
    return storage
        .submit([](storage::DbConnection& c) {
            storage::SettingsStore store(c);
            NetSettings n;
            if (auto v = store.string(kOfflineKey); v && *v) {
                n.offline = **v == "true";
            }
            if (auto v = store.integer(kCacheLimitKey); v && *v && **v > 0) {
                n.cacheLimitMb = **v;
            }
            if (auto v = store.string(kProxyKey); v && *v) {
                n.proxy = **v;
            }
            return storage::StorageResult<NetSettings>(std::move(n));
        })
        .get()
        .value_or(NetSettings{});
}

}  // namespace

std::string keyLocationName(KeyLocation location) {
    switch (location) {
        case KeyLocation::None: return "no key";
        case KeyLocation::Environment: return "the environment";
        case KeyLocation::Keyring: return "the system keyring";
        case KeyLocation::Database: return "the user database";
        case KeyLocation::Session: return "this session only";
    }
    return {};
}

std::string DataServices::keyVariable(std::string_view providerKey) {
    std::string name = "DS_API_KEY_";
    for (const char c : providerKey) {
        name += std::isalnum(static_cast<unsigned char>(c)) != 0
                    ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                    : '_';
    }
    return name;
}

common::Result<std::unique_ptr<DataServices>> DataServices::create(
    const DataServicesOptions& options) {
    auto presets = net::loadProviderPresets(options.providerPresets);
    if (!presets) {
        return std::unexpected(presets.error());
    }
    std::unique_ptr<DataServices> d(new DataServices());
    d->userStorage_ = options.userStorage;
    d->keyring_ = options.keyring;

    // Providers: built-in presets synced into the user database, plus the user's own.
    d->providers_ = *presets;
    if (options.userStorage != nullptr) {
        auto listed = options.userStorage
                          ->submit([&presets](storage::DbConnection& c) {
                              storage::ProviderStore store(c);
                              if (auto synced = store.syncBuiltin(*presets); !synced) {
                                  return storage::StorageResult<std::vector<net::ProviderConfig>>(
                                      std::unexpected(synced.error()));
                              }
                              return store.list();
                          })
                          .get();
        if (listed) {
            d->providers_ = std::move(*listed);
        } else {
            log::warn("Data providers: {}", listed.error().message);
        }
        d->ledger_ = std::make_unique<storage::StorageUsageLedger>(*options.userStorage);
    } else {
        d->ledger_ = std::make_unique<net::InMemoryUsageLedger>();
    }
    std::ranges::sort(d->providers_, {}, &net::ProviderConfig::name);

    // Cache index in its own cache.db (FR-NET-04); in memory if it cannot be opened.
    std::error_code ec;
    std::filesystem::create_directories(options.dataDirectory, ec);
    auto cacheDb =
        storage::StorageService::start({.databaseFile = options.dataDirectory / "cache.db",
                                        .migrations = storage::cacheSchemaMigrations()});
    if (cacheDb) {
        (void)storage::ensurePrivateFile(options.dataDirectory / "cache.db");
        d->cacheStorage_ = std::move(*cacheDb);
        d->cacheIndex_ = std::make_unique<storage::StorageCacheIndex>(*d->cacheStorage_);
    } else {
        log::warn("Tile cache index in memory only: {}", cacheDb.error().message);
        d->cacheIndex_ = std::make_unique<net::InMemoryCacheIndex>();
    }
    // Network settings saved in Settings › Cache (FR-NET-05, 06, 08).
    const NetSettings saved =
        options.userStorage != nullptr ? readNetSettings(*options.userStorage) : NetSettings{};
    const std::uint64_t limit = saved.cacheLimitMb
                                    ? static_cast<std::uint64_t>(*saved.cacheLimitMb) << 20U
                                    : options.cacheLimitBytes;
    d->cache_ =
        std::make_unique<net::DiskCache>(options.dataDirectory / "cache", *d->cacheIndex_, limit);
    net::HttpClientOptions http = options.http;
    d->proxy_ = saved.proxy;
    if (http.proxy.empty()) {
        http.proxy = saved.proxy;
    }
    d->http_ = std::make_unique<net::HttpClient>(std::move(http));
    d->http_->setOffline(saved.offline.value_or(false));
    d->client_ = std::make_unique<net::DataClient>(*d->http_, d->cache_.get(), d->ledger_.get());

    // API keys (FR-NET-07): the environment, else the OS keyring, else the database copy.
    for (const net::ProviderConfig& p : d->providers_) {
        if (p.requiresKey) {
            d->loadKey(p, options.keysFromEnvironment);
        }
    }
    log::info("Data providers: {} ({} elevation)", d->providers_.size(),
              d->elevationProviders().size());
    return d;
}

void DataServices::loadKey(const net::ProviderConfig& p, bool fromEnvironment) {
    keyLocations_.insert_or_assign(p.key, KeyLocation::None);
    if (fromEnvironment) {
        if (const char* key = std::getenv(keyVariable(p.key).c_str());
            key != nullptr && *key != '\0') {
            client_->setApiKey(p.key, trimmed(key));
            keyLocations_.insert_or_assign(p.key, KeyLocation::Environment);
            return;
        }
    }
    // After one failure the keyring is not asked again (each try may wait for a timeout).
    if (keyring_ != nullptr && keyringProblem_.empty()) {
        auto stored = keyring_->read(apiKeyEntry(p.key));
        if (!stored) {
            keyringProblem_ = stored.error().message;
            log::warn("API keys: {}", keyringProblem_);
        } else if (*stored && !(*stored)->empty()) {
            client_->setApiKey(p.key, **stored);
            keyLocations_.insert_or_assign(p.key, KeyLocation::Keyring);
            return;
        }
    }
    if (userStorage_ != nullptr) {
        auto stored = userStorage_
                          ->submit([key = p.key](storage::DbConnection& c) {
                              return storage::ProviderStore(c).credential(key);
                          })
                          .get();
        if (stored && *stored && !(*stored)->empty()) {
            client_->setApiKey(p.key, **stored);
            keyLocations_.insert_or_assign(p.key, KeyLocation::Database);
        }
    }
}

DataServices::~DataServices() {
    // Stop the network first: pending callbacks still use the cache and the ledger.
    client_.reset();
    http_.reset();
}

std::vector<const net::ProviderConfig*> DataServices::elevationProviders() const {
    std::vector<const net::ProviderConfig*> out;
    for (const net::ProviderConfig& p : providers_) {
        if (p.kind == "elevation" && p.protocol == "rest_bbox") {
            out.push_back(&p);
        }
    }
    return out;
}

const net::ProviderConfig* DataServices::provider(std::string_view key) const noexcept {
    const auto it = std::ranges::find(providers_, key, &net::ProviderConfig::key);
    return it == providers_.end() ? nullptr : &*it;
}

common::Result<void> DataServices::saveProvider(net::ProviderConfig provider) {
    provider.key = trimmed(provider.key);
    if (provider.key.empty()) {
        return common::makeError("the provider needs a key");
    }
    if (const net::ProviderConfig* old = this->provider(provider.key);
        old != nullptr && old->origin != "user") {
        return common::makeError(std::format(
            "\"{}\" is a built-in provider; duplicate it to make a provider of your own",
            provider.key));
    }
    provider.origin = "user";
    // The same checks as a provider file (FR-TER-O02).
    common::Json json = net::toJson(provider);
    json["schemaVersion"] = net::kProviderSchemaVersion;
    auto parsed = net::parseProvider(json);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    parsed->origin = "user";
    if (userStorage_ != nullptr) {
        auto saved = userStorage_
                         ->submit([p = *parsed](storage::DbConnection& c) {
                             return storage::ProviderStore(c).saveUser(p);
                         })
                         .get();
        if (!saved) {
            return common::makeError(saved.error().message);
        }
    }
    const auto it = std::ranges::find(providers_, parsed->key, &net::ProviderConfig::key);
    if (it != providers_.end()) {
        *it = *parsed;
    } else {
        providers_.push_back(*parsed);
    }
    std::ranges::sort(providers_, {}, &net::ProviderConfig::name);
    if (parsed->requiresKey && !keyLocations_.contains(parsed->key)) {
        loadKey(*parsed, true);
    }
    log::info("Data provider {} saved", parsed->key);
    emit providersChanged();
    return {};
}

common::Result<void> DataServices::removeProvider(std::string_view key, bool deleteCache) {
    const auto it = std::ranges::find(providers_, key, &net::ProviderConfig::key);
    if (it == providers_.end()) {
        return common::makeError(std::format("unknown data provider \"{}\"", key));
    }
    if (it->origin != "user") {
        return common::makeError("built-in providers cannot be removed");
    }
    const std::string k(key);
    if (userStorage_ != nullptr) {
        auto removed = userStorage_
                           ->submit([k](storage::DbConnection& c) {
                               storage::ProviderStore store(c);
                               (void)store.clearCredential(k);
                               return store.removeUser(k);
                           })
                           .get();
        if (!removed) {
            return common::makeError(removed.error().message);
        }
    }
    if (keyring_ != nullptr && keyringProblem_.empty()) {
        if (auto ok = keyring_->remove(apiKeyEntry(k)); !ok) {
            log::warn("API keys: {}", ok.error().message);
        }
    }
    client_->setApiKey(k, {});
    keyLocations_.erase(k);
    if (deleteCache) {
        if (auto ok = cache_->clear(k, true); !ok) {
            log::warn("Cache of {}: {}", k, ok.error().message);
        }
    }
    providers_.erase(it);
    log::info("Data provider {} removed", k);
    emit providersChanged();
    emit apiKeysChanged();
    return {};
}

void DataServices::setApiKey(std::string_view providerKey, std::string key) {
    keyLocations_.insert_or_assign(std::string(providerKey),
                                   key.empty() ? KeyLocation::None : KeyLocation::Session);
    client_->setApiKey(providerKey, std::move(key));
    emit apiKeysChanged();
}

common::Result<KeyLocation> DataServices::storeApiKey(std::string_view providerKey,
                                                      std::string_view key) {
    if (keyLocation(providerKey) == KeyLocation::Environment) {
        return common::makeError(std::format("the key comes from the environment variable {}",
                                             keyVariable(providerKey)));
    }
    const std::string k(providerKey);
    const std::string value = trimmed(key);
    const std::string entry = apiKeyEntry(k);
    KeyLocation where = KeyLocation::None;
    if (value.empty()) {
        // Forget it everywhere.
        if (keyring_ != nullptr) {
            if (auto ok = keyring_->remove(entry); !ok) {
                keyringProblem_ = ok.error().message;
            }
        }
    } else if (keyring_ != nullptr) {
        if (auto ok = keyring_->write(entry, value); ok) {
            where = KeyLocation::Keyring;
            keyringProblem_.clear();
        } else {
            keyringProblem_ = ok.error().message;
            log::warn("API keys: {}", keyringProblem_);
        }
    }
    if (userStorage_ != nullptr) {
        // The database copy only when the keyring failed: never both (NFR-SEC-02a).
        const bool fallback = !value.empty() && where == KeyLocation::None;
        auto saved =
            userStorage_
                ->submit([k, value, fallback](storage::DbConnection& c) {
                    storage::ProviderStore store(c);
                    return fallback ? store.setCredential(k, value) : store.clearCredential(k);
                })
                .get();
        if (!saved) {
            return common::makeError(saved.error().message);
        }
        if (fallback) {
            where = KeyLocation::Database;
            if (const auto& file = userStorage_->info().databaseFile; !file.empty()) {
                (void)storage::ensurePrivateFile(file);
            }
            log::warn("API key of {} stored in the user database (no keyring available)", k);
        }
    }
    if (!value.empty() && where == KeyLocation::None) {
        where = KeyLocation::Session;
    }
    client_->setApiKey(k, value);
    keyLocations_.insert_or_assign(k, where);
    emit apiKeysChanged();
    return where;
}

bool DataServices::hasApiKey(std::string_view providerKey) const {
    return client_->hasApiKey(providerKey);
}

KeyLocation DataServices::keyLocation(std::string_view providerKey) const {
    const auto it = keyLocations_.find(providerKey);
    if (it != keyLocations_.end()) {
        return it->second;
    }
    return hasApiKey(providerKey) ? KeyLocation::Session : KeyLocation::None;
}

std::optional<net::QuotaStatus> DataServices::quota(const net::ProviderConfig& provider) {
    auto q = client_->quota(provider);
    if (!q) {
        log::warn("Quota of {}: {}", provider.name, q.error().message);
        return std::nullopt;
    }
    return *q;
}

ProviderUsageInfo DataServices::usage(const net::ProviderConfig& provider) {
    ProviderUsageInfo u;
    u.quota = quota(provider);
    const std::int64_t since = nowUnixS() - net::kQuotaWindowS;
    if (userStorage_ != nullptr) {
        auto last = userStorage_
                        ->submit([key = provider.key, since](storage::DbConnection& c) {
                            return storage::ProviderStore(c).usageSince(key, since);
                        })
                        .get();
        if (last) {
            u.last24h = *last;
        }
    } else if (const auto* ledger = dynamic_cast<const net::InMemoryUsageLedger*>(ledger_.get())) {
        for (const net::ProviderCall& c : ledger->calls()) {
            if (c.providerKey == provider.key && c.calledAtUnixS >= since) {
                ++u.last24h.calls;
                u.last24h.errors += c.httpStatus == 0 || c.httpStatus >= 400 ? 1 : 0;
                u.last24h.bytes += c.bytes;
            }
        }
    }
    u.session = client_->stats(provider.key);
    u.cache.provider = provider.key;
    for (const net::CacheUsage& c : cacheUsage()) {
        if (c.provider == provider.key) {
            u.cache = c;
        }
    }
    u.circuitOpen = http_->circuitOpen(provider.key);
    return u;
}

net::GeoRect DataServices::testArea(const net::DatasetConfig& dataset) {
    double lat = 46.5;
    double lon = 8.0;
    if (dataset.coverage) {
        lat = (dataset.coverage->south + dataset.coverage->north) / 2.0;
        lon = (dataset.coverage->west + dataset.coverage->east) / 2.0;
    }
    constexpr double kHalf = 0.005;
    return net::GeoRect{lon - kHalf, lat - kHalf, lon + kHalf, lat + kHalf};
}

void DataServices::testConnection(const net::ProviderConfig& provider, const std::string& dataset,
                                  std::function<void(const ConnectionTest&)> done) {
    // Results are handed to the GUI thread; the QPointer drops them once this is gone.
    auto deliver = [self = QPointer<DataServices>(this),
                    done = std::move(done)](const ConnectionTest& t) mutable {
        QMetaObject::invokeMethod(
            self.data(),
            [self, done = std::move(done), t] {
                if (self) {
                    done(t);
                }
            },
            Qt::QueuedConnection);
    };
    const net::DatasetConfig* d =
        provider.dataset(dataset.empty() ? provider.defaultDataset : dataset);
    if (d == nullptr) {
        deliver(ConnectionTest{.message = std::format("unknown dataset \"{}\"", dataset)});
        return;
    }
    const net::GeoRect area = testArea(*d);
    const auto start = std::chrono::steady_clock::now();
    client_->fetch(
        provider, net::BBoxRequest{.dataset = d->id, .area = area, .noCache = true}, -1.0,
        [deliver = std::move(deliver), start, area,
         label = provider.name](net::NetResult<net::Payload> r) mutable {
            ConnectionTest t;
            t.area = area;
            t.latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (!r) {
                t.message = r.error().message;
                deliver(t);
                return;
            }
            t.bytes = r->body.size();
            const std::array<std::string, 1> pieces{std::move(r->body)};
            auto raster = terrain::RasterSource::openMemory(pieces, {}, label + " test");
            if (!raster) {
                t.message =
                    "the answer is not a usable elevation raster: " + raster.error().message;
            } else {
                const terrain::SourceMetadata& m = (*raster)->metadata();
                t.ok = true;
                t.message = std::format("{} × {} px, heights {:.0f} … {:.0f} m", m.widthPx,
                                        m.heightPx, m.minHeightM, m.maxHeightM);
            }
            deliver(t);
        });
}

void DataServices::saveSetting(std::string key, std::string value) {
    if (userStorage_ == nullptr) {
        return;
    }
    static_cast<void>(userStorage_->submit(
        [key = std::move(key), value = std::move(value)](storage::DbConnection& c) {
            if (auto ok = storage::SettingsStore(c).setString(key, value); !ok) {
                log::error("Cannot save setting '{}': {}", key, ok.error().message);
            }
        }));
}

void DataServices::setOffline(bool offline) {
    if (offline == http_->offline()) {
        return;
    }
    http_->setOffline(offline);
    saveSetting(std::string(kOfflineKey), offline ? "true" : "false");
    log::info("Offline mode {}", offline ? "on" : "off");
    emit networkSettingsChanged();
}

bool DataServices::offline() const noexcept { return http_->offline(); }

common::Result<void> DataServices::setCacheLimit(std::uint64_t bytes) {
    if (auto ok = cache_->setLimit(bytes); !ok) {
        return common::makeError(ok.error().message);
    }
    if (userStorage_ != nullptr) {
        static_cast<void>(userStorage_->submit(
            [mb = static_cast<std::int64_t>(bytes >> 20U)](storage::DbConnection& c) {
                if (auto ok = storage::SettingsStore(c).setInteger(kCacheLimitKey, mb); !ok) {
                    log::error("Cannot save the cache limit: {}", ok.error().message);
                }
            }));
    }
    emit networkSettingsChanged();
    return {};
}

std::uint64_t DataServices::cacheLimit() const { return cache_->limit(); }

std::vector<net::CacheUsage> DataServices::cacheUsage() {
    auto u = cache_->usage();
    if (!u) {
        log::warn("Cache: {}", u.error().message);
        return {};
    }
    return std::move(*u);
}

common::Result<void> DataServices::clearCache(bool includePinned) {
    auto ok = cache_->clear({}, includePinned);
    emit networkSettingsChanged();
    if (!ok) {
        return common::makeError(ok.error().message);
    }
    log::info("Cache cleared{}", includePinned ? ", downloaded areas too" : "");
    return {};
}

common::Result<int> DataServices::releasePinnedAreas() {
    auto released = cache_->releasePinned({});
    emit networkSettingsChanged();
    if (!released) {
        return common::makeError(released.error().message);
    }
    return *released;
}

const std::filesystem::path& DataServices::cacheDirectory() const noexcept {
    return cache_->root();
}

void DataServices::setProxy(const std::string& proxy) {
    proxy_ = trimmed(proxy);
    saveSetting(std::string(kProxyKey), proxy_);
}

std::string downloadQuestion(const DownloadRequest& r) {
    const double mb = static_cast<double>(r.estimate.estimatedBytes) / 1.0e6;
    std::string text = std::format(
        "Download elevation data from {} ({})?\n\n{} request(s), about {:.1f} MB, "
        "for {:.1f} × {:.1f} km.",
        r.providerName, r.datasetName, r.estimate.uncachedRequests, mb, r.area.widthKm(),
        r.area.heightKm());
    if (r.estimate.quota) {
        text += std::format("\nThis {}.",
                            net::quotaNotice(*r.estimate.quota, r.estimate.uncachedRequests));
    }
    text +=
        "\n\nThe requested area is visible to the provider. The data is kept in the cache "
        "for offline use.";
    return text;
}

bool DataServices::confirmDownload(QWidget* parent, const DownloadRequest& request) const {
    if (confirm_) {
        return confirm_(parent, request);
    }
    return QMessageBox::question(parent, tr("Download Terrain Data"),
                                 QString::fromStdString(downloadQuestion(request)),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::Yes) == QMessageBox::Yes;
}

common::Result<bool> ensureOnlineData(QWidget* parent, DataServices& data,
                                      const terrain::OnlineTerrainRequest& request) {
    auto dataset = terrain::onlineDataset(request);
    if (!dataset) {
        return common::makeError(dataset.error().message);
    }
    auto area = terrain::onlineRequiredArea(request);
    if (!area) {
        return common::makeError(area.error().message);
    }
    net::DataClient& client = data.client();
    const net::ProviderConfig& provider = request.provider;
    auto estimate = client.estimateDownload(provider, (*dataset)->id, *area);
    if (!estimate) {
        return common::makeError(estimate.error().message);
    }
    if (estimate->uncachedRequests == 0) {
        return true;
    }
    if (data.http().offline()) {
        return common::makeError(
            std::format("offline: the {} data of this area is not downloaded yet", provider.name));
    }
    if (provider.requiresKey && !data.hasApiKey(provider.key)) {
        return common::makeError(std::format(
            "{} needs an API key: enter it in Settings › Data providers", provider.name));
    }
    if (estimate->quota && estimate->uncachedRequests > estimate->quota->remaining) {
        return common::makeError(
            std::format("{}: the download {}", provider.name,
                        net::quotaNotice(*estimate->quota, estimate->uncachedRequests)));
    }
    const DownloadRequest question{provider.name, (*dataset)->name, *area, *estimate};
    if (!data.confirmDownload(parent, question)) {
        return false;
    }

    using Outcome = net::NetResult<std::vector<std::string>>;
    auto outcome = runWithProgress<Outcome>(
        parent,
        QCoreApplication::translate("ds::app", "Downloading %1 elevation data…")
            .arg(QString::fromStdString(provider.name)),
        [&](std::stop_token stop, const ProgressReport& report) -> Outcome {
            auto promise = std::make_shared<std::promise<Outcome>>();
            std::future<Outcome> done = promise->get_future();
            auto job = client.downloadArea(
                provider, (*dataset)->id, *area,
                [report](const net::DownloadProgress& p) {
                    report(p.requestsTotal > 0 ? static_cast<double>(p.requestsDone) /
                                                     static_cast<double>(p.requestsTotal)
                                               : 1.0);
                },
                [promise](Outcome r) { promise->set_value(std::move(r)); });
            if (!job) {
                return std::unexpected(job.error());
            }
            bool cancelled = false;
            while (done.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready) {
                if (!cancelled && stop.stop_requested()) {
                    (*job)->cancel();
                    cancelled = true;
                }
            }
            return done.get();
        });
    if (!outcome) {
        return false;  // Cancelled.
    }
    if (!*outcome) {
        if (outcome->error().code == net::NetErrorCode::Cancelled) {
            return false;
        }
        return common::makeError(outcome->error().message);
    }
    log::info("Downloaded {} piece(s) of {} {}", (*outcome)->size(), provider.name, (*dataset)->id);
    return true;
}

}  // namespace ds::app
