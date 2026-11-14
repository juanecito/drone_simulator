// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief Type-safe, `std::variant`-based publish/subscribe bus (FR-LOOP-07).
/// @ingroup common

namespace ds::common {

/// @brief Synchronous event bus for a closed set of event types.
/// @details Handlers run on the thread that calls publish(), in subscription order.
/// Subscribing and unsubscribing are thread-safe and may happen inside a handler (the change
/// applies from the next publish()).
template <typename... Events>
class EventBus {
public:
    /// @brief Any of the event types.
    using Event = std::variant<Events...>;
    /// @brief Handler for every event.
    using Handler = std::function<void(const Event&)>;
    /// @brief Identifies a subscription for unsubscribe().
    using SubscriptionId = std::uint64_t;

    /// @brief Calls @p handler for every published event.
    SubscriptionId subscribe(Handler handler) {
        const std::scoped_lock lock(mutex_);
        const SubscriptionId id = ++lastId_;
        handlers_.push_back({id, std::move(handler)});
        return id;
    }

    /// @brief Calls @p handler only for events of type @p E.
    template <typename E>
    SubscriptionId subscribe(std::function<void(const E&)> handler) {
        return subscribe(Handler([h = std::move(handler)](const Event& event) {
            if (const E* e = std::get_if<E>(&event)) {
                h(*e);
            }
        }));
    }

    /// @brief Removes a subscription. Returns false if @p id is unknown.
    bool unsubscribe(SubscriptionId id) {
        const std::scoped_lock lock(mutex_);
        const auto it = std::ranges::find(handlers_, id, &Entry::id);
        if (it == handlers_.end()) {
            return false;
        }
        handlers_.erase(it);
        return true;
    }

    /// @brief Delivers @p event to every current subscriber.
    void publish(const Event& event) const {
        std::vector<Entry> handlers;
        {
            const std::scoped_lock lock(mutex_);
            handlers = handlers_;
        }
        for (const Entry& entry : handlers) {
            entry.handler(event);
        }
    }

    /// @brief Number of subscriptions.
    [[nodiscard]] std::size_t subscriberCount() const {
        const std::scoped_lock lock(mutex_);
        return handlers_.size();
    }

private:
    struct Entry {
        SubscriptionId id;
        Handler handler;
    };
    mutable std::mutex mutex_;
    std::vector<Entry> handlers_;
    SubscriptionId lastId_ = 0;
};

}  // namespace ds::common
