// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <queue>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/// @file
/// @brief Prioritised worker pool for background jobs such as terrain chunks (SPECIFICATION
/// §3.3).
/// @ingroup common

namespace ds::common {

/// @brief Fixed set of worker threads running prioritised jobs.
/// @details Jobs with a lower priority value run first; equal priorities run in submission
/// order. Jobs must not throw (an escaping exception is logged and swallowed). The
/// destructor discards jobs that have not started and joins the threads.
class WorkerPool {
public:
    /// @brief A unit of work.
    using Job = std::move_only_function<void()>;

    /// @brief Default thread count: hardware threads − 2, at least 1 (SPECIFICATION §3.3).
    [[nodiscard]] static std::size_t defaultThreadCount() noexcept;

    /// @brief Starts @p threads workers (0 ⇒ defaultThreadCount()).
    explicit WorkerPool(std::size_t threads = 0);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    /// @brief Queues @p job with @p priority (lower runs earlier).
    void submit(double priority, Job job);

    /// @brief Number of worker threads.
    [[nodiscard]] std::size_t threadCount() const noexcept { return threads_.size(); }

    /// @brief Jobs queued but not started.
    [[nodiscard]] std::size_t pending() const;

    /// @brief Blocks until the queue is empty and no job is running (tests, shutdown).
    void waitIdle();

private:
    struct Entry {
        double priority;
        std::uint64_t order;
        mutable Job job;
        bool operator<(const Entry& other) const noexcept {
            // std::priority_queue is a max-heap: "less" means "runs later".
            if (priority != other.priority) {
                return priority > other.priority;
            }
            return order > other.order;
        }
    };

    void run(const std::stop_token& stop);

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::priority_queue<Entry> queue_;
    std::uint64_t nextOrder_ = 0;
    std::size_t running_ = 0;
    std::vector<std::jthread> threads_;
};

}  // namespace ds::common
