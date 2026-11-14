// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctime>
#include <format>
#include <random>
#include <set>

#include "ds/common/log.hpp"
#include "ds/common/version.hpp"
#include "ds/net/url_template.hpp"

#include <curl/curl.h>

namespace ds::net {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

void initCurlOnce() {
    static const bool initialised = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)initialised;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) != 0)) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) != 0)) {
        s.remove_suffix(1);
    }
    return s;
}

std::string mediaType(std::string_view contentType) {
    return lower(trim(contentType.substr(0, contentType.find(';'))));
}

/// Response headers of the final response (reset on every new status line).
struct Headers {
    std::string etag;
    std::string lastModified;
    std::optional<milliseconds> retryAfter;
    std::optional<std::int64_t> maxAgeS;
    bool noStore = false;

    void parse(std::string_view line) {
        if (line.starts_with("HTTP/")) {
            *this = Headers{};
            return;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return;
        }
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string_view value = trim(line.substr(colon + 1));
        if (name == "etag") {
            etag = std::string(value);
        } else if (name == "last-modified") {
            lastModified = std::string(value);
        } else if (name == "retry-after") {
            long long seconds = 0;
            const auto* end = value.data() + value.size();
            if (auto [p, ec] = std::from_chars(value.data(), end, seconds);
                ec == std::errc{} && p == end && seconds >= 0) {
                retryAfter = milliseconds(seconds * 1000);
            } else {
                const std::string date(value);
                if (const std::time_t t = curl_getdate(date.c_str(), nullptr); t > 0) {
                    const auto delta = t - std::time(nullptr);
                    retryAfter = milliseconds(std::max<std::int64_t>(0, delta) * 1000);
                }
            }
        } else if (name == "cache-control") {
            const std::string v = lower(value);
            noStore = v.find("no-store") != std::string::npos;
            if (const std::size_t p = v.find("max-age="); p != std::string::npos) {
                std::int64_t age = 0;
                const char* b = v.data() + p + 8;
                if (auto [q, ec] = std::from_chars(b, v.data() + v.size(), age);
                    ec == std::errc{}) {
                    maxAgeS = age;
                }
            }
        }
    }
};

struct Pending {
    RequestId id = 0;
    HttpRequest request;
    HttpCallback callback;
    int attempt = 0;  // Attempts already made.
    Clock::time_point notBefore{};
    std::uint64_t order = 0;
};

struct Active {
    Pending pending;
    CURL* easy = nullptr;
    curl_slist* headerList = nullptr;
    std::string body;
    Headers headers;
    bool tooLarge = false;
};

struct Group {
    int maxConcurrent = 2;
    std::optional<double> rps;
    int active = 0;
    Clock::time_point nextStart{};
    int consecutiveFailures = 0;
    Clock::time_point openUntil{};
};

std::size_t writeBody(char* data, std::size_t size, std::size_t count, void* user) {
    auto* a = static_cast<Active*>(user);
    const std::size_t n = size * count;
    if (a->body.size() + n > a->pending.request.maxBytes) {
        a->tooLarge = true;
        return 0;  // Aborts the transfer (CURLE_WRITE_ERROR).
    }
    a->body.append(data, n);
    return n;
}

std::size_t writeHeader(char* data, std::size_t size, std::size_t count, void* user) {
    auto* a = static_cast<Active*>(user);
    a->headers.parse(std::string_view(data, size * count));
    return size * count;
}

}  // namespace

struct HttpClient::Impl {
    HttpClientOptions options;
    std::atomic<bool>* offline = nullptr;
    std::atomic<bool>* reachable = nullptr;

    mutable std::mutex mutex;
    std::vector<Pending> queue;                        // Guarded by mutex.
    std::set<RequestId> cancelRequests;                // Guarded by mutex.
    std::map<std::string, Group, std::less<>> groups;  // Guarded by mutex.
    RequestId nextId = 1;                              // Guarded by mutex.
    std::uint64_t nextOrder = 0;                       // Guarded by mutex.
    std::size_t activeCount = 0;  // Guarded by mutex (mirror of active.size()).
    bool stopping = false;        // Guarded by mutex.

