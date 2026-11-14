// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "ds/common/random.hpp"
#include "ds/common/worker_pool.hpp"

namespace {

using ds::common::SplitMix64;
using ds::common::WorkerPool;

TEST(SplitMix64, KnownSequenceAndRanges) {
    // Reference values of SplitMix64 for seed 0 (Vigna's reference implementation).
    SplitMix64 rng(0);
    EXPECT_EQ(rng.next(), 0xE220A8397B1DCDAFULL);
    EXPECT_EQ(rng.next(), 0x6E789E6AA1B965F4ULL);
    EXPECT_EQ(rng.next(), 0x06C45D188009454FULL);

    SplitMix64 a(99);
    for (int i = 0; i < 10000; ++i) {
        const double u = a.uniform();
        EXPECT_GE(u, 0.0);
        EXPECT_LT(u, 1.0);
        EXPECT_LT(a.below(7), 7U);
    }
    EXPECT_NE(ds::common::randomSeed(), ds::common::randomSeed());
}

TEST(WorkerPool, RunsAllJobsInPriorityOrder) {
    WorkerPool pool(1);
    std::mutex mutex;
    std::vector<int> order;
    std::atomic<bool> release{false};
    // Block the single worker so the next jobs queue up.
    pool.submit(0.0, [&] {
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (const int p : {5, 1, 3, 1, 4}) {
        pool.submit(p, [&, p] {
            const std::scoped_lock lock(mutex);
            order.push_back(p);
        });
    }
    EXPECT_EQ(pool.pending(), 5U);
    release = true;
    pool.waitIdle();
    EXPECT_EQ(order, (std::vector<int>{1, 1, 3, 4, 5}));
}

TEST(WorkerPool, ParallelAndSurvivesExceptions) {
    WorkerPool pool(4);
    EXPECT_EQ(pool.threadCount(), 4U);
    EXPECT_GE(WorkerPool::defaultThreadCount(), 1U);
    std::atomic<int> done{0};
    std::mutex mutex;
    std::set<std::thread::id> threads;
    for (int i = 0; i < 64; ++i) {
        pool.submit(1.0, [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            {
                const std::scoped_lock lock(mutex);
                threads.insert(std::this_thread::get_id());
            }
            ++done;
        });
    }
    pool.submit(2.0, [] { throw std::runtime_error("boom"); });
    pool.waitIdle();
    EXPECT_EQ(done, 64);
    EXPECT_GT(threads.size(), 1U);
}

TEST(WorkerPool, DestructorDiscardsQueuedJobs) {
    std::atomic<int> ran{0};
    {
        WorkerPool pool(1);
        pool.submit(0.0, [] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
        for (int i = 0; i < 100; ++i) {
            pool.submit(1.0, [&] { ++ran; });
        }
    }
    EXPECT_LT(ran, 100);
}

}  // namespace
