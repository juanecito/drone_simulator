// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/data_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

#include "ds/common/log.hpp"
#include "ds/common/sha256.hpp"
#include "ds/net/block_planner.hpp"
#include "ds/net/url_template.hpp"

namespace ds::net {
namespace {

std::int64_t systemNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

NetResult<std::string> buildUrl(const ProviderConfig& p, const DatasetConfig& d,
                                const GeoRect& area, std::string_view key) {
    const std::string& tmpl = p.templateFor(d);
    std::map<std::string, std::string, std::less<>> values{
        {"dataset", d.id},
        {"south", std::format("{:.7f}", area.south)},
        {"north", std::format("{:.7f}", area.north)},
        {"west", std::format("{:.7f}", area.west)},
        {"east", std::format("{:.7f}", area.east)},
        {"bbox", area.canonical()},
        {"crs", p.crs},
        {"format", p.format},
    };
    const double resKm = d.resolutionM / 1000.0;
    values["width"] = std::format("{}", static_cast<long>(std::ceil(area.widthKm() / resKm)));
    values["height"] = std::format("{}", static_cast<long>(std::ceil(area.heightKm() / resKm)));
    const bool keyInTemplate = tmpl.find("{apiKey}") != std::string::npos;
    if (keyInTemplate) {
        values["apiKey"] = std::string(key);
    }
    auto url = expandUrlTemplate(tmpl, values);
    if (!url) {
        return url;
    }
    if (!keyInTemplate && !key.empty() && p.apiKey.location == ApiKeyPlacement::Location::Query) {
        return appendQueryParameter(*url, p.apiKey.name, key);
    }
    return url;
}

std::optional<std::int64_t> expiry(const ProviderConfig& p, const HttpResponse* r,
                                   std::int64_t now) {
    std::optional<std::int64_t> e;
    if (p.maxAgeDays) {
        e = now + (static_cast<std::int64_t>(*p.maxAgeDays) * 86400);
    }
    if (r != nullptr && r->maxAgeS) {
        e = e ? std::min(*e, now + *r->maxAgeS) : now + *r->maxAgeS;
    }
    return e;
}

Payload fromCached(CachedPayload c) {
    return Payload{
        std::move(c.body), c.entry.contentType, c.entry.contentHash, c.entry.key, true, c.stale};
}

}  // namespace

// --- DownloadJob --------------------------------------------------------------------------------

void DownloadJob::cancel() {
    cancelled_.store(true);
    std::vector<RequestId> ids;
    {
        const std::scoped_lock lock(mutex_);
        ids = requests_;
    }
    for (const RequestId id : ids) {
        if (id != 0 && cancelRequest_) {
            cancelRequest_(id);
        }
    }
}

DownloadProgress DownloadJob::progress() const {
    const std::scoped_lock lock(mutex_);
    return progress_;
}

// --- DataClient ---------------------------------------------------------------------------------

DataClient::DataClient(HttpClient& http, DiskCache* cache, UsageLedger* ledger,
                       std::function<std::int64_t()> clock)
    : http_(&http), cache_(cache), ledger_(ledger), clock_(clock ? std::move(clock) : systemNow) {}

void DataClient::setApiKey(std::string_view providerKey, std::string key) {
    const std::scoped_lock lock(mutex_);
    if (key.empty()) {
        keys_.erase(std::string(providerKey));
    } else {
        keys_[std::string(providerKey)] = std::move(key);
    }
}

bool DataClient::hasApiKey(std::string_view providerKey) const {
    return !apiKey(providerKey).empty();
}

std::string DataClient::apiKey(std::string_view providerKey) const {
    const std::scoped_lock lock(mutex_);
    const auto it = keys_.find(providerKey);
    return it == keys_.end() ? std::string{} : it->second;
}

NetResult<std::string> DataClient::cacheKey(const ProviderConfig& provider,
                                            const DatasetConfig& dataset, const GeoRect& area) {
    return buildUrl(provider, dataset, area, {});
}

NetResult<std::optional<QuotaStatus>> DataClient::quota(const ProviderConfig& provider) {
    if (!provider.dailyCallLimit || ledger_ == nullptr) {
        return std::optional<QuotaStatus>{};
    }
    auto s = quotaStatus(*ledger_, provider.key, *provider.dailyCallLimit, clock_());
    if (!s) {
        return netError(NetErrorCode::Cache, "usage ledger: " + s.error().message);
    }
    return std::optional{*s};
}

RequestId DataClient::fetch(const ProviderConfig& provider, const BBoxRequest& request,
                            double priority, PayloadCallback callback) {
    const DatasetConfig* dataset =
        provider.dataset(request.dataset.empty() ? provider.defaultDataset : request.dataset);
    if (dataset == nullptr) {
        callback(
            netError(NetErrorCode::InvalidRequest,
                     std::format("unknown dataset \"{}\" for {}", request.dataset, provider.name)));
        return 0;
    }
    if (!request.area.valid()) {
        callback(netError(NetErrorCode::InvalidRequest, "invalid area"));
        return 0;
    }
    auto key = cacheKey(provider, *dataset, request.area);
    if (!key) {
        callback(std::unexpected(key.error()));
        return 0;
    }
    const std::int64_t now = clock_();

    std::optional<CachedPayload> cached;
    if (cache_ != nullptr && provider.allowCache) {
        auto got = cache_->get(provider.key, *key, now);
        if (!got) {
            log::warn("net: {}", got.error().message);
        } else if (*got) {
            if (!(*got)->stale) {
                callback(fromCached(std::move(**got)));
                return 0;
            }
            cached = std::move(**got);
        }
    }
    auto fallback = [&](NetError error) {
        if (cached) {
            callback(fromCached(std::move(*cached)));
        } else {
            callback(std::unexpected(std::move(error)));
        }
    };
    if (http_->offline()) {
        fallback(
            NetError{NetErrorCode::Offline,
                     std::format("offline: {} data for this area is not cached", provider.name)});
        return 0;
    }
    auto budget = quota(provider);
    if (budget && *budget && (*budget)->exhausted()) {
        fallback(NetError{NetErrorCode::QuotaExceeded,
                          std::format("the daily limit of {} calls to {} is used up; using cached "
                                      "data only",
                                      (*budget)->limit, provider.name)});
        return 0;
    }
    const std::string secret = apiKey(provider.key);
    if (provider.requiresKey && secret.empty()) {
        fallback(NetError{
            NetErrorCode::MissingKey,
            std::format("{} needs an API key (Settings → Data providers)", provider.name)});
        return 0;
    }
    auto url = buildUrl(provider, *dataset, request.area, secret);
    if (!url) {
        callback(std::unexpected(url.error()));
        return 0;
    }

    HttpRequest http;
    http.url = std::move(*url);
    if (!secret.empty()) {
        http.secrets.push_back(secret);
        if (provider.apiKey.location == ApiKeyPlacement::Location::Header &&
            provider.templateFor(*dataset).find("{apiKey}") == std::string::npos) {
            http.headers.push_back(std::format("{}: {}", provider.apiKey.name, secret));
        }
    }
    if (cached && !cached->entry.etag.empty()) {
        http.headers.push_back("If-None-Match: " + cached->entry.etag);
    } else if (cached && !cached->entry.lastModified.empty()) {
        http.headers.push_back("If-Modified-Since: " + cached->entry.lastModified);
    }
    http.group = provider.key;
    http.priority = priority;
    http.allowInsecureHttp = provider.insecureHttp;
    http.maxBytes = provider.maxResponseBytes;
    http.contentTypes = provider.contentTypes;
    if (provider.dailyCallLimit) {
        // Retries spend scarce calls: at most one retry for quota-limited providers.
        http.retry = RetryPolicy{.maxAttempts = 2};
    }
    http.isQuotaError = [patterns = provider.quotaErrorPatterns](long status,
                                                                 std::string_view body) {
        if (status < 400) {
            return false;
        }
        const std::string text = lower(body);
        return std::ranges::any_of(patterns, [&](const std::string& p) {
            return text.find(lower(p)) != std::string::npos;
        });
    };
    if (ledger_ != nullptr) {
        http.onAttempt = [ledger = ledger_, clock = clock_, providerKey = provider.key,
                          datasetId = dataset->id, area = request.area](const AttemptInfo& a) {
            if (auto ok = ledger->record(
                    ProviderCall{providerKey, clock(), datasetId, area, a.httpStatus, a.bytes});
                !ok) {
                log::warn("net: cannot record provider call: {}", ok.error().message);
            }
        };
    }
    http_->setGroupLimits(provider.key, provider.maxConcurrent, provider.rateLimitRps);

    auto onDone = [cache = cache_, clock = clock_, provider = provider, key = *key,
                   format = provider.format, secrets = http.secrets, stale = std::move(cached),
                   callback = std::move(callback)](NetResult<HttpResponse> r) mutable {
        const std::int64_t t = clock();
        if (r && r->status == 304 && stale) {
            if (cache != nullptr) {
                (void)cache->revalidated(provider.key, key, expiry(provider, &*r, t), t);
            }
            stale->stale = false;
            callback(fromCached(std::move(*stale)));
            return;
        }
        if (!r) {
            const NetErrorCode c = r.error().code;
            if (stale && c != NetErrorCode::Cancelled && c != NetErrorCode::InvalidRequest) {
                log::info("net: {}; using cached data", r.error().message);
                callback(fromCached(std::move(*stale)));
            } else {
                callback(std::unexpected(std::move(r.error())));
            }
            return;
        }
        if (auto ok = checkPayload(r->body, format); !ok) {
            NetError e = ok.error();
            e.message = redact(e.message, secrets);
            callback(std::unexpected(std::move(e)));
            return;
        }
        Payload p{std::move(r->body), r->contentType, {}, key, false, false};
        p.contentHash = common::sha256Hex(p.body);
        if (cache != nullptr && provider.allowCache && !r->noStore) {
            PayloadMeta meta{r->contentType, r->etag, r->lastModified, expiry(provider, &*r, t)};
            if (auto stored = cache->put(provider.key, key, p.body, meta, t); !stored) {
                log::warn("net: {}", stored.error().message);
            }
        }
        callback(std::move(p));
    };
    return http_->submit(std::move(http), std::move(onDone));
}

NetResult<std::optional<Payload>> DataClient::cached(const ProviderConfig& provider,
                                                     const BBoxRequest& request) {
    const DatasetConfig* dataset =
        provider.dataset(request.dataset.empty() ? provider.defaultDataset : request.dataset);
    if (dataset == nullptr) {
        return netError(NetErrorCode::InvalidRequest, std::format("unknown dataset \"{}\" for {}",
                                                                  request.dataset, provider.name));
    }
    if (cache_ == nullptr || !provider.allowCache) {
        return std::optional<Payload>{};
    }
    auto key = cacheKey(provider, *dataset, request.area);
    if (!key) {
        return std::unexpected(key.error());
    }
    auto got = cache_->get(provider.key, *key, clock_());
    if (!got) {
        return std::unexpected(got.error());
    }
    if (!*got) {
        return std::optional<Payload>{};
    }
    return std::optional{fromCached(std::move(**got))};
}

NetResult<DownloadEstimate> DataClient::estimateDownload(const ProviderConfig& provider,
                                                         std::string_view datasetId,
                                                         const GeoRect& area) {
    const DatasetConfig* dataset =
        provider.dataset(datasetId.empty() ? provider.defaultDataset : datasetId);
    if (dataset == nullptr) {
        return netError(NetErrorCode::InvalidRequest,
                        std::format("unknown dataset \"{}\"", datasetId));
    }
    auto plan = planExtentRequests(area, provider, *dataset);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    DownloadEstimate e;
    e.requests = std::move(*plan);
    for (const GeoRect& r : e.requests) {
        bool cached = false;
        if (cache_ != nullptr) {
            auto key = cacheKey(provider, *dataset, r);
            auto got = key ? cache_->get(provider.key, *key, clock_())
                           : NetResult<std::optional<CachedPayload>>{};
            cached = got && *got && !(*got)->stale;
        }
        if (!cached) {
            e.uncachedRequests += 1;
            e.estimatedBytes += estimateResponseBytes(r, *dataset);
        }
    }
    auto q = quota(provider);
    if (!q) {
        return std::unexpected(q.error());
    }
    e.quota = *q;
    return e;
}

NetResult<std::shared_ptr<DownloadJob>> DataClient::downloadArea(
    const ProviderConfig& provider, std::string_view datasetId, const GeoRect& area,
    std::function<void(const DownloadProgress&)> onProgress,
    std::move_only_function<void(NetResult<std::vector<std::string>>)> done) {
    auto estimate = estimateDownload(provider, datasetId, area);
    if (!estimate) {
        return std::unexpected(estimate.error());
    }
    if (estimate->quota && estimate->uncachedRequests > estimate->quota->remaining) {
        return netError(NetErrorCode::QuotaExceeded,
                        std::format("{}: {}", provider.name,
                                    quotaNotice(*estimate->quota, estimate->uncachedRequests)));
    }
    auto job = std::make_shared<DownloadJob>();
    const auto total = static_cast<int>(estimate->requests.size());
    job->progress_.requestsTotal = total;
    job->cacheKeys_.resize(estimate->requests.size());
    job->requests_.resize(estimate->requests.size(), 0);
    job->done_ = std::move(done);
    job->onProgress_ = std::move(onProgress);
    job->cancelRequest_ = [http = http_](RequestId id) { http->cancel(id); };
    const std::string dataset(datasetId.empty() ? provider.defaultDataset : datasetId);

    for (std::size_t i = 0; i < estimate->requests.size(); ++i) {
        const GeoRect piece = estimate->requests[i];
        auto onPiece = [cache = cache_, job, i, providerKey = provider.key](NetResult<Payload> r) {
            bool last = false;
            DownloadProgress snapshot;
            {
                const std::scoped_lock lock(job->mutex_);
                if (r) {
                    job->cacheKeys_[i] = r->cacheKey;
                    job->progress_.bytes += r->body.size();
                    if (cache != nullptr) {
                        (void)cache->setPinned(providerKey, r->cacheKey, true);
                    }
                } else if (!job->firstError_) {
                    job->firstError_ = r.error();
                }
                job->progress_.requestsDone += 1;
                snapshot = job->progress_;
                last = job->progress_.requestsDone == job->progress_.requestsTotal;
            }
            if (job->onProgress_) {
                job->onProgress_(snapshot);
            }
            if (last) {
                if (job->cancelled_.load()) {
                    job->done_(netError(NetErrorCode::Cancelled, "download cancelled"));
                } else if (job->firstError_) {
                    job->done_(std::unexpected(*job->firstError_));
                } else {
                    job->done_(job->cacheKeys_);
                }
                job->finished_.store(true);
            }
        };
        if (job->cancelled_.load()) {
            onPiece(netError(NetErrorCode::Cancelled, "download cancelled"));
            continue;
        }
        const RequestId id = fetch(provider, BBoxRequest{dataset, piece}, 0.0, std::move(onPiece));
        const std::scoped_lock lock(job->mutex_);
        job->requests_[i] = id;
    }
    return job;
}

NetResult<void> checkPayload(std::string_view body, std::string_view format) {
    if (body.empty()) {
        return netError(NetErrorCode::InvalidPayload, "empty response");
    }
    if (lower(format) == "gtiff") {
        const bool tiff = body.size() >= 4 && (body.starts_with(std::string_view("II*\0", 4)) ||
                                               body.starts_with(std::string_view("MM\0*", 4)) ||
                                               body.starts_with(std::string_view("II+\0", 4)) ||
                                               body.starts_with(std::string_view("MM\0+", 4)));
        if (!tiff) {
            const std::string_view head = body.substr(0, std::min<std::size_t>(body.size(), 120));
            const bool printable = std::ranges::all_of(head, [](char c) {
                return std::isprint(static_cast<unsigned char>(c)) != 0 || c == '\n' || c == '\r' ||
                       c == '\t';
            });
            return netError(NetErrorCode::InvalidPayload,
                            printable ? std::format("expected a GeoTIFF, got: {}", head)
                                      : std::string("expected a GeoTIFF"));
        }
    }
    return {};
}

}  // namespace ds::net