    CURLM* multi = nullptr;
    std::map<CURL*, std::unique_ptr<Active>> active;  // Network thread only.
    std::mt19937_64 rng{std::random_device{}()};      // Network thread only.
    std::jthread thread;

    struct Completion {
        HttpCallback callback;
        NetResult<HttpResponse> result;
    };

    Group& groupLocked(const std::string& name) {
        auto it = groups.find(name);
        if (it == groups.end()) {
            it = groups.emplace(name, Group{}).first;
        }
        return it->second;
    }

    [[nodiscard]] std::string redacted(const HttpRequest& r, std::string_view text) const {
        return redact(text, r.secrets);
    }

    milliseconds backoff(const RetryPolicy& p, int attempt) {
        const auto cap = std::min<std::int64_t>(p.maxDelay.count(), p.baseDelay.count()
                                                                        << std::min(attempt, 20));
        std::uniform_int_distribution<std::int64_t> jitter(cap / 2, std::max<std::int64_t>(cap, 1));
        return milliseconds(jitter(rng));  // "Equal jitter": between half and the full delay.
    }

    void run(const std::stop_token& stop) {
        while (true) {
            std::vector<Completion> done;
            {
                std::unique_lock lock(mutex);
                if (stopping || stop.stop_requested()) {
                    break;
                }
                applyCancellations(done);
                startEligible(done);
            }
            deliver(done);

            int running = 0;
            curl_multi_perform(multi, &running);
            int left = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi, &left)) {
                if (msg->msg == CURLMSG_DONE) {
                    finish(msg->easy_handle, msg->data.result, done);
                }
            }
            deliver(done);
            curl_multi_poll(multi, nullptr, 0, static_cast<int>(pollTimeout().count()), nullptr);
        }
        // Shutdown: everything still queued or running is cancelled.
        std::vector<Completion> done;
        {
            const std::scoped_lock lock(mutex);
            for (Pending& p : queue) {
                done.push_back(
                    {std::move(p.callback), netError(NetErrorCode::Cancelled, "client shut down")});
            }
            queue.clear();
        }
        for (auto& [easy, a] : active) {
            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
            curl_slist_free_all(a->headerList);
            done.push_back({std::move(a->pending.callback),
                            netError(NetErrorCode::Cancelled, "client shut down")});
        }
        active.clear();
        deliver(done);
    }

    milliseconds pollTimeout() {
        const std::scoped_lock lock(mutex);
        milliseconds wait{100};
        const auto now = Clock::now();
        for (const Pending& p : queue) {
            if (p.notBefore > now) {
                wait = std::min(wait, std::chrono::ceil<milliseconds>(p.notBefore - now));
            }
        }
        return std::max(milliseconds{1}, wait);
    }

    static void deliver(std::vector<Completion>& done) {
        for (Completion& c : done) {
            if (c.callback) {
                c.callback(std::move(c.result));
            }
        }
        done.clear();
    }

    void applyCancellations(std::vector<Completion>& done) {
        for (const RequestId id : cancelRequests) {
            if (auto it = std::ranges::find(queue, id, &Pending::id); it != queue.end()) {
                done.push_back({std::move(it->callback),
                                netError(NetErrorCode::Cancelled, "request cancelled")});
                queue.erase(it);
                continue;
            }
            for (auto it = active.begin(); it != active.end(); ++it) {
                if (it->second->pending.id == id) {
                    Active& a = *it->second;
                    curl_multi_remove_handle(multi, a.easy);
                    curl_easy_cleanup(a.easy);
                    curl_slist_free_all(a.headerList);
                    groupLocked(a.pending.request.group).active -= 1;
                    --activeCount;
                    done.push_back({std::move(a.pending.callback),
                                    netError(NetErrorCode::Cancelled, "request cancelled")});
                    active.erase(it);
                    break;
                }
            }
        }
        cancelRequests.clear();
    }

    void startEligible(std::vector<Completion>& done) {
        const auto now = Clock::now();
        // Highest priority (lowest value) first, then submission order.
        std::ranges::sort(queue, [](const Pending& a, const Pending& b) {
            return a.request.priority != b.request.priority
                       ? a.request.priority < b.request.priority
                       : a.order < b.order;
        });
        for (auto it = queue.begin(); it != queue.end();) {
            if (std::cmp_greater_equal(activeCount, options.maxConcurrent)) {
                break;
            }
            Group& g = groupLocked(it->request.group);
            if (g.openUntil > now && it->attempt == 0) {
                done.push_back({std::move(it->callback),
                                netError(NetErrorCode::CircuitOpen,
                                         std::format("provider \"{}\" is failing; paused after "
                                                     "{} errors",
                                                     it->request.group, g.consecutiveFailures))});
                it = queue.erase(it);
                continue;
            }
            if (it->notBefore > now || g.active >= g.maxConcurrent || g.nextStart > now) {
                ++it;
                continue;
            }
            if (g.rps) {
                g.nextStart = now + std::chrono::duration_cast<Clock::duration>(
                                        std::chrono::duration<double>(1.0 / *g.rps));
            }
            if (auto started = startTransfer(std::move(*it)); !started) {
                done.push_back(std::move(started.error()));
            } else {
                g.active += 1;
                ++activeCount;
            }
            it = queue.erase(it);
        }
    }

    std::expected<void, Completion> startTransfer(Pending p) {
        CURL* easy = curl_easy_init();
        if (easy == nullptr) {
            return std::unexpected(
                Completion{std::move(p.callback),
                           netError(NetErrorCode::Connection, "curl_easy_init failed")});
        }
        auto a = std::make_unique<Active>();
        a->easy = easy;
        a->pending = std::move(p);
        const HttpRequest& r = a->pending.request;
        const char* protocols = r.allowInsecureHttp ? "http,https" : "https";
        curl_easy_setopt(easy, CURLOPT_URL, r.url.c_str());
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, protocols);
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, static_cast<long>(options.maxRedirects));
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(options.connectTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(options.totalTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING,
                         "");  // gzip/deflate; limit is on decoded bytes.
        curl_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(r.maxBytes));
        const std::string agent = options.userAgent.empty()
                                      ? std::format("DroneSimulator/{}", common::kVersion)
                                      : options.userAgent;
        curl_easy_setopt(easy, CURLOPT_USERAGENT, agent.c_str());  // libcurl copies strings.
        if (!options.proxy.empty()) {
            curl_easy_setopt(easy, CURLOPT_PROXY, options.proxy.c_str());
        }
        for (const std::string& h : r.headers) {
            a->headerList = curl_slist_append(a->headerList, h.c_str());
        }
        if (a->headerList != nullptr) {
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, a->headerList);
        }
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &writeBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, a.get());
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &writeHeader);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, a.get());
        curl_multi_add_handle(multi, easy);
        log::debug("net: GET {}", redacted(r, r.url));
        active.emplace(easy, std::move(a));
        return {};
    }

    /// Classifies a finished transfer: success, retry, or failure.
    void finish(CURL* easy, CURLcode code, std::vector<Completion>& done) {
        auto node = active.extract(easy);
        if (node.empty()) {
            return;
        }
        std::unique_ptr<Active> a = std::move(node.mapped());
        curl_multi_remove_handle(multi, easy);
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        const char* ct = nullptr;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &ct);
        const std::string contentType = ct != nullptr ? mediaType(ct) : std::string{};
        curl_easy_cleanup(easy);
        curl_slist_free_all(a->headerList);

        Pending& p = a->pending;
        const HttpRequest& r = p.request;
        p.attempt += 1;
        const RetryPolicy policy = r.retry.value_or(options.retry);
        if (status > 0 && r.onAttempt) {
            r.onAttempt(AttemptInfo{status, a->body.size()});
        }

        std::optional<NetError> error;
        bool retriable = false;
        std::optional<milliseconds> delay;
        if (code != CURLE_OK) {
            const std::string what = redacted(r, curl_easy_strerror(code));
            switch (code) {
                case CURLE_OPERATION_TIMEDOUT:
                    error = NetError{NetErrorCode::Timeout, "timed out", 0};
                    retriable = true;
                    break;
                case CURLE_TOO_MANY_REDIRECTS:
                    error = NetError{NetErrorCode::TooManyRedirects, "too many redirects", 0};
                    break;
                case CURLE_FILESIZE_EXCEEDED:
                    error = NetError{NetErrorCode::TooLarge, "response too large", 0};
                    break;
                case CURLE_WRITE_ERROR:
                    error = a->tooLarge ? NetError{NetErrorCode::TooLarge, "response too large", 0}
                                        : NetError{NetErrorCode::Connection, what, 0};
                    break;
                case CURLE_UNSUPPORTED_PROTOCOL:
                    error = NetError{NetErrorCode::InsecureUrl,
                                     "redirected to a protocol that is not allowed", 0};
                    break;
                case CURLE_COULDNT_RESOLVE_HOST:
                case CURLE_COULDNT_RESOLVE_PROXY:
                case CURLE_COULDNT_CONNECT:
                    error = NetError{NetErrorCode::Connection, what, 0};
                    retriable = true;
                    reachable->store(false);
                    break;
                case CURLE_PEER_FAILED_VERIFICATION:
                case CURLE_SSL_CONNECT_ERROR:
                case CURLE_SSL_CERTPROBLEM:
                case CURLE_SSL_CACERT_BADFILE:
                    error = NetError{NetErrorCode::Connection, "TLS error: " + what, 0};
                    break;
                default:
                    error = NetError{NetErrorCode::Connection, what, 0};
                    retriable = true;
                    break;
            }
        } else if ((status >= 200 && status < 300) || status == 304) {
            if (status != 304 && !r.contentTypes.empty() &&
                std::ranges::none_of(r.contentTypes, [&](const std::string& allowed) {
                    return mediaType(allowed) == contentType;
                })) {
                error =
                    NetError{NetErrorCode::ContentType,
                             std::format("unexpected content type \"{}\"", contentType), status};
            }
        } else {
            const std::string_view body(a->body.data(),
                                        std::min<std::size_t>(a->body.size(), 4096));
            if (r.isQuotaError && r.isQuotaError(status, body)) {
                error = NetError{
                    NetErrorCode::QuotaExceeded,
                    std::format("daily call limit of \"{}\" reached (HTTP {})", r.group, status),
                    status};
            } else {
                error = NetError{
                    NetErrorCode::HttpStatus,
                    std::format("HTTP {}: {}", status, redacted(r, trim(body.substr(0, 200)))),
                    status};
                retriable = status == 429 || status >= 500;
                if (status == 429 && a->headers.retryAfter) {
                    delay = *a->headers.retryAfter;
                    retriable = *delay <= policy.maxRetryAfter;
                }
            }
        }

        const std::scoped_lock lock(mutex);
        Group& g = groupLocked(r.group);
        g.active -= 1;
        --activeCount;
        const bool serverFailure =
            error &&
            (error->code == NetErrorCode::Timeout || error->code == NetErrorCode::Connection ||
             (error->code == NetErrorCode::HttpStatus &&
              (error->httpStatus >= 500 || error->httpStatus == 429)));
        if (serverFailure) {
            g.consecutiveFailures += 1;
            if (g.consecutiveFailures >= options.breaker.failureThreshold) {
                g.openUntil = Clock::now() + options.breaker.cooldown;
                log::warn("net: circuit for \"{}\" opened after {} failures", r.group,
                          g.consecutiveFailures);
                retriable = false;
            }
        } else if (!error || error->httpStatus > 0) {
            g.consecutiveFailures = 0;
            reachable->store(true);
        }

        if (!error) {
            HttpResponse response;
            response.status = status;
            response.body = std::move(a->body);
            response.contentType = contentType;
            response.etag = a->headers.etag;
            response.lastModified = a->headers.lastModified;
            response.maxAgeS = a->headers.maxAgeS;
            response.noStore = a->headers.noStore;
            response.attempts = p.attempt;
            done.push_back({std::move(p.callback), std::move(response)});
            return;
        }
        if (retriable && p.attempt < policy.maxAttempts) {
            const milliseconds wait = delay.value_or(backoff(policy, p.attempt - 1));
            log::info("net: {} — retry {} of {} in {} ms", error->message, p.attempt,
                      policy.maxAttempts - 1, wait.count());
            p.notBefore = Clock::now() + wait;
            queue.push_back(std::move(p));
            return;
        }
        if (p.attempt > 1) {
            error->message += std::format(" (after {} attempts)", p.attempt);
        }
        log::warn("net: {} failed: {}", redacted(r, r.url), error->message);
        done.push_back({std::move(p.callback), std::unexpected(std::move(*error))});
    }
};

