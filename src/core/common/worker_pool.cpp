// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/common/worker_pool.hpp"

#include <algorithm>
#include <exception>

#include "ds/common/log.hpp"

namespace ds::common {

std::size_t WorkerPool::defaultThreadCount() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 3 ? hardware - 2 : 1;
}

WorkerPool::WorkerPool(std::size_t threads) {
    const std::size_t count = threads == 0 ? defaultThreadCount() : threads;
    threads_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads_.emplace_back([this](const std::stop_token& stop) { run(stop); });
    }
}

WorkerPool::~WorkerPool() {
    {
        const std::scoped_lock lock(mutex_);
        queue_ = {};
    }
    for (auto& thread : threads_) {
        thread.request_stop();
    }
    wake_.notify_all();
    threads_.clear();  // Joins.
}

void WorkerPool::submit(double priority, Job job) {
    {
        const std::scoped_lock lock(mutex_);
        queue_.push(Entry{priority, nextOrder_++, std::move(job)});
    }
    wake_.notify_one();
}

std::size_t WorkerPool::pending() const {
    const std::scoped_lock lock(mutex_);
    return queue_.size();
}

void WorkerPool::waitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

void WorkerPool::run(const std::stop_token& stop) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
                return;  // Stop requested.
            }
            job = std::move(queue_.top().job);
            queue_.pop();
            ++running_;
        }
        try {
            job();
        } catch (const std::exception& e) {
            log::error("Background job failed: {}", e.what());
        } catch (...) {
            log::error("Background job failed with an unknown exception");
        }
        {
            const std::scoped_lock lock(mutex_);
            --running_;
            if (queue_.empty() && running_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

}  // namespace ds::common
