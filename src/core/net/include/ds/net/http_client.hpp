// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ds/net/net_error.hpp"

/// @file
/// @brief Asynchronous HTTP(S) client on libcurl's multi interface, running on the network
/// thread (FR-NET-01 … FR-NET-03, FR-NET-10). libcurl types stay inside this module (§3.1).
/// @ingroup net

namespace ds::net {

/// @brief Identifies a submitted request (for cancel()).
using RequestId = std::uint64_t;

/// @brief Retry behaviour for timeouts, connection errors, 5xx and 429 (FR-NET-03).
struct RetryPolicy {
    int maxAttempts = 4;                               ///< Including the first attempt.
    std::chrono::milliseconds baseDelay{500};          ///< First backoff (doubles each time).
    std::chrono::milliseconds maxDelay{30'000};        ///< Backoff cap.
    std::chrono::milliseconds maxRetryAfter{120'000};  ///< Longer Retry-After ⇒ give up.
};

/// @brief Circuit breaker per request group (FR-NET-03).
struct CircuitBreakerPolicy {
    int failureThreshold = 5;                    ///< Consecutive failed attempts that open it.
    std::chrono::milliseconds cooldown{60'000};  ///< How long it stays open.
};

/// @brief Client-wide settings.
struct HttpClientOptions {
    std::string userAgent;  ///< Empty ⇒ `DroneSimulator/<version>` (FR-NET-10).
    std::chrono::milliseconds connectTimeout{10'000};  ///< FR-NET-02.
    std::chrono::milliseconds totalTimeout{30'000};    ///< FR-NET-02.
    int maxRedirects = 5;                              ///< FR-NET-02.
    int maxConcurrent = 8;                             ///< Simultaneous transfers overall.
    RetryPolicy retry;                                 ///< Default retry policy.
    CircuitBreakerPolicy breaker;                      ///< Circuit breaker.
    std::string proxy;  ///< Explicit proxy (Settings); empty ⇒ `https_proxy`/`no_proxy`.
};

/// @brief What one attempt produced (for quota accounting).
struct AttemptInfo {
    long httpStatus = 0;      ///< 0 if no response was received.
    std::uint64_t bytes = 0;  ///< Body bytes received.
};

/// @brief A request.
struct HttpRequest {
    std::string url;                   ///< Full URL; may contain secrets.
    std::vector<std::string> headers;  ///< Extra headers, `Name: value`.
    std::vector<std::string> secrets;  ///< Redacted from every message and log (FR-NET-07).
    std::string group;      ///< Concurrency / rate-limit / circuit-breaker group (provider key).
    double priority = 0.0;  ///< Lower runs first (FR-NET-01).
    bool allowInsecureHttp = false;                     ///< Permit `http://` (FR-NET-02).
    std::uint64_t maxBytes = std::uint64_t{64} << 20U;  ///< Response size limit.
    std::vector<std::string> contentTypes;              ///< Allowed content types; empty ⇒ any.
    std::optional<RetryPolicy> retry;                   ///< Overrides the client's policy.
    /// @brief Recognises a daily-quota error, which fails at once and is never retried
    /// (FR-TER-O17).
    std::function<bool(long status, std::string_view body)> isQuotaError;
    /// @brief Called on the network thread after every attempt that reached the server.
    std::function<void(const AttemptInfo&)> onAttempt;
};

/// @brief A successful response (2xx, or 304 for a conditional request).
struct HttpResponse {
    long status = 0;                      ///< HTTP status.
    std::string body;                     ///< Response body.
    std::string contentType;              ///< Content type without parameters, lower case.
    std::string etag;                     ///< ETag header.
    std::string lastModified;             ///< Last-Modified header.
    std::optional<std::int64_t> maxAgeS;  ///< Cache-Control max-age.
    bool noStore = false;                 ///< Cache-Control no-store.
    int attempts = 0;                     ///< Attempts made.
};

/// @brief Receives the outcome of a request, on the network thread. Called exactly once,
/// including for cancelled requests.
using HttpCallback = std::move_only_function<void(NetResult<HttpResponse>)>;

/// @brief Asynchronous HTTP client with priorities, cancellation, limits, retries, rate limits
/// and a circuit breaker per group. Thread-safe.
class HttpClient {
public:
    /// @brief Starts the network thread.
    explicit HttpClient(HttpClientOptions options = {});
    /// @brief Cancels everything (callbacks receive Cancelled) and joins the thread.
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    /// @brief Queues @p request. Fails at once (through @p callback) when offline, for an
    /// insecure URL or when the group's circuit is open.
    RequestId submit(HttpRequest request, HttpCallback callback);
    /// @brief Cancels a queued or running request (no-op if it already finished).
    void cancel(RequestId id);

    /// @brief Limits a group to @p maxConcurrent transfers and @p requestsPerSecond starts.
    void setGroupLimits(std::string_view group, int maxConcurrent,
                        std::optional<double> requestsPerSecond);
    /// @brief Manual offline mode (FR-NET-06): new requests fail with Offline.
    void setOffline(bool offline) noexcept;
    /// @brief True in manual offline mode.
    [[nodiscard]] bool offline() const noexcept { return offline_.load(); }
    /// @brief False after a request failed because the network or host was unreachable,
    /// true again after the next success (automatic offline detection, FR-NET-06).
    [[nodiscard]] bool networkReachable() const noexcept { return reachable_.load(); }
    /// @brief True while the circuit of @p group is open (shown in the UI, FR-NET-03).
    [[nodiscard]] bool circuitOpen(std::string_view group) const;
    /// @brief Requests queued or running.
    [[nodiscard]] std::size_t pendingCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> offline_{false};
    std::atomic<bool> reachable_{true};
};

}  // namespace ds::net