HttpClient::HttpClient(HttpClientOptions options) : impl_(std::make_unique<Impl>()) {
    initCurlOnce();
    impl_->options = std::move(options);
    impl_->offline = &offline_;
    impl_->reachable = &reachable_;
    impl_->multi = curl_multi_init();
    impl_->thread =
        std::jthread([impl = impl_.get()](const std::stop_token& stop) { impl->run(stop); });
}

HttpClient::~HttpClient() {
    {
        const std::scoped_lock lock(impl_->mutex);
        impl_->stopping = true;
    }
    curl_multi_wakeup(impl_->multi);
    impl_->thread.join();
    curl_multi_cleanup(impl_->multi);
}

RequestId HttpClient::submit(HttpRequest request, HttpCallback callback) {
    std::optional<NetError> rejected;
    if (offline_.load()) {
        rejected = NetError{NetErrorCode::Offline, "offline mode", 0};
    } else if (!request.url.starts_with("https://") &&
               !(request.allowInsecureHttp && request.url.starts_with("http://"))) {
        rejected =
            NetError{NetErrorCode::InsecureUrl, "only https:// is allowed for this provider", 0};
    }
    RequestId id = 0;
    {
        const std::scoped_lock lock(impl_->mutex);
        id = impl_->nextId++;
        if (!rejected) {
            Pending p;
            p.id = id;
            p.request = std::move(request);
            p.callback = std::move(callback);
            p.order = impl_->nextOrder++;
            impl_->queue.push_back(std::move(p));
        }
    }
    if (rejected) {
        callback(std::unexpected(std::move(*rejected)));
    } else {
        curl_multi_wakeup(impl_->multi);
    }
    return id;
}

void HttpClient::cancel(RequestId id) {
    {
        const std::scoped_lock lock(impl_->mutex);
        impl_->cancelRequests.insert(id);
    }
    curl_multi_wakeup(impl_->multi);
}

void HttpClient::setGroupLimits(std::string_view group, int maxConcurrent,
                                std::optional<double> requestsPerSecond) {
    const std::scoped_lock lock(impl_->mutex);
    Group& g = impl_->groupLocked(std::string(group));
    g.maxConcurrent = std::max(1, maxConcurrent);
    g.rps = requestsPerSecond;
}

void HttpClient::setOffline(bool offline) noexcept { offline_.store(offline); }

bool HttpClient::circuitOpen(std::string_view group) const {
    const std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->groups.find(group);
    return it != impl_->groups.end() && it->second.openUntil > Clock::now();
}

std::size_t HttpClient::pendingCount() const {
    const std::scoped_lock lock(impl_->mutex);
    return impl_->queue.size() + impl_->activeCount;
}

}  // namespace ds::net
