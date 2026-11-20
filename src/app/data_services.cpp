// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/app/data_services.hpp"

#include <QCoreApplication>
#include <QMessageBox>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <future>

#include "ds/app/progress_task.hpp"
#include "ds/common/log.hpp"
#include "ds/storage/data_location.hpp"
#include "ds/storage/migrations.hpp"
#include "ds/storage/provider_store.hpp"
#include "ds/storage/storage_service.hpp"

namespace ds::app {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("ds::app::DataServices", text); }

}  // namespace

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
    d->cache_ = std::make_unique<net::DiskCache>(options.dataDirectory / "cache", *d->cacheIndex_,
                                                 options.cacheLimitBytes);
    d->http_ = std::make_unique<net::HttpClient>(options.http);
    d->client_ = std::make_unique<net::DataClient>(*d->http_, d->cache_.get(), d->ledger_.get());

    // API keys (FR-NET-07): the environment, else the fallback copy in the user database.
    for (const net::ProviderConfig& p : d->providers_) {
        if (!p.requiresKey) {
            continue;
        }
        if (options.keysFromEnvironment) {
            if (const char* key = std::getenv(keyVariable(p.key).c_str());
                key != nullptr && *key != '\0') {
                d->client_->setApiKey(p.key, key);
                continue;
            }
        }
        if (options.userStorage != nullptr) {
            auto stored = options.userStorage
                              ->submit([key = p.key](storage::DbConnection& c) {
                                  return storage::ProviderStore(c).credential(key);
                              })
                              .get();
            if (stored && *stored && !(*stored)->empty()) {
                d->client_->setApiKey(p.key, **stored);
            }
        }
    }
    log::info("Data providers: {} ({} elevation)", d->providers_.size(),
              d->elevationProviders().size());
    return d;
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

void DataServices::setApiKey(std::string_view providerKey, std::string key) {
    client_->setApiKey(providerKey, std::move(key));
}

bool DataServices::hasApiKey(std::string_view providerKey) const {
    return client_->hasApiKey(providerKey);
}

std::optional<net::QuotaStatus> DataServices::quota(const net::ProviderConfig& provider) {
    auto q = client_->quota(provider);
    if (!q) {
        log::warn("Quota of {}: {}", provider.name, q.error().message);
        return std::nullopt;
    }
    return *q;
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
            "{} needs an API key: set {} (Settings › Data providers comes in the next version)",
            provider.name, DataServices::keyVariable(provider.key)));
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
