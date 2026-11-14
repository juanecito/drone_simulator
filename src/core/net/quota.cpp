// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/net/quota.hpp"

#include <algorithm>
#include <format>

namespace ds::net {

common::Result<std::vector<std::int64_t>> InMemoryUsageLedger::callTimesSince(
    std::string_view providerKey, std::int64_t sinceUnixS) {
    const std::scoped_lock lock(mutex_);
    std::vector<std::int64_t> out;
    for (const ProviderCall& c : calls_) {
        if (c.providerKey == providerKey && c.calledAtUnixS >= sinceUnixS) {
            out.push_back(c.calledAtUnixS);
        }
    }
    std::ranges::sort(out);
    return out;
}

common::Result<void> InMemoryUsageLedger::record(const ProviderCall& call) {
    const std::scoped_lock lock(mutex_);
    calls_.push_back(call);
    return {};
}

std::vector<ProviderCall> InMemoryUsageLedger::calls() const {
    const std::scoped_lock lock(mutex_);
    return calls_;
}

common::Result<QuotaStatus> quotaStatus(UsageLedger& ledger, std::string_view providerKey,
                                        int dailyLimit, std::int64_t nowUnixS) {
    // A call made exactly 24 h ago has just left the window.
    auto times = ledger.callTimesSince(providerKey, nowUnixS - kQuotaWindowS + 1);
    if (!times) {
        return std::unexpected(times.error());
    }
    QuotaStatus s;
    s.limit = dailyLimit;
    s.used = static_cast<int>(times->size());
    s.remaining = std::max(0, dailyLimit - s.used);
    if (s.exhausted() && !times->empty()) {
        // The call that frees a slot is the (used − limit + 1)-th oldest.
        const auto index = static_cast<std::size_t>(s.used - dailyLimit);
        s.nextFreeAtUnixS = (*times)[std::min(index, times->size() - 1)] + kQuotaWindowS;
    }
    return s;
}

std::string quotaNotice(const QuotaStatus& status, int callsNeeded) {
    if (callsNeeded > status.remaining) {
        return std::format("needs {} calls but only {} of your {} daily calls remain", callsNeeded,
                           status.remaining, status.limit);
    }
    return std::format("uses {} of your {} remaining calls today", callsNeeded, status.remaining);
}

}  // namespace ds::net
