// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ds/common/error.hpp"
#include "ds/net/geo_rect.hpp"

/// @file
/// @brief Daily call budgets of quota-limited providers, counted over a rolling 24 h window
/// (FR-TER-O17, FR-NET-09).
/// @ingroup net

namespace ds::net {

/// @brief Length of the quota window in seconds (24 h).
inline constexpr std::int64_t kQuotaWindowS = 24 * 3600;

/// @brief One request that reached a provider (one row of `provider_calls`).
struct ProviderCall {
    std::string providerKey;         ///< Provider.
    std::int64_t calledAtUnixS = 0;  ///< When the request was sent (UTC, Unix seconds).
    std::string dataset;             ///< Dataset id.
    std::optional<GeoRect> area;     ///< Requested bounding box.
    long httpStatus = 0;             ///< Status received (0 if none).
    std::uint64_t bytes = 0;         ///< Response size.
};

/// @brief Persistent record of provider calls.
/// @details Implemented by `core/storage` over the `provider_calls` table; tests use
/// InMemoryUsageLedger. Implementations must be thread-safe.
class UsageLedger {
public:
    virtual ~UsageLedger() = default;
    /// @brief Times (Unix seconds, ascending) of the calls to @p providerKey at or after
    /// @p sinceUnixS.
    [[nodiscard]] virtual common::Result<std::vector<std::int64_t>> callTimesSince(
        std::string_view providerKey, std::int64_t sinceUnixS) = 0;
    /// @brief Adds @p call.
    [[nodiscard]] virtual common::Result<void> record(const ProviderCall& call) = 0;
};

/// @brief Thread-safe in-memory ledger (tests, in-memory database fallback).
class InMemoryUsageLedger final : public UsageLedger {
public:
    [[nodiscard]] common::Result<std::vector<std::int64_t>> callTimesSince(
        std::string_view providerKey, std::int64_t sinceUnixS) override;
    [[nodiscard]] common::Result<void> record(const ProviderCall& call) override;
    /// @brief Every recorded call.
    [[nodiscard]] std::vector<ProviderCall> calls() const;

private:
    mutable std::mutex mutex_;
    std::vector<ProviderCall> calls_;
};

/// @brief Budget of a provider at one moment.
struct QuotaStatus {
    int limit = 0;      ///< Calls allowed per 24 h.
    int used = 0;       ///< Calls in the last 24 h.
    int remaining = 0;  ///< limit − used, at least 0.
    /// When the oldest call in the window leaves it (a call becomes available again).
    std::optional<std::int64_t> nextFreeAtUnixS;

    /// @brief True if no call is left.
    [[nodiscard]] bool exhausted() const noexcept { return remaining <= 0; }
};

/// @brief Budget of @p providerKey at @p nowUnixS for a limit of @p dailyLimit calls.
[[nodiscard]] common::Result<QuotaStatus> quotaStatus(UsageLedger& ledger,
                                                      std::string_view providerKey, int dailyLimit,
                                                      std::int64_t nowUnixS);

/// @brief The pre-download notice of FR-TER-O17, e.g. "uses 2 of your 48 remaining calls today".
[[nodiscard]] std::string quotaNotice(const QuotaStatus& status, int callsNeeded);

}  // namespace ds::net
